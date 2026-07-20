#include "AddonManager.h"
#include "../core/AppPaths.h"
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
#include <QDebug>
#include <cstring>

#include "miniz.h"

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!cfgHeader.isEmpty()) rq.setRawHeader("X-MMV-Config", cfgHeader);
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
// base64url(JSON), sent as the X-MMV-Config header so the service uses THIS user's keys (not baked-in ones).
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

// --- Stremio protocol dialect ------------------------------------------------------------------------
// Stremio addons are HTTP services with a manifest.json declaring resources (catalog/meta/stream) + types,
// and routes /catalog/{type}/{id}.json, /meta/{type}/{id}.json, /stream/{type}/{id}.json. We translate
// those into our MediaCatalog / MediaDetail / MediaItem models so they appear as ordinary catalogs.

// Detect + parse a Stremio manifest into one of our AddonManifests (catalogs keyed "type/id"), plus the
// declared resources/types. Returns false if it isn't a Stremio manifest.
static bool parseStremioManifest(const QByteArray& json, AddonManifest* outM, QStringList* outRes, QStringList* outTypes)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (!o.contains(QStringLiteral("resources")) || !o.contains(QStringLiteral("types"))) return false;

    AddonManifest m;
    m.id = o.value(QStringLiteral("id")).toString();
    m.name = o.value(QStringLiteral("name")).toString(m.id);
    m.version = o.value(QStringLiteral("version")).toString();
    m.type = QStringLiteral("media-source"); // present it like one of ours
    m.description = o.value(QStringLiteral("description")).toString();

    QStringList resources, types;
    for (const QJsonValue& r : o.value(QStringLiteral("resources")).toArray())
        resources << (r.isString() ? r.toString() : r.toObject().value(QStringLiteral("name")).toString());
    for (const QJsonValue& t : o.value(QStringLiteral("types")).toArray()) types << t.toString();

    for (const QJsonValue& cv : o.value(QStringLiteral("catalogs")).toArray())
    {
        const QJsonObject c = cv.toObject();
        const QString ctype = c.value(QStringLiteral("type")).toString();
        const QString cid = c.value(QStringLiteral("id")).toString();
        // Skip catalogs that require a search/genre to return anything (no plain landing page).
        bool requiresExtra = false;
        for (const QJsonValue& ev : c.value(QStringLiteral("extra")).toArray())
            if (ev.toObject().value(QStringLiteral("isRequired")).toBool()) { requiresExtra = true; break; }
        for (const QJsonValue& ev : c.value(QStringLiteral("extraRequired")).toArray())
            { Q_UNUSED(ev); requiresExtra = true; }
        if (requiresExtra) continue;
        AddonCatalog cat;
        cat.id = ctype + QStringLiteral("/") + cid; // encode type+id for the route
        cat.type = ctype;                            // drives our icons/routing
        cat.name = c.value(QStringLiteral("name")).toString(ctype);
        m.catalogs.push_back(cat);
    }
    *outM = m; *outRes = resources; *outTypes = types;
    return true;
}

// Build an addon (Stremio dialect if applicable, else our own) from a cached manifest. Returns null on bad data.
static std::unique_ptr<LoadedAddon> buildRemoteAddon(const QString& base, const QByteArray& manifestJson)
{
    AddonManifest manifest; QStringList res, types;
    const bool isStremio = parseStremioManifest(manifestJson, &manifest, &res, &types);
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
    return entry;
}

static QUrl stremioCatalogUrl(const QString& base, const QString& typeSlashId, const QString& query, int page)
{
    const int slash = typeSlashId.indexOf(QLatin1Char('/'));
    const QString type = slash > 0 ? typeSlashId.left(slash) : typeSlashId;
    const QString id   = slash > 0 ? typeSlashId.mid(slash + 1) : QString();
    QString u = base + QStringLiteral("/catalog/") + segEnc(type) + QStringLiteral("/") + segEnc(id);
    QStringList extra;
    if (!query.isEmpty()) extra << QStringLiteral("search=") + segEnc(query);
    if (page > 1)         extra << QStringLiteral("skip=") + QString::number((page - 1) * 100); // Stremio paginates by skip
    if (!extra.isEmpty()) u += QStringLiteral("/") + extra.join(QLatin1Char('&'));
    return QUrl(u + QStringLiteral(".json"));
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

// One candidate stream from a /stream response: either a direct http url, or a torrent to resolve via debrid.
struct StremioStreamOpt { QString url; QString mime; QString infoHash; int fileIdx = -1; };

// A real BitTorrent infoHash is 40 hex chars (v1) or a 32-char base32 string. This rejects placeholder
// junk some addons emit on failure (e.g. Debridio returns infoHash "#" with a "report this issue" title).
static bool isValidInfoHash(const QString& h)
{
    if (h.size() == 40)
    { for (QChar c : h) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false; return true; }
    if (h.size() == 32)
    { for (QChar c : h) { const QChar u = c.toUpper(); if (!((u >= 'A' && u <= 'Z') || (u >= '2' && u <= '7'))) return false; } return true; }
    return false;
}

// {streams:[{url|infoHash|fileIdx|...}]} -> all *usable* candidates, in the addon's order (best first).
static QVector<StremioStreamOpt> parseStremioStreams(const QByteArray& body)
{
    QVector<StremioStreamOpt> out;
    for (const QJsonValue& sv : QJsonDocument::fromJson(body).object().value(QStringLiteral("streams")).toArray())
    {
        const QJsonObject s = sv.toObject();
        StremioStreamOpt o;
        o.url = s.value(QStringLiteral("url")).toString();
        o.mime = s.value(QStringLiteral("mime")).toString();
        o.infoHash = s.value(QStringLiteral("infoHash")).toString();
        o.fileIdx = s.contains(QStringLiteral("fileIdx")) ? s.value(QStringLiteral("fileIdx")).toInt() : -1;
        if (o.url.startsWith(QStringLiteral("http")) || isValidInfoHash(o.infoHash)) out.push_back(o);
    }
    return out;
}

static QString torboxApiKey() { store().sync(); return store().value(QStringLiteral("debrid/torbox/apikey")).toString().trimmed(); }

// One-line append to <app>/stream_debug.log so a stream-resolution run can be traced after the fact
// (no API key/token is ever written - callers pass already-masked text).
static void streamLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
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
        if (!cat.items.isEmpty()) // don't cache an empty/failed fetch
            catalogCache_.insert(key, { QDateTime::currentMSecsSinceEpoch(), cat });
    });

    // MMV_PREFETCH_TTL_S (seconds, >0) compresses the catalog-cache TTL for tests; it also scales the
    // CatalogPrefetcher's resweep cadence (which reads catalogCacheTtlMs()). Unset -> the 30-minute default.
    const int ttlOverrideS = qEnvironmentVariableIntValue("MMV_PREFETCH_TTL_S");
    if (ttlOverrideS > 0)
    {
        catalogCacheTtlMs_ = qint64(ttlOverrideS) * 1000;
        static bool loggedOverride = false; // once per process, not per AddonManager
        if (!loggedOverride)
        {
            loggedOverride = true;
            streamLog(QStringLiteral("prefetch: MMV_PREFETCH_TTL_S override active - catalog cache TTL %1s")
                          .arg(ttlOverrideS));
        }
    }

    nam_ = new QNetworkAccessManager(this);
    // MMV_ADDONS_ROOT lets a test (probe_addon) point discovery at an isolated fixture dir instead of the
    // portable <app>/addons folder; unset in normal runs, so behaviour is unchanged.
    root_ = qEnvironmentVariableIsSet("MMV_ADDONS_ROOT")
                ? qEnvironmentVariable("MMV_ADDONS_ROOT")
                : AppPaths::dataDir() + QStringLiteral("/addons");
    QDir().mkpath(root_);
    reload();
    seedDefaultStremioSources(); // add Cinemeta once so Stremio movie/series catalogs work out of the box
    refreshRemoteManifests();    // pick up any catalogs an addon added since we last cached its manifest
    checkAddonUpdates();         // self-update local addons that publish a newer package (manifest updateUrl)
}

void AddonManager::refreshRemoteManifests()
{
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    for (const QString& base : remoteSourceUrls())
    {
        QNetworkRequest rq((QUrl(base + QStringLiteral("/manifest.json"))));
        rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
        rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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

            const QString tmp = QDir::tempPath() + QStringLiteral("/mmv-update-") + t.id + QStringLiteral(".addon");
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
    // First-ever run: Cinemeta (movie/series metadata + IMDB ids) so Stremio catalogs work out of the box.
    if (!store().value(QStringLiteral("addon.stremio.seeded")).toBool())
    {
        store().setValue(QStringLiteral("addon.stremio.seeded"), true); store().sync();
        addRemoteSource(QStringLiteral("https://v3-cinemeta.strem.io/manifest.json"));
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

    // One-time: seed Torrentio as a stream source. It returns infoHashes (raw torrents); our TorBox resolver
    // (Settings -> General -> Streaming) turns the cached ones into playable links - no third-party debrid.
    if (!store().value(QStringLiteral("addon.torrentio.seeded")).toBool())
    {
        store().setValue(QStringLiteral("addon.torrentio.seeded"), true); store().sync();
        addRemoteSource(QStringLiteral("https://torrentio.strem.io/manifest.json"));
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
    QNetworkRequest rq(stremio ? stremioCatalogUrl(base, catalogId, query, page)
                               : remoteCatalogUrl(base, catalogId, query, page, filters));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Liveness: without a transfer timeout a black-holed remote never finishes, its result signal never fires,
    // and anything waiting on this reqId (e.g. a prefetcher in-flight slot) wedges. 15s matches the other sites.
    rq.setTransferTimeout(15000);
    if (!stremio) { const QByteArray cfg = remoteConfigHeader(src); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqId, base, stremio] {
        reply->deleteLater();
        MediaCatalog cat;
        if (reply->error() == QNetworkReply::NoError)
        {
            cat = stremio ? parseStremioCatalog(reply->readAll()) : MediaCatalog::fromJson(reply->readAll());
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Liveness: without a transfer timeout a black-holed remote never finishes, its result signal never fires,
    // and anything waiting on this reqId (e.g. a prefetcher in-flight slot) wedges. 15s matches the other sites.
    rq.setTransferTimeout(15000);
    if (!stremio) { const QByteArray cfg = remoteConfigHeader(src); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Liveness: without a transfer timeout a black-holed remote never finishes, its result signal never fires,
    // and anything waiting on this reqId (e.g. a prefetcher in-flight slot) wedges. 15s matches the other sites.
    rq.setTransferTimeout(15000);
    if (!stremio) { const QByteArray cfg = remoteConfigHeader(src); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
        cr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
            mr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
                dr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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

void AddonManager::resolveStremioStream(const MediaItem& item,
                                        std::function<void(const QString&, const QString&)> cb)
{
    // Query EVERY installed Stremio addon that offers streams for this type (like Stremio aggregates), collect
    // all candidate streams, then play the first direct http url - or resolve the first cached torrent via TorBox.
    QVector<LoadedAddon*> providers;
    for (LoadedAddon* s : sources_)
        if (s->stremio && isEnabled(s->manifest.id) && s->stremioResources.contains(QStringLiteral("stream"))
            && (s->stremioTypes.isEmpty() || s->stremioTypes.contains(item.type)))
            providers.push_back(s);
    if (providers.isEmpty()) { streamLog(QStringLiteral("stremio: no stream providers for type %1").arg(item.type)); cb(QString(), QString()); return; }
    streamLog(QStringLiteral("stremio: querying %1 stream provider(s) for %2 %3").arg(providers.size()).arg(item.type, item.id));

    auto opts = std::make_shared<QVector<StremioStreamOpt>>();
    auto pending = std::make_shared<int>(providers.size());
    for (LoadedAddon* p : providers)
    {
        QNetworkRequest rq(stremioStreamUrl(p->baseUrl, item.type, item.id));
        rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setTransferTimeout(15000);
        QNetworkReply* reply = nam_->get(rq);
        connect(reply, &QNetworkReply::finished, this, [this, reply, opts, pending, cb] {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) *opts += parseStremioStreams(reply->readAll());
            else streamLog(QStringLiteral("stremio: stream request error: %1").arg(reply->errorString()));
            if (--*pending != 0) return; // wait for every provider

            // Prefer a direct http url (instant).
            for (const StremioStreamOpt& o : *opts)
                if (o.url.startsWith(QStringLiteral("http"))) { streamLog(QStringLiteral("stremio: playing direct http url")); cb(o.url, o.mime); return; }

            // Otherwise the candidates are torrents. Batch-check which hashes TorBox has cached in ONE request
            // (rather than probing each torrent's full resolve chain in turn), then resolve only the first hit.
            const QString key = torboxApiKey();
            int torrents = 0; for (const StremioStreamOpt& o : *opts) if (!o.infoHash.isEmpty()) ++torrents;
            streamLog(QStringLiteral("stremio: %1 streams, %2 torrent(s), torbox key %3")
                          .arg(opts->size()).arg(torrents).arg(key.isEmpty() ? QStringLiteral("missing") : QStringLiteral("present")));
            if (key.isEmpty() || torrents == 0) { cb(QString(), QString()); return; }

            // Cap the batch (addons list best-first, so the top results are the relevant ones) to keep the
            // checkcached URL a sane length - a raw-torrent addon can return hundreds of candidates.
            const int kMaxHashes = 60;
            QStringList hashes;
            for (const StremioStreamOpt& o : *opts)
            {
                if (o.infoHash.isEmpty()) continue;
                const QString h = o.infoHash.toLower();
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
            br.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
            br.setTransferTimeout(15000);
            streamLog(QStringLiteral("torbox: batch checkcached %1 hash(es)").arg(hashes.size()));
            QNetworkReply* bre = nam_->get(br);
            connect(bre, &QNetworkReply::finished, this, [this, bre, opts, cb] {
                bre->deleteLater();
                QSet<QString> cached;
                if (bre->error() == QNetworkReply::NoError)
                {
                    const QJsonObject data = QJsonDocument::fromJson(bre->readAll()).object().value(QStringLiteral("data")).toObject();
                    for (auto it = data.begin(); it != data.end(); ++it) cached.insert(it.key().toLower());
                }
                else streamLog(QStringLiteral("torbox: batch checkcached error: %1").arg(bre->errorString()));
                streamLog(QStringLiteral("torbox: %1 candidate(s) cached").arg(cached.size()));

                // First candidate (addons list best-first) whose hash is cached -> resolve just that one.
                for (const StremioStreamOpt& o : *opts)
                    if (!o.infoHash.isEmpty() && cached.contains(o.infoHash.toLower()))
                    { resolveTorBoxInfoHash(o.infoHash, o.fileIdx, [cb](const QString& url) { cb(url, QString()); }); return; }
                cb(QString(), QString()); // nothing cached -> can't stream right now
            });
        });
    }
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
                                            std::function<void(const QString&, const QString&)> cb, int attempt)
{
    if (idx >= providers->size()) // none of the file providers had it -> fall back to Stremio stream addons
    { MediaItem it; it.type = type; it.id = imdbStreamId; resolveStremioStream(it, cb); return; }
    resolveStream(providers->at(idx), fileProviderItem(type, imdbStreamId),
                  [this, providers, idx, type, imdbStreamId, cb, attempt](const QString& url, const QString& mime) {
        if (!url.isEmpty()) cb(url, mime);
        else resolveFromFileProviders(providers, idx + 1, type, imdbStreamId, cb, attempt);
    }, attempt);
}

void AddonManager::resolveStreamByImdb(const QString& type, const QString& imdbStreamId,
                                       std::function<void(const QString&, const QString&)> cb, int attempt)
{
    if (imdbStreamId.isEmpty()) { cb(QString(), QString()); return; }
    // Prefer the user's file provider(s) - non-Stremio remote media-sources (e.g. Allarr) that serve the
    // actual files - then fall back to Stremio stream addons. attempt (?n=K) asks the provider for an
    // alternate source when the user rejects the current one.
    auto providers = std::make_shared<QVector<LoadedAddon*>>();
    for (LoadedAddon* s : sources_)
        if (s->transport == LoadedAddon::RemoteHttp && !s->stremio && s->isMediaSource() && isEnabled(s->manifest.id))
            providers->push_back(s);
    if (providers->isEmpty())
    { MediaItem it; it.type = type; it.id = imdbStreamId; resolveStremioStream(it, cb); return; }
    resolveFromFileProviders(providers, 0, type, imdbStreamId, cb, attempt);
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(45000); // a provider title search can sweep several indexers; allow time, but don't hang
    { const QByteArray cfg = remoteConfigHeader(prov); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
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
        resolveStream(prov, hit, [cb](const QString& url, const QString& mime) { cb(url, mime, QString(), false); });
    });
}

void AddonManager::resolveStream(LoadedAddon* src, const MediaItem& item,
                                 std::function<void(const QString&, const QString&)> cb, int attempt)
{
    if (!src || src->transport != LoadedAddon::RemoteHttp) { cb(QString(), QString()); return; }
    if (src->stremio) { resolveStremioStream(item, cb); return; } // aggregate across Stremio stream addons
    const QString base = src->baseUrl;
    QNetworkRequest rq(remoteStreamUrl(base, item.type, item.id, attempt));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(90000); // debrid-resolving a torrent can be slow, but must not hang forever - a
                                  // timeout ends as an empty result so the caller can report an outcome
    { const QByteArray cfg = remoteConfigHeader(src); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
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
        cb(url, mime);
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
        rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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
