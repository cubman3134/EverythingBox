#include "AddonManager.h"
#include "AddonContext.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <cstring>

#include "miniz.h"

static QSettings& store()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/goliath.ini"),
                       QSettings::IniFormat);
    return s;
}

AddonManager::AddonManager()
{
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

    QFile sf(dir + QStringLiteral("/") + manifest.entryOrDefault());
    if (manifest.type == QStringLiteral("media-source") && sf.open(QIODevice::ReadOnly))
    {
        const QString source = QString::fromUtf8(sf.readAll());
        const QString storageDir = root_ + QStringLiteral("/_storage/") + manifest.id;
        auto ctx = std::make_unique<AddonContext>(manifest, storageDir);
        QString err;
        entry->addon = JsAddon::load(source, std::move(ctx), &err);
        if (!entry->addon)
            qWarning().noquote() << QStringLiteral("addon '%1' failed to load: %2").arg(manifest.id, err);
    }

    LoadedAddon* raw = entry.get();
    loaded_.push_back(std::move(entry));
    if (raw->isMediaSource())
        sources_.push_back(raw);
}

MediaCatalog AddonManager::catalog(LoadedAddon* src)
{
    if (!src || !src->addon) return {};
    const QString json = src->addon->invoke(QStringLiteral("getCatalog"), QStringLiteral("{}"));
    return resolved(MediaCatalog::fromJson(json.toUtf8()), src->dir);
}

MediaCatalog AddonManager::search(LoadedAddon* src, const QString& query)
{
    if (!src || !src->addon || !src->addon->hasFunction(QStringLiteral("search"))) return {};
    // Pass {"query":"..."} as a JSON string (Qt builds the escaping correctly).
    const QByteArray arg = QJsonDocument(QJsonObject{ { QStringLiteral("query"), query } }).toJson(QJsonDocument::Compact);
    const QString json = src->addon->invoke(QStringLiteral("search"), QString::fromUtf8(arg));
    return resolved(MediaCatalog::fromJson(json.toUtf8()), src->dir);
}

MediaCatalog AddonManager::resolved(MediaCatalog cat, const QString& addonDir) const
{
    for (MediaItem& it : cat.items)
        it.url = resolveUrl(it.url, addonDir);
    return cat;
}

QString AddonManager::resolveUrl(const QString& url, const QString& addonDir) const
{
    if (url.isEmpty()) return url;
    if (url.contains(QStringLiteral("://"))) return url; // http(s)/file/magnet - leave as-is

    QFileInfo fi(url);
    if (fi.isAbsolute() && fi.exists()) return fi.absoluteFilePath();

    // Relative: prefer the addon's own folder, then the app directory.
    const QString inAddon = QDir::cleanPath(addonDir + QStringLiteral("/") + url);
    if (QFile::exists(inAddon)) return inAddon;
    const QString inApp = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/") + url);
    if (QFile::exists(inApp)) return inApp;
    return url; // unresolved - hand it back unchanged
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
