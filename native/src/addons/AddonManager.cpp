#include "AddonManager.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/LanguageCodes.h"  // header-only canonical language mapping (no Settings.cpp link needed here)
#include "../core/BrandMigration.h"  // the reserved-namespace guard still covers the previous prefix until migrated
#include "../core/SubtitleHash.h"    // the OSDb moviehash used as the /subtitles videoHash extra
#include "AddonContext.h"
#include "JsAddon.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QUrlQuery>
#include <QSet>
#include <QDateTime>
#include <QStandardPaths>
#include <QDebug>
#include <cstring>

#include "miniz.h"

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// --- pure helpers (thread-safe: no AddonManager state) -----------------------------------------------

// Resolve a relative item URL/thumbnail to an absolute path (addon folder first, then app dir).
static QString resolveUrlIn(const QString& url, const QString& addonDir)
{
    if (url.isEmpty()) return url;
    if (url.contains(QStringLiteral("://"))) return url; // http(s)/file/magnet - leave as-is
    QFileInfo fi(url);
    if (fi.isAbsolute() && fi.exists()) return fi.absoluteFilePath();
    const QString inAddon = QDir::cleanPath(addonDir + QStringLiteral("/") + url);
    if (QFile::exists(inAddon)) return inAddon;
    const QString inApp = QDir::cleanPath(AppPaths::dataDir() + QStringLiteral("/") + url);
    if (QFile::exists(inApp)) return inApp;
    return url;
}

// Load the addon in a fresh JS context, invoke one function, and return the resolved catalog. Runs on
// whatever thread calls it (GUI for the sync API, a pool thread for the async API) - self-contained.
static MediaCatalog executeRequest(const AddonRequest& req)
{
    if (req.source.isEmpty()) return {};
    auto ctx = std::make_unique<AddonContext>(req.manifest, req.storageDir);
    QString err;
    std::unique_ptr<JsAddon> addon = JsAddon::load(req.source, std::move(ctx), &err);
    if (!addon)
    {
        qWarning().noquote() << QStringLiteral("addon '%1' failed to load: %2").arg(req.manifest.id, err);
        return {};
    }
    // getDetail/search are optional; getCatalog is assumed present.
    if ((req.function == QStringLiteral("getDetail") || req.function == QStringLiteral("search"))
        && !addon->hasFunction(req.function))
        return {};

    MediaCatalog cat = MediaCatalog::fromJson(addon->invoke(req.function, req.argJson).toUtf8());
    for (MediaItem& it : cat.items)
    {
        it.url = resolveUrlIn(it.url, req.dir);
        it.thumbnailUrl = resolveUrlIn(it.thumbnailUrl, req.dir);
    }
    return cat;
}

// Load the addon in a fresh JS context and invoke getMeta(), returning the parsed item metadata. Like
// executeRequest but for the single-item detail header; getMeta is optional (absent -> invalid detail).
static MediaDetail executeMetaRequest(const AddonRequest& req)
{
    if (req.source.isEmpty()) return {};
    auto ctx = std::make_unique<AddonContext>(req.manifest, req.storageDir);
    QString err;
    std::unique_ptr<JsAddon> addon = JsAddon::load(req.source, std::move(ctx), &err);
    if (!addon) return {};
    if (!addon->hasFunction(req.function)) return {};

    MediaDetail d = MediaDetail::fromJson(addon->invoke(req.function, req.argJson).toUtf8());
    d.imageUrl = resolveUrlIn(d.imageUrl, req.dir);
    return d;
}

static QString itemArg(const MediaItem& item)
{
    // id + type have always been sent; also pass the title/subtitle/platform/alt-names so a metadata
    // provider (esp. the game artwork aggregators) can search by name and disambiguate by console. All
    // optional — existing addons that only read id/type are unaffected.
    QJsonObject a{ { QStringLiteral("id"), item.id }, { QStringLiteral("type"), item.type } };
    if (!item.title.isEmpty())      a.insert(QStringLiteral("title"), item.title);
    if (!item.subtitle.isEmpty())   a.insert(QStringLiteral("subtitle"), item.subtitle);
    if (!item.systemHint.isEmpty()) a.insert(QStringLiteral("systemHint"), item.systemHint);
    if (!item.altNames.isEmpty())   a.insert(QStringLiteral("altNames"), QJsonArray::fromStringList(item.altNames));
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

static QString catalogArg(const QString& catalogId, const QString& query, int page,
                          const QMap<QString, QString>& filters = {})
{
    QJsonObject a;
    if (!catalogId.isEmpty()) a.insert(QStringLiteral("catalog"), catalogId);
    if (!query.isEmpty())     a.insert(QStringLiteral("query"), query);
    a.insert(QStringLiteral("page"), page);
    // Selected filters (genre/year/rating/sort) -> the addon's getCatalog applies them.
    for (auto it = filters.constBegin(); it != filters.constEnd(); ++it)
        if (!it.value().isEmpty()) a.insert(it.key(), it.value());
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

// --- remote (HTTP) addon transport helpers -----------------------------------------------------------
// A remote addon speaks the SAME JSON contract as a local JS addon (MediaCatalog / MediaDetail), just over
// HTTP. URLs are path-style and end in ".json" so a service can be a Cloudflare Worker OR plain static
// files (GitHub Pages / a local folder) - both serve the exact same layout:
//   {base}/manifest.json
//   {base}/catalog/{catalogId}.json            (+ "/search={q}" and/or "/page={n}" path segments)
//   {base}/detail/{type}/{id}.json             (+ "/page={n}")   -> a container's children
//   {base}/meta/{type}/{id}.json               -> the detail-header metadata
static QString segEnc(const QString& s) { return QString::fromUtf8(QUrl::toPercentEncoding(s)); }

static QUrl remoteCatalogUrl(const QString& base, const QString& catalogId, const QString& query, int page,
                             const QMap<QString, QString>& filters = {})
{
    QString u = base + QStringLiteral("/catalog/") + segEnc(catalogId.isEmpty() ? QStringLiteral("default") : catalogId);
    QStringList extra;
    if (!query.isEmpty()) extra << QStringLiteral("search=") + segEnc(query);
    for (auto it = filters.constBegin(); it != filters.constEnd(); ++it) // genre=/year=/rating=/sort=
        if (!it.value().isEmpty()) extra << it.key() + QLatin1Char('=') + segEnc(it.value());
    if (page > 1)         extra << QStringLiteral("page=") + QString::number(page);
    if (!extra.isEmpty()) u += QStringLiteral("/") + extra.join(QLatin1Char('&'));
    return QUrl(u + QStringLiteral(".json"));
}

static QUrl remoteDetailUrl(const QString& base, const QString& type, const QString& id, int page)
{
    QString u = base + QStringLiteral("/detail/") + segEnc(type.isEmpty() ? QStringLiteral("item") : type)
              + QStringLiteral("/") + segEnc(id);
    if (page > 1) u += QStringLiteral("/page=") + QString::number(page);
    return QUrl(u + QStringLiteral(".json"));
}

static QUrl remoteMetaUrl(const QString& base, const QString& type, const QString& id)
{
    return QUrl(base + QStringLiteral("/meta/") + segEnc(type.isEmpty() ? QStringLiteral("item") : type)
               + QStringLiteral("/") + segEnc(id) + QStringLiteral(".json"));
}

static QUrl remoteStreamUrl(const QString& base, const QString& type, const QString& id, int attempt = 0)
{
    QString u = base + QStringLiteral("/stream/") + segEnc(type.isEmpty() ? QStringLiteral("item") : type)
              + QStringLiteral("/") + segEnc(id) + QStringLiteral(".json");
    QStringList q;
    if (attempt > 0) q << (QStringLiteral("n=") + QString::number(attempt)); // ask the provider for the n-th best source
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    // Desktop can shell out to curl, so tell the addon it may hand back a Cloudflare-gated source (lolroms)
    // as a DIRECT url for us to fetch ourselves — bypassing its proxy and any Cloudflare-tunnel size limit.
    q << QStringLiteral("dl=curl");
#endif
    if (!q.isEmpty()) u += QStringLiteral("?") + q.join(QLatin1Char('&'));
    return QUrl(u);
}

// Resolve a (possibly relative) item URL/thumbnail returned by a remote addon against its base URL.
static QString resolveRemoteUrl(const QString& url, const QString& base)
{
    if (url.isEmpty() || url.contains(QStringLiteral("://"))) return url;
    return QUrl(base + QStringLiteral("/")).resolved(QUrl(url)).toString();
}

// Blocking GET for the synchronous API (console probe / tests). The UI never uses this - it goes through
// the async dispatchRemote* path instead.
static QByteArray httpGetBlocking(const QUrl& url, const QByteArray& cfgHeader = {}, QString* err = nullptr)
{
    QNetworkAccessManager nam;
    QNetworkRequest rq(url);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!cfgHeader.isEmpty()) rq.setRawHeader(AppBrand::kConfigHeader, cfgHeader);
    QEventLoop loop;
    QNetworkReply* reply = nam.get(rq);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray data;
    if (reply->error() == QNetworkReply::NoError) data = reply->readAll();
    else if (err) *err = reply->errorString();
    reply->deleteLater();
    return data;
}

// Normalise a user-entered URL to the service base (drop a trailing "/manifest.json" and slash).
static QString normalizeBase(const QString& raw)
{
    QString b = raw.trimmed();
    if (b.endsWith(QStringLiteral("/manifest.json"))) b.chop(int(strlen("/manifest.json")));
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b;
}

static QString manifestCacheKey(const QString& base)
{
    const QByteArray h = QCryptographicHash::hash(base.toUtf8(), QCryptographicHash::Md5).toHex();
    return QStringLiteral("addon.remote.manifest.") + QString::fromUtf8(h);
}

// Per-addon ETag of the last package we pulled from its updateUrl, so an unchanged package answers 304.
static QString updateEtagKey(const QString& id)
{
    return QStringLiteral("addon.update.etag.") + id;
}

// Compare dotted version strings numerically (1.10 > 1.9). Missing trailing parts count as 0; a non-numeric
// part is treated as 0. Returns <0 if a<b, 0 if equal, >0 if a>b.
static int versionCompare(const QString& a, const QString& b)
{
    const QStringList pa = a.split(QLatin1Char('.'));
    const QStringList pb = b.split(QLatin1Char('.'));
    const int n = qMax(pa.size(), pb.size());
    for (int i = 0; i < n; ++i)
    {
        const int va = i < pa.size() ? pa[i].section(QLatin1Char('-'), 0, 0).toInt() : 0;
        const int vb = i < pb.size() ? pb[i].section(QLatin1Char('-'), 0, 0).toInt() : 0;
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

// Read the "version" out of a .addon package held in memory (its top-level manifest.json). Returns false if
// the bytes aren't a readable zip with a valid manifest, so a junk/HTML response can't be mistaken for one.
static bool packageVersion(const QByteArray& pkg, QString* versionOut)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, pkg.constData(), size_t(pkg.size()), 0)) return false;
    bool ok = false;
    const int idx = mz_zip_reader_locate_file(&zip, "manifest.json", nullptr, 0);
    if (idx >= 0)
    {
        size_t sz = 0;
        if (void* data = mz_zip_reader_extract_to_heap(&zip, mz_uint(idx), &sz, 0))
        {
            const AddonManifest m = AddonManifest::fromJson(QByteArray(static_cast<char*>(data), int(sz)), &ok);
            if (ok && versionOut) *versionOut = m.version;
            mz_free(data);
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

// A /stream response is either {"url":"...","mime":"..."} or {"streams":[{"url","mime"}...]}; take the
// first playable url and resolve it against the addon base. Returns url (and mime via out-param).
static QString parseStreamJson(const QByteArray& body, const QString& base, QString* mime = nullptr)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    QJsonObject src = o;
    if (!o.contains(QStringLiteral("url")))
    {
        const QJsonArray streams = o.value(QStringLiteral("streams")).toArray();
        if (streams.isEmpty()) return {};
        src = streams.first().toObject();
    }
    if (mime) *mime = src.value(QStringLiteral("mime")).toString();
    return resolveRemoteUrl(src.value(QStringLiteral("url")).toString(), base);
}

// Per-user config for a remote addon: the user's values for the addon's declared settings (API keys etc.),
// base64url(JSON), sent as the AppBrand::kConfigHeader header so the service uses THIS user's keys (not
// baked-in ones). Exactly ONE header is sent — deliberately no dual-send of the legacy name, which would
// put user API keys in a second header indefinitely and leave the rename with no end. That makes DEPLOY
// ORDER load-bearing: any remote service must be redeployed to read the new header BEFORE an app build
// sending it ships, or its per-user credentials silently arrive empty (see the note in the aiocatalog
// Worker's fetch handler).
static QByteArray remoteConfigHeader(const LoadedAddon* src)
{
    if (!src) return {};
    QJsonObject o;
    for (const AddonSetting& s : src->manifest.settings)
    {
        const QString v = AddonContext::readConfig(src->manifest.id, s.key);
        if (!v.isEmpty()) o.insert(s.key, v);
    }
    if (o.isEmpty()) return {};
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding);
}

// Headers attached only to OUR OWN server (non-Stremio) addon requests: the per-caller config
// blob and the preferred content language. Never sent to third-party Stremio addons. The preferred
// language comes from the header-only LanguageCodes::readPreferred (guarding on content/language key
// PRESENCE, so an explicit "no preference" never resurfaces the legacy subs/language) reading the
// file-static store() above — no link dependency on Settings.cpp, which probe_addon/probe_gameagg/
// probe_engine compile AddonManager.cpp without linking.
static void applyServerHeaders(QNetworkRequest& rq, const LoadedAddon* src)
{
    const QByteArray cfg = remoteConfigHeader(src);
    if (!cfg.isEmpty()) rq.setRawHeader(AppBrand::kConfigHeader, cfg);
    const QString lang = LanguageCodes::readPreferred(store());
    if (!lang.isEmpty()) rq.setRawHeader("Accept-Language", lang.toUtf8());
}

// --- emulator BIOS provisioning (through the EBS/Allarr file provider) --------------------------------
// A BIOS catalog item id is "bios:bs:{systemId}:{fileName}". The fileName is everything after the systemId
// (it may itself contain '/', e.g. "hatari/tos/tos.img", but never a ':'), so split on the first ':' after
// the fixed "bios:bs:" prefix. Returns false for anything that isn't a BIOS item id.
static bool parseBiosItemId(const QString& id, QString* systemId, QString* fileName)
{
    static const QString kPrefix = QStringLiteral("bios:bs:");
    if (!id.startsWith(kPrefix)) return false;
    const QString rest = id.mid(kPrefix.size());
    const int colon = rest.indexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    const QString sys = rest.left(colon);
    const QString fn  = rest.mid(colon + 1);
    if (fn.isEmpty()) return false;
    if (systemId) *systemId = sys;
    if (fileName) *fileName = fn;
    return true;
}

// Lowercase-hex md5 of a byte buffer / of a file on disk (streamed), to match a catalog item's subtitle.
static QString md5HexOf(const QByteArray& data)
{ return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex()); }
static QString md5HexOfFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Md5);
    if (!h.addData(&f)) return QString();
    return QString::fromLatin1(h.result().toHex());
}

// The BIOS catalog URL ("/catalog/bios:bios[.json | /search={systemId}.json]") and the per-item stream URL
// ("/stream/game/{id}.json" — deliberately WITHOUT the ?dl=curl the media /stream path adds, so a BIOS
// always resolves to the provider's plain relative files/ url). segEnc/remoteCatalogUrl already percent-
// encode the colons the same way every other provider request does, which the server decodes.
static QUrl biosCatalogUrl(const QString& base, const QString& systemId)
{ return remoteCatalogUrl(base, QStringLiteral("bios:bios"), systemId, 1); }
static QUrl biosStreamUrl(const QString& base, const QString& itemId)
{ return QUrl(base + QStringLiteral("/stream/game/") + segEnc(itemId) + QStringLiteral(".json")); }

// {metas:[...]} for the bios catalog -> the BIOS files it lists (id parsed for systemId/fileName, subtitle
// carried as the expected md5). Bad/foreign rows are skipped.
static QList<AddonManager::BiosServerFile> parseBiosFiles(const QByteArray& body)
{
    QList<AddonManager::BiosServerFile> out;
    const MediaCatalog cat = MediaCatalog::fromJson(body);
    for (const MediaItem& it : cat.items)
    {
        QString sys, fn;
        if (!parseBiosItemId(it.id, &sys, &fn)) continue;
        out.append({ fn, it.subtitle.trimmed().toLower(), it.id });
    }
    return out;
}

// One async, best-effort BIOS provisioning run for a system, chained on QNetworkReply::finished (no nested
// event loop): GET the per-system bios catalog -> filter to files missing from destDir (or present with the
// wrong md5) -> for each, resolve its /stream, download the bytes, verify md5 when present, and write it.
// Parented to the caller's context, so a torn-down launch cancels the whole chain and onDone never runs.
namespace {
class BiosServerFetcher : public QObject
{
public:
    BiosServerFetcher(QString base, QByteArray cfg,
                      QString systemId, QString destDir, QObject* context,
                      std::function<void(const QString&)> onStatus, std::function<void()> onDone)
        : QObject(context), base_(std::move(base)), cfg_(std::move(cfg)),
          destDir_(std::move(destDir)), onStatus_(std::move(onStatus)), onDone_(std::move(onDone))
    {
        // Own QNAM (parented to this): a torn-down launch destroys the fetcher, which aborts + deletes every
        // in-flight reply with it — so nothing leaks and no stale callback fires.
        nam_ = new QNetworkAccessManager(this);
        QNetworkRequest rq(biosCatalogUrl(base_, systemId));
        apply(rq);
        rq.setTransferTimeout(45000);
        QNetworkReply* r = nam_->get(rq);
        connect(r, &QNetworkReply::finished, this, [this, r] { onCatalog(r); });
    }

private:
    void apply(QNetworkRequest& rq) const
    {
        rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        if (!cfg_.isEmpty()) rq.setRawHeader(AppBrand::kConfigHeader, cfg_);
    }
    void finish() { if (onDone_) onDone_(); deleteLater(); }
    void onCatalog(QNetworkReply* r)
    {
        r->deleteLater();
        if (r->error() == QNetworkReply::NoError)
        {
            for (const AddonManager::BiosServerFile& f : parseBiosFiles(r->readAll()))
            {
                const QString out = destDir_ + QStringLiteral("/") + f.fileName;
                if (QFile::exists(out))
                {
                    if (f.md5.isEmpty()) continue;                        // presence-only, already have it
                    if (md5HexOfFile(out) == f.md5) continue;            // present and verified good
                }
                pending_.append(f);                                      // missing, or a wrong-hash copy to replace
            }
        }
        next();
    }
    void next()
    {
        if (pending_.isEmpty()) { finish(); return; }
        cur_ = pending_.takeFirst();
        if (onStatus_) onStatus_(tr("Downloading BIOS %1…").arg(cur_.fileName));
        QNetworkRequest rq(biosStreamUrl(base_, cur_.itemId));
        apply(rq);
        rq.setTransferTimeout(45000);
        QNetworkReply* r = nam_->get(rq);
        connect(r, &QNetworkReply::finished, this, [this, r] { onStream(r); });
    }
    void onStream(QNetworkReply* r)
    {
        r->deleteLater();
        QString url;
        if (r->error() == QNetworkReply::NoError) url = parseStreamJson(r->readAll(), base_);
        if (url.isEmpty()) { next(); return; }                           // couldn't resolve — leave missing, move on
        QNetworkRequest rq((QUrl(url)));
        apply(rq);
        rq.setTransferTimeout(60000);                                    // generous for the largest dump (~4 MB PS2)
        QNetworkReply* dr = nam_->get(rq);
        connect(dr, &QNetworkReply::finished, this, [this, dr] { onBytes(dr); });
    }
    void onBytes(QNetworkReply* r)
    {
        r->deleteLater();
        if (r->error() == QNetworkReply::NoError)
        {
            const QByteArray data = r->readAll();
            // Verify md5 when the catalog carried one; a mismatch (or an empty body) writes NOTHING — the
            // core/emulator then reports "BIOS not found" itself, exactly as a failed fetch would.
            if (!data.isEmpty() && (cur_.md5.isEmpty() || md5HexOf(data) == cur_.md5))
            {
                const QString out = destDir_ + QStringLiteral("/") + cur_.fileName;
                QDir().mkpath(QFileInfo(out).absolutePath());            // fileName may include a subfolder
                QFile f(out);
                if (f.open(QIODevice::WriteOnly)) { f.write(data); f.close(); }
            }
        }
        next();
    }

    QNetworkAccessManager* nam_ = nullptr;
    QString base_;
    QByteArray cfg_;
    QString destDir_;
    std::function<void(const QString&)> onStatus_;
    std::function<void()> onDone_;
    QList<AddonManager::BiosServerFile> pending_;
    AddonManager::BiosServerFile cur_;
};
} // namespace

// --- Stremio protocol dialect ------------------------------------------------------------------------
// Stremio addons are HTTP services with a manifest.json declaring resources (catalog/meta/stream) + types,
// and routes /catalog/{type}/{id}.json, /meta/{type}/{id}.json, /stream/{type}/{id}.json. We translate
// those into our MediaCatalog / MediaDetail / MediaItem models so they appear as ordinary catalogs.

// Detect + parse a Stremio manifest into one of our AddonManifests. The RULES live in StremioTranslate;
// this only maps the result onto the shapes the rest of AddonManager already consumes.
static bool parseStremioManifest(const QByteArray& json, AddonManifest* outM, QStringList* outRes,
                                 QStringList* outTypes, StremioTranslate::Manifest* outSm)
{
    const StremioTranslate::Manifest sm = StremioTranslate::parseManifest(json);
    if (!sm.isValid()) return false;

    AddonManifest m;
    m.id = sm.id;
    m.name = sm.name;
    m.version = sm.version;
    m.type = QStringLiteral("media-source");   // present it like one of ours
    m.description = sm.description;

    for (const StremioTranslate::Catalog& c : sm.catalogs)
    {
        // Every declared catalog is carried, and its verdict rides with it. SearchOnly must remain reachable
        // by the search fan-out (dropping it is what made a search-only addon invisible all over again), and
        // the searchOnly flag is what keeps it out of the browse shelves. Unsatisfiable is carried for the
        // opposite reason: it can never be FETCHED, but "skipped with a reason" has to be a reason the user
        // can read, and a catalog dropped here has no surface left to say it on. dispatchRemoteCatalog
        // answers it locally with that reason instead of ever building a request for it.
        AddonCatalog cat;
        cat.id         = c.routeId();
        cat.type       = c.type;
        cat.name       = c.name;
        cat.searchOnly = (c.use == StremioTranslate::CatalogUse::SearchOnly);
        if (c.use == StremioTranslate::CatalogUse::Unsatisfiable) cat.skipReason = c.skipReason;
        m.catalogs.push_back(cat);
    }

    *outM = m; *outRes = sm.resources; *outTypes = sm.types; *outSm = sm;
    return true;
}

// Build an addon (Stremio dialect if applicable, else our own) from a cached manifest. Returns null on bad data.
static std::unique_ptr<LoadedAddon> buildRemoteAddon(const QString& base, const QByteArray& manifestJson)
{
    AddonManifest manifest; QStringList res, types; StremioTranslate::Manifest sm;
    const bool isStremio = parseStremioManifest(manifestJson, &manifest, &res, &types, &sm);
    bool ok = isStremio;
    if (!isStremio) manifest = AddonManifest::fromJson(manifestJson, &ok);
    if (!ok) return nullptr;
    if (!isStremio && manifest.type != QStringLiteral("media-source")) return nullptr;

    auto entry = std::make_unique<LoadedAddon>();
    entry->transport = LoadedAddon::RemoteHttp;
    entry->baseUrl = base;
    entry->manifest = manifest;
    entry->stremio = isStremio;
    entry->stremioResources = res;
    entry->stremioTypes = types;
    entry->stremioManifest = sm;
    return entry;
}

// Build the Stremio catalog URL for a route id ("type/id"), using the parsed catalog so its required-extra
// defaults are applied.
//
// The fallback below is NOT for an unparsed manifest: every stremio LoadedAddon is built by buildRemoteAddon,
// which always assigns stremioManifest, so a match failure never means "manifest unknown". What actually
// reaches it is an EMPTY routeId — i.e. a caller that asked for a catalog without naming one (requestSearch,
// and two LibraryView call sites). The URL it then produces ("/catalog///...") is not a real route, so any
// caller that wants results must pass a real catalog id; the fallback only keeps a malformed ask from
// crashing, it cannot make it work.
static QUrl stremioCatalogUrl(const LoadedAddon* src, const QString& routeId, const QString& query, int page,
                              const QMap<QString, QString>& filters)
{
    QMap<QString, QString> extras = filters;
    if (!query.isEmpty()) extras.insert(QStringLiteral("search"), query);
    if (page > 1)         extras.insert(QStringLiteral("skip"), QString::number((page - 1) * 100));

    for (const StremioTranslate::Catalog& c : src->stremioManifest.catalogs)
        if (c.routeId() == routeId) return QUrl(src->baseUrl + StremioTranslate::catalogPath(c, extras));

    StremioTranslate::Catalog bare;
    const int slash = routeId.indexOf(QLatin1Char('/'));
    bare.type = slash > 0 ? routeId.left(slash) : routeId;
    bare.id   = slash > 0 ? routeId.mid(slash + 1) : QString();
    return QUrl(src->baseUrl + StremioTranslate::catalogPath(bare, extras));
}

// A catalog's declared extras -> the filter dropdowns the UI renders, mirroring what the non-Stremio branch
// gets from MediaCatalog::fromJson (first option "" = "Any"; an empty selection is simply omitted from the
// filters map, which is what lets a required extra fall back to its preset). `search` and `skip` are the
// protocol's own query/paging knobs — stremioCatalogUrl owns those — and a free-form extra with no declared
// options has nothing to put in a combo, so neither becomes a filter.
static QVector<CatalogFilter> stremioCatalogFilters(const LoadedAddon* src, const QString& routeId)
{
    QVector<CatalogFilter> out;
    for (const StremioTranslate::Catalog& c : src->stremioManifest.catalogs)
    {
        if (c.routeId() != routeId) continue;
        for (const StremioTranslate::Extra& e : c.extras)
        {
            if (e.name.isEmpty() || e.options.isEmpty()) continue;
            if (e.name == QStringLiteral("search") || e.name == QStringLiteral("skip")) continue;
            CatalogFilter f;
            f.key = e.name;
            f.label = e.name; f.label[0] = f.label[0].toUpper();
            // No "Any" row for an extra we PRESET: the request carries that value whether the user touches the
            // combo or not, so offering "Any" (and selecting it by default) would state a grid is unfiltered
            // while it is filtered. Index 0 is the preset itself, which is also options.first() — picking it
            // explicitly produces the identical request.
            if (!c.presets.contains(e.name)) f.options.push_back({ QString(), QObject::tr("Any") });
            // Every option, never a prefix of them: optionsLimit is how many values a user may SELECT (schema
            // default 1), not how long the list is. Capping here would hide 17 of a 20-genre list behind an
            // "optionsLimit: 3" that means something else entirely.
            for (const QString& opt : e.options) f.options.push_back({ opt, opt });
            out.push_back(f);
        }
        break;
    }
    return out;
}
static QUrl stremioMetaUrl(const QString& base, const QString& type, const QString& id)
{ return QUrl(base + QStringLiteral("/meta/") + segEnc(type) + QStringLiteral("/") + segEnc(id) + QStringLiteral(".json")); }
static QUrl stremioStreamUrl(const QString& base, const QString& type, const QString& id)
{ return QUrl(base + QStringLiteral("/stream/") + segEnc(type) + QStringLiteral("/") + segEnc(id) + QStringLiteral(".json")); }

// {metas:[{id,type,name,poster,...}]} -> our catalog. Series are containers (drill into episodes via meta).
static MediaCatalog parseStremioCatalog(const QByteArray& body)
{
    MediaCatalog cat;
    const QJsonArray metas = QJsonDocument::fromJson(body).object().value(QStringLiteral("metas")).toArray();
    for (const QJsonValue& mv : metas)
    {
        const QJsonObject m = mv.toObject();
        MediaItem it;
        it.id = m.value(QStringLiteral("id")).toString();
        it.type = m.value(QStringLiteral("type")).toString();
        it.title = m.value(QStringLiteral("name")).toString();
        it.thumbnailUrl = m.value(QStringLiteral("poster")).toString();
        it.expandable = (it.type == QStringLiteral("series"));
        if (!it.id.isEmpty()) cat.items.push_back(it);
    }
    cat.hasMore = (metas.size() >= 100); // a full page -> probably more (Stremio doesn't report it)
    return cat;
}

static QString joinStremioList(const QJsonArray& a, int max = 6)
{
    QStringList parts;
    for (const QJsonValue& v : a) { parts << v.toString(); if (parts.size() >= max) break; }
    return parts.join(QStringLiteral(", "));
}

// {meta:{...}} -> the detail-page header.
static MediaDetail parseStremioMeta(const QByteArray& body)
{
    MediaDetail d;
    const QJsonObject m = QJsonDocument::fromJson(body).object().value(QStringLiteral("meta")).toObject();
    if (m.isEmpty()) return d;
    d.title = m.value(QStringLiteral("name")).toString();
    d.overview = m.value(QStringLiteral("description")).toString();
    d.imageUrl = m.value(QStringLiteral("poster")).toString();
    const QString genres = joinStremioList(m.value(QStringLiteral("genres")).toArray());
    if (!genres.isEmpty()) d.facts.push_back({ QStringLiteral("Genres"), genres });
    const QString cast = joinStremioList(m.value(QStringLiteral("cast")).toArray());
    if (!cast.isEmpty()) d.facts.push_back({ QStringLiteral("Cast"), cast });
    const QString director = joinStremioList(m.value(QStringLiteral("director")).toArray());
    if (!director.isEmpty()) d.facts.push_back({ QStringLiteral("Director"), director });
    const QString rel = m.value(QStringLiteral("releaseInfo")).toString();
    if (!rel.isEmpty()) d.facts.push_back({ QStringLiteral("Released"), rel });
    const QString runtime = m.value(QStringLiteral("runtime")).toString();
    if (!runtime.isEmpty()) d.facts.push_back({ QStringLiteral("Runtime"), runtime });
    const double imdb = m.value(QStringLiteral("imdbRating")).toString().toDouble();
    if (imdb > 0) d.facts.push_back({ QStringLiteral("IMDb"), QString::number(imdb) });
    d.valid = !d.title.isEmpty();
    return d;
}

// meta.videos[] -> episode children (id "tt:S:E", typed "series" so /stream/series/<id> resolves).
static MediaCatalog parseStremioVideos(const QByteArray& body)
{
    MediaCatalog cat;
    const QJsonObject m = QJsonDocument::fromJson(body).object().value(QStringLiteral("meta")).toObject();
    for (const QJsonValue& vv : m.value(QStringLiteral("videos")).toArray())
    {
        const QJsonObject v = vv.toObject();
        MediaItem it;
        it.id = v.value(QStringLiteral("id")).toString();
        it.type = QStringLiteral("series"); // the stream route uses the series type for episodes
        it.title = v.value(QStringLiteral("name")).toString(v.value(QStringLiteral("title")).toString());
        const int s = v.value(QStringLiteral("season")).toInt(-1), e = v.value(QStringLiteral("episode")).toInt(-1);
        if (s >= 0 && e >= 0) it.subtitle = QStringLiteral("S%1 · E%2").arg(s).arg(e);
        it.thumbnailUrl = v.value(QStringLiteral("thumbnail")).toString();
        it.expandable = false;
        if (!it.id.isEmpty()) cat.items.push_back(it);
    }
    return cat;
}

// Stream parsing (candidates, infoHash validation, ordering, the row cap) lives in StremioTranslate — one
// copy, pinned by probe_stremio against real fixtures. This file only aggregates across addons.

static QString torboxApiKey() { store().sync(); return store().value(QStringLiteral("debrid/torbox/apikey")).toString().trimmed(); }

// One-line append to <app>/stream_debug.log so a stream-resolution run can be traced after the fact
// (no API key/token is ever written - callers pass already-masked text).
static void streamLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
}

// Trace that a chosen source needs HTTP headers. NAMES AND COUNT ONLY — never a value. proxyHeaders
// routinely carry a signed-URL token or a session cookie, and stream_debug.log is a file users paste into
// bug reports; the same rule the Trakt and debrid paths follow for their keys.
static void logStreamHeaders(const StremioTranslate::StreamCandidate& c)
{
    if (c.requestHeaders.isEmpty()) return;
    streamLog(QStringLiteral("stremio: source needs ") + StreamHeaders::logSummary(c.requestHeaders));
}

// --- AddonManager ------------------------------------------------------------------------------------

AddonManager::AddonManager(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<MediaCatalog>("MediaCatalog");
    qRegisterMetaType<MediaDetail>("MediaDetail");

    // Cache each requestCatalog result once it arrives, so a re-open serves it instantly (see requestCatalog).
    // Only reqIds registered in pendingCatalogKey_ (from requestCatalog) are cached; a cache-hit re-emit isn't.
    connect(this, &AddonManager::catalogReady, this, [this](int reqId, const MediaCatalog& cat) {
        const auto it = pendingCatalogKey_.constFind(reqId);
        if (it == pendingCatalogKey_.constEnd()) return;
        const QString key = it.value();
        pendingCatalogKey_.erase(it);
        // Arrival-time enabled check (symmetry with cachedCatalog/requestCatalog): if the source was disabled
        // while this fetch was in flight, don't re-populate its cache — setEnabled already dropped its entries
        // and requestCatalog fail-fasts a disabled source, so an in-flight reply is the only re-insert window.
        // The cache key is "<manifestId>|…", so the prefix before the first '|' is the source id.
        if (isEnabled(key.left(key.indexOf(QLatin1Char('|')))) && !cat.items.isEmpty()) // skip empty/failed too
            catalogCache_.insert(key, { QDateTime::currentMSecsSinceEpoch(), cat });
    });

    // EB_PREFETCH_TTL_S (seconds, >0) compresses the catalog-cache TTL for tests; it also scales the
    // CatalogPrefetcher's resweep cadence (which reads catalogCacheTtlMs()). Unset -> the 30-minute default.
    const int ttlOverrideS = qEnvironmentVariableIntValue("EB_PREFETCH_TTL_S");
    if (ttlOverrideS > 0)
    {
        catalogCacheTtlMs_ = qint64(ttlOverrideS) * 1000;
        static bool loggedOverride = false; // once per process, not per AddonManager
        if (!loggedOverride)
        {
            loggedOverride = true;
            streamLog(QStringLiteral("prefetch: EB_PREFETCH_TTL_S override active - catalog cache TTL %1s")
                          .arg(ttlOverrideS));
        }
    }

    nam_ = new QNetworkAccessManager(this);
    // EB_ADDONS_ROOT lets a test (probe_addon) point discovery at an isolated fixture dir instead of the
    // portable <app>/addons folder; unset in normal runs, so behaviour is unchanged.
    root_ = qEnvironmentVariableIsSet("EB_ADDONS_ROOT")
                ? qEnvironmentVariable("EB_ADDONS_ROOT")
                : AppPaths::dataDir() + QStringLiteral("/addons");
    QDir().mkpath(root_);
    reload();
    // EB_ADDONS_ROOT is the probes' hermetic-fixture override: with it set, skip every startup network
    // kick (default-source seeding, remote-manifest refresh, addon self-update). A live fetch landing
    // mid-probe fires reload()+sourcesChanged() at an arbitrary moment — which flushes/resweeps the
    // prefetcher under test and pollutes its deterministic job counts with real-world catalogs.
    if (!qEnvironmentVariableIsSet("EB_ADDONS_ROOT"))
    {
        seedDefaultStremioSources(); // one-time default sources + migrations
        refreshRemoteManifests();    // pick up any catalogs an addon added since we last cached its manifest
        checkAddonUpdates();         // self-update local addons that publish a newer package (manifest updateUrl)
    }
}

void AddonManager::refreshRemoteManifests()
{
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    for (const QString& base : remoteSourceUrls())
    {
        QNetworkRequest rq((QUrl(base + QStringLiteral("/manifest.json"))));
        rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setTransferTimeout(15000);
        QNetworkReply* reply = nam_->get(rq);
        connect(reply, &QNetworkReply::finished, this, [this, reply, base] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;           // offline/down -> keep cached manifest
            const QByteArray data = reply->readAll();
            if (data.isEmpty() || !buildRemoteAddon(base, data)) return;    // ignore junk / invalid
            if (data == store().value(manifestCacheKey(base)).toByteArray()) return; // unchanged -> nothing to do
            streamLog(QStringLiteral("manifest refresh: %1 changed - reloading").arg(base));
            store().setValue(manifestCacheKey(base), data);
            store().sync();
            reload();              // rebuild sources from the refreshed cache
            emit sourcesChanged(); // the UI rebuilds its tabs, picking up new catalogs (e.g. retro games)
        });
    }
}

void AddonManager::checkAddonUpdates()
{
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    // Snapshot the targets first: installPackage()/reload() in the callbacks rebuilds loaded_, which would
    // invalidate any iterator held here. (id, updateUrl, installed version) is all we need per addon.
    struct Target { QString id; QString url; QString version; };
    QVector<Target> targets;
    for (const auto& up : loaded_)
        if (up->transport == LoadedAddon::JsLocal && !up->manifest.updateUrl.isEmpty())
            targets.push_back({ up->manifest.id, up->manifest.updateUrl, up->manifest.version });

    for (const Target& t : targets)
    {
        QNetworkRequest rq{ QUrl(t.url) };
        rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setTransferTimeout(20000);
        const QByteArray etag = store().value(updateEtagKey(t.id)).toByteArray();
        if (!etag.isEmpty()) rq.setRawHeader("If-None-Match", etag); // unchanged package -> cheap 304

        QNetworkReply* reply = nam_->get(rq);
        connect(reply, &QNetworkReply::finished, this, [this, reply, t] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || status == 304) return; // offline / unchanged
            const QByteArray pkg = reply->readAll();
            const QByteArray newEtag = reply->rawHeader("ETag");
            if (pkg.isEmpty()) return;

            QString remoteVer;
            if (!packageVersion(pkg, &remoteVer)) return;                 // not a real .addon package (junk/HTML)
            // Remember the ETag either way, so we don't re-pull an unchanged package that just isn't newer.
            if (!newEtag.isEmpty()) { store().setValue(updateEtagKey(t.id), newEtag); store().sync(); }
            if (versionCompare(remoteVer, t.version) <= 0) return;        // same or older -> keep what we have

            const QString tmp = QDir::tempPath() + QStringLiteral("/eb-update-") + t.id + QStringLiteral(".addon");
            QFile f(tmp);
            if (!f.open(QIODevice::WriteOnly)) return;
            f.write(pkg);
            f.close();
            QString err;
            if (installPackage(tmp, &err)) // replaces the addon folder in place + reload()s the source list
            {
                streamLog(QStringLiteral("addon update: %1 %2 -> %3").arg(t.id, t.version, remoteVer));
                emit sourcesChanged(); // the UI rebuilds its tabs against the refreshed addon
            }
            else
                streamLog(QStringLiteral("addon update: %1 package rejected: %2").arg(t.id, err));
            QFile::remove(tmp);
        });
    }
}

void AddonManager::reload()
{
    loaded_.clear();
    sources_.clear();
    catalogCache_.clear();      // the addon set is changing; don't serve a stale catalog for it
    pendingCatalogKey_.clear();

    const QFileInfoList dirs = QDir(root_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& d : dirs)
    {
        if (d.fileName() == QStringLiteral("_storage")) continue; // addon-private storage, not an addon
        loadFolder(d.absoluteFilePath());
    }
    loadRemoteSources(); // URL-only HTTP addons (built from their cached manifests)

    // The brand migration cannot rename per-addon config, because the id that keys it is whatever manifest.id
    // says and only the load above knows that. So it leaves those keys alone and we reconcile here, where the
    // real ids finally exist — including the remote ones, which is the case that has no folder to inspect and
    // therefore no answer until its cached manifest has been read. Idempotent and near-free on the ordinary
    // run; it earns its keep once, on the first launch after an install whose keys an earlier build stranded.
    const QStringList ids = installedIds();
    const int restored = BrandMigration::reconcileAddonConfig(AppPaths::dataDir(), ids);
    if (restored)
        streamLog(QStringLiteral("addon config: restored %1 stranded setting(s) to the add-on ids in use")
                      .arg(restored));   // COUNT only — these values are user credentials

    // ...and the same repair on the other surface (#58): favourites and playlists store the id of the add-on
    // an item came from INSIDE their JSON blob, so an add-on whose spelling the migration guessed at leaves
    // the user with a favourite that opens to "That favourite's source addon isn't available". Runs here for
    // the same reason: `ids` is the only place the answer exists. Covers every profile, not just the current
    // one — the stores are per-profile and reload() is not.
    const int repointed = BrandMigration::reconcileAddonRefs(AppPaths::dataDir(), ids);
    if (repointed)
        streamLog(QStringLiteral("addon refs: re-pointed %1 stored favourite/playlist reference(s) "
                                 "to the add-on ids in use").arg(repointed));
}

QStringList AddonManager::remoteSourceUrls() const
{
    const QJsonArray arr = QJsonDocument::fromJson(
        store().value(QStringLiteral("addon.remote.urls")).toByteArray()).array();
    QStringList urls;
    for (const QJsonValue& v : arr) urls << v.toString();
    return urls;
}

void AddonManager::loadRemoteSources()
{
    for (const QString& base : remoteSourceUrls())
    {
        const QByteArray mf = store().value(manifestCacheKey(base)).toByteArray();
        if (mf.isEmpty()) continue; // manifest not fetched yet (added on a previous run that failed) - skip
        auto entry = buildRemoteAddon(base, mf); // Stremio dialect or our own, auto-detected
        if (!entry) continue;
        LoadedAddon* raw = entry.get();
        loaded_.push_back(std::move(entry));
        if (raw->isMediaSource()) sources_.push_back(raw);
    }
}

// Seed/reconcile the default Stremio sources so movies/TV work out of the box. Each step is one-time
// (flagged) and the user stays in control afterwards (they can add/remove any of these from Settings).
void AddonManager::seedDefaultStremioSources()
{
    // First-ever-run latch. This block once seeded Cinemeta; Cinemeta was later dropped at the user's
    // request (the cinemeta.removed migration below), which left seed-then-remove RACING: addRemoteSource
    // persists its URL only when the async manifest fetch lands, so the removal migration — running
    // synchronously right after — found nothing to remove, latched itself, and the fetch then persisted
    // Cinemeta anyway (every fresh install re-acquired it). A fresh install now just sets the latch.
    if (!store().value(QStringLiteral("addon.stremio.seeded")).toBool())
    {
        store().setValue(QStringLiteral("addon.stremio.seeded"), true); store().sync();
    }

    // One-time: drop Debridio. Its debrid backend is unreliable (dead playback links + "error report this
    // issue" placeholders); streams now come from a raw-torrent addon resolved through our own TorBox key.
    if (!store().value(QStringLiteral("addon.debridio.removed")).toBool())
    {
        store().setValue(QStringLiteral("addon.debridio.removed"), true); store().sync();
        for (const QString& u : remoteSourceUrls())
            if (u.contains(QStringLiteral("debridio"), Qt::CaseInsensitive)) removeRemoteSource(u);
    }

    // One-time: drop Cinemeta at the user's request.
    if (!store().value(QStringLiteral("addon.cinemeta.removed")).toBool())
    {
        store().setValue(QStringLiteral("addon.cinemeta.removed"), true); store().sync();
        for (const QString& u : remoteSourceUrls())
            if (u.contains(QStringLiteral("cinemeta"), Qt::CaseInsensitive)) removeRemoteSource(u);
    }

    // Second pass: anyone who ran the racy seed above has Cinemeta persisted anyway (the first removal
    // ran before its async add landed — see the latch block's comment). Remove it again under a fresh
    // latch, now that the seed itself is gone.
    if (!store().value(QStringLiteral("addon.cinemeta.removed2")).toBool())
    {
        store().setValue(QStringLiteral("addon.cinemeta.removed2"), true); store().sync();
        for (const QString& u : remoteSourceUrls())
            if (u.contains(QStringLiteral("cinemeta"), Qt::CaseInsensitive)) removeRemoteSource(u);
    }

    // One-time: seed Torrentio as a stream source. It returns infoHashes (raw torrents); our TorBox resolver
    // (Settings -> General -> Streaming) turns the cached ones into playable links - no third-party debrid.
    // The host is strem.FUN, not strem.io: torrentio.strem.io is NXDOMAIN, so seeding it gave every new
    // install a stream provider that could never answer.
    if (!store().value(QStringLiteral("addon.torrentio.seeded")).toBool())
    {
        store().setValue(QStringLiteral("addon.torrentio.seeded"), true); store().sync();
        addRemoteSource(QStringLiteral("https://torrentio.strem.fun/manifest.json"));
    }

    // One-time: repoint an ALREADY-PERSISTED torrentio.strem.io entry at the live host. Fixing the seed above
    // only helps a fresh install; anyone who ran an earlier build has the dead URL in addon.remote.urls and
    // would keep it forever (no error surfaces — a dead stream source just never returns candidates).
    // Runs after the seed block so a first run sets both flags and finds nothing to migrate.
    if (!store().value(QStringLiteral("addon.torrentio.host.migrated")).toBool())
    {
        store().setValue(QStringLiteral("addon.torrentio.host.migrated"), true); store().sync();
        const QString dead = QStringLiteral("torrentio.strem.io");
        const QString live = QStringLiteral("torrentio.strem.fun");
        // remoteSourceUrls() returns a fresh list by value, so removing/adding inside the loop is safe.
        for (const QString& u : remoteSourceUrls())
        {
            if (!u.contains(dead, Qt::CaseInsensitive)) continue;
            QString fixed = u;
            fixed.replace(dead, live, Qt::CaseInsensitive);
            removeRemoteSource(u);        // drops the URL and its cached (never-fetched) manifest
            addRemoteSource(fixed);       // re-adds it, fetching the live manifest
        }
    }
}

void AddonManager::loadFolder(const QString& dir)
{
    QFile mf(dir + QStringLiteral("/manifest.json"));
    if (!mf.open(QIODevice::ReadOnly)) return;

    bool ok = false;
    AddonManifest manifest = AddonManifest::fromJson(mf.readAll(), &ok);
    if (!ok) { qWarning() << "addon: invalid manifest in" << dir; return; }

    auto entry = std::make_unique<LoadedAddon>();
    entry->manifest = manifest;
    entry->dir = dir;

    // Read (but don't evaluate) the script - it's compiled per request on a worker thread.
    QFile sf(dir + QStringLiteral("/") + manifest.entryOrDefault());
    if (manifest.type == QStringLiteral("media-source") && sf.open(QIODevice::ReadOnly))
    {
        entry->source = QString::fromUtf8(sf.readAll());
        entry->hasScript = !entry->source.isEmpty();
    }

    LoadedAddon* raw = entry.get();
    loaded_.push_back(std::move(entry));
    if (raw->isMediaSource())
        sources_.push_back(raw);
}

QVector<AddonCatalog> AddonManager::catalogs(LoadedAddon* src) const
{
    if (!src) return {};
    if (!src->manifest.catalogs.isEmpty()) return src->manifest.catalogs;
    // A remote addon with no declared catalogs has nothing to browse - e.g. a stream-only resolver like
    // Allarr/Torrentio (resources:["stream"], catalogs:[]). Don't synthesize a phantom media-type tab for it.
    if (src->transport == LoadedAddon::RemoteHttp) return {};
    // A pure metadata provider (metaFor set, catalogs empty) — SteamGridDB/IGDB/ScreenScraper/TheGamesDB —
    // is only ever fanned out via getMeta and must NOT appear as a browsable source either.
    if (!src->manifest.metaFor.isEmpty()) return {};
    // A local script addon with no declared catalogs implicitly exposes a single "mixed" catalog.
    AddonCatalog c;
    c.name = src->manifest.name.isEmpty() ? src->manifest.id : src->manifest.name;
    c.type = QStringLiteral("mixed");
    return { c }; // implicit single catalog (id empty)
}

AddonRequest AddonManager::buildRequest(LoadedAddon* src, const QString& function, const QString& argJson) const
{
    AddonRequest req;
    if (src) { req.source = src->source; req.manifest = src->manifest; req.dir = src->dir; }
    req.storageDir = root_ + QStringLiteral("/_storage/") + (src ? src->manifest.id : QString());
    req.function = function;
    req.argJson = argJson;
    return req;
}

// ---- synchronous ----
// (Remote sources fetch over HTTP with a blocking GET; the same parsing/resolution as the async path.)
static MediaCatalog remoteCatalogBlocking(const QUrl& url, const QString& base, const QByteArray& cfg)
{
    MediaCatalog cat = MediaCatalog::fromJson(httpGetBlocking(url, cfg));
    for (MediaItem& it : cat.items)
    {
        it.url = resolveRemoteUrl(it.url, base);
        it.thumbnailUrl = resolveRemoteUrl(it.thumbnailUrl, base);
    }
    return cat;
}

MediaCatalog AddonManager::catalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page)
{
    if (!src) return {};
    if (src->transport == LoadedAddon::RemoteHttp)
        return remoteCatalogBlocking(remoteCatalogUrl(src->baseUrl, catalogId, query, page), src->baseUrl,
                                     remoteConfigHeader(src));
    return executeRequest(buildRequest(src, QStringLiteral("getCatalog"), catalogArg(catalogId, query, page)));
}

MediaCatalog AddonManager::detail(LoadedAddon* src, const MediaItem& item, int page)
{
    if (!src) return {};
    if (src->transport == LoadedAddon::RemoteHttp)
        return remoteCatalogBlocking(remoteDetailUrl(src->baseUrl, item.type, item.id, page), src->baseUrl,
                                     remoteConfigHeader(src));
    const QString arg = QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("id"), item.id }, { QStringLiteral("type"), item.type },
        { QStringLiteral("page"), page } }).toJson(QJsonDocument::Compact));
    return executeRequest(buildRequest(src, QStringLiteral("getDetail"), arg));
}

MediaCatalog AddonManager::search(LoadedAddon* src, const QString& query)
{
    if (!src) return {};
    if (src->transport == LoadedAddon::RemoteHttp)
        return remoteCatalogBlocking(remoteCatalogUrl(src->baseUrl, QString(), query, 1), src->baseUrl,
                                     remoteConfigHeader(src));
    const QString arg = QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("query"), query } }).toJson(QJsonDocument::Compact));
    return executeRequest(buildRequest(src, QStringLiteral("search"), arg));
}

MediaDetail AddonManager::meta(LoadedAddon* src, const MediaItem& item)
{
    if (!src) return {};
    if (src->transport == LoadedAddon::RemoteHttp)
    {
        MediaDetail d = MediaDetail::fromJson(
            httpGetBlocking(remoteMetaUrl(src->baseUrl, item.type, item.id), remoteConfigHeader(src)));
        d.imageUrl = resolveRemoteUrl(d.imageUrl, src->baseUrl);
        return d;
    }
    return executeMetaRequest(buildRequest(src, QStringLiteral("getMeta"), itemArg(item)));
}

// ---- asynchronous ----
int AddonManager::dispatch(const AddonRequest& req)
{
    const int reqId = ++reqCounter_;
    auto* watcher = new QFutureWatcher<MediaCatalog>(this);
    connect(watcher, &QFutureWatcher<MediaCatalog>::finished, this, [this, reqId, watcher] {
        const MediaCatalog cat = watcher->result();
        watcher->deleteLater();
        emit catalogReady(reqId, cat);
    });
    watcher->setFuture(QtConcurrent::run([req] { return executeRequest(req); }));
    return reqId;
}

int AddonManager::dispatchMeta(const AddonRequest& req)
{
    const int reqId = ++reqCounter_;
    auto* watcher = new QFutureWatcher<MediaDetail>(this);
    connect(watcher, &QFutureWatcher<MediaDetail>::finished, this, [this, reqId, watcher] {
        const MediaDetail d = watcher->result();
        watcher->deleteLater();
        emit metaReady(reqId, d);
    });
    watcher->setFuture(QtConcurrent::run([req] { return executeMetaRequest(req); }));
    return reqId;
}

int AddonManager::requestMeta(LoadedAddon* src, const MediaItem& item)
{
    if (!src) return -1;
    if (src->transport == LoadedAddon::RemoteHttp) return dispatchRemoteMeta(src, item);
    return dispatchMeta(buildRequest(src, QStringLiteral("getMeta"), itemArg(item)));
}

QString AddonManager::catalogCacheKey(LoadedAddon* src, const QString& catalogId, const QString& query,
                                      int page, const QMap<QString, QString>& filters) const
{
    QString k = src->manifest.id + QLatin1Char('|') + catalogId + QLatin1Char('|') + query
              + QLatin1Char('|') + QString::number(page);
    for (auto it = filters.constBegin(); it != filters.constEnd(); ++it) // QMap iterates in sorted key order
        if (!it.value().isEmpty())
            k += QLatin1Char('|') + it.key() + QLatin1Char('=') + it.value();
    return k;
}

std::optional<MediaCatalog> AddonManager::cachedCatalog(LoadedAddon* src, const QString& catalogId,
                                                        const QString& query, int page,
                                                        const QMap<QString, QString>& filters) const
{
    if (!src) return std::nullopt;
    if (!isEnabled(src->manifest.id)) return std::nullopt; // stale-disabled: never serve a disabled source
    const QString key = catalogCacheKey(src, catalogId, query, page, filters);
    const auto it = catalogCache_.constFind(key);
    if (it == catalogCache_.constEnd()) return std::nullopt;
    if (QDateTime::currentMSecsSinceEpoch() - it->atMs >= catalogCacheTtlMs_) return std::nullopt; // expired
    return it->cat;
}

bool AddonManager::hasCachedCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page,
                                    const QMap<QString, QString>& filters) const
{
    if (!src) return false;
    if (!isEnabled(src->manifest.id)) return false; // stale-disabled: a disabled source is never "warm"
    const QString key = catalogCacheKey(src, catalogId, query, page, filters);
    const auto it = catalogCache_.constFind(key);
    if (it == catalogCache_.constEnd()) return false;
    return QDateTime::currentMSecsSinceEpoch() - it->atMs < catalogCacheTtlMs_; // false if expired
}

int AddonManager::requestCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page,
                                 const QMap<QString, QString>& filters)
{
    if (!src) return -1;
    // Fail fast for a disabled source: don't serve its cache (the stale-disabled landmine) and don't fetch
    // either — a fetch would silently re-populate the cache for a source the user just turned off. Callers
    // only act on enabled sources (SearchAggregator/LibraryView gate on isEnabled explicitly; HomeView only
    // browses enabled sources' tabs), so the -1 is inert for the UI, same as the existing null-src return.
    if (!isEnabled(src->manifest.id)) return -1;

    // Serve a recent browse/landing result from cache (e.g. the console list, which rarely changes) instead
    // of re-fetching. Delivered on the next event-loop turn so the caller records its reqId first.
    const QString key = catalogCacheKey(src, catalogId, query, page, filters);
    const auto cached = catalogCache_.constFind(key);
    if (cached != catalogCache_.constEnd()
        && QDateTime::currentMSecsSinceEpoch() - cached->atMs < catalogCacheTtlMs_)
    {
        const int reqId = ++reqCounter_;
        const MediaCatalog cat = cached->cat;
        QMetaObject::invokeMethod(this, [this, reqId, cat] { emit catalogReady(reqId, cat); }, Qt::QueuedConnection);
        return reqId;
    }

    // Instrumentation (EB_PREFETCH_LOG=1, off by default): reaching here is a cache MISS -> a REAL fetch (JS
    // getCatalog or HTTP GET) is about to run. Zero of these lines during a menu walk is the warm-path proof;
    // they should all cluster at prefetch-sweep time. Cache-served requests return above and never log.
    static const bool kLogFetch = qEnvironmentVariableIntValue("EB_PREFETCH_LOG") > 0;
    if (kLogFetch) streamLog(QStringLiteral("catalog fetch %1|%2 page=%3%4")
                                 .arg(src->manifest.id, catalogId).arg(page)
                                 .arg(query.isEmpty() ? QString() : QStringLiteral(" q=") + query));

    const int reqId = (src->transport == LoadedAddon::RemoteHttp)
        ? dispatchRemoteCatalog(src, catalogId, query, page, filters)
        : dispatch(buildRequest(src, QStringLiteral("getCatalog"), catalogArg(catalogId, query, page, filters)));
    pendingCatalogKey_[reqId] = key; // store the result under this key when it arrives (see the constructor)
    return reqId;
}

int AddonManager::requestDetail(LoadedAddon* src, const MediaItem& item, int page,
                                const QMap<QString, QString>& filters, const QString& query)
{
    if (!src) return -1;
    if (src->transport == LoadedAddon::RemoteHttp) return dispatchRemoteDetail(src, item, page);
    QJsonObject a{ { QStringLiteral("id"), item.id }, { QStringLiteral("type"), item.type },
                   { QStringLiteral("page"), page } };
    if (!query.isEmpty()) a.insert(QStringLiteral("query"), query); // search WITHIN this container (e.g. a console)
    for (auto it = filters.constBegin(); it != filters.constEnd(); ++it) // genre/sort for a container's children
        if (!it.value().isEmpty()) a.insert(it.key(), it.value());
    return dispatch(buildRequest(src, QStringLiteral("getDetail"), QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact))));
}

int AddonManager::requestSearch(LoadedAddon* src, const QString& query)
{
    if (!src) return -1;
    if (src->transport == LoadedAddon::RemoteHttp)
        return dispatchRemoteCatalog(src, QString(), query, 1);
    const QString arg = QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("query"), query } }).toJson(QJsonDocument::Compact));
    return dispatch(buildRequest(src, QStringLiteral("search"), arg));
}

// ---- remote (HTTP) dispatch: async on the GUI thread, same catalogReady/metaReady result signals ----

int AddonManager::dispatchRemoteCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page,
                                        const QMap<QString, QString>& filters)
{
    const int reqId = ++reqCounter_;
    const QString base = src->baseUrl;
    const bool stremio = src->stremio;

    // ---- Two things this add-on cannot do, said out loud instead of presenting as an empty shelf ----
    // Both answer with the SAME synthetic type:"info" row the unreachable-add-on branch below uses (one
    // surface for "here is why this is empty"), and both answer LOCALLY: neither request could succeed, so
    // building one would only spend a round-trip to arrive at the same nothing. Delivered on the next
    // event-loop turn, exactly like the cache hit in requestCatalog, so the caller records this reqId first.
    QString cannot;
    if (stremio && src->stremioManifest.configurationRequired)
    {
        const QString name = src->manifest.name.isEmpty() ? src->manifest.id : src->manifest.name;
        cannot = tr("%1 needs to be configured before it can show anything.").arg(name);
        // Only when the add-on says it HAS a configuration page. Its base URL is that page (the Stremio
        // convention), so this points at somewhere real rather than telling the user to go look for it.
        if (src->stremioManifest.configurable && !base.isEmpty())
            cannot += QLatin1Char(' ') + tr("Open its configuration page at %1, then re-add it.").arg(base);
    }
    else if (stremio && catalogId.isEmpty())
    {
        // A caller asked this Stremio add-on for a catalog without naming one. Stremio has no "default"
        // catalog route — every catalog is /catalog/{type}/{id}.json — so the URL builder produced
        // "{base}/catalog//.json", which 404s by construction — the "/catalog//.json - server replied: Not
        // Found" line against Cinemeta in stream_debug.log. Reachable from the two LibraryView call sites that
        // pass an empty id for "this source's landing page" (onSourceChanged fires on its own when the source
        // list populates, so no user action is needed) and from requestSearch. Nothing caches a failed fetch,
        // so it repeats on every visit. Answer locally instead of spending a round-trip on a route that
        // cannot exist.
        const QString name = src->manifest.name.isEmpty() ? src->manifest.id : src->manifest.name;
        cannot = src->manifest.catalogs.isEmpty()
                     ? tr("%1 doesn't publish any browsable catalogs.").arg(name)
                     : tr("Pick one of %1's catalogs — it has no single default list.").arg(name);
    }
    else if (stremio)
    {
        for (const AddonCatalog& c : src->manifest.catalogs)
            if (c.id == catalogId && !c.skipReason.isEmpty())
            { cannot = tr("“%1” %2.").arg(c.name, c.skipReason); break; }
    }
    if (!cannot.isEmpty())
    {
        MediaCatalog cat;
        cat.title = tr("Unavailable");
        MediaItem info; info.type = QStringLiteral("info"); info.title = cannot;
        cat.items.push_back(info);
        QMetaObject::invokeMethod(this, [this, reqId, cat] { emit catalogReady(reqId, cat); }, Qt::QueuedConnection);
        return reqId;
    }

    // Stremio declares its filters on the MANIFEST, not on the catalog response, so they're resolved here and
    // ridden into the reply handler; the non-Stremio branch gets its equivalents from MediaCatalog::fromJson.
    const QVector<CatalogFilter> stremioFilters = stremio ? stremioCatalogFilters(src, catalogId)
                                                          : QVector<CatalogFilter>();
    QNetworkRequest rq(stremio ? stremioCatalogUrl(src, catalogId, query, page, filters)
                               : remoteCatalogUrl(base, catalogId, query, page, filters));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Liveness: without a transfer timeout a black-holed remote never finishes, its result signal never fires,
    // and anything waiting on this reqId (e.g. a prefetcher in-flight slot) wedges. 15s matches the other sites.
    rq.setTransferTimeout(15000);
    if (!stremio) applyServerHeaders(rq, src);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqId, base, stremio, stremioFilters] {
        reply->deleteLater();
        MediaCatalog cat;
        if (reply->error() == QNetworkReply::NoError)
        {
            cat = stremio ? parseStremioCatalog(reply->readAll()) : MediaCatalog::fromJson(reply->readAll());
            if (stremio) cat.filters = stremioFilters; // genre & co., so the filter bar can round-trip them back
            for (MediaItem& it : cat.items)
            {
                it.url = resolveRemoteUrl(it.url, base);
                it.thumbnailUrl = resolveRemoteUrl(it.thumbnailUrl, base);
            }
        }
        else
        {
            // Don't fail silently to an empty tab - surface the reason so a misconfigured/unreachable add-on
            // (e.g. a stale access token -> every request 404s) is diagnosable instead of looking "empty".
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            MediaItem info; info.type = QStringLiteral("info");
            info.title = http == 404
                ? tr("Couldn't load this add-on's catalog (HTTP 404). Its URL or access token may be out of date - "
                     "re-add it in Settings ▸ Add-ons.")
                : tr("Couldn't reach this add-on: %1").arg(reply->errorString());
            cat.title = tr("Unavailable");
            cat.items.push_back(info);
            streamLog(QStringLiteral("catalog fetch failed (%1): %2").arg(http).arg(reply->errorString()));
        }
        emit catalogReady(reqId, cat);
    });
    return reqId;
}

int AddonManager::dispatchRemoteDetail(LoadedAddon* src, const MediaItem& item, int page)
{
    const int reqId = ++reqCounter_;
    const QString base = src->baseUrl;
    const bool stremio = src->stremio;
    // Stremio has no separate "children" route: a series' episodes come from its /meta videos[].
    QNetworkRequest rq(stremio ? stremioMetaUrl(base, item.type, item.id)
                               : remoteDetailUrl(base, item.type, item.id, page));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Liveness: without a transfer timeout a black-holed remote never finishes, its result signal never fires,
    // and anything waiting on this reqId (e.g. a prefetcher in-flight slot) wedges. 15s matches the other sites.
    rq.setTransferTimeout(15000);
    if (!stremio) applyServerHeaders(rq, src);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqId, base, stremio] {
        reply->deleteLater();
        MediaCatalog cat;
        if (reply->error() == QNetworkReply::NoError)
        {
            cat = stremio ? parseStremioVideos(reply->readAll()) : MediaCatalog::fromJson(reply->readAll());
            for (MediaItem& it : cat.items)
            {
                it.url = resolveRemoteUrl(it.url, base);
                it.thumbnailUrl = resolveRemoteUrl(it.thumbnailUrl, base);
            }
        }
        else
        {
            // Same as a failed catalog: surface the reason instead of an empty grid the user can't explain (a
            // fullscreen/controller user never sees the status bar, so it has to be an on-screen row).
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            MediaItem info; info.type = QStringLiteral("info");
            info.title = http >= 400
                ? tr("Couldn't load this (HTTP %1). The add-on or its source may be unavailable — try again shortly.").arg(http)
                : tr("Couldn't reach the add-on: %1").arg(reply->errorString());
            cat.items.push_back(info);
            streamLog(QStringLiteral("detail fetch failed (%1): %2").arg(http).arg(reply->errorString()));
        }
        emit catalogReady(reqId, cat);
    });
    return reqId;
}

int AddonManager::dispatchRemoteMeta(LoadedAddon* src, const MediaItem& item)
{
    const int reqId = ++reqCounter_;
    const QString base = src->baseUrl;
    const bool stremio = src->stremio;
    QNetworkRequest rq(stremio ? stremioMetaUrl(base, item.type, item.id) : remoteMetaUrl(base, item.type, item.id));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Liveness: without a transfer timeout a black-holed remote never finishes, its result signal never fires,
    // and anything waiting on this reqId (e.g. a prefetcher in-flight slot) wedges. 15s matches the other sites.
    rq.setTransferTimeout(15000);
    if (!stremio) applyServerHeaders(rq, src);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqId, base, stremio] {
        reply->deleteLater();
        MediaDetail d;
        if (reply->error() == QNetworkReply::NoError)
        {
            d = stremio ? parseStremioMeta(reply->readAll()) : MediaDetail::fromJson(reply->readAll());
            d.imageUrl = resolveRemoteUrl(d.imageUrl, base);
        }
        emit metaReady(reqId, d);
    });
    return reqId;
}

// The TorBox debrid REST API. Resolving a cached torrent to a stream URL is a small async chain:
//   checkcached -> createtorrent -> mylist(files) -> requestdl.
static const char* kTorBoxApi = "https://api.torbox.app/v1/api";

void AddonManager::resolveTorBoxInfoHash(const QString& infoHash, int fileIdx,
                                         std::function<void(const QString&)> cb)
{
    const QString key = torboxApiKey();
    if (key.isEmpty() || infoHash.isEmpty()) { streamLog(QStringLiteral("torbox: no key / empty hash")); cb(QString()); return; }
    const QByteArray bearer = "Bearer " + key.toUtf8();
    const QString shortHash = infoHash.left(8).toLower();

    // 1) Only stream torrents TorBox already has cached (otherwise it'd just queue a download).
    QUrl chk(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/checkcached"));
    QUrlQuery cq; cq.addQueryItem(QStringLiteral("hash"), infoHash.toLower());
    cq.addQueryItem(QStringLiteral("format"), QStringLiteral("object"));
    chk.setQuery(cq);
    QNetworkRequest rq(chk);
    rq.setRawHeader("Authorization", bearer);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setTransferTimeout(15000);
    streamLog(QStringLiteral("torbox: checkcached %1").arg(shortHash));
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, infoHash, shortHash, fileIdx, key, bearer, cb] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        { streamLog(QStringLiteral("torbox: checkcached error %1: %2").arg(shortHash, reply->errorString())); cb(QString()); return; }
        const QJsonObject data = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toObject();
        if (data.isEmpty()) { streamLog(QStringLiteral("torbox: %1 not cached").arg(shortHash)); cb(QString()); return; } // can't stream it now
        streamLog(QStringLiteral("torbox: %1 cached -> createtorrent").arg(shortHash));

        // 2) Add the (cached) magnet to the account to obtain a torrent_id.
        const QByteArray boundary = "tbb" + QCryptographicHash::hash(infoHash.toUtf8(), QCryptographicHash::Md5).toHex().left(12);
        QByteArray fb;
        fb += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"magnet\"\r\n\r\n";
        fb += "magnet:?xt=urn:btih:" + infoHash.toUtf8() + "\r\n--" + boundary + "--\r\n";
        QNetworkRequest cr((QUrl(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/createtorrent"))));
        cr.setRawHeader("Authorization", bearer);
        cr.setHeader(QNetworkRequest::ContentTypeHeader, "multipart/form-data; boundary=" + boundary);
        cr.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        cr.setTransferTimeout(20000);
        QNetworkReply* cre = nam_->post(cr, fb);
        connect(cre, &QNetworkReply::finished, this, [this, cre, shortHash, fileIdx, key, bearer, cb] {
            cre->deleteLater();
            if (cre->error() != QNetworkReply::NoError)
            { streamLog(QStringLiteral("torbox: createtorrent error %1: %2").arg(shortHash, cre->errorString())); cb(QString()); return; }
            const QJsonObject cd = QJsonDocument::fromJson(cre->readAll()).object().value(QStringLiteral("data")).toObject();
            const QString torrentId = QString::number(cd.value(QStringLiteral("torrent_id")).toVariant().toLongLong());
            if (torrentId.isEmpty() || torrentId == QStringLiteral("0")) { streamLog(QStringLiteral("torbox: no torrent_id %1").arg(shortHash)); cb(QString()); return; }
            streamLog(QStringLiteral("torbox: torrent_id %1 -> mylist").arg(torrentId));

            // 3) List the torrent's files to choose one (the episode by index, else the largest video).
            QUrl ml(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/mylist"));
            QUrlQuery mq; mq.addQueryItem(QStringLiteral("id"), torrentId); mq.addQueryItem(QStringLiteral("bypass_cache"), QStringLiteral("true"));
            ml.setQuery(mq);
            QNetworkRequest mr(ml);
            mr.setRawHeader("Authorization", bearer);
            mr.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
            mr.setTransferTimeout(15000);
            QNetworkReply* mre = nam_->get(mr);
            connect(mre, &QNetworkReply::finished, this, [this, mre, torrentId, fileIdx, key, cb] {
                mre->deleteLater();
                if (mre->error() != QNetworkReply::NoError)
                { streamLog(QStringLiteral("torbox: mylist error %1: %2").arg(torrentId, mre->errorString())); cb(QString()); return; }
                const QJsonValue dv = QJsonDocument::fromJson(mre->readAll()).object().value(QStringLiteral("data"));
                const QJsonObject td = dv.isArray() ? dv.toArray().first().toObject() : dv.toObject(); // id-> object or [object]
                const QJsonArray files = td.value(QStringLiteral("files")).toArray();
                if (files.isEmpty()) { streamLog(QStringLiteral("torbox: mylist no files %1").arg(torrentId)); cb(QString()); return; }
                int chosen = -1; qint64 bestSize = -1;
                static const QStringList vids = { ".mkv", ".mp4", ".avi", ".m4v", ".webm", ".ts", ".mov" };
                for (int i = 0; i < files.size(); ++i)
                {
                    const QJsonObject f = files[i].toObject();
                    const QString name = f.value(QStringLiteral("name")).toString().toLower();
                    bool isVid = false; for (const QString& e : vids) if (name.endsWith(e)) { isVid = true; break; }
                    if (!isVid) continue;
                    const qint64 sz = f.value(QStringLiteral("size")).toVariant().toLongLong();
                    if (sz > bestSize) { bestSize = sz; chosen = i; }
                }
                if (fileIdx >= 0 && fileIdx < files.size()) chosen = fileIdx; // honour the addon's episode pick
                if (chosen < 0) chosen = 0;
                const QString fileId = QString::number(files[chosen].toObject().value(QStringLiteral("id")).toVariant().toLongLong());
                streamLog(QStringLiteral("torbox: %1 files, file_id %2 -> requestdl").arg(files.size()).arg(fileId));

                // 4) Ask for the direct download/stream link.
                QUrl dl(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/requestdl"));
                QUrlQuery dq; dq.addQueryItem(QStringLiteral("token"), key);
                dq.addQueryItem(QStringLiteral("torrent_id"), torrentId); dq.addQueryItem(QStringLiteral("file_id"), fileId);
                dl.setQuery(dq);
                QNetworkRequest dr(dl);
                dr.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
                dr.setTransferTimeout(15000);
                QNetworkReply* dre = nam_->get(dr);
                connect(dre, &QNetworkReply::finished, this, [dre, cb] {
                    dre->deleteLater();
                    if (dre->error() != QNetworkReply::NoError)
                    { streamLog(QStringLiteral("torbox: requestdl error: %1").arg(dre->errorString())); cb(QString()); return; }
                    const QJsonValue d = QJsonDocument::fromJson(dre->readAll()).object().value(QStringLiteral("data"));
                    const QString url = d.toString();
                    streamLog(url.isEmpty() ? QStringLiteral("torbox: requestdl returned no url") : QStringLiteral("torbox: GOT stream url"));
                    cb(url);
                });
            });
        });
    });
}

void AddonManager::listStremioStreams(const MediaItem& item,
                                      std::function<void(const QVector<StremioTranslate::StreamCandidate>&)> cb,
                                      int maxRowsPerAddon)
{
    // Ask every enabled Stremio addon that offers streams for this type AND claims this id space (like
    // Stremio aggregates), then merge the answers into ONE list ordered by the translator's rule.
    QVector<LoadedAddon*> enabled;
    QVector<StremioTranslate::Manifest> manifests;
    for (LoadedAddon* s : sources_)
        if (s->stremio && isEnabled(s->manifest.id))
        { enabled.push_back(s); manifests.push_back(s->stremioManifest); }

    bool fellBackToAll = false;
    const QVector<int> chosen = StremioTranslate::routeProviders(manifests, QStringLiteral("stream"),
                                                                 item.type, item.id, &fellBackToAll);
    QVector<LoadedAddon*> providers;
    for (int i : chosen) providers.push_back(enabled[i]);

    if (providers.isEmpty())
    {
        streamLog(QStringLiteral("stremio: no stream providers for type %1").arg(item.type));
        cb({});
        return;
    }
    if (fellBackToAll)
        streamLog(QStringLiteral("stremio: idPrefixes matched no provider for %1 — asking all %2")
                      .arg(item.id).arg(providers.size()));
    streamLog(QStringLiteral("stremio: querying %1 stream provider(s) for %2 %3")
                  .arg(providers.size()).arg(item.type, item.id));

    // One slot per provider, filled in place, so a provider's block stays a block: mergeCandidates is what
    // orders the aggregate, and it can only do that if it is handed the blocks rather than a pre-flattened
    // list somebody already assumed was sorted.
    auto blocks = std::make_shared<QVector<QVector<StremioTranslate::StreamCandidate>>>(providers.size());
    auto pending = std::make_shared<int>(providers.size());
    for (int pi = 0; pi < providers.size(); ++pi)
    {
        LoadedAddon* p = providers[pi];
        QNetworkRequest rq(stremioStreamUrl(p->baseUrl, item.type, item.id));
        rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setTransferTimeout(15000);
        QNetworkReply* reply = nam_->get(rq);
        connect(reply, &QNetworkReply::finished, this, [reply, blocks, pending, cb, pi, maxRowsPerAddon] {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError)
                (*blocks)[pi] = StremioTranslate::parseStreams(reply->readAll(), maxRowsPerAddon);
            else streamLog(QStringLiteral("stremio: stream request error: %1").arg(reply->errorString()));
            if (--*pending != 0) return; // wait for every provider

            // Each provider's block arrived already sorted and capped; the CONCATENATION is not, so order the
            // aggregate by the same rule before anyone picks from it.
            const QVector<StremioTranslate::StreamCandidate> all = StremioTranslate::mergeCandidates(*blocks);
            streamLog(QStringLiteral("stremio: %1 candidate stream(s)").arg(all.size()));
            cb(all);
        });
    }
}

void AddonManager::listStremioSubtitles(const QString& type, const QString& id, const QString& localPath,
                                        std::function<void(const QVector<StremioTranslate::SubtitleAddonResult>&)> cb)
{
    // Same shape as listStremioStreams: route over enabled Stremio addons that offer THIS resource for THIS id
    // space, GET each one's endpoint, aggregate the answers on the GUI thread. The only differences are the
    // resource name, the extras (the OSDb hash, added below) and that there is no cross-provider ranking — a
    // subtitle list has no "instant beats a debrid round-trip" rule, so the blocks are simply concatenated in
    // provider order.
    QVector<LoadedAddon*> enabled;
    QVector<StremioTranslate::Manifest> manifests;
    for (LoadedAddon* s : sources_)
        if (s->stremio && isEnabled(s->manifest.id))
        { enabled.push_back(s); manifests.push_back(s->stremioManifest); }

    bool fellBackToAll = false;
    const QVector<int> chosen = StremioTranslate::routeProviders(manifests, QStringLiteral("subtitles"),
                                                                 type, id, &fellBackToAll);
    QVector<LoadedAddon*> providers;
    for (int i : chosen) providers.push_back(enabled[i]);

    if (providers.isEmpty())
    {
        streamLog(QStringLiteral("stremio: no subtitle providers for type %1").arg(type));
        cb({});
        return;
    }

    // The videoHash/videoSize extras — the highest-accuracy match a subtitle addon can make. Computed once,
    // on the GUI thread (SubtitleHash::ofFile is documented GUI-thread-only), and shared by every provider's
    // request. Absent when there is no local file (a streaming source) — the addon then matches on id alone.
    QMap<QString, QString> extras;
    if (!localPath.isEmpty())
    {
        const QString h = SubtitleHash::ofFile(localPath);
        if (!h.isEmpty())
        {
            extras.insert(QStringLiteral("videoHash"), h);
            const qint64 sz = QFileInfo(localPath).size();
            if (sz > 0) extras.insert(QStringLiteral("videoSize"), QString::number(sz));
        }
    }
    const QString path = StremioTranslate::subtitlesPath(type, id, extras);

    if (fellBackToAll)
        streamLog(QStringLiteral("stremio: subtitle idPrefixes matched no provider for %1 — asking all %2")
                      .arg(id).arg(providers.size()));
    streamLog(QStringLiteral("stremio: querying %1 subtitle provider(s) for %2 %3")
                  .arg(providers.size()).arg(type, id));

    if (!nam_) nam_ = new QNetworkAccessManager(this);
    auto blocks = std::make_shared<QVector<QVector<StremioTranslate::SubtitleAddonResult>>>(providers.size());
    auto pending = std::make_shared<int>(providers.size());
    for (int pi = 0; pi < providers.size(); ++pi)
    {
        LoadedAddon* p = providers[pi];
        QNetworkRequest rq(QUrl(p->baseUrl + path));
        rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setTransferTimeout(15000);
        QNetworkReply* reply = nam_->get(rq);
        connect(reply, &QNetworkReply::finished, this, [reply, blocks, pending, cb, pi] {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError)
                (*blocks)[pi] = StremioTranslate::parseSubtitlesResponse(reply->readAll());
            else streamLog(QStringLiteral("stremio: subtitle request error: %1").arg(reply->errorString()));
            if (--*pending != 0) return; // wait for every provider

            QVector<StremioTranslate::SubtitleAddonResult> all;
            for (const auto& block : *blocks) all += block;
            streamLog(QStringLiteral("stremio: %1 add-on subtitle(s)").arg(all.size()));
            cb(all);
        });
    }
}

void AddonManager::downloadSubtitleFile(const QString& url, const QString& lang,
                                        std::function<void(const QString& localPath)> cb)
{
    if (url.isEmpty()) { cb(QString()); return; }
    // Mirror SubtitleFetcher's download-to-file: same subs cache dir, a stable name so replays reuse the file.
    // The name is keyed on a hash of the url (the only stable identity an addon row has), plus the language for
    // legibility, plus the url's own extension when it has one (mpv detects .vtt/.ass by extension; default .srt).
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                        + QStringLiteral("/subs");
    QDir().mkpath(dir);
    QString ext = QFileInfo(QUrl(url).path()).suffix().toLower();
    static const QStringList known{ QStringLiteral("srt"), QStringLiteral("vtt"), QStringLiteral("ass"),
                                    QStringLiteral("ssa"), QStringLiteral("sub") };
    if (!known.contains(ext)) ext = QStringLiteral("srt");
    const QString tag = QString::fromLatin1(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    QString safeLang;
    for (const QChar c : lang) if (c.isLetterOrNumber()) safeLang += c;
    if (safeLang.isEmpty()) safeLang = QStringLiteral("sub");
    const QString path = QStringLiteral("%1/addon-%2-%3.%4").arg(dir, safeLang, tag, ext);

    if (!nam_) nam_ = new QNetworkAccessManager(this);
    QNetworkRequest rq{ QUrl(url) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(20000);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [reply, path, cb] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            streamLog(QStringLiteral("stremio: subtitle download failed (%1)").arg(reply->errorString()));
            cb(QString());
            return;
        }
        const QByteArray bytes = reply->readAll();
        QFile f(path);
        if (bytes.isEmpty() || !f.open(QIODevice::WriteOnly)) { cb(QString()); return; }
        f.write(bytes);
        f.close();
        streamLog(QStringLiteral("stremio: saved add-on subtitle %1 (%2 bytes)")
                      .arg(QFileInfo(path).fileName()).arg(bytes.size()));
        cb(path);
    });
}

bool AddonManager::hasSubtitleProvider(const QString& type) const
{
    for (LoadedAddon* s : sources_)
    {
        if (!isEnabled(s->manifest.id)) continue;
        if (s->stremio && s->stremioResources.contains(QStringLiteral("subtitles"))
            && (s->stremioTypes.isEmpty() || s->stremioTypes.contains(type)))
            return true;
    }
    return false;
}

// How many infoHashes one TorBox /torrents/checkcached request may carry — a URL-length bound, since the
// hashes go in the query string. It is ALSO how many rows the resolution path asks each addon to parse: a
// candidate that was never parsed is a cached release that can never be found, so parsing fewer rows than
// this can carry throws away hits for nothing.
static constexpr int kMaxHashes = 60;

// ONE walk of the preference-ordered list, taking the first entry that can start right now. `cachedHashes`
// empty means the debrid path is unavailable (no key, no torrents, failed batch check), so only direct urls
// qualify — no special case needed for that.
//
// Direct and cached are checked TOGETHER, in rank order, because after the top pick turns out to be cold the
// correct next choice is simply the best remaining playable row — and the sort already says an instant http
// url outranks every torrent. Checking only cached torrents here and sweeping for direct urls afterwards
// silently ranked a cached torrent above an instant url a remembered-but-cold bingeGroup had displaced.
bool AddonManager::playFirstPlayable(const QVector<StremioTranslate::StreamCandidate>& ordered,
                                     const QSet<QString>& cachedHashes,
                                     const StreamCb& cb,
                                     int from)
{
    for (int i = qMax(0, from); i < ordered.size(); ++i)
    {
        const StremioTranslate::StreamCandidate& c = ordered.at(i);
        if (c.isDirect())
        {
            streamLog(QStringLiteral("stremio: playing direct http url"));
            logStreamHeaders(c);
            // forPlayUrl with the SAME url is not a no-op worth skipping: it is the one place that states
            // "these headers belong to this url", so the two lines below stay symmetrical with the debrid
            // leg — where the url changes and the headers must therefore not survive.
            cb(c.url, c.mime, StreamHeaders::forPlayUrl(c.requestHeaders, c.url, c.url));
            return true;
        }
        if (!c.infoHash.isEmpty() && cachedHashes.contains(c.infoHash.toLower()))
        {
            streamLog(QStringLiteral("torbox: resolving the best CACHED torrent"));
            // A resolve can come back empty even for a hash the batch check just reported as cached (a
            // createtorrent/mylist/requestdl step can fail or hand back no url). Ending the WHOLE attempt on
            // that throws away every other candidate the same batch check already proved cached — so continue
            // down the ranked list from the next entry instead, and only report "nothing playable" once the
            // list is genuinely exhausted.
            //
            // `ordered` and `cachedHashes` are borrowed references owned by the caller's frame (one call site
            // passes a temporary `{}`), so the continuation copies both rather than capturing them.
            auto rest = std::make_shared<QVector<StremioTranslate::StreamCandidate>>(ordered);
            auto cached = std::make_shared<QSet<QString>>(cachedHashes);
            const StremioTranslate::StreamCandidate chosen = c;   // copied: `c` is a reference into `ordered`
            resolveTorBoxInfoHash(c.infoHash, c.fileIdx, [this, rest, cached, cb, i, chosen](const QString& url) {
                // A debrid-resolved url is a DIFFERENT host from the one the addon declared its headers for,
                // so forPlayUrl drops them. Passing `chosen.requestHeaders` straight through here is exactly
                // the cross-host leak this feature has to not have.
                if (!url.isEmpty())
                { cb(url, QString(), StreamHeaders::forPlayUrl(chosen.requestHeaders, chosen.url, url)); return; }
                streamLog(QStringLiteral("torbox: cached torrent failed to resolve — trying the next candidate"));
                if (!playFirstPlayable(*rest, *cached, cb, i + 1)) cb(QString(), QString(), {});
            });
            return true;
        }
    }
    return false;
}

void AddonManager::playStremioCandidates(std::shared_ptr<QVector<StremioTranslate::StreamCandidate>> ordered,
                                         StreamCb cb)
{
    if (ordered->isEmpty()) { cb(QString(), QString(), {}); return; }
    if (ordered->first().isDirect())
    {
        const StremioTranslate::StreamCandidate& c = ordered->first();
        streamLog(QStringLiteral("stremio: playing direct http url"));
        logStreamHeaders(c);
        cb(c.url, c.mime, StreamHeaders::forPlayUrl(c.requestHeaders, c.url, c.url));
        return;
    }

    // The pick is a torrent. Batch-check which hashes TorBox has cached in ONE request (rather than probing
    // each torrent's full resolve chain in turn), then resolve only the first hit.
    const QString key = torboxApiKey();
    int torrents = 0;
    for (const StremioTranslate::StreamCandidate& c : *ordered) if (!c.infoHash.isEmpty()) ++torrents;
    streamLog(QStringLiteral("stremio: %1 streams, %2 torrent(s), torbox key %3")
                  .arg(ordered->size()).arg(torrents).arg(key.isEmpty() ? QStringLiteral("missing") : QStringLiteral("present")));
    if (key.isEmpty() || torrents == 0)
    { if (!playFirstPlayable(*ordered, {}, cb)) cb(QString(), QString(), {}); return; }

    // Cap the batch (the list is preference-ordered, so the top entries are the relevant ones) to keep the
    // checkcached URL a sane length - a raw-torrent addon can return hundreds of candidates.
    QStringList hashes;
    for (const StremioTranslate::StreamCandidate& c : *ordered)
    {
        if (c.infoHash.isEmpty()) continue;
        const QString h = c.infoHash.toLower();
        if (!hashes.contains(h)) hashes << h;
        if (hashes.size() >= kMaxHashes) break;
    }
    if (torrents > hashes.size())
        streamLog(QStringLiteral("torbox: checking top %1 of %2 torrents").arg(hashes.size()).arg(torrents));
    QUrl chk(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/checkcached"));
    QUrlQuery cq; cq.addQueryItem(QStringLiteral("hash"), hashes.join(QLatin1Char(',')));
    cq.addQueryItem(QStringLiteral("format"), QStringLiteral("object"));
    chk.setQuery(cq);
    QNetworkRequest br(chk);
    br.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    br.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    br.setTransferTimeout(15000);
    streamLog(QStringLiteral("torbox: batch checkcached %1 hash(es)").arg(hashes.size()));
    QNetworkReply* bre = nam_->get(br);
    connect(bre, &QNetworkReply::finished, this, [this, bre, ordered, cb] {
        bre->deleteLater();
        QSet<QString> cached;
        if (bre->error() == QNetworkReply::NoError)
        {
            const QJsonObject data = QJsonDocument::fromJson(bre->readAll()).object().value(QStringLiteral("data")).toObject();
            for (auto it = data.begin(); it != data.end(); ++it) cached.insert(it.key().toLower());
        }
        else streamLog(QStringLiteral("torbox: batch checkcached error: %1").arg(bre->errorString()));
        streamLog(QStringLiteral("torbox: %1 candidate(s) cached").arg(cached.size()));

        // First candidate in preference order that is playable now — an instant url or a cached hash, whichever
        // ranks higher. Nothing qualifying means nothing streamable right now.
        if (!playFirstPlayable(*ordered, cached, cb)) cb(QString(), QString(), {});
    });
}

void AddonManager::resolveStremioStream(const MediaItem& item,
                                        StreamCb cb,
                                        const QString& preferGroup)
{
    listStremioStreams(item, [this, cb, preferGroup](const QVector<StremioTranslate::StreamCandidate>& all) {
        const int idx = StremioTranslate::pickAuto(all, preferGroup);
        if (idx < 0) { cb(QString(), QString(), {}); return; }
        if (idx != 0)
            streamLog(QStringLiteral("stremio: binge memory chose row %1 over the top-ranked one").arg(idx));
        // The chosen release first, then the rest in rank order. The tail is not decoration: TorBox can only
        // stream what it has cached, and the chosen release may be cold.
        auto ordered = std::make_shared<QVector<StremioTranslate::StreamCandidate>>();
        ordered->push_back(all[idx]);
        for (int i = 0; i < all.size(); ++i) if (i != idx) ordered->push_back(all[i]);
        playStremioCandidates(ordered, cb);
    },
    // Ask for as many rows as the batch cached-check can carry, NOT the picker's 30. These extra rows are
    // never shown to anyone; they exist so a user whose only cached release ranks 31-60 still plays.
    kMaxHashes);
}

bool AddonManager::hasStreamProvider(const QString& type) const
{
    for (LoadedAddon* s : sources_)
    {
        if (!isEnabled(s->manifest.id)) continue;
        if (s->stremio && s->stremioResources.contains(QStringLiteral("stream"))
            && (s->stremioTypes.isEmpty() || s->stremioTypes.contains(type)))
            return true; // a Stremio stream addon
        if (s->transport == LoadedAddon::RemoteHttp && !s->stremio && s->isMediaSource())
            return true; // a non-Stremio file provider (e.g. Allarr) - resolves via its /stream endpoint
    }
    return false;
}

bool AddonManager::hasFileProvider() const
{
    for (LoadedAddon* s : sources_)
        if (s->transport == LoadedAddon::RemoteHttp && !s->stremio && s->isMediaSource() && isEnabled(s->manifest.id))
            return true;
    return false;
}

// The id scheme a non-Stremio file provider (Allarr) expects: "mv:{imdb}" for a movie, "ep:{imdb}:{S}:{E}"
// for an episode (the imdb stream id already carries :S:E), under the /stream/{movie|series} route.
static MediaItem fileProviderItem(const QString& type, const QString& imdbStreamId)
{
    MediaItem mi;
    mi.type = (type == QStringLiteral("movie")) ? QStringLiteral("movie") : QStringLiteral("series");
    mi.id   = (type == QStringLiteral("movie") ? QStringLiteral("mv:") : QStringLiteral("ep:")) + imdbStreamId;
    return mi;
}

void AddonManager::resolveFromFileProviders(std::shared_ptr<QVector<LoadedAddon*>> providers, int idx,
                                            const QString& type, const QString& imdbStreamId,
                                            StreamCb cb, int attempt,
                                            const QString& preferGroup)
{
    if (idx >= providers->size()) // none of the file providers had it -> fall back to Stremio stream addons
    { MediaItem it; it.type = type; it.id = imdbStreamId; resolveStremioStream(it, cb, preferGroup); return; }
    resolveStream(providers->at(idx), fileProviderItem(type, imdbStreamId),
                  [this, providers, idx, type, imdbStreamId, cb, attempt, preferGroup](
                      const QString& url, const QString& mime, const StreamHeaders::Headers& headers) {
        if (!url.isEmpty()) cb(url, mime, headers);
        else resolveFromFileProviders(providers, idx + 1, type, imdbStreamId, cb, attempt, preferGroup);
    }, attempt);
}

void AddonManager::resolveStreamByImdb(const QString& type, const QString& imdbStreamId,
                                       StreamCb cb, int attempt,
                                       const QString& preferGroup)
{
    if (imdbStreamId.isEmpty()) { cb(QString(), QString(), {}); return; }
    // Prefer the user's file provider(s) - non-Stremio remote media-sources (e.g. Allarr) that serve the
    // actual files - then fall back to Stremio stream addons. attempt (?n=K) asks the provider for an
    // alternate source when the user rejects the current one. preferGroup only reaches the Stremio leg —
    // a bingeGroup is a Stremio concept and a file provider has nothing to match it against.
    auto providers = std::make_shared<QVector<LoadedAddon*>>();
    for (LoadedAddon* s : sources_)
        if (s->transport == LoadedAddon::RemoteHttp && !s->stremio && s->isMediaSource() && isEnabled(s->manifest.id))
            providers->push_back(s);
    if (providers->isEmpty())
    { MediaItem it; it.type = type; it.id = imdbStreamId; resolveStremioStream(it, cb, preferGroup); return; }
    resolveFromFileProviders(providers, 0, type, imdbStreamId, cb, attempt, preferGroup);
}

void AddonManager::resolveDocumentByQuery(const QString& query, const QString& catalogType,
                                          std::function<void(const QString&, const QString&, const QString&, bool)> cb)
{
    if (query.trimmed().isEmpty()) { cb(QString(), QString(), QString(), false); return; }
    // Pick the first enabled file provider (a non-Stremio remote media-source, e.g. Allarr) that exposes a
    // catalog of this type, and use ITS catalog id to search.
    LoadedAddon* prov = nullptr; QString catId;
    for (LoadedAddon* s : sources_)
    {
        if (s->transport != LoadedAddon::RemoteHttp || s->stremio || !s->isMediaSource()
            || !isEnabled(s->manifest.id)) continue;
        for (const AddonCatalog& c : catalogs(s))
            if (c.type == catalogType) { prov = s; catId = c.id; break; }
        if (prov) break;
    }
    if (!prov) { streamLog(QStringLiteral("doc-bridge: no file provider for type %1").arg(catalogType)); cb(QString(), QString(), QString(), false); return; }

    const QString base = prov->baseUrl;
    streamLog(QStringLiteral("doc-bridge: searching %1 catalog '%2' for \"%3\"").arg(prov->manifest.id, catId, query));
    QNetworkRequest rq(remoteCatalogUrl(base, catId, query, 1));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(45000); // a provider title search can sweep several indexers; allow time, but don't hang
    applyServerHeaders(rq, prov);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, prov, base, cb] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            // Couldn't reach the provider (down / refused / timed out) - report it as such, NOT "no match".
            // Cloudflare fronts many self-hosted providers; it wraps tunnel/origin problems in a 5xx with a
            // body like "error code: 1033" (the tunnel isn't connected). Turn those into a plain-English cause
            // so the toast tells the user what to do, instead of the raw "server replied:".
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            QString cf; { const int p = body.indexOf("error code:"); if (p >= 0) cf = QString::fromLatin1(body.mid(p + 11, 8)).trimmed(); }
            QString why;
            if (http == 530 || cf == QStringLiteral("1033"))
                why = tr("its Cloudflare tunnel is offline (error 1033). Start the server and its tunnel, then retry");
            else if (http >= 520 && http <= 527)
                why = tr("its host is unreachable (Cloudflare error %1)").arg(cf.isEmpty() ? QString::number(http) : cf);
            else if (http >= 500)
                why = tr("it returned a server error (HTTP %1)").arg(http);
            else if (http >= 400)
                why = tr("it rejected the request (HTTP %1)").arg(http);
            else
                why = reply->errorString(); // connection refused / timed out / DNS - Qt's message is already clear
            streamLog(QStringLiteral("doc-bridge: search error: %1 (http %2%3)").arg(reply->errorString())
                          .arg(http).arg(cf.isEmpty() ? QString() : QStringLiteral(", cf ") + cf));
            cb(QString(), QString(), why, false);
            return;
        }
        MediaItem hit;
        MediaCatalog cat = MediaCatalog::fromJson(reply->readAll());
        for (MediaItem& it : cat.items) it.url = resolveRemoteUrl(it.url, base);
        // Prefer the first openable leaf; fall back to the very first result.
        for (const MediaItem& it : cat.items) if (!it.expandable) { hit = it; break; }
        if (hit.id.isEmpty() && hit.url.isEmpty() && !cat.items.isEmpty()) hit = cat.items.first();
        streamLog(QStringLiteral("doc-bridge: %1 result(s), picked id=%2").arg(cat.items.size()).arg(hit.id));
        if (hit.id.isEmpty() && hit.url.isEmpty()) { cb(QString(), QString(), QString(), true); return; } // reached, zero results
        if (!hit.url.isEmpty()) { cb(hit.url, hit.mime, QString(), false); return; } // already a direct file
        // A document (comic/book/audiobook) fetched from a file provider — not the playback path, and a file
        // provider declares no proxyHeaders, so there is nothing to carry here.
        resolveStream(prov, hit, [cb](const QString& url, const QString& mime, const StreamHeaders::Headers&) {
            cb(url, mime, QString(), false);
        });
    });
}

// --- emulator BIOS provisioning ---------------------------------------------------------------------

LoadedAddon* AddonManager::biosFileProvider() const
{
    // The BIOS source is the same file provider (Allarr) that serves movies/comics/roms: an enabled,
    // non-Stremio remote media-source. When several are installed, prefer one that actually declares a
    // `bios:` catalog; otherwise fall back to the first file provider and query its bios:bios route directly.
    LoadedAddon* fallback = nullptr;
    for (LoadedAddon* s : sources_)
    {
        if (s->transport != LoadedAddon::RemoteHttp || s->stremio || !s->isMediaSource()
            || !isEnabled(s->manifest.id)) continue;
        if (!fallback) fallback = s;
        for (const AddonCatalog& c : s->manifest.catalogs)
            if (c.id.startsWith(QStringLiteral("bios:"))) return s;
    }
    return fallback;
}

bool AddonManager::hasBiosProvider() const { return biosFileProvider() != nullptr; }

QList<AddonManager::BiosServerFile> AddonManager::biosFilesForSystem(const QString& systemId, QString* providerErr) const
{
    LoadedAddon* prov = biosFileProvider();
    if (!prov) return {};
    QString err;
    const QByteArray body = httpGetBlocking(biosCatalogUrl(prov->baseUrl, systemId), remoteConfigHeader(prov), &err);
    if (!err.isEmpty()) { if (providerErr) *providerErr = err; return {}; }
    return parseBiosFiles(body);
}

QList<AddonManager::BiosServerSystem> AddonManager::biosCatalog(QString* providerErr) const
{
    LoadedAddon* prov = biosFileProvider();
    if (!prov) return {};
    QString err;
    const QByteArray body = httpGetBlocking(biosCatalogUrl(prov->baseUrl, QString()), remoteConfigHeader(prov), &err);
    if (!err.isEmpty()) { if (providerErr) *providerErr = err; return {}; }

    // Group the flat catalog into per-system buckets, preserving the server's ordering.
    QList<BiosServerSystem> systems;
    QHash<QString, int> where;
    const MediaCatalog cat = MediaCatalog::fromJson(body);
    for (const MediaItem& it : cat.items)
    {
        QString sys, fn;
        if (!parseBiosItemId(it.id, &sys, &fn)) continue;
        if (!where.contains(sys)) { where.insert(sys, systems.size()); systems.append({ sys, {} }); }
        systems[where.value(sys)].files.append({ fn, it.subtitle.trimmed().toLower(), it.id });
    }
    return systems;
}

bool AddonManager::fetchBiosFile(const BiosServerFile& file, const QString& outPath, QString* err) const
{
    LoadedAddon* prov = biosFileProvider();
    if (!prov) { if (err) *err = tr("No BIOS server is configured."); return false; }
    const QByteArray cfg = remoteConfigHeader(prov);
    const QString base = prov->baseUrl;

    // Resolve the item to a download url via /stream, then GET the bytes.
    QString serr;
    const QByteArray sbody = httpGetBlocking(biosStreamUrl(base, file.itemId), cfg, &serr);
    if (!serr.isEmpty()) { if (err) *err = serr; return false; }
    const QString url = parseStreamJson(sbody, base);
    if (url.isEmpty()) { if (err) *err = tr("No download link for %1.").arg(file.fileName); return false; }

    QString derr;
    const QByteArray data = httpGetBlocking(QUrl(url), cfg, &derr);
    if (!derr.isEmpty()) { if (err) *err = derr; return false; }
    if (data.isEmpty()) { if (err) *err = tr("Empty download for %1.").arg(file.fileName); return false; }

    // Verify the md5 when the catalog carried one; reject (write nothing) on mismatch.
    if (!file.md5.isEmpty() && md5HexOf(data) != file.md5)
    { if (err) *err = tr("MD5 mismatch for %1 (corrupt or a different dump).").arg(file.fileName); return false; }

    QDir().mkpath(QFileInfo(outPath).absolutePath());   // fileName may include a subfolder (e.g. hatari/tos/…)
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = tr("Couldn't write %1.").arg(outPath); return false; }
    f.write(data);
    f.close();
    return true;
}

void AddonManager::ensureBiosAsync(const QString& systemId, const QString& destDir, QObject* context,
                                   const std::function<void(const QString&)>& onStatus,
                                   const std::function<void()>& onDone)
{
    LoadedAddon* prov = biosFileProvider();
    if (!prov)   // BIOS requires a server now — with none configured this is a clean no-op
    { if (onDone) onDone(); return; }

    QDir().mkpath(destDir);
    // Deletes itself when the chain settles (or is cancelled by `context`'s destruction).
    new BiosServerFetcher(prov->baseUrl, remoteConfigHeader(prov), systemId, destDir,
                          context ? context : this, onStatus, onDone);
}

void AddonManager::resolveStream(LoadedAddon* src, const MediaItem& item,
                                 StreamCb cb, int attempt,
                                 const QString& preferGroup)
{
    if (!src || src->transport != LoadedAddon::RemoteHttp) { cb(QString(), QString(), {}); return; }
    // Aggregate across Stremio stream addons. preferGroup only matters here: a bingeGroup is a Stremio
    // concept, and the non-Stremio /stream branch below has nothing to match it against.
    if (src->stremio) { resolveStremioStream(item, cb, preferGroup); return; }
    const QString base = src->baseUrl;
    QNetworkRequest rq(remoteStreamUrl(base, item.type, item.id, attempt));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(90000); // debrid-resolving a torrent can be slow, but must not hang forever - a
                                  // timeout ends as an empty result so the caller can report an outcome
    applyServerHeaders(rq, src);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, base, cb] {
        reply->deleteLater();
        QString url, mime, notice;
        bool curl = false;
        if (reply->error() == QNetworkReply::NoError)
        {
            const QByteArray body = reply->readAll();
            url = parseStreamJson(body, base, &mime);
            const QJsonObject o = QJsonDocument::fromJson(body).object();
            // A "notice" accompanies an empty result when the addon has no link yet but a message for the
            // user (e.g. Allarr just sent the release to debrid to cache). Stash it for the callback.
            notice = o.value(QStringLiteral("notice")).toString();
            // "curl":true means url is a direct Cloudflare-gated source we should fetch with a browser-UA curl.
            curl = o.value(QStringLiteral("curl")).toBool();
        }
        lastStreamNotice_ = notice;
        lastStreamCurl_ = curl;
        // A file provider (Allarr) serves the file from its own host and declares no proxyHeaders — this leg
        // carries none, and must not inherit any from a Stremio candidate resolved earlier.
        cb(url, mime, {});
    });
}

QString AddonManager::resolveStreamSync(LoadedAddon* src, const MediaItem& item)
{
    if (!src || src->transport != LoadedAddon::RemoteHttp) return {};
    return parseStreamJson(
        httpGetBlocking(remoteStreamUrl(src->baseUrl, item.type, item.id), remoteConfigHeader(src)), src->baseUrl);
}

// MangaDex page resolution. A chapter item id is "mangadexch:{verId1,verId2,...}" - all the language
// versions of one chapter number. We pick the English version (when several exist), then ask MangaDex's
// at-home server for that chapter's image host + filenames, and build the ordered page URLs.
void AddonManager::resolveMangaChapterPages(const QString& chapterItemId,
                                            std::function<void(const QStringList&)> cb)
{
    static const QString kMdxApi = QStringLiteral("https://api.mangadex.org");
    QString csv = chapterItemId;
    const int colon = csv.indexOf(QLatin1Char(':'));
    if (colon >= 0) csv = csv.mid(colon + 1);
    const QStringList ids = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
    streamLog(QStringLiteral("manga: resolve %1 (%2 version id(s))").arg(chapterItemId).arg(ids.size()));
    if (ids.isEmpty()) { cb({}); return; }

    // Given a single chapter id, fetch its at-home server and assemble the page URLs.
    auto fetchPages = [this, cb](const QString& chapterId) {
        streamLog(QStringLiteral("manga: at-home for chapter %1").arg(chapterId));
        QNetworkRequest rq((QUrl(kMdxApi + QStringLiteral("/at-home/server/") + chapterId)));
        rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
        rq.setTransferTimeout(15000);
        QNetworkReply* reply = nam_->get(rq);
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            QStringList urls;
            if (reply->error() != QNetworkReply::NoError)
                streamLog(QStringLiteral("manga: at-home error: %1").arg(reply->errorString()));
            else
            {
                const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
                const QString base = o.value(QStringLiteral("baseUrl")).toString();
                const QJsonObject ch = o.value(QStringLiteral("chapter")).toObject();
                const QString hash = ch.value(QStringLiteral("hash")).toString();
                if (!base.isEmpty() && !hash.isEmpty())
                    for (const QJsonValue& f : ch.value(QStringLiteral("data")).toArray())
                        urls << base + QStringLiteral("/data/") + hash + QStringLiteral("/") + f.toString();
                else
                    streamLog(QStringLiteral("manga: at-home missing baseUrl/hash"));
            }
            streamLog(QStringLiteral("manga: resolved %1 page(s)").arg(urls.size()));
            cb(urls);
        });
    };

    // Fetch every version's metadata at once so we can skip "external" releases (licensed chapters hosted
    // off-site, which have NO page images on MangaDex) and prefer an English *hosted* version - falling back
    // to any hosted version (e.g. a non-English fan translation) so something readable opens when one exists.
    QUrl q(kMdxApi + QStringLiteral("/chapter"));
    QUrlQuery qq;
    qq.addQueryItem(QStringLiteral("limit"), QString::number(ids.size()));
    for (const QString& id : ids) qq.addQueryItem(QStringLiteral("ids[]"), id);
    q.setQuery(qq);
    QNetworkRequest rq(q);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setTransferTimeout(15000);
    QNetworkReply* reply = nam_->get(rq);
    const QString fallback = ids.first();
    connect(reply, &QNetworkReply::finished, this, [reply, fallback, cb, fetchPages] {
        reply->deleteLater();
        QString englishHosted, anyHosted, anyHostedLang;
        if (reply->error() == QNetworkReply::NoError)
        {
            const QJsonArray data = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toArray();
            for (const QJsonValue& dv : data)
            {
                const QJsonObject d = dv.toObject(), a = d.value(QStringLiteral("attributes")).toObject();
                const bool external = !a.value(QStringLiteral("externalUrl")).toString().isEmpty();
                const int pages = a.value(QStringLiteral("pages")).toInt();
                if (external || pages <= 0) continue;          // no hosted page images for this version
                const QString id = d.value(QStringLiteral("id")).toString();
                const QString lang = a.value(QStringLiteral("translatedLanguage")).toString();
                if (anyHosted.isEmpty()) { anyHosted = id; anyHostedLang = lang; }
                if (lang == QStringLiteral("en") && englishHosted.isEmpty()) englishHosted = id;
            }
        }
        if (!englishHosted.isEmpty()) { streamLog(QStringLiteral("manga: using English hosted version")); fetchPages(englishHosted); return; }
        if (!anyHosted.isEmpty()) { streamLog(QStringLiteral("manga: no English pages; using hosted '%1' version").arg(anyHostedLang)); fetchPages(anyHosted); return; }
        streamLog(QStringLiteral("manga: all versions are external/licensed - no hosted pages anywhere"));
        cb({}); // nothing has hosted pages (every version is an off-site licensed release)
    });
}

// ---- install / remove ------------------------------------------------------------------------------

bool AddonManager::installPackage(const QString& addonPackagePath, QString* error)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, addonPackagePath.toUtf8().constData(), 0))
    { if (error) *error = QStringLiteral("Not a readable addon package."); return false; }

    auto fail = [&](const QString& m) { if (error) *error = m; mz_zip_reader_end(&zip); return false; };

    // Read manifest.json (top-level) to get the addon id.
    int mfIndex = mz_zip_reader_locate_file(&zip, "manifest.json", nullptr, 0);
    if (mfIndex < 0) return fail(QStringLiteral("Package has no manifest.json."));
    size_t mfSize = 0;
    void* mfData = mz_zip_reader_extract_to_heap(&zip, mfIndex, &mfSize, 0);
    if (!mfData) return fail(QStringLiteral("Could not read the package manifest."));
    bool ok = false;
    const AddonManifest manifest = AddonManifest::fromJson(QByteArray(static_cast<char*>(mfData), int(mfSize)), &ok);
    mz_free(mfData);
    if (!ok) return fail(QStringLiteral("Package manifest is invalid."));

    // Reserved-namespace guard: the bundled addons (com.everythingbox.*) ship inside the app and are
    // NEVER installed through this path. Refuse a package that claims such an id so a downloaded /
    // side-loaded package can't impersonate a bundled source or overwrite its folder under root_.
    // Until the addon-id migration is confirmed, folders under root_ may STILL be named with the previous
    // namespace, so that namespace is still impersonable and stays reserved too. The tolerance retires itself
    // once the flag is set — by then nothing under root_ carries the old prefix for a package to collide with.
    const bool legacyStillReserved = !BrandMigration::done(BrandMigration::Step::AddonIds)
                                     && manifest.id.startsWith(QLatin1String(AppBrand::Legacy::kAddonPrefix));
    if (manifest.id.startsWith(QLatin1String(AppBrand::kAddonPrefix)) || legacyStillReserved)
    {
        streamLog(QStringLiteral("addon install: refused reserved-namespace id %1").arg(manifest.id));
        return fail(QStringLiteral("Refused: \"%1\" uses the reserved %2* namespace.")
                        .arg(manifest.id, QLatin1String(legacyStillReserved ? AppBrand::Legacy::kAddonPrefix
                                                                            : AppBrand::kAddonPrefix)));
    }

    const QString dest = root_ + QStringLiteral("/") + manifest.id;
    QDir(dest).removeRecursively();
    QDir().mkpath(dest);

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        const QString name = QFileInfo(QString::fromUtf8(st.m_filename)).fileName(); // top-level only
        if (name.isEmpty()) continue;
        mz_zip_reader_extract_to_file(&zip, i, (dest + QStringLiteral("/") + name).toUtf8().constData(), 0);
    }
    mz_zip_reader_end(&zip);

    reload();
    return true;
}

bool AddonManager::removeAddon(const QString& id)
{
    if (id.isEmpty()) return false;
    const bool ok = QDir(root_ + QStringLiteral("/") + id).removeRecursively();
    if (ok) reload();
    return ok;
}

// ---- remote sources (URL-only) ---------------------------------------------------------------------

void AddonManager::addRemoteSource(const QString& url)
{
    const QString base = normalizeBase(url);
    if (base.isEmpty()) { emit remoteSourceResult(false, tr("Enter a valid addon URL.")); return; }

    QNetworkRequest rq((QUrl(base + QStringLiteral("/manifest.json"))));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, base] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        { emit remoteSourceResult(false, tr("Couldn't reach that addon: %1").arg(reply->errorString())); return; }

        const QByteArray data = reply->readAll();
        auto built = buildRemoteAddon(base, data); // validates (Stremio or our own)
        if (!built)
        { emit remoteSourceResult(false, tr("That URL isn't a valid addon.")); return; }
        const AddonManifest m = built->manifest;

        // Persist the URL (once) + cache its manifest. We store ONLY the URL + manifest, never any code.
        QStringList urls = remoteSourceUrls();
        if (!urls.contains(base)) urls << base;
        QJsonArray arr;
        for (const QString& u : urls) arr.append(u);
        store().setValue(QStringLiteral("addon.remote.urls"), QJsonDocument(arr).toJson(QJsonDocument::Compact));
        store().setValue(manifestCacheKey(base), data);
        store().sync();

        reload();
        emit remoteSourceResult(true, tr("Added \"%1\".").arg(m.name.isEmpty() ? m.id : m.name));
        emit sourcesChanged();
    });
}

bool AddonManager::removeRemoteSource(const QString& url)
{
    const QString base = normalizeBase(url);
    QStringList urls = remoteSourceUrls();
    if (!urls.removeAll(base)) return false;
    QJsonArray arr;
    for (const QString& u : urls) arr.append(u);
    store().setValue(QStringLiteral("addon.remote.urls"), QJsonDocument(arr).toJson(QJsonDocument::Compact));
    store().remove(manifestCacheKey(base));
    store().sync();
    reload();
    emit sourcesChanged();
    return true;
}

bool AddonManager::isEnabled(const QString& id) const
{
    return store().value(QStringLiteral("addon.enabled.") + id, true).toBool();
}

LoadedAddon* AddonManager::metaProviderFor(LoadedAddon* exclude, const QString& type) const
{
    for (LoadedAddon* s : sources_)
    {
        if (s == exclude || !s->hasScript || !isEnabled(s->manifest.id)) continue; // local script addon (AIO)
        for (const AddonCatalog& c : catalogs(s)) if (c.type == type) return s;
    }
    return nullptr;
}

QVector<LoadedAddon*> AddonManager::metaProvidersFor(const QString& type) const
{
    QVector<LoadedAddon*> out;
    for (LoadedAddon* s : sources_)
        if (s->hasScript && isEnabled(s->manifest.id) && s->manifest.metaFor.contains(type))
            out << s;
    return out;
}

void AddonManager::setEnabled(const QString& id, bool enabled)
{
    store().setValue(QStringLiteral("addon.enabled.") + id, enabled);
    store().sync();
    if (!enabled)
    {
        // Drop this source's cached catalogs so nothing stale can be served after it's turned off. Cache keys
        // are "<manifestId>|catalog|query|page|…" (see catalogCacheKey), so the id + '|' prefix isolates them.
        const QString prefix = id + QLatin1Char('|');
        for (auto it = catalogCache_.begin(); it != catalogCache_.end(); )
            it = it.key().startsWith(prefix) ? catalogCache_.erase(it) : std::next(it);
    }
    emit sourceEnabledChanged(id, enabled); // the prefetcher resweeps; the UI drops/serves accordingly
}
