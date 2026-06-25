#include "AddonManager.h"
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
#include <QDebug>
#include <cstring>

#include "miniz.h"

static QSettings& store()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"),
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
    const QString inApp = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/") + url);
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
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("id"), item.id }, { QStringLiteral("type"), item.type } }).toJson(QJsonDocument::Compact));
}

static QString catalogArg(const QString& catalogId, const QString& query, int page)
{
    QJsonObject a;
    if (!catalogId.isEmpty()) a.insert(QStringLiteral("catalog"), catalogId);
    if (!query.isEmpty())     a.insert(QStringLiteral("query"), query);
    a.insert(QStringLiteral("page"), page);
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

static QUrl remoteCatalogUrl(const QString& base, const QString& catalogId, const QString& query, int page)
{
    QString u = base + QStringLiteral("/catalog/") + segEnc(catalogId.isEmpty() ? QStringLiteral("default") : catalogId);
    QStringList extra;
    if (!query.isEmpty()) extra << QStringLiteral("search=") + segEnc(query);
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

static QUrl remoteStreamUrl(const QString& base, const QString& type, const QString& id)
{
    return QUrl(base + QStringLiteral("/stream/") + segEnc(type.isEmpty() ? QStringLiteral("item") : type)
               + QStringLiteral("/") + segEnc(id) + QStringLiteral(".json"));
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

// {streams:[{url|infoHash|fileIdx|...}]} -> all candidates, in the addon's order (best first, by convention).
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
        if (o.url.startsWith(QStringLiteral("http")) || !o.infoHash.isEmpty()) out.push_back(o);
    }
    return out;
}

static QString torboxApiKey() { store().sync(); return store().value(QStringLiteral("debrid/torbox/apikey")).toString().trimmed(); }

// --- AddonManager ------------------------------------------------------------------------------------

AddonManager::AddonManager(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<MediaCatalog>("MediaCatalog");
    qRegisterMetaType<MediaDetail>("MediaDetail");
    nam_ = new QNetworkAccessManager(this);
    root_ = QCoreApplication::applicationDirPath() + QStringLiteral("/addons");
    QDir().mkpath(root_);
    reload();
    seedDefaultStremioSources(); // add Cinemeta once so Stremio movie/series catalogs work out of the box
}

void AddonManager::reload()
{
    loaded_.clear();
    sources_.clear();

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

// Add Cinemeta (Stremio's public movie/series metadata addon) on first run, so movie/series catalogs and
// IMDB ids work out of the box for any Stremio stream addon the user adds. One-time; the user can remove it.
void AddonManager::seedDefaultStremioSources()
{
    if (store().value(QStringLiteral("addon.stremio.seeded")).toBool()) return;
    store().setValue(QStringLiteral("addon.stremio.seeded"), true);
    store().sync();
    addRemoteSource(QStringLiteral("https://v3-cinemeta.strem.io/manifest.json"));
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

int AddonManager::requestCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page)
{
    if (!src) return -1;
    if (src->transport == LoadedAddon::RemoteHttp) return dispatchRemoteCatalog(src, catalogId, query, page);
    return dispatch(buildRequest(src, QStringLiteral("getCatalog"), catalogArg(catalogId, query, page)));
}

int AddonManager::requestDetail(LoadedAddon* src, const MediaItem& item, int page)
{
    if (!src) return -1;
    if (src->transport == LoadedAddon::RemoteHttp) return dispatchRemoteDetail(src, item, page);
    const QString arg = QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("id"), item.id }, { QStringLiteral("type"), item.type },
        { QStringLiteral("page"), page } }).toJson(QJsonDocument::Compact));
    return dispatch(buildRequest(src, QStringLiteral("getDetail"), arg));
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

int AddonManager::dispatchRemoteCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page)
{
    const int reqId = ++reqCounter_;
    const QString base = src->baseUrl;
    const bool stremio = src->stremio;
    QNetworkRequest rq(stremio ? stremioCatalogUrl(base, catalogId, query, page)
                               : remoteCatalogUrl(base, catalogId, query, page));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
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
    if (key.isEmpty() || infoHash.isEmpty()) { cb(QString()); return; }
    const QByteArray bearer = "Bearer " + key.toUtf8();

    // 1) Only stream torrents TorBox already has cached (otherwise it'd just queue a download).
    QUrl chk(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/checkcached"));
    QUrlQuery cq; cq.addQueryItem(QStringLiteral("hash"), infoHash.toLower());
    cq.addQueryItem(QStringLiteral("format"), QStringLiteral("object"));
    chk.setQuery(cq);
    QNetworkRequest rq(chk);
    rq.setRawHeader("Authorization", bearer);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, infoHash, fileIdx, key, bearer, cb] {
        reply->deleteLater();
        const QJsonObject data = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toObject();
        if (data.isEmpty()) { cb(QString()); return; } // not cached -> can't stream it now

        // 2) Add the (cached) magnet to the account to obtain a torrent_id.
        const QByteArray boundary = "tbb" + QCryptographicHash::hash(infoHash.toUtf8(), QCryptographicHash::Md5).toHex().left(12);
        QByteArray fb;
        fb += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"magnet\"\r\n\r\n";
        fb += "magnet:?xt=urn:btih:" + infoHash.toUtf8() + "\r\n--" + boundary + "--\r\n";
        QNetworkRequest cr((QUrl(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/createtorrent"))));
        cr.setRawHeader("Authorization", bearer);
        cr.setHeader(QNetworkRequest::ContentTypeHeader, "multipart/form-data; boundary=" + boundary);
        cr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        QNetworkReply* cre = nam_->post(cr, fb);
        connect(cre, &QNetworkReply::finished, this, [this, cre, fileIdx, key, bearer, cb] {
            cre->deleteLater();
            const QJsonObject cd = QJsonDocument::fromJson(cre->readAll()).object().value(QStringLiteral("data")).toObject();
            const QString torrentId = QString::number(cd.value(QStringLiteral("torrent_id")).toVariant().toLongLong());
            if (torrentId.isEmpty() || torrentId == QStringLiteral("0")) { cb(QString()); return; }

            // 3) List the torrent's files to choose one (the episode by index, else the largest video).
            QUrl ml(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/mylist"));
            QUrlQuery mq; mq.addQueryItem(QStringLiteral("id"), torrentId); mq.addQueryItem(QStringLiteral("bypass_cache"), QStringLiteral("true"));
            ml.setQuery(mq);
            QNetworkRequest mr(ml);
            mr.setRawHeader("Authorization", bearer);
            mr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
            QNetworkReply* mre = nam_->get(mr);
            connect(mre, &QNetworkReply::finished, this, [this, mre, torrentId, fileIdx, key, cb] {
                mre->deleteLater();
                const QJsonValue dv = QJsonDocument::fromJson(mre->readAll()).object().value(QStringLiteral("data"));
                const QJsonObject td = dv.isArray() ? dv.toArray().first().toObject() : dv.toObject(); // id-> object or [object]
                const QJsonArray files = td.value(QStringLiteral("files")).toArray();
                if (files.isEmpty()) { cb(QString()); return; }
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

                // 4) Ask for the direct download/stream link.
                QUrl dl(QString::fromLatin1(kTorBoxApi) + QStringLiteral("/torrents/requestdl"));
                QUrlQuery dq; dq.addQueryItem(QStringLiteral("token"), key);
                dq.addQueryItem(QStringLiteral("torrent_id"), torrentId); dq.addQueryItem(QStringLiteral("file_id"), fileId);
                dl.setQuery(dq);
                QNetworkRequest dr(dl);
                dr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
                QNetworkReply* dre = nam_->get(dr);
                connect(dre, &QNetworkReply::finished, this, [dre, cb] {
                    dre->deleteLater();
                    const QJsonValue d = QJsonDocument::fromJson(dre->readAll()).object().value(QStringLiteral("data"));
                    cb(d.toString());
                });
            });
        });
    });
}

// Try each torrent candidate in turn via TorBox until one is cached + resolves (clean member recursion).
static void resolveTorBoxList(AddonManager* mgr, std::shared_ptr<QVector<StremioStreamOpt>> opts, int idx,
                              std::function<void(const QString&, const QString&)> cb)
{
    while (idx < opts->size() && opts->at(idx).infoHash.isEmpty()) ++idx;
    if (idx >= opts->size()) { cb(QString(), QString()); return; }
    const StremioStreamOpt o = opts->at(idx);
    mgr->resolveTorBoxInfoHash(o.infoHash, o.fileIdx, [mgr, opts, idx, cb](const QString& url) {
        if (!url.isEmpty()) cb(url, QString());
        else resolveTorBoxList(mgr, opts, idx + 1, cb); // not cached/resolvable -> next
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
    if (providers.isEmpty()) { cb(QString(), QString()); return; }

    auto opts = std::make_shared<QVector<StremioStreamOpt>>();
    auto pending = std::make_shared<int>(providers.size());
    for (LoadedAddon* p : providers)
    {
        QNetworkRequest rq(stremioStreamUrl(p->baseUrl, item.type, item.id));
        rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(rq);
        connect(reply, &QNetworkReply::finished, this, [this, reply, opts, pending, cb] {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) *opts += parseStremioStreams(reply->readAll());
            if (--*pending != 0) return; // wait for every provider

            // Prefer a direct http url (instant).
            for (const StremioStreamOpt& o : *opts)
                if (o.url.startsWith(QStringLiteral("http"))) { cb(o.url, o.mime); return; }
            // Otherwise resolve torrents through TorBox, trying each in order until one is cached + resolves.
            if (torboxApiKey().isEmpty()) { cb(QString(), QString()); return; }
            resolveTorBoxList(this, opts, 0, cb);
        });
    }
}

void AddonManager::resolveStream(LoadedAddon* src, const MediaItem& item,
                                 std::function<void(const QString&, const QString&)> cb)
{
    if (!src || src->transport != LoadedAddon::RemoteHttp) { cb(QString(), QString()); return; }
    if (src->stremio) { resolveStremioStream(item, cb); return; } // aggregate across Stremio stream addons
    const QString base = src->baseUrl;
    QNetworkRequest rq(remoteStreamUrl(base, item.type, item.id));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    { const QByteArray cfg = remoteConfigHeader(src); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [reply, base, cb] {
        reply->deleteLater();
        QString url, mime;
        if (reply->error() == QNetworkReply::NoError) url = parseStreamJson(reply->readAll(), base, &mime);
        cb(url, mime);
    });
}

QString AddonManager::resolveStreamSync(LoadedAddon* src, const MediaItem& item)
{
    if (!src || src->transport != LoadedAddon::RemoteHttp) return {};
    return parseStreamJson(
        httpGetBlocking(remoteStreamUrl(src->baseUrl, item.type, item.id), remoteConfigHeader(src)), src->baseUrl);
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

void AddonManager::setEnabled(const QString& id, bool enabled)
{
    store().setValue(QStringLiteral("addon.enabled.") + id, enabled);
    store().sync();
}
