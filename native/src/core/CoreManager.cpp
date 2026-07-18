#include "CoreManager.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QProgressDialog>
#include <QMessageBox>
#include <cstring>

#include "miniz.h"
#include "BiosCatalog.h"

QString CoreManager::coresDir()
{
    const QString d = AppPaths::dataDir() + QStringLiteral("/cores");
    QDir().mkpath(d);
    return d;
}

// ---- Per-OS libretro core packaging --------------------------------------------------------------------
// The buildbot ships a different file per platform; pick the right subpath, file name and shared-library
// extension. NB: Android is also Q_OS_LINUX, so it must be tested first.
static QString buildbotSubpath()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows/x86_64/latest/");
#elif defined(Q_OS_MACOS)
  #if defined(Q_PROCESSOR_ARM)
    return QStringLiteral("apple/osx/arm64/latest/");
  #else
    return QStringLiteral("apple/osx/x86_64/latest/");
  #endif
#elif defined(Q_OS_ANDROID)
    return QStringLiteral("android/latest/arm64-v8a/");
#else
    return QStringLiteral("linux/x86_64/latest/");
#endif
}

static QString libExt()
{
#if defined(Q_OS_WIN)
    return QStringLiteral(".dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral(".dylib");
#else
    return QStringLiteral(".so"); // Linux + Android
#endif
}

// The core's on-disk file name (also the buildbot file name minus ".zip"). Android cores carry an extra
// "_android" marker before the .so.
static QString coreFileName(const QString& coreName)
{
#if defined(Q_OS_ANDROID)
    return coreName + QStringLiteral("_libretro_android.so");
#else
    return coreName + QStringLiteral("_libretro") + libExt();
#endif
}

QString CoreManager::corePath(const QString& coreName)
{
    return coresDir() + QStringLiteral("/") + coreFileName(coreName);
}

bool CoreManager::isInstalled(const QString& coreName)
{
    return QFile::exists(corePath(coreName));
}

// Extract the first shared library (matching ext: .dll/.dylib/.so) from a zip (in memory) into destDir.
// Returns the written path via outDll.
static bool unzipLibFromMemory(const QByteArray& zipData, const QString& destDir, const QString& ext, QString* outDll)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipData.constData(), static_cast<size_t>(zipData.size()), 0))
        return false;

    bool ok = false;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st))
            continue;
        const QString name = QString::fromUtf8(st.m_filename);
        if (name.endsWith(ext, Qt::CaseInsensitive))
        {
            const QString out = destDir + QStringLiteral("/") + QFileInfo(name).fileName();
            if (mz_zip_reader_extract_to_file(&zip, i, out.toUtf8().constData(), 0))
            {
                if (outDll) *outDll = out;
                ok = true;
            }
            break;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

QString CoreManager::ensureCore(const QString& coreName, QString* error,
                                const std::function<void(int)>& onProgress)
{
    if (isInstalled(coreName))
        return corePath(coreName);

    // libretro nightly buildbot - the right build for the running OS/arch (Windows .dll, macOS .dylib,
    // Linux .so, Android _android.so).
    const QString url = QStringLiteral("https://buildbot.libretro.com/nightly/")
                        + buildbotSubpath() + coreFileName(coreName) + QStringLiteral(".zip");

    QNetworkAccessManager nam;
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVaultNative"));
    QNetworkReply* reply = nam.get(req);

    // Report progress to the caller's inline indicator (no popup). The event loop keeps the UI responsive
    // while the download runs.
    if (onProgress) onProgress(0);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64 r, qint64 t) {
        if (t > 0 && onProgress) onProgress(static_cast<int>(r * 100 / t));
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError)
    {
        const QString err = reply->errorString();
        reply->deleteLater();
        if (error) *error = QObject::tr("Couldn't download core ‘%1’: %2").arg(coreName, err);
        return QString();
    }

    const QByteArray data = reply->readAll();
    reply->deleteLater();

    QString dll;
    if (!unzipLibFromMemory(data, coresDir(), libExt(), &dll))
    {
        if (error) *error = QObject::tr("Downloaded ‘%1’ but couldn't extract the core.").arg(coreName);
        return QString();
    }
    return isInstalled(coreName) ? corePath(coreName) : dll;
}

QString CoreManager::systemDir()
{
    const QString d = AppPaths::dataDir() + QStringLiteral("/system");
    QDir().mkpath(d);
    return d;
}

void CoreManager::ensureBios(const QString& systemId, const QString& destDir,
                             const std::function<void(const QString&)>& onStatus)
{
    const QList<BiosFile>& files = BiosCatalog::forSystem(systemId);
    if (files.isEmpty())
        return; // this system needs no BIOS — nothing to do

    QDir().mkpath(destDir);
    QNetworkAccessManager nam;
    for (const BiosFile& bf : files)
    {
        const QString out = destDir + QStringLiteral("/") + bf.fileName;
        if (QFile::exists(out))
            continue; // already have it
        QDir().mkpath(QFileInfo(out).absolutePath()); // fileName may include a subfolder (e.g. hatari/tos/tos.img)

        if (onStatus) onStatus(QObject::tr("Downloading BIOS %1…").arg(bf.fileName));
        QNetworkRequest req((QUrl(bf.url)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVaultNative"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam.get(req);

        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (reply->error() == QNetworkReply::NoError)
        {
            const QByteArray data = reply->readAll();
            QFile f(out);
            if (f.open(QIODevice::WriteOnly))
            {
                f.write(data);
                f.close();
            }
        }
        // On error: leave the file missing. The core/emulator surfaces "BIOS not found" itself, exactly
        // as it would have before this feature — best-effort, never blocks the launch.
        reply->deleteLater();
    }
}

namespace {

// One async BIOS fetch: downloads the missing files for a system one at a time, chained on each reply's
// finished() signal — no nested event loop — then reports done and deletes itself. Parented to the caller's
// context object, so a caller that goes away mid-download tears the chain down and the callbacks never fire.
class BiosFetcher : public QObject
{
public:
    BiosFetcher(QList<BiosFile> files, const QString& destDir, QObject* context,
                std::function<void(const QString&)> onStatus, std::function<void()> onDone)
        : QObject(context), files_(std::move(files)), destDir_(destDir),
          onStatus_(std::move(onStatus)), onDone_(std::move(onDone))
    {
        nam_ = new QNetworkAccessManager(this);
        // A dead network must fail the file (leaving it missing, as the sync path does) rather than leave
        // the waiting launch pending forever. Generous enough for the largest catalog file (~4 MB PS2 BIOS).
        nam_->setTransferTimeout(60000);
        next();
    }

private:
    void next()
    {
        if (files_.isEmpty())
        {
            if (onDone_) onDone_();
            deleteLater();
            return;
        }
        const BiosFile bf = files_.takeFirst();
        const QString out = destDir_ + QStringLiteral("/") + bf.fileName;
        QDir().mkpath(QFileInfo(out).absolutePath()); // fileName may include a subfolder (e.g. hatari/tos/tos.img)

        if (onStatus_) onStatus_(QObject::tr("Downloading BIOS %1…").arg(bf.fileName));
        QNetworkRequest req((QUrl(bf.url)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVaultNative"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, out] {
            if (reply->error() == QNetworkReply::NoError)
            {
                const QByteArray data = reply->readAll();
                QFile f(out);
                if (f.open(QIODevice::WriteOnly))
                {
                    f.write(data);
                    f.close();
                }
            }
            // On error: leave the file missing — the core/emulator reports "BIOS not found" itself.
            reply->deleteLater();
            next();
        });
    }

    QList<BiosFile> files_;
    QString destDir_;
    std::function<void(const QString&)> onStatus_;
    std::function<void()> onDone_;
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace

void CoreManager::ensureBiosAsync(const QString& systemId, const QString& destDir, QObject* context,
                                  const std::function<void(const QString&)>& onStatus,
                                  const std::function<void()>& onDone)
{
    QList<BiosFile> missing;
    for (const BiosFile& bf : BiosCatalog::forSystem(systemId))
        if (!QFile::exists(destDir + QStringLiteral("/") + bf.fileName))
            missing.append(bf);

    // The common case — BIOS already on disk (or the system needs none) — completes synchronously, so a
    // warm launch is byte-for-byte the old flow with no queued round-trip.
    if (missing.isEmpty())
    {
        if (onDone) onDone();
        return;
    }

    QDir().mkpath(destDir);
    new BiosFetcher(missing, destDir, context, onStatus, onDone); // deletes itself when the chain settles
}

namespace {

// One async core download: fetches the buildbot zip, extracts the shared library, reports the result, and
// deletes itself. Parented to the caller's context object, so a caller that goes away mid-download aborts
// the transfer and the callbacks never fire (same lifetime model as BiosFetcher above).
class CoreFetcher : public QObject
{
public:
    CoreFetcher(const QString& coreName, const QString& url, QObject* context,
                std::function<void(int)> onProgress,
                std::function<void(const QString&, const QString&)> onDone)
        : QObject(context), coreName_(coreName),
          onProgress_(std::move(onProgress)), onDone_(std::move(onDone))
    {
        nam_ = new QNetworkAccessManager(this);
        // An inactivity timeout: a stalled buildbot connection fails the launch with a message instead of
        // hanging it forever. Bytes still flowing (however slowly) keep resetting it, so a slow link on a
        // 10-40 MB core is fine.
        nam_->setTransferTimeout(60000);

        QNetworkRequest req((QUrl(url)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVaultNative"));
        QNetworkReply* reply = nam_->get(req);
        if (onProgress_) onProgress_(0);
        connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 r, qint64 t) {
            if (t <= 0 || !onProgress_) return;
            const int pct = static_cast<int>(r * 100 / t);
            if (pct != lastPct_) { lastPct_ = pct; onProgress_(pct); } // report only on change (each update repaints a toast)
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            if (reply->error() != QNetworkReply::NoError)
            {
                finish(QString(), QObject::tr("Couldn't download core ‘%1’: %2").arg(coreName_, reply->errorString()));
            }
            else
            {
                const QByteArray data = reply->readAll();
                QString dll;
                if (!unzipLibFromMemory(data, CoreManager::coresDir(), libExt(), &dll))
                    finish(QString(), QObject::tr("Downloaded ‘%1’ but couldn't extract the core.").arg(coreName_));
                else
                    finish(CoreManager::isInstalled(coreName_) ? CoreManager::corePath(coreName_) : dll, QString());
            }
            reply->deleteLater();
        });
    }

private:
    void finish(const QString& path, const QString& error)
    {
        if (onDone_) onDone_(path, error);
        deleteLater();
    }

    QString coreName_;
    std::function<void(int)> onProgress_;
    std::function<void(const QString&, const QString&)> onDone_;
    QNetworkAccessManager* nam_ = nullptr;
    int lastPct_ = -1;
};

} // namespace

void CoreManager::ensureCoreAsync(const QString& coreName, QObject* context,
                                  const std::function<void(int)>& onProgress,
                                  const std::function<void(const QString&, const QString&)>& onDone)
{
    // The common case — core already installed — completes synchronously, so a warm launch is byte-for-byte
    // the old flow with no queued round-trip.
    if (isInstalled(coreName))
    {
        if (onDone) onDone(corePath(coreName), QString());
        return;
    }

    const QString url = QStringLiteral("https://buildbot.libretro.com/nightly/")
                        + buildbotSubpath() + coreFileName(coreName) + QStringLiteral(".zip");
    new CoreFetcher(coreName, url, context, onProgress, onDone); // deletes itself when the download settles
}
