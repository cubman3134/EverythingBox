#include "AppUpdater.h"
#include "ArchiveRom.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QProcess>
#include <QUrl>
#include <QDesktopServices>

namespace {
constexpr const char* kReleasesApi =
    "https://api.github.com/repos/cubman3134/MyMediaVault/releases/latest";
constexpr const char* kReleasesPage =
    "https://github.com/cubman3134/MyMediaVault/releases/latest";

// Compare dotted versions numerically ("0.10.0" > "0.9.0"), ignoring any "-suffix". >0 => a is newer.
int versionCompare(const QString& a, const QString& b)
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

// Which release asset is this platform's build?
bool isThisPlatformAsset(const QString& name)
{
    const QString n = name.toLower();
#if defined(Q_OS_WIN)
    return n.contains(QStringLiteral("windows")) && n.endsWith(QStringLiteral(".zip"));
#elif defined(Q_OS_MACOS)
    return n.contains(QStringLiteral("macos")) && n.endsWith(QStringLiteral(".dmg"));
#else
    return n.contains(QStringLiteral("linux")) && n.endsWith(QStringLiteral(".appimage"));
#endif
}
} // namespace

AppUpdater::AppUpdater(QObject* parent) : QObject(parent) {}

QString AppUpdater::currentVersion() { return QCoreApplication::applicationVersion(); }

void AppUpdater::checkForUpdate()
{
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    QNetworkRequest rq{ QUrl(QString::fromLatin1(kReleasesApi)) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setRawHeader("Accept", "application/vnd.github+json");
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(20000);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            emit checkFailed(reply->errorString());
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        QString tag = o.value(QStringLiteral("tag_name")).toString();      // e.g. "v0.3.0"
        if (tag.startsWith(QLatin1Char('v'))) tag.remove(0, 1);
        if (tag.isEmpty()) { emit checkFailed(QStringLiteral("no release tag")); return; }

        // Find this platform's downloadable asset.
        pendingUrl_.clear(); pendingAsset_.clear();
        for (const QJsonValue& av : o.value(QStringLiteral("assets")).toArray())
        {
            const QJsonObject a = av.toObject();
            const QString name = a.value(QStringLiteral("name")).toString();
            if (isThisPlatformAsset(name))
            {
                pendingAsset_ = name;
                pendingUrl_ = a.value(QStringLiteral("browser_download_url")).toString();
                break;
            }
        }
        latestVersion_ = tag;
        latestNotes_ = o.value(QStringLiteral("body")).toString();

        if (versionCompare(tag, currentVersion()) <= 0)
        {
            pendingUrl_.clear(); pendingAsset_.clear();
            emit upToDate();
            return;
        }
        emit updateAvailable(tag, latestNotes_);
    });
}

void AppUpdater::downloadAndApply()
{
    if (pendingUrl_.isEmpty()) { emit applyFailed(QStringLiteral("no update is pending")); return; }
    if (!nam_) nam_ = new QNetworkAccessManager(this);

    const QString dir = QDir::tempPath() + QStringLiteral("/mmv-app-update");
    QDir().mkpath(dir);
    const QString archivePath = dir + QStringLiteral("/") + pendingAsset_;

    emit progress(tr("Downloading My Media Vault %1…").arg(latestVersion_), -1);
    QNetworkRequest rq{ QUrl(pendingUrl_) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(120000);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0)
            emit progress(tr("Downloading update… %1%").arg(int(got * 100 / total)), int(got * 100 / total));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, archivePath] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            emit applyFailed(tr("Download failed: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.isEmpty()) { emit applyFailed(tr("The update download was empty.")); return; }
        QFile f(archivePath);
        if (!f.open(QIODevice::WriteOnly)) { emit applyFailed(tr("Couldn't save the update.")); return; }
        f.write(body); f.close();

        QString err;
        if (!applyArchive(archivePath, &err))
            emit applyFailed(err);
        // On success applyArchive quits the app; the helper relaunches it.
    });
}

// Extract the release archive and hand off to a helper that swaps the files once we've exited, then quit.
bool AppUpdater::applyArchive(const QString& archivePath, QString* error)
{
#if defined(Q_OS_WIN)
    const QString root = QFileInfo(archivePath).absolutePath();
    const QString staging = root + QStringLiteral("/staging");
    QDir(staging).removeRecursively();
    QString aerr;
    if (!ArchiveRom::extractAll(archivePath, staging, &aerr))
    {
        if (error) *error = tr("Couldn't unpack the update: %1").arg(aerr);
        return false;
    }
    // The zip lays the dist out at its root; if a tool nested it in a single folder, descend into that.
    QString src = staging;
    {
        const QFileInfoList entries = QDir(staging).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
        if (entries.size() == 1 && entries.first().isDir()
            && !QFileInfo::exists(staging + QStringLiteral("/MyMediaVault.exe")))
            src = entries.first().absoluteFilePath();
    }
    if (!QFileInfo::exists(src + QStringLiteral("/MyMediaVault.exe")))
    {
        if (error) *error = tr("The update package didn't contain the app.");
        return false;
    }

    const QString installDir = QCoreApplication::applicationDirPath();
    const qint64 pid = QCoreApplication::applicationPid();
    const QString cmdPath = root + QStringLiteral("/apply.cmd");
    // robocopy /IS /IT overwrite even same-size files; /R:30 /W:1 retries any file still briefly locked as we exit.
    const QString script =
        QStringLiteral(
            "@echo off\r\n"
            ":wait\r\n"
            "tasklist /FI \"PID eq %1\" 2>NUL | find \"%1\" >NUL && ( timeout /t 1 /nobreak >NUL & goto wait )\r\n"
            "robocopy \"%2\" \"%3\" /E /IS /IT /R:30 /W:1 /NFL /NDL /NJH /NJS >NUL\r\n"
            "rmdir /S /Q \"%4\" 2>NUL\r\n"
            "start \"\" \"%3\\MyMediaVault.exe\"\r\n")
        .arg(QString::number(pid),
             QDir::toNativeSeparators(src),
             QDir::toNativeSeparators(installDir),
             QDir::toNativeSeparators(staging));
    QFile cf(cmdPath);
    if (!cf.open(QIODevice::WriteOnly | QIODevice::Text)) { if (error) *error = tr("Couldn't write the updater."); return false; }
    cf.write(script.toLocal8Bit());
    cf.close();

    emit progress(tr("Installing update and restarting…"), -1);
    // Detached + minimized so it survives our exit; it waits for our PID, swaps files, and relaunches.
    if (!QProcess::startDetached(QStringLiteral("cmd"),
            { QStringLiteral("/c"), QStringLiteral("start"), QString(), QStringLiteral("/min"),
              QStringLiteral("cmd"), QStringLiteral("/c"), QDir::toNativeSeparators(cmdPath) }))
    {
        if (error) *error = tr("Couldn't start the updater.");
        return false;
    }
    QCoreApplication::quit(); // let the helper take over
    return true;
#else
    // macOS (.dmg) / Linux (.AppImage) aren't swapped in place here — open the release page so the user grabs it.
    Q_UNUSED(archivePath);
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kReleasesPage)));
    if (error) *error = tr("Opened the releases page to finish updating.");
    return false;
#endif
}
