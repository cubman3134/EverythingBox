#include "CoreManager.h"

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
    const QString d = QCoreApplication::applicationDirPath() + QStringLiteral("/cores");
    QDir().mkpath(d);
    return d;
}

QString CoreManager::corePath(const QString& coreName)
{
    return coresDir() + QStringLiteral("/") + coreName + QStringLiteral("_libretro.dll");
}

bool CoreManager::isInstalled(const QString& coreName)
{
    return QFile::exists(corePath(coreName));
}

// Extract the first *.dll from a zip (in memory) into destDir. Returns the written path via outDll.
static bool unzipDllFromMemory(const QByteArray& zipData, const QString& destDir, QString* outDll)
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
        if (name.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive))
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

QString CoreManager::ensureCore(const QString& coreName, QWidget* parent, QString* error)
{
    if (isInstalled(coreName))
        return corePath(coreName);

    // libretro nightly buildbot (Windows x64). Per-OS handling comes with the mobile/macOS/Linux ports.
    const QString url = QStringLiteral("https://buildbot.libretro.com/nightly/windows/x86_64/latest/")
                        + coreName + QStringLiteral("_libretro.dll.zip");

    QProgressDialog progress(QObject::tr("Downloading core '%1'…").arg(coreName),
                             QObject::tr("Cancel"), 0, 100, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);

    QNetworkAccessManager nam;
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVaultNative"));
    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::downloadProgress, &progress, [&](qint64 r, qint64 t) {
        if (t > 0) progress.setValue(static_cast<int>(r * 100 / t));
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError)
    {
        const QString err = reply->errorString();
        reply->deleteLater();
        if (error && err != QStringLiteral("Operation canceled")) // user-cancel is silent
            *error = QObject::tr("Couldn't download core ‘%1’: %2").arg(coreName, err);
        return QString();
    }

    const QByteArray data = reply->readAll();
    reply->deleteLater();

    QString dll;
    if (!unzipDllFromMemory(data, coresDir(), &dll))
    {
        if (error) *error = QObject::tr("Downloaded ‘%1’ but couldn't extract the core.").arg(coreName);
        return QString();
    }
    return isInstalled(coreName) ? corePath(coreName) : dll;
}
