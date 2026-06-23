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

// --- AddonManager ------------------------------------------------------------------------------------

AddonManager::AddonManager(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<MediaCatalog>("MediaCatalog");
    qRegisterMetaType<MediaDetail>("MediaDetail");
    nam_ = new QNetworkAccessManager(this);
    root_ = QCoreApplication::applicationDirPath() + QStringLiteral("/addons");
    QDir().mkpath(root_);
    reload();
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
        bool ok = false;
        AddonManifest manifest = AddonManifest::fromJson(mf, &ok);
        if (!ok) continue;

        auto entry = std::make_unique<LoadedAddon>();
        entry->transport = LoadedAddon::RemoteHttp;
        entry->baseUrl = base;
        entry->manifest = manifest;
        LoadedAddon* raw = entry.get();
        loaded_.push_back(std::move(entry));
        if (raw->isMediaSource()) sources_.push_back(raw);
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
    QNetworkRequest rq(remoteCatalogUrl(base, catalogId, query, page));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    { const QByteArray cfg = remoteConfigHeader(src); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqId, base] {
        reply->deleteLater();
        MediaCatalog cat;
        if (reply->error() == QNetworkReply::NoError)
        {
            cat = MediaCatalog::fromJson(reply->readAll());
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
    QNetworkRequest rq(remoteDetailUrl(base, item.type, item.id, page));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    { const QByteArray cfg = remoteConfigHeader(src); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqId, base] {
        reply->deleteLater();
        MediaCatalog cat;
        if (reply->error() == QNetworkReply::NoError)
        {
            cat = MediaCatalog::fromJson(reply->readAll());
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
    QNetworkRequest rq(remoteMetaUrl(base, item.type, item.id));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    { const QByteArray cfg = remoteConfigHeader(src); if (!cfg.isEmpty()) rq.setRawHeader("X-MMV-Config", cfg); }
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, reqId, base] {
        reply->deleteLater();
        MediaDetail d;
        if (reply->error() == QNetworkReply::NoError)
        {
            d = MediaDetail::fromJson(reply->readAll());
            d.imageUrl = resolveRemoteUrl(d.imageUrl, base);
        }
        emit metaReady(reqId, d);
    });
    return reqId;
}

void AddonManager::resolveStream(LoadedAddon* src, const MediaItem& item,
                                 std::function<void(const QString&, const QString&)> cb)
{
    if (!src || src->transport != LoadedAddon::RemoteHttp) { cb(QString(), QString()); return; }
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
        bool ok = false;
        const AddonManifest m = AddonManifest::fromJson(data, &ok);
        if (!ok || m.type != QStringLiteral("media-source"))
        { emit remoteSourceResult(false, tr("That URL isn't a valid media-source addon.")); return; }

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
