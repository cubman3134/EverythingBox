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

QString CoreManager::ensureCore(const QString& coreName, QWidget* parent, QString* error,
                                const std::function<void(int)>& onProgress)
{
    Q_UNUSED(parent);
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
