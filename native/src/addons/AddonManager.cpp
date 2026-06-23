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
#include <QFutureWatcher>
#include <QtConcurrent>
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

// --- AddonManager ------------------------------------------------------------------------------------

AddonManager::AddonManager(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<MediaCatalog>("MediaCatalog");
    qRegisterMetaType<MediaDetail>("MediaDetail");
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
MediaCatalog AddonManager::catalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page)
{
    if (!src) return {};
    return executeRequest(buildRequest(src, QStringLiteral("getCatalog"), catalogArg(catalogId, query, page)));
}

MediaCatalog AddonManager::detail(LoadedAddon* src, const MediaItem& item, int page)
{
    if (!src) return {};
    const QString arg = QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("id"), item.id }, { QStringLiteral("type"), item.type },
        { QStringLiteral("page"), page } }).toJson(QJsonDocument::Compact));
    return executeRequest(buildRequest(src, QStringLiteral("getDetail"), arg));
}

MediaCatalog AddonManager::search(LoadedAddon* src, const QString& query)
{
    if (!src) return {};
    const QString arg = QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("query"), query } }).toJson(QJsonDocument::Compact));
    return executeRequest(buildRequest(src, QStringLiteral("search"), arg));
}

MediaDetail AddonManager::meta(LoadedAddon* src, const MediaItem& item)
{
    if (!src) return {};
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
    return dispatchMeta(buildRequest(src, QStringLiteral("getMeta"), itemArg(item)));
}

int AddonManager::requestCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page)
{
    if (!src) return -1;
    return dispatch(buildRequest(src, QStringLiteral("getCatalog"), catalogArg(catalogId, query, page)));
}

int AddonManager::requestDetail(LoadedAddon* src, const MediaItem& item, int page)
{
    if (!src) return -1;
    const QString arg = QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("id"), item.id }, { QStringLiteral("type"), item.type },
        { QStringLiteral("page"), page } }).toJson(QJsonDocument::Compact));
    return dispatch(buildRequest(src, QStringLiteral("getDetail"), arg));
}

int AddonManager::requestSearch(LoadedAddon* src, const QString& query)
{
    if (!src) return -1;
    const QString arg = QString::fromUtf8(QJsonDocument(QJsonObject{
        { QStringLiteral("query"), query } }).toJson(QJsonDocument::Compact));
    return dispatch(buildRequest(src, QStringLiteral("search"), arg));
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

bool AddonManager::isEnabled(const QString& id) const
{
    return store().value(QStringLiteral("addon.enabled.") + id, true).toBool();
}

void AddonManager::setEnabled(const QString& id, bool enabled)
{
    store().setValue(QStringLiteral("addon.enabled.") + id, enabled);
    store().sync();
}
