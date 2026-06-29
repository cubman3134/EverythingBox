#include "EmulatorManager.h"

#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

static QSettings appIni()
{
    return QSettings(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"),
                     QSettings::IniFormat);
}

QString EmulatorManager::emulatorsRoot()
{
    QSettings s = appIni();
    QString d = s.value(QStringLiteral("emulators/root")).toString();
    if (d.isEmpty())
        d = QCoreApplication::applicationDirPath() + QStringLiteral("/emulators");
    QDir().mkpath(d);
    return d;
}

void EmulatorManager::setEmulatorsRoot(const QString& dir)
{
    QSettings s = appIni();
    s.setValue(QStringLiteral("emulators/root"), dir);
    s.sync();
}

bool EmulatorManager::launchFullscreen()
{
    QSettings s = appIni();
    return s.value(QStringLiteral("emulators/fullscreen"), true).toBool();
}

void EmulatorManager::setLaunchFullscreen(bool on)
{
    QSettings s = appIni();
    s.setValue(QStringLiteral("emulators/fullscreen"), on);
    s.sync();
}

QString EmulatorManager::installDir(const ExternalEmulator& em)
{
    const QString d = emulatorsRoot() + QStringLiteral("/") + em.id;
    QDir().mkpath(d);
    return d;
}

QString EmulatorManager::resolveBinary(const ExternalEmulator& em)
{
    const QString base = emulatorsRoot() + QStringLiteral("/") + em.id;
#if defined(Q_OS_WIN)
    const QStringList& cands = em.winBinaries;
#elif defined(Q_OS_MACOS)
    const QStringList& cands = em.macBinaries;
#else
    const QStringList& cands = em.linuxBinaries;
#endif
    for (const QString& c : cands)
    {
        const QString p = base + QStringLiteral("/") + c;
        if (QFileInfo::exists(p))
            return p;
    }
    // Fallback: some emulators extract into a version-named subfolder (e.g. azahar-windows-msvc-<ver>/),
    // so search recursively for the candidate binary by name, preferring the order listed.
    if (QDir(base).exists())
    {
        for (const QString& c : cands)
        {
            const QString name = QFileInfo(c).fileName();
            QDirIterator it(base, QStringList{ name }, QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext())
                return it.next();
        }
    }
    return QString();
}

EmulatorManager::EmulatorManager(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

void EmulatorManager::play(const ExternalEmulator& em, const QString& rom)
{
    if (busy_) { emit failed(tr("An emulator is already running.")); return; }
    em_ = em; rom_ = rom; launchAfterInstall_ = true; busy_ = true;
    const QString bin = resolveBinary(em);
    if (!bin.isEmpty()) { launch(bin); return; }
    startInstall();
}

void EmulatorManager::install(const ExternalEmulator& em)
{
    if (busy_) { emit failed(tr("An emulator operation is already in progress.")); return; }
    em_ = em; rom_.clear(); launchAfterInstall_ = false; busy_ = true;
    startInstall();
}

void EmulatorManager::terminateGame()
{
    if (game_) game_->kill();
}

QString EmulatorManager::platformArtifact() const
{
#if defined(Q_OS_WIN)
    return em_.winArtifact;
#elif defined(Q_OS_MACOS)
    return em_.macArtifact;
#else
    return em_.linuxArtifact;
#endif
}

void EmulatorManager::startInstall()
{
#if defined(Q_OS_WIN)
    fetchArtifactList();
#else
    // macOS (.dmg) / Linux (.flatpak/AppImage) need per-format handling we haven't built yet.
    busy_ = false;
    emit failed(tr("Auto-install isn't available on this OS yet. Download %1 from %2 and put it in %3.")
                    .arg(em_.displayName, em_.homepage, installDir(em_)));
#endif
}

void EmulatorManager::fetchArtifactList()
{
    emit status(tr("Looking up the latest %1…").arg(em_.displayName), -1);
    QNetworkRequest rq{ QUrl(em_.updateJsonUrl) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            busy_ = false;
            emit failed(tr("Couldn't reach the %1 download server: %2").arg(em_.displayName, reply->errorString()));
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QString want = platformArtifact();
        QString url;
        if (root.contains(QStringLiteral("artifacts")))
        {
            // Dolphin-style update JSON: exact "system" match.
            for (const QJsonValue& v : root.value(QStringLiteral("artifacts")).toArray())
            {
                const QJsonObject o = v.toObject();
                if (o.value(QStringLiteral("system")).toString() == want)
                {
                    url = o.value(QStringLiteral("url")).toString();
                    break;
                }
            }
        }
        else if (root.contains(QStringLiteral("assets")))
        {
            // GitHub releases API: pick the archive asset whose name contains the platform substring
            // (e.g. "windows-msvc"); skip installers/other formats by requiring a .zip/.7z.
            for (const QJsonValue& v : root.value(QStringLiteral("assets")).toArray())
            {
                const QJsonObject o = v.toObject();
                const QString name = o.value(QStringLiteral("name")).toString();
                if (name.contains(want, Qt::CaseInsensitive)
                    && (name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)
                        || name.endsWith(QStringLiteral(".7z"), Qt::CaseInsensitive)))
                {
                    url = o.value(QStringLiteral("browser_download_url")).toString();
                    break;
                }
            }
        }
        if (url.isEmpty())
        {
            busy_ = false;
            emit failed(tr("No %1 download was listed for this platform.").arg(em_.displayName));
            return;
        }
        downloadArchive(url);
    });
}

void EmulatorManager::downloadArchive(const QString& url)
{
    QString suffix = QStringLiteral(".7z");
    const QString path = QUrl(url).path();
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0) suffix = path.mid(dot);
    archivePath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                       .filePath(em_.id + QStringLiteral("-download") + suffix);

    QFile* out = new QFile(archivePath_);
    if (!out->open(QIODevice::WriteOnly))
    {
        delete out; busy_ = false;
        emit failed(tr("Couldn't write the download to disk."));
        return;
    }
    QNetworkRequest rq{ QUrl(url) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::readyRead, this, [reply, out] { out->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 r, qint64 t) {
        emit status(tr("Downloading %1…").arg(em_.displayName), t > 0 ? int(r * 100 / t) : -1);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, out] {
        out->write(reply->readAll()); out->close(); delete out;
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString es = reply->errorString();
        reply->deleteLater();
        if (!ok)
        {
            QFile::remove(archivePath_); busy_ = false;
            emit failed(tr("Download failed: %1").arg(es));
            return;
        }
        extractArchive();
    });
}

void EmulatorManager::extractArchive()
{
    emit status(tr("Extracting %1…").arg(em_.displayName), -1);
    // Windows ships bsdtar (libarchive) which reads .7z and .zip; the GNU tar that may be first on PATH
    // does not, so invoke the System32 copy by full path.
    const QString tar = QStringLiteral("C:/Windows/System32/tar.exe");
    QProcess* p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, p](int code, QProcess::ExitStatus) {
        p->deleteLater();
        QFile::remove(archivePath_);
        const QString bin = resolveBinary(em_);
        if (code != 0 || bin.isEmpty())
        {
            busy_ = false;
            emit failed(tr("Couldn't extract %1. You can install it manually from %2 into %3.")
                            .arg(em_.displayName, em_.homepage, installDir(em_)));
            return;
        }
        emit installed(em_.displayName);
        if (launchAfterInstall_) launch(bin);
        else busy_ = false;
    });
    p->start(tar, { QStringLiteral("-xf"), QDir::toNativeSeparators(archivePath_),
                    QStringLiteral("-C"), QDir::toNativeSeparators(installDir(em_)) });
    if (!p->waitForStarted(5000))
    {
        p->deleteLater(); QFile::remove(archivePath_); busy_ = false;
        emit failed(tr("Couldn't run the archive extractor."));
    }
}

void EmulatorManager::launch(const QString& binary)
{
    QString tmpl = em_.argsTemplate;
    tmpl.replace(QStringLiteral("{fs}"), launchFullscreen() ? em_.fullscreenArgs : em_.windowedArgs);

    QStringList args;
    const QStringList parts = tmpl.split(QLatin1Char(' '), Qt::SkipEmptyParts); // empties (e.g. blank {fs}) dropped
    for (QString a : parts)
        args << (a.contains(QStringLiteral("{rom}")) ? a.replace(QStringLiteral("{rom}"), rom_) : a);

    game_ = new QProcess(this);
    game_->setWorkingDirectory(QFileInfo(binary).absolutePath());
    connect(game_, &QProcess::started, this, [this] { emit launched(em_.displayName); });
    connect(game_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        busy_ = false;
        if (game_) { game_->deleteLater(); game_ = nullptr; }
        emit finished(code);
    });
    connect(game_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
        {
            busy_ = false;
            if (game_) { game_->deleteLater(); game_ = nullptr; }
            emit failed(tr("Couldn't start %1.").arg(em_.displayName));
        }
    });
    game_->start(binary, args);
}
