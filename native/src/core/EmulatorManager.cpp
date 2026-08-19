#include "EmulatorManager.h"
#include "AppBrand.h"
#include "AppPaths.h"

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
#include <cctype>
#include <QProcess>
#include <QDeadlineTimer>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QEventLoop>
#include <QThread>
#include <QDeadlineTimer>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include "CoreManager.h"
#include "BiosCatalog.h"
#include "LaunchOptionsStore.h"   // appendExtraArgs — the per-game extra-args lever (issue #51)
#include "ControllerSeats.h"      // pure multi-seat controller model + per-emulator player-N INI mapping (issue #104)
#include "Settings.h"             // ps3AutoUpdate() — gates ONLY the pre-boot PS3 *game* update; the firmware install is ungated
#include "core/ps3/Ps3UpdateCoordinator.h" // orchestrates check→install for a PS3 game before RPCS3 boots
#include "core/ps3/Ps3TitleId.h"           // read a PS3 game's Title ID from the rom path (folder or .pkg)
#include "core/ps3/Ps3Firmware.h"          // auto-installs Sony's PS3UPDAT.PUP into RPCS3's dev_flash pre-boot
#include "core/ps3/Ps3InstalledVersion.h"  // the installed game-update version on disk — the result an --installpkg run is waited on for

#ifdef EVERYTHINGBOX_HAVE_SDL
#define SDL_MAIN_HANDLED          // never let SDL take over main()
#include <SDL.h>
#endif

#ifdef Q_OS_IOS
// iOS: standalone external emulators are impossible here — there is no QProcess, and iOS can't launch
// downloaded executables. The class keeps its API so callers link unchanged; every operation reports
// unavailability (mirroring how the feature is gated off the Android build).
EmulatorManager::EmulatorManager(QObject* parent) : QObject(parent) {}
QString EmulatorManager::emulatorsRoot() { return QDir(AppPaths::dataDir()).filePath(QStringLiteral("emulators")); }
void EmulatorManager::setEmulatorsRoot(const QString&) {}
QString EmulatorManager::installDir(const ExternalEmulator& em) { return QDir(emulatorsRoot()).filePath(em.id); }
QString EmulatorManager::resolveBinary(const ExternalEmulator&) { return QString(); }
bool EmulatorManager::launchFullscreen() { return true; }
void EmulatorManager::setLaunchFullscreen(bool) {}
void EmulatorManager::play(const ExternalEmulator&, const QString&, const QString&, const EmuGfx::Settings&)
{ emit failed(tr("Standalone emulators aren't available on iOS.")); }
void EmulatorManager::install(const ExternalEmulator&)
{ emit failed(tr("Standalone emulators aren't available on iOS.")); }
void EmulatorManager::terminateGame() {}
void EmulatorManager::closeGame() {}
#else

// Some download hosts (e.g. richwhitehouse.com / BigPEmu) block non-browser requests via Mod_Security, so
// present a normal browser User-Agent. GitHub and Dolphin accept it too (they just require a non-empty UA).
static const QString kBrowserUA = QStringLiteral(
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

// True if a release asset / download file name is the one we want for this OS: contains the platform marker,
// has an archive extension, and isn't a core / debug / unsigned / dev build. Shared by the GitHub-asset and
// HTML-scrape paths.
static bool assetMatches(const QString& name, const QString& want)
{
    if (want.isEmpty()) return false;
    const QString n = name.toLower();
    for (const char* s : { "libretro", "symbols", "dbg", "pdb", "unsigned", "dev" })
        if (n.contains(QLatin1String(s))) return false;
    if (!n.contains(want.toLower())) return false;
    for (const char* e : { ".zip", ".7z", ".appimage", ".dmg", ".tar.gz", ".tgz", ".tar.xz", ".txz" })
        if (n.endsWith(QLatin1String(e))) return true;
    return false;
}

static QSettings appIni()
{
    return QSettings(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                     QSettings::IniFormat);
}

QString EmulatorManager::emulatorsRoot()
{
    QSettings s = appIni();
    QString d = s.value(QStringLiteral("emulators/root")).toString();
    if (d.isEmpty())
        d = AppPaths::dataDir() + QStringLiteral("/emulators");
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
    // First: an absolute find-rule (a user pointing at an install they already have) is used verbatim; a
    // relative one is resolved under "emulators/<id>/". Shared with the probe via the header oracle.
    const QString direct = EmulatorRegistry::resolveBinaryFrom(cands, base);
    if (!direct.isEmpty())
        return direct;
    // Fallback: some emulators extract into a version-named subfolder (e.g. azahar-windows-msvc-<ver>/),
    // so search recursively for the candidate binary by name, preferring the order listed. (Absolute
    // candidates were already resolved above, so this only concerns the relative built-in find-rules.)
    if (QDir(base).exists())
    {
        for (const QString& c : cands)
        {
            if (QDir::isAbsolutePath(c)) continue;
            const QString name = QFileInfo(c).fileName();
            QDirIterator it(base, QStringList{ name }, QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext())
                return it.next();
        }
    }
#if defined(Q_OS_LINUX)
    // A Flatpak install has no file under our folder - check the Flatpak DB and return a launch sentinel.
    if (!em.flatpakAppId.isEmpty())
    {
        for (const QStringList& argv : { QStringList{ QStringLiteral("info"), QStringLiteral("--user"), em.flatpakAppId },
                                         QStringList{ QStringLiteral("info"), em.flatpakAppId } })
        {
            QProcess q; q.start(QStringLiteral("flatpak"), argv); q.waitForFinished(8000);
            if (q.exitStatus() == QProcess::NormalExit && q.exitCode() == 0)
                return QStringLiteral("flatpak-run:") + em.flatpakAppId;
        }
    }
#endif
    return QString();
}

EmulatorManager::EmulatorManager(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

void EmulatorManager::play(const ExternalEmulator& em, const QString& rom, const QString& extraArgs,
                           const EmuGfx::Settings& gfx)
{
    if (busy_) { emit failed(tr("An emulator is already running.")); return; }
    em_ = em; rom_ = rom; extraArgs_ = extraArgs; gfx_ = gfx; launchAfterInstall_ = true; busy_ = true;
    const QString bin = resolveBinary(em);
    // A user-defined emulator (no update source) can't be auto-downloaded — it points at a binary the user
    // already has. If we couldn't resolve it, say so plainly instead of trying (and failing) to install.
    // Checked before taking ownership of the launch context below: a launch that dies right here must not
    // cancel async work a previous launch may still have pending.
    if (bin.isEmpty() && !EmulatorRegistry::hasInstallSource(em))
    {
        busy_ = false;
        emit failed(tr("Couldn't find %1's program. Check the \"binary\" path in its emulators/*.json entry.")
                        .arg(em.displayName));
        return;
    }
    // This launch now owns the manager: retire the previous launch's context, cancelling any async work it
    // still had pending (BIOS/keys chains, the RPCS3 update worker's boot continuation) before it can act
    // on a stale launch.
    delete launchCtx_;
    launchCtx_ = new QObject(this);
    if (!bin.isEmpty()) { launch(bin); return; }
    startInstall();
}

void EmulatorManager::install(const ExternalEmulator& em)
{
    if (busy_) { emit failed(tr("An emulator operation is already in progress.")); return; }
    // Auto-install is a built-in-table privilege: a user-defined emulator has no update source, so there is
    // nothing to download. Never enter the download machinery for it.
    if (!EmulatorRegistry::hasInstallSource(em))
    {
        emit failed(tr("%1 is a user-defined emulator — point it at a program you already have; "
                       "there is nothing to download.").arg(em.displayName));
        return;
    }
    em_ = em; rom_.clear(); extraArgs_.clear(); launchAfterInstall_ = false; busy_ = true;
    // An install-only run rewrites the emulator's files on disk, so it retires the launch context the same
    // way a new launch does: pending async work from an earlier launch must not fire mid-reinstall.
    delete launchCtx_;
    launchCtx_ = new QObject(this);
    startInstall();
}

void EmulatorManager::terminateGame()
{
    if (game_) game_->kill();
}

void EmulatorManager::closeGame()
{
    if (!game_) return;
    // Ask the emulator to close (posts WM_CLOSE on Windows) so it saves SRAM/state and quits cleanly, the way
    // RetroBat's exit hotkey does. If it ignores the request, force it after a short grace period. The finished
    // handler clears game_, so the fallback is a no-op once it has actually exited.
    game_->terminate();
    QTimer::singleShot(3000, this, [this] {
        if (game_ && game_->state() != QProcess::NotRunning) game_->kill();
    });
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

QString EmulatorManager::platformUpdateUrl() const
{
#if defined(Q_OS_WIN)
    if (!em_.winUpdateUrl.isEmpty()) return em_.winUpdateUrl;
#elif defined(Q_OS_MACOS)
    if (!em_.macUpdateUrl.isEmpty()) return em_.macUpdateUrl;
#else
    if (!em_.linuxUpdateUrl.isEmpty()) return em_.linuxUpdateUrl;
#endif
    return em_.updateJsonUrl; // shared source (most emulators)
}

void EmulatorManager::startInstall()
{
    fetchArtifactList(); // resolves the per-OS artifact URL, then downloadArchive() -> installDownloaded()
}

void EmulatorManager::fetchArtifactList()
{
    if (platformArtifact().isEmpty()) // no build published for this OS (e.g. Xenia has no macOS build)
    {
        busy_ = false;
        emit failed(tr("%1 has no build for this operating system. You can get it from %2.")
                        .arg(em_.displayName, em_.homepage));
        return;
    }
    emit status(tr("Looking up the latest %1…").arg(em_.displayName), -1);
    QNetworkRequest rq{ QUrl(platformUpdateUrl()) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, kBrowserUA);
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
        const QByteArray body = reply->readAll();
        const QJsonObject root = QJsonDocument::fromJson(body).object();
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
            // GitHub releases API: the asset whose name carries the platform marker (e.g. "windows-msvc").
            for (const QJsonValue& v : root.value(QStringLiteral("assets")).toArray())
            {
                const QJsonObject o = v.toObject();
                if (assetMatches(o.value(QStringLiteral("name")).toString(), want))
                {
                    url = o.value(QStringLiteral("browser_download_url")).toString();
                    break;
                }
            }
        }
        else
        {
            // No JSON API (e.g. BigPEmu): scrape the download page's HTML for a matching build URL.
            const QString html = QString::fromUtf8(body);
            QRegularExpression re(QStringLiteral("https?://[^\\s\"'<>]+"));
            auto it = re.globalMatch(html);
            while (it.hasNext())
            {
                const QString cand = it.next().captured(0);
                if (assetMatches(cand, want)) { url = cand; break; }
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, kBrowserUA);
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
        installDownloaded();
    });
}

// Install the downloaded artifact according to its format (which varies per OS).
void EmulatorManager::installDownloaded()
{
    const QString low = archivePath_.toLower();
    if (low.endsWith(QStringLiteral(".dmg")))            installDmg();       // macOS
    else if (low.endsWith(QStringLiteral(".flatpak")))   installFlatpak();   // Linux
    else if (low.endsWith(QStringLiteral(".appimage")))  installAppImage();  // Linux
    else                                                 extractArchive();   // .zip / .7z
}

void EmulatorManager::extractArchive()
{
    emit status(tr("Extracting %1…").arg(em_.displayName), -1);
    const QString a = QDir::toNativeSeparators(archivePath_);
    const QString dir = QDir::toNativeSeparators(installDir(em_));
    const bool isZip = archivePath_.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive);

    // bsdtar (libarchive) reads .zip and .7z. Windows ships it at System32\tar.exe and macOS at /usr/bin/tar;
    // Linux's GNU tar can't read .zip/.7z, so fall back to bsdtar/unzip/7z there. Try each until one works.
    QList<QPair<QString, QStringList>> cmds;
#if defined(Q_OS_WIN)
    cmds.append({ QStringLiteral("C:/Windows/System32/tar.exe"), { QStringLiteral("-xf"), a, QStringLiteral("-C"), dir } });
#elif defined(Q_OS_MACOS)
    cmds.append({ QStringLiteral("tar"), { QStringLiteral("-xf"), a, QStringLiteral("-C"), dir } }); // .zip (and .7z if libarchive has lzma)
    if (!isZip) { // .7z (e.g. RPCS3 mac) - macOS has no bundled 7z, so fall back to p7zip if installed
        cmds.append({ QStringLiteral("7z"),  { QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-o") + dir, a } });
        cmds.append({ QStringLiteral("7za"), { QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-o") + dir, a } });
    }
#else
    cmds.append({ QStringLiteral("bsdtar"), { QStringLiteral("-xf"), a, QStringLiteral("-C"), dir } });
    if (isZip) cmds.append({ QStringLiteral("unzip"), { QStringLiteral("-o"), a, QStringLiteral("-d"), dir } });
    else       cmds.append({ QStringLiteral("7z"), { QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-o") + dir, a } });
    cmds.append({ QStringLiteral("tar"), { QStringLiteral("-xf"), a, QStringLiteral("-C"), dir } }); // .tar.* last resort
#endif
    tryExtract(cmds, 0);
}

// Run extractor candidates in order; the first that starts and exits 0 wins. (Lets Linux fall back from
// bsdtar to unzip/7z without us having to probe which tools are installed.)
void EmulatorManager::tryExtract(const QList<QPair<QString, QStringList>>& cmds, int index)
{
    if (index >= cmds.size())
    {
        QFile::remove(archivePath_); archivePath_.clear(); busy_ = false;
        emit failed(tr("Couldn't extract %1. You can install it manually from %2 into %3.")
                        .arg(em_.displayName, em_.homepage, installDir(em_)));
        return;
    }
    QProcess* p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, p, cmds, index](int code, QProcess::ExitStatus st) {
        if (st == QProcess::NormalExit && code == 0)
        {
            p->deleteLater();
            QFile::remove(archivePath_); archivePath_.clear();
            finishInstall();
        }
        else { p->deleteLater(); tryExtract(cmds, index + 1); }   // this tool failed; try the next
    });
    connect(p, &QProcess::errorOccurred, this, [this, p, cmds, index](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { p->deleteLater(); tryExtract(cmds, index + 1); } // tool not installed
    });
    p->start(cmds[index].first, cmds[index].second);
}

// macOS: mount the .dmg, copy the .app bundle into the install dir, detach.
void EmulatorManager::installDmg()
{
    emit status(tr("Installing %1…").arg(em_.displayName), -1);
    QProcess* att = new QProcess(this);
    connect(att, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, att](int code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(att->readAllStandardOutput());
        att->deleteLater();
        QString vol; // the mount point, e.g. /Volumes/Dolphin (last column of the output)
        for (const QString& line : out.split(QLatin1Char('\n')))
        {
            const int i = line.indexOf(QStringLiteral("/Volumes/"));
            if (i >= 0) vol = line.mid(i).trimmed();
        }
        auto fail = [this](const QString& v) { if (!v.isEmpty()) { QProcess d; d.start(QStringLiteral("hdiutil"), { QStringLiteral("detach"), v, QStringLiteral("-force") }); d.waitForFinished(20000); }
                                               QFile::remove(archivePath_); busy_ = false;
                                               emit failed(tr("Couldn't install %1 from the disk image.").arg(em_.displayName)); };
        if (code != 0 || vol.isEmpty()) { fail(vol); return; }
        QString app;
        for (const QFileInfo& fi : QDir(vol).entryInfoList(QStringList{ QStringLiteral("*.app") }, QDir::Dirs))
        { app = fi.absoluteFilePath(); break; }
        bool ok = false;
        if (!app.isEmpty())
        {
            QProcess cp; cp.start(QStringLiteral("cp"), { QStringLiteral("-R"), app, installDir(em_) + QLatin1Char('/') });
            cp.waitForFinished(180000);
            ok = cp.exitStatus() == QProcess::NormalExit && cp.exitCode() == 0;
        }
        QProcess det; det.start(QStringLiteral("hdiutil"), { QStringLiteral("detach"), vol, QStringLiteral("-force") });
        det.waitForFinished(20000);
        QFile::remove(archivePath_); archivePath_.clear();
        if (!ok) { busy_ = false; emit failed(tr("Couldn't copy %1 out of the disk image.").arg(em_.displayName)); return; }
        finishInstall();
    });
    att->start(QStringLiteral("hdiutil"), { QStringLiteral("attach"), QStringLiteral("-nobrowse"),
                                            QStringLiteral("-noverify"), archivePath_ });
    if (!att->waitForStarted(8000))
    { att->deleteLater(); QFile::remove(archivePath_); busy_ = false; emit failed(tr("Couldn't mount the disk image.")); }
}

// Linux: an AppImage is the runnable program itself - move it into place and mark it executable.
void EmulatorManager::installAppImage()
{
    emit status(tr("Installing %1…").arg(em_.displayName), -1);
    const QString dest = installDir(em_) + QStringLiteral("/") + em_.id + QStringLiteral(".AppImage");
    QFile::remove(dest);
    if (!QFile::rename(archivePath_, dest))
    {
        if (!QFile::copy(archivePath_, dest)) { QFile::remove(archivePath_); busy_ = false; emit failed(tr("Couldn't install %1.").arg(em_.displayName)); return; }
        QFile::remove(archivePath_);
    }
    QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                | QFileDevice::ReadOther | QFileDevice::ExeOther);
    archivePath_.clear();
    finishInstall();
}

// Linux: install the Flatpak per-user; it's then launched via "flatpak run <appId>" (see resolveBinary/launch).
void EmulatorManager::installFlatpak()
{
    emit status(tr("Installing %1 (Flatpak)…").arg(em_.displayName), -1);
    QProcess* p = new QProcess(this);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, p](int code, QProcess::ExitStatus) {
        p->deleteLater();
        QFile::remove(archivePath_); archivePath_.clear();
        if (code != 0) { busy_ = false; emit failed(tr("Flatpak install failed for %1.").arg(em_.displayName)); return; }
        finishInstall();
    });
    p->start(QStringLiteral("flatpak"), { QStringLiteral("install"), QStringLiteral("--user"),
                                          QStringLiteral("-y"), QStringLiteral("--noninteractive"), archivePath_ });
    if (!p->waitForStarted(8000))
    { p->deleteLater(); QFile::remove(archivePath_); busy_ = false;
      emit failed(tr("Flatpak isn't available. Install it, or get %1 from %2.").arg(em_.displayName, em_.homepage)); }
}

void EmulatorManager::finishInstall()
{
    const QString bin = resolveBinary(em_);
    if (bin.isEmpty())
    {
        busy_ = false;
        emit failed(tr("Installed %1 but couldn't locate its program in %2.").arg(em_.displayName, installDir(em_)));
        return;
    }
    emit installed(em_.displayName);
    if (launchAfterInstall_) launch(bin);
    else busy_ = false;
}

// The config half of prepareBios, run after the BIOS files have settled: point PCSX2's ini at the fetched
// image so -batch boots without its first-run wizard. The BIOS filename now comes from whatever the server
// actually dropped into <dir>/bios (the catalog owns the names), so this stays agnostic of any hardcoded list.
static void wireBiosConfig(bool portable, const QString& binDir)
{
    if (!portable) return;
    const QString biosDir = binDir + QStringLiteral("/bios");
    const QStringList found = QDir(biosDir).entryList(QDir::Files, QDir::Name);
    if (found.isEmpty()) return; // nothing landed — let PCSX2 surface its own "no BIOS" instead of a bad ini

    const QString inis = binDir + QStringLiteral("/inis");
    QDir().mkpath(inis);
    const QString cfg = inis + QStringLiteral("/PCSX2.ini");
    if (!QFile::exists(cfg))
    {
        QFile f(cfg);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream ts(&f);
            // SettingsVersion is mandatory: without it PCSX2's settings-version check fails at startup with
            // "settings failed to load" and it never boots. SetupWizardIncomplete=false skips the first-run wizard.
            ts << "[UI]\n" << "SettingsVersion = 1\n" << "SetupWizardIncomplete = false\n\n"
               << "[Filenames]\n" << "BIOS = " << found.first() << "\n";
            f.close();
        }
    }
}

// Put a BIOS where a standalone emulator expects it before we boot a game. Which emulators need one (and
// the system whose BIOS to fetch) comes from BiosCatalog, so the emulator registry stays untouched. For
// PCSX2: a portable.ini marker beside the exe makes it keep config + bios under our folder; the BIOS image
// goes in "<dir>/bios"; and a best-effort PCSX2.ini pre-selects that BIOS and skips the first-run wizard so
// -batch boots cleanly. Everything is best-effort and idempotent — present files are left untouched, and
// config is only written when absent so we never clobber the user's own settings.
// The fetch is asynchronous (no nested event loop): onDone runs once the files land — immediately when
// nothing needs downloading — with progress on the status signal (which feeds the launch wait page). The
// chain is parented to launchCtx_, so a torn-down launch cancels it and onDone never runs.
void EmulatorManager::prepareBios(const QString& binDir, const std::function<void()>& onDone)
{
    const BiosCatalog::ExternalBios b = BiosCatalog::forExternalEmulator(em_.id);
    if (b.systemId.isEmpty())
    {
        onDone(); // this emulator ships everything it needs
        return;
    }

    if (b.portable)
    {
        const QString marker = binDir + QStringLiteral("/portable.ini");
        if (!QFile::exists(marker)) { QFile m(marker); if (m.open(QIODevice::WriteOnly)) m.close(); }
    }

    CoreManager::ensureBiosAsync(b.systemId, binDir + QStringLiteral("/bios"), launchCtx_,
                                 [this](const QString& s) { emit status(s, -1); },
                                 [b, binDir, onDone] { wireBiosConfig(b.portable, binDir); onDone(); });
}

// True if keys.txt at `path` actually contains keys (a 32-hex-char line) rather than being absent or just the
// blank comment-only placeholder Cemu writes when it starts without keys.
static bool cemuKeysPresent(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (QByteArray line : lines)
    {
        line = line.trimmed();
        if (line.size() < 32) continue;
        bool hex = true;
        for (int i = 0; i < 32; ++i) if (!std::isxdigit(static_cast<unsigned char>(line[i]))) { hex = false; break; }
        if (hex) return true;
    }
    return false;
}

// Write `content` to `path` only if nothing is there yet, creating parent dirs. Never clobbers a real config
// the user (or a prior run) already wrote — first-run seeding must be a no-op on an already-configured install.
static void seedFileIfAbsent(const QString& path, const QByteArray& content)
{
    if (QFileInfo::exists(path)) return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) { f.write(content); f.close(); }
}

// Append `section` to the ini at `path` if `marker` isn't already in the file. Used to add a controller block
// to an ini another step already wrote (PCSX2.ini, DuckStation settings.ini, Dolphin.ini) without clobbering it.
static void appendIniSectionIfAbsent(const QString& path, const QByteArray& marker, const QByteArray& section)
{
    QByteArray existing;
    { QFile r(path); if (r.open(QIODevice::ReadOnly)) { existing = r.readAll(); r.close(); } }
    if (existing.contains(marker)) return; // already has this block (user's own or a prior seed)
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        if (!existing.isEmpty() && !existing.endsWith('\n')) f.write("\n");
        f.write(section);
        f.close();
    }
}

// Upsert "key = value" inside [section] of an INI: replace the value if the key is present, else add it (creating
// the section if needed). Unlike appendIniSectionIfAbsent this UPDATES an existing key — needed for the RA token,
// which can change on re-login and must stay in sync with EB.
static void setIniKey(const QString& path, const QString& section, const QString& key, const QString& value)
{
    QStringList lines;
    { QFile r(path); if (r.open(QIODevice::ReadOnly | QIODevice::Text)) { lines = QString::fromUtf8(r.readAll()).split(QLatin1Char('\n')); r.close(); } }

    const QString header = QStringLiteral("[%1]").arg(section);
    const QString newLine = QStringLiteral("%1 = %2").arg(key, value);
    int secStart = -1;
    for (int i = 0; i < lines.size(); ++i) if (lines[i].trimmed() == header) { secStart = i; break; }

    if (secStart < 0) // no such section — append it at the end
    {
        if (!lines.isEmpty() && !lines.last().trimmed().isEmpty()) lines << QString();
        lines << header << newLine;
    }
    else
    {
        int keyIdx = -1, secEnd = lines.size();
        for (int i = secStart + 1; i < lines.size(); ++i)
        {
            const QString t = lines[i].trimmed();
            if (t.startsWith(QLatin1Char('['))) { secEnd = i; break; }          // next section starts
            if (t.section(QLatin1Char('='), 0, 0).trimmed().compare(key, Qt::CaseInsensitive) == 0) { keyIdx = i; break; }
        }
        if (keyIdx >= 0) lines[keyIdx] = newLine;      // replace the value
        else             lines.insert(secEnd, newLine); // add at the end of the section
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write(lines.join(QLatin1Char('\n')).toUtf8()); f.close(); }
}

// Feed EB's RetroAchievements login into a standalone emulator's own RA client, so it unlocks natively against
// the same account (a standalone emulator is a separate process — EB can't run rcheevos against its memory the
// way it does for in-process cores). Runs every launch to keep the token fresh; does nothing (leaves the config
// untouched) when EB isn't signed into RA.
//
// Only emulators that accept a RAW rcheevos token in their config qualify. VERIFIED LIVE:
//   • PCSX2 ([Achievements] Username/Token) — logs in successfully with EB's token.
//   • DuckStation ([Cheevos]) — does NOT work: it ENCRYPTS its stored token with a machine key and rejects a raw
//     one ("Invalid encrypted login token"). We only hold the token (not the password), so there's no way to feed
//     it; writing one would just nag a failed login every launch. DuckStation manages its own RA login instead.
// Other RA-capable emulators (RPCS3/Dolphin/Flycast/PPSSPP) can be added once verified the same way — check the
// emulator's log shows "logged in successfully", not an encrypt/decrypt error, with a real token.
void EmulatorManager::prepareAchievements(const QString& binDir)
{
    QSettings ini = appIni();
    const QString user = ini.value(QStringLiteral("ra/user")).toString();
    const QString token = ini.value(QStringLiteral("ra/token")).toString();
    if (user.isEmpty() || token.isEmpty()) return; // not signed into RetroAchievements in EB

    QString path, section;
    if (em_.id == QStringLiteral("pcsx2")) { path = binDir + QStringLiteral("/inis/PCSX2.ini"); section = QStringLiteral("Achievements"); }
    else return;

    setIniKey(path, section, QStringLiteral("Enabled"), QStringLiteral("true"));
    setIniKey(path, section, QStringLiteral("Username"), user);
    setIniKey(path, section, QStringLiteral("Token"), token); // credential — never logged
}

// Write the resolved graphics quartet (issue #103) into the emulator's own config before it boots — the
// RetroBat promise that "users should not have to open the emulator to configure it", for internal resolution /
// aspect / vsync / renderer / MSAA. The resolution comes pre-resolved (per-game override already layered over
// the per-system default by the caller): an all-unset gfx yields NO edits, so a game with no override never
// touches the emulator's config and a hand-tuned install is left exactly as the user left it.
//
// MERGE, NEVER CLOBBER. Each edit is a key-level upsert via setIniKey: it replaces only that key inside its
// section and preserves every other key the user (or an earlier prep step) wrote — the same discipline the RA
// token write uses. Before the first time we ever edit a given config file, we snapshot it once to
// "<file>.eb-orig" so "EverythingBox changed my emulator settings" is always reversible (the issue's backup
// discipline). Which keys map where is the pure EmuGfx::configEdits table; this side only applies them.
void EmulatorManager::prepareGraphicsSettings(const QString& binDir)
{
    if (gfx_.isEmpty()) return;  // no override for this launch -> the emulator keeps its own graphics config

    const QVector<EmuGfx::ConfigEdit> edits = EmuGfx::configEdits(em_.id, gfx_);
    if (edits.isEmpty()) return; // this emulator supports none of the set levers -> nothing to write

    QSet<QString> backedUp;      // one snapshot per file, before its first edit this launch
    for (const EmuGfx::ConfigEdit& e : edits)
    {
        const QString path = binDir + QLatin1Char('/') + e.file;
        if (!backedUp.contains(e.file))
        {
            backedUp.insert(e.file);
            const QString orig = path + QStringLiteral(".eb-orig");
            if (QFileInfo::exists(path) && !QFileInfo::exists(orig))
                QFile::copy(path, orig); // best-effort, one-time: the pre-EB config, always reversible
        }
        setIniKey(path, e.section, e.key, e.value); // key-level merge: preserves every other key in the file
        qInfo("EmulatorManager: gfx write %s [%s] %s = %s",
              qUtf8Printable(e.file), qUtf8Printable(e.section), qUtf8Printable(e.key), qUtf8Printable(e.value));
    }
}

// The live pads to seat, in connection order (game controllers only, matching the in-process tier's port
// assignment in Gamepad::openControllers). Enumerated fresh at launch via SDL — who is plugged in changes per
// session — carrying each pad's connection index, joystick GUID (reserved for future GUID pinning) and name.
// Empty when SDL isn't compiled in or no controller is attached; the caller then falls back to seeding P1 only,
// exactly as before this issue. This is the one non-headlessly-testable seam: it needs live SDL + real pads.
static QVector<ControllerSeats::PadInfo> enumerateConnectedPads()
{
    QVector<ControllerSeats::PadInfo> pads;
#ifdef EVERYTHINGBOX_HAVE_SDL
    const bool alreadyInit = SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0;
    if (!alreadyInit)
    {
        SDL_SetMainReady();
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return pads;
        if (char* base = SDL_GetBasePath())
        {
            const std::string db = std::string(base) + "gamecontrollerdb.txt";
            SDL_free(base);
            SDL_GameControllerAddMappingsFromFile(db.c_str()); // match the in-process tier's mappings; -1 if absent
        }
    }
    int seatIdx = 0;
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n && seatIdx < ControllerSeats::kMaxSeats; ++i)
    {
        if (!SDL_IsGameController(i)) continue; // only real game controllers take a seat (skips HID-keyboard phantoms)
        ControllerSeats::PadInfo p;
        p.index = seatIdx++;
        char guid[33] = {0};
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        p.guid = QString::fromLatin1(guid);
        const char* nm = SDL_GameControllerNameForIndex(i);
        p.name = nm ? QString::fromUtf8(nm) : QString();
        pads.push_back(p);
    }
    if (!alreadyInit) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER); // leave SDL exactly as we found it
#endif
    return pads;
}

// Migrate EB's OWN prior Dolphin controller seed (the pre-SDL XInput block) up to the new SDL profile, so an
// already-deployed install starts feeding a DualSense / any non-Xbox pad. Append-if-absent alone can't do this:
// the [GCPad{n+1}] marker is already present, so the seeder skips it and the stale XInput device string survives.
// For each seat we're about to seed, if GCPadNew.ini's section is BYTE-IDENTICAL to EB's prior XInput seed we
// replace it with the SDL body; a section that differs by any byte (a user's hand-edited mapping, or one Dolphin
// itself rewrote) is left untouched. The file is only rewritten when a section actually changed.
static void migrateDolphinGcPadIni(const QString& path, const QVector<ControllerSeats::Seat>& seats)
{
    QByteArray contents;
    { QFile r(path); if (!r.open(QIODevice::ReadOnly)) return; contents = r.readAll(); r.close(); }
    QByteArray updated = contents;
    for (const ControllerSeats::Seat& seat : seats)
    {
        if (seat.index < 0 || seat.index >= ControllerSeats::kMaxSeats) continue;
        updated = ControllerSeats::replaceDolphinGcPadSectionIfEbSeed(
            updated, seat.index,
            ControllerSeats::dolphinGcPadBodyLegacyXInput(seat.index),
            ControllerSeats::dolphinGcPadBody(seat.index, seat.pad.name));
    }
    if (updated == contents) return; // no EB-seed section present -> leave the file exactly as found
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) { f.write(updated); f.close(); }
}

// Auto-map the players' controllers inside each standalone emulator so a game boots with working input — the
// thing RetroBat/ES-DE do that makes a pad "just work". Without this, most standalone emulators launch with no
// binding and the user has to open each emulator's input menu and hand-map every button.
//
// MULTI-SEAT (issue #104): we enumerate the live pads, assign them seats 0..3 (ControllerSeats::assignSeats),
// and seed each player's block — so four pads on the couch get players 1-4 in Dolphin/PCSX2/DuckStation/Cemu
// without opening any input dialog. Every block is written ONLY WHEN ABSENT (a user's own mapping is never
// overwritten). Which player maps to which (file, section, device) is the pure ControllerSeats::controllerEdits
// table; this side only applies each edit with the existing merge idioms (append-if-marker-absent, or a
// whole-file seed for Cemu's per-controller XML). With no pad enumerated (SDL absent / none attached) we fall
// back to a single seat 0, reproducing the pre-#104 P1-only write byte-for-byte. On Windows the device strings
// key on the XInput slot index, so no per-controller GUID is needed; GUID pinning is deferred (see #104).
void EmulatorManager::prepareControllerConfig(const QString& binDir)
{
    const QString& id = em_.id;

    // Only these four emulators are auto-seated; others (melonDS below) keep their own handling. Cemu and Dolphin
    // use Windows XInput device strings, so they are seated only on Windows (as before).
    bool multiSeat = (id == QStringLiteral("pcsx2") || id == QStringLiteral("duckstation"));
#ifdef Q_OS_WIN
    if (id == QStringLiteral("cemu") || id == QStringLiteral("dolphin")) multiSeat = true;
#endif
    if (multiSeat)
    {
        QVector<ControllerSeats::Seat> seats =
            ControllerSeats::assignSeats(enumerateConnectedPads());
        if (seats.isEmpty()) seats.push_back(ControllerSeats::Seat{ 0, {} }); // no pad -> seed P1, as before

        // Update EB's own prior XInput seed to the SDL profile on an already-deployed install (append-if-absent
        // can't rewrite a section that is already present). Only touches sections byte-identical to EB's old seed.
        if (id == QStringLiteral("dolphin"))
            migrateDolphinGcPadIni(binDir + QStringLiteral("/User/Config/GCPadNew.ini"), seats);

        const QString appdata = (id == QStringLiteral("cemu")) ? qEnvironmentVariable("APPDATA") : QString();
        for (const ControllerSeats::Seat& seat : seats)
            for (const ControllerSeats::ConfigEdit& e : ControllerSeats::controllerEdits(id, seat.index, seat.pad))
            {
                if (e.marker.isEmpty())
                {
                    seedFileIfAbsent(binDir + QLatin1Char('/') + e.file, e.body);          // whole-file seed (Cemu XML)
                    if (!appdata.isEmpty())                                                 // Cemu also reads %APPDATA%\Cemu
                        seedFileIfAbsent(appdata + QStringLiteral("/Cemu/") + e.file, e.body);
                }
                else
                    appendIniSectionIfAbsent(binDir + QLatin1Char('/') + e.file, e.marker.toUtf8(), e.body);
            }
        return;
    }

    // ---- melonDS: ships with every input unmapped (-1), so a fresh install plays nothing until you configure
    // it by hand. Seed a working keyboard + XInput controller map (RetroBat-style). Keyboard values are Qt::Key
    // codes; joystick values are SDL joystick button indices, with the D-pad as hat 0 (0x100|dir). Only applied
    // when still unmapped, so a user's own mapping is never clobbered. ----
    if (id == QStringLiteral("melonds"))
    {
        struct M { const char* k; int kb; int joy; };
        static const M kMap[] = {
            { "A", 88, 1 }, { "B", 90, 0 }, { "Select", 16777219, 6 }, { "Start", 16777220, 7 },
            { "Right", 16777236, 258 }, { "Left", 16777234, 264 }, { "Up", 16777235, 257 }, { "Down", 16777237, 260 },
            { "R", 87, 5 }, { "L", 81, 4 }, { "X", 83, 3 }, { "Y", 65, 2 },
        };
        const QString tomlPath = binDir + QStringLiteral("/melonDS.toml");
        QFile f(tomlPath);
        if (f.exists())
        {
            if (!f.open(QIODevice::ReadOnly)) return;
            QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            f.close();
            // Patch only if the Keyboard section's A is still unmapped (i.e. melonDS's fresh default).
            QString sec; bool unmapped = false;
            for (const QString& l : lines)
            {
                const QString s = l.trimmed();
                if (s.startsWith(QLatin1Char('['))) sec = s;
                else if (sec == QLatin1String("[Instance0.Keyboard]") && s.startsWith(QLatin1String("A "))
                         && s.endsWith(QLatin1String("-1"))) unmapped = true;
            }
            if (!unmapped) return; // already mapped (by the user or a prior seed) -> leave it
            sec.clear();
            for (QString& l : lines)
            {
                const QString s = l.trimmed();
                if (s.startsWith(QLatin1Char('['))) { sec = s; continue; }
                const int eq = s.indexOf(QLatin1Char('='));
                if (eq < 0) continue;
                const QString key = s.left(eq).trimmed();
                for (const M& m : kMap)
                    if (key == QLatin1String(m.k))
                    {
                        if (sec == QLatin1String("[Instance0.Keyboard]")) l = QStringLiteral("%1 = %2").arg(key).arg(m.kb);
                        else if (sec == QLatin1String("[Instance0.Joystick]")) l = QStringLiteral("%1 = %2").arg(key).arg(m.joy);
                    }
            }
            if (f.open(QIODevice::WriteOnly)) { f.write(lines.join(QLatin1Char('\n')).toUtf8()); f.close(); }
        }
        else
        {
            // Brand-new install (melonDS hasn't run yet): write a minimal toml with just the input sections;
            // melonDS merges it and fills everything else with its own defaults.
            QString t = QStringLiteral("[Instance0]\nJoystickID = 0\n\n[Instance0.Keyboard]\n");
            for (const M& m : kMap) t += QStringLiteral("%1 = %2\n").arg(QLatin1String(m.k)).arg(m.kb);
            t += QStringLiteral("\n[Instance0.Joystick]\n");
            for (const M& m : kMap) t += QStringLiteral("%1 = %2\n").arg(QLatin1String(m.k)).arg(m.joy);
            seedFileIfAbsent(tomlPath, t.toUtf8());
        }
        return;
    }
}

// Several standalone emulators block a fresh install with a first-run wizard / consent dialog / welcome screen
// before you can boot a game. Frontends (RetroBat / ES-DE / Batocera) skip these by pre-seeding a minimal config
// with the "setup already done" flag set — the same trick prepareBios uses for PCSX2 and prepareCemuConfig for
// Cemu. Do it for the rest here: seed each emulator's config (only when absent, so existing setups are untouched)
// so a brand-new install boots straight into the game. Config keys are minimal — every emulator fills the rest
// with its own defaults. Emulators with no blocking first-run prompt (PPSSPP, melonDS, Flycast, Azahar, BigPEmu,
// Ryujinx) get nothing; firmware/BIOS that some still need to actually run games is a genuine one-time user
// requirement, not a skippable prompt, and is out of scope here.
void EmulatorManager::prepareFirstRunConfig(const QString& binDir)
{
    const QString& id = em_.id;

    if (id == QStringLiteral("duckstation"))
    {
        // Multi-step Setup Wizard (language/BIOS/controllers/game-dirs). portable.txt keeps config next to the
        // exe; SetupWizardIncomplete=false is the exact key that suppresses the wizard.
        seedFileIfAbsent(binDir + QStringLiteral("/portable.txt"), QByteArray());
        seedFileIfAbsent(binDir + QStringLiteral("/settings.ini"),
            "[Main]\nSetupWizardIncomplete = false\nStartFullscreen = true\nConfirmPowerOff = false\n"
            "PauseOnFocusLoss = false\n");
    }
    else if (id == QStringLiteral("dolphin"))
    {
        // "Allow Usage Statistics Reporting?" consent popup. PermissionAsked=True suppresses it; Enabled=False
        // opts out of actually sending anything. portable.txt puts config under ./User/Config next to the exe.
        seedFileIfAbsent(binDir + QStringLiteral("/portable.txt"), QByteArray());
        seedFileIfAbsent(binDir + QStringLiteral("/User/Config/Dolphin.ini"),
            "[Analytics]\nEnabled = False\nPermissionAsked = True\n\n[Interface]\nConfirmStop = False\n\n"
            "[Display]\nFullscreen = True\n");
    }
    else if (id == QStringLiteral("rpcs3"))
    {
        // "Welcome to RPCS3" modal (Exit closes the app). RPCS3 is portable on Windows (config next to the exe).
        // (PS3 firmware is auto-installed just before launch — see Ps3Firmware::maybeInstall in the launch path.)
        seedFileIfAbsent(binDir + QStringLiteral("/GuiConfigs/CurrentSettings.ini"),
            "[main_window]\ninfoBoxEnabledWelcome=false\nconfirmationBoxExitGame=false\n\n"
            "[Meta]\ncheckUpdateStart=false\n");
    }
    else if (id == QStringLiteral("vita3k"))
    {
        // Vita3K REJECTS a partial config.yml — it validates the whole schema and, on any missing key, discards
        // the file and regenerates its own with the welcome/firmware prompts back ON. So seeding a few keys never
        // worked. Instead, patch those keys in the (complete) config.yml Vita3K writes itself: it's absent on the
        // very first launch (the welcome shows once — fine, since Vita3K needs a one-time firmware/setup step
        // anyway), then suppressed on every launch after. Version-robust: we edit whatever schema is present.
        const QString cfg = binDir + QStringLiteral("/config.yml");
        QFile f(cfg);
        if (QFile::exists(cfg) && f.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            f.close();
            struct KV { const char* key; const char* val; };
            static const KV kWant[] = { { "show-welcome", "false" }, { "warn-missing-firmware", "false" }, { "initial-setup", "true" } };
            for (QString& line : lines)
                for (const KV& kv : kWant)
                    if (line.startsWith(QLatin1String(kv.key) + QLatin1Char(':')))
                        line = QStringLiteral("%1: %2").arg(QLatin1String(kv.key), QLatin1String(kv.val));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write(lines.join(QLatin1Char('\n')).toUtf8()); f.close(); }
        }
    }
    else if (id == QStringLiteral("xemu"))
    {
        // "First Boot — configure machine settings" welcome panel. An xemu.toml next to the exe makes xemu
        // portable and read it. (Xbox BIOS/MCPX/HDD are still required to boot — a separate one-time user step.)
        seedFileIfAbsent(binDir + QStringLiteral("/xemu.toml"), "[general]\nshow_welcome = false\n");
    }
#ifdef Q_OS_WIN
    else if (id == QStringLiteral("xenia"))
    {
        // Xenia's one-time disclaimer is a native Win32 MessageBox gated on a REGISTRY flag (HKCU\Software\Xenia
        // XEFLAGS, a REG_QWORD; bit 0 = "disclaimer acknowledged"), NOT its .toml — so writing config can't skip
        // it. Pre-set the flag with reg.exe (QSettings can't reliably emit REG_QWORD). Only if not already set.
        QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\Xenia"), QSettings::NativeFormat);
        if (!reg.contains(QStringLiteral("XEFLAGS")))
        {
            QProcess::execute(QStringLiteral("reg"), {
                QStringLiteral("add"), QStringLiteral("HKCU\\SOFTWARE\\Xenia"),
                QStringLiteral("/v"), QStringLiteral("XEFLAGS"),
                QStringLiteral("/t"), QStringLiteral("REG_QWORD"),
                QStringLiteral("/d"), QStringLiteral("1"), QStringLiteral("/f") });
        }
    }
#endif
}

// Cemu shows a "Getting Started" wizard (game-path/graphics-pack prompts) on its very first launch. It decides
// "first launch" solely by whether settings.xml exists (CemuApp.cpp: isFirstStart = !exists(settings.xml)), so
// pre-seeding a minimal settings.xml makes Cemu skip the wizard and boot straight into the game — the RetroBat/
// ES-DE "no per-emulator setup" model (mirrors what prepareBios does for PCSX2's setup wizard). Existence alone
// is what matters; Cemu fills every other value with its default on load and rewrites the full file on exit.
// Never clobbers an existing config.
void EmulatorManager::prepareCemuConfig(const QString& binDir)
{
    if (em_.id != QStringLiteral("cemu")) return;

    QStringList dirs;
    dirs << binDir; // in case a future Cemu build runs portable (settings next to the exe)
#ifdef Q_OS_WIN
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) dirs << appdata + QStringLiteral("/Cemu"); // where non-portable Cemu 2.x reads it
#else
    dirs << QDir::homePath() + QStringLiteral("/.config/Cemu");
#endif

    for (const QString& d : dirs)
    {
        const QString cfg = d + QStringLiteral("/settings.xml");
        if (QFile::exists(cfg)) continue; // respect the user's own config — only seed when absent
        QDir().mkpath(d);
        QFile f(cfg);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            // fullscreen matches our -f launch; everything else defaults.
            f.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<content>\n\t<fullscreen>true</fullscreen>\n</content>\n");
            f.close();
        }
    }
}

// Cemu can't decrypt Wii U titles without keys.txt (the console's title/common keys). Fetch it on demand into
// the folder(s) Cemu reads — next to the exe (portable) AND its per-user data dir (%APPDATA%\Cemu on Windows,
// where a non-portable Cemu 2.x looks) — the same best-effort model as prepareBios. Kept out of the app repo
// (copyrighted keys); pulled from a maintained gist when Cemu is set up. Skips paths that already have real
// keys, and overwrites Cemu's blank placeholder.
// Asynchronous like prepareBios: onDone runs once keys.txt has settled — immediately for non-Cemu emulators
// or when real keys are already everywhere Cemu looks. The fetch's manager is parented to launchCtx_, so a
// torn-down launch aborts it and onDone never runs. Must complete before prepareCemuDiscKey (a fetched
// keys.txt overwrites its targets, which would drop an already-appended disc key).
void EmulatorManager::prepareCemuKeys(const QString& binDir, const std::function<void()>& onDone)
{
    if (em_.id != QStringLiteral("cemu")) { onDone(); return; }

    QStringList targets;
    targets << binDir + QStringLiteral("/keys.txt");
#ifdef Q_OS_WIN
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) targets << appdata + QStringLiteral("/Cemu/keys.txt");
#else
    targets << QDir::homePath() + QStringLiteral("/.config/Cemu/keys.txt");
#endif

    QStringList todo;
    for (const QString& t : targets) if (!cemuKeysPresent(t)) todo << t;
    if (todo.isEmpty()) { onDone(); return; } // real keys already in place wherever Cemu looks

    emit status(tr("Fetching Cemu keys…"), -1);
    QNetworkRequest rq((QUrl(QStringLiteral(
        "https://gist.githubusercontent.com/xXPhenomXx/093b352723ec51644453a9528a8dc87e/raw"))));
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(20000);
    auto* nam = new QNetworkAccessManager(launchCtx_); // dies with the launch context, aborting the transfer
    QNetworkReply* reply = nam->get(rq);
    connect(reply, &QNetworkReply::finished, launchCtx_, [nam, reply, todo, onDone] {
        if (reply->error() == QNetworkReply::NoError)
        {
            const QByteArray body = reply->readAll();
            if (!body.isEmpty())
                for (const QString& t : todo)
                {
                    QDir().mkpath(QFileInfo(t).absolutePath());
                    QFile f(t); if (f.open(QIODevice::WriteOnly)) { f.write(body); f.close(); }
                }
        }
        // On failure, leave it: Cemu will prompt for keys itself, exactly as before.
        reply->deleteLater();
        nam->deleteLater();
        onDone();
    });
}

// A Wii U retail disc image (.wux/.wud) is encrypted with a unique per-disc title key. Scene/No-Intro archives
// ship that key as a 16-byte <game>.key beside the image, which ArchiveRom extracts next to the ROM (same base
// name, .key extension). Cemu decrypts a disc by brute-forcing every key in keys.txt against the disc header,
// so the disc key has to live in keys.txt. (Cemu also supports a <image>.key sidecar, but that fallback was
// added after the 2.6 build we ship — it's ignored there — so keys.txt is the portable route.) Append the disc
// key's hex to every keys.txt Cemu reads, de-duplicated so repeated launches don't pile up. Best-effort.
void EmulatorManager::prepareCemuDiscKey(const QString& binDir)
{
    if (em_.id != QStringLiteral("cemu")) return;
    const QString ext = QFileInfo(rom_).suffix().toLower();
    if (ext != QStringLiteral("wux") && ext != QStringLiteral("wud")) return;

    // The companion key sits next to the image with the extension swapped to .key ("Game.wux" -> "Game.key").
    const QString keyPath = QFileInfo(rom_).absolutePath() + QLatin1Char('/')
                            + QFileInfo(rom_).completeBaseName() + QStringLiteral(".key");
    QFile kf(keyPath);
    if (!kf.open(QIODevice::ReadOnly)) return; // no companion key shipped with this ROM — nothing to add
    const QByteArray raw = kf.readAll();
    kf.close();
    if (raw.size() != 16) return;          // a disc title key is exactly 16 bytes; anything else isn't one
    const QByteArray hex = raw.toHex();    // lowercase 32-char hex, the keys.txt line format

    QStringList targets;
    targets << binDir + QStringLiteral("/keys.txt");
#ifdef Q_OS_WIN
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) targets << appdata + QStringLiteral("/Cemu/keys.txt");
#else
    targets << QDir::homePath() + QStringLiteral("/.config/Cemu/keys.txt");
#endif

    for (const QString& t : targets)
    {
        QByteArray content;
        QFile f(t);
        if (f.open(QIODevice::ReadOnly)) { content = f.readAll(); f.close(); }
        if (content.toLower().contains(hex)) continue; // already listed — don't duplicate
        QDir().mkpath(QFileInfo(t).absolutePath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Append))
        {
            if (!content.isEmpty() && !content.endsWith('\n')) f.write("\n");
            f.write(hex);
            f.write("\n");
            f.close();
        }
    }
}

// ---- Save-data backup / centralization for standalone emulators -------------------------------------------
// Each standalone emulator writes its saves to its own scattered folder (Cemu's mlc, PCSX2/DuckStation memory
// cards, Dolphin's User/GC & Wii NAND, ...). We snapshot those into one app-owned tree, <app>/saves/emulators/
// <id>/, on every game exit — which centralizes them AND gets them cloud-synced for free (CloudSync already zips
// <app>/saves recursively). On launch we seed an emulator that has no saves yet from that central copy, so a
// fresh install / a new device picks up your progress. We never overwrite an emulator's existing saves (no
// clobber), so this is safe; it's a backup + fresh-device restore, not a two-way merge.

namespace {
bool dirHasFiles(const QString& dir)
{
    if (!QFileInfo::exists(dir)) return false;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext();
}
void copyTree(const QString& src, const QString& dst)
{
    QDir().mkpath(dst);
    QDirIterator it(src, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString from = it.next();
        const QString to = dst + QLatin1Char('/') + QDir(src).relativeFilePath(from);
        QDir().mkpath(QFileInfo(to).absolutePath());
        QFile::remove(to);      // overwrite an older copy
        QFile::copy(from, to);
    }
}
} // namespace

QList<QPair<QString, QString>> EmulatorManager::emulatorSaveDirs(const QString& id, const QString& binDir)
{
    QList<QPair<QString, QString>> out;
    auto add = [&](const QString& dir, const QString& label) { out.append({ dir, label }); };
#ifdef Q_OS_WIN
    const QString appdata = qEnvironmentVariable("APPDATA");
#endif
    if (id == QStringLiteral("cemu"))
    {
#ifdef Q_OS_WIN
        if (!appdata.isEmpty()) add(appdata + QStringLiteral("/Cemu/mlc01/usr/save"), QStringLiteral("cemu"));
#endif
        add(binDir + QStringLiteral("/mlc01/usr/save"), QStringLiteral("cemu")); // portable fallback
    }
    else if (id == QStringLiteral("dolphin"))
    {
        add(binDir + QStringLiteral("/User/GC"),         QStringLiteral("dolphin/GC"));   // GameCube memory cards
        add(binDir + QStringLiteral("/User/Wii/title"),  QStringLiteral("dolphin/Wii"));  // Wii NAND saves
    }
    else if (id == QStringLiteral("pcsx2"))       add(binDir + QStringLiteral("/memcards"), QStringLiteral("pcsx2"));
    else if (id == QStringLiteral("duckstation")) add(binDir + QStringLiteral("/memcards"), QStringLiteral("duckstation"));
    else if (id == QStringLiteral("rpcs3"))       add(binDir + QStringLiteral("/dev_hdd0/home"), QStringLiteral("rpcs3"));
    else if (id == QStringLiteral("ppsspp"))      add(binDir + QStringLiteral("/memstick/PSP/SAVEDATA"), QStringLiteral("ppsspp"));
    else if (id == QStringLiteral("vita3k"))      add(binDir + QStringLiteral("/ux0/user/00/savedata"), QStringLiteral("vita3k"));
    else if (id == QStringLiteral("flycast"))     add(binDir + QStringLiteral("/data"), QStringLiteral("flycast"));
    else if (id == QStringLiteral("xenia"))       add(binDir + QStringLiteral("/content"), QStringLiteral("xenia"));
    else if (id == QStringLiteral("ryujinx"))
    {
#ifdef Q_OS_WIN
        if (!appdata.isEmpty()) add(appdata + QStringLiteral("/Ryujinx/bis/user/save"), QStringLiteral("ryujinx"));
#endif
        add(binDir + QStringLiteral("/portable/bis/user/save"), QStringLiteral("ryujinx"));
    }
    return out;
}

void EmulatorManager::backupSaves(const QString& binDir)
{
    const QString central = AppPaths::dataDir() + QStringLiteral("/saves/emulators/") + em_.id;
    for (const auto& sd : emulatorSaveDirs(em_.id, binDir))
        if (dirHasFiles(sd.first))
            copyTree(sd.first, central + QLatin1Char('/') + sd.second);
}

void EmulatorManager::restoreSaves(const QString& binDir)
{
    const QString central = AppPaths::dataDir() + QStringLiteral("/saves/emulators/") + em_.id;
    for (const auto& sd : emulatorSaveDirs(em_.id, binDir))
    {
        const QString backup = central + QLatin1Char('/') + sd.second;
        if (dirHasFiles(backup) && !dirHasFiles(sd.first)) // only seed an emulator that has no saves of its own
            copyTree(backup, sd.first);
    }
}

void EmulatorManager::launch(const QString& binary)
{
    QString tmpl = em_.argsTemplate;
    tmpl.replace(QStringLiteral("{fs}"), launchFullscreen() ? em_.fullscreenArgs : em_.windowedArgs);
    // Per-game extra args (issue #51) appended AFTER the resolved template — its own whole tokens, past the
    // positional {rom}. A blank extra (the overwhelmingly common case) leaves tmpl byte-for-byte unchanged.
    tmpl = LaunchOpts::appendExtraArgs(tmpl, extraArgs_);

    QStringList args;
    // Use the platform's native separators for the ROM path: PCSX2 rejects a forward-slash path on Windows
    // ("filename does not exist") even though most emulators accept it. No-op on Linux/macOS where / is native.
    const QString romNative = QDir::toNativeSeparators(rom_);
    const QStringList parts = tmpl.split(QLatin1Char(' '), Qt::SkipEmptyParts); // empties (e.g. blank {fs}) dropped
    for (QString a : parts)
    {
        if (a.contains(QStringLiteral("{rom}"))) a.replace(QStringLiteral("{rom}"), romNative);
        if (!a.isEmpty()) args << a; // drop a blank {rom} (a no-game launch, e.g. opening an emulator's own UI)
    }

    // A Flatpak "binary" is the sentinel "flatpak-run:<appId>": run via `flatpak run <appId> <emu args>`.
    QString program = binary;
    const QString fpPrefix = QStringLiteral("flatpak-run:");
    const bool isFlatpak = binary.startsWith(fpPrefix);
    if (isFlatpak)
    {
        program = QStringLiteral("flatpak");
        args = QStringList{ QStringLiteral("run"), binary.mid(fpPrefix.size()) } + args;
    }
#if !defined(Q_OS_WIN)
    else
    {
        // Ensure the extracted binary / AppImage is executable (zip extraction may not preserve the bit).
        const QFileInfo fi(binary);
        if (fi.exists())
            QFile::setPermissions(binary, fi.permissions() | QFileDevice::ExeOwner
                                          | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    }
#endif

    const QString binDir = QFileInfo(binary).absolutePath();
    if (isFlatpak)
    {
        startGameProcess(program, args, binDir, true);
        return;
    }

    // RPCS3 only: before booting, make sure the PS3 console firmware is installed (auto-installing Sony's
    // official PS3UPDAT.PUP if dev_flash is missing) and install the game's official Sony update PKG chain
    // so the player runs the patched game with no manual step. Both run on a worker thread (real network +
    // process I/O), then the normal local-launch continuation runs on the UI thread. Every failure inside
    // falls through to a plain boot — this path never prevents a launch. `program` is the RPCS3 binary here
    // (the Flatpak sentinel returned above), so it doubles as the `--installfw` / `--installpkg` executable.
    if (em_.id == QStringLiteral("rpcs3"))
    {
        runPs3UpdateThenLaunch(program, args, binDir);
        return;
    }

    finishLocalLaunch(program, args, binDir);
}

// The on-disk prep + process start for a locally-installed emulator. Extracted from launch() so both the normal
// path and the RPCS3 post-update path reach the exact same launch logic without duplicating it.
//
// Emulators that can't boot without a copyrighted BIOS (PCSX2) / decryption keys (Cemu): make sure they're in
// place next to the binary before launching. Best-effort and only on local installs we control on disk. The
// BIOS fetch is asynchronous, so the GUI thread never waits on the network: the rest of the pre-launch prep and
// the process start run as its continuation — the launch still happens only once the BIOS has settled. The
// chain is parented to the per-launch context object (created when play()/install() took ownership of the
// manager), so a torn-down manager (or a launch superseded before its download finished) cancels it and the
// process never starts.
void EmulatorManager::finishLocalLaunch(const QString& program, const QStringList& args, const QString& binDir)
{
    prepareBios(binDir, [this, program, args, binDir] {
        prepareFirstRunConfig(binDir);
        prepareCemuConfig(binDir);
        prepareControllerConfig(binDir); // after the above wrote the base inis to append to
        prepareAchievements(binDir);     // sync EB's RetroAchievements login into the emulator
        prepareGraphicsSettings(binDir); // write the resolved graphics quartet (issue #103) into its config
        prepareCemuKeys(binDir, [this, program, args, binDir] { // async too (gist fetch, Cemu only)
            prepareCemuDiscKey(binDir); // appends to the keys.txt the fetch may have just (over)written
            restoreSaves(binDir); // seed saves from the central backup if this install has none
            startGameProcess(program, args, binDir, false);
        });
    });
}

namespace {

// Synchronous HTTPS GET of a small Sony text feed with peer verification DISABLED (these endpoints'
// certificate CNs do not match their hosts, so the default handshake would fail). Runs on the calling
// worker thread via a local event loop, keeping the UI thread off the network. NoError -> body; any
// transport/HTTP error -> nullopt.
std::optional<QByteArray> fetchSonyTextFeed(const QString& url)
{
    QNetworkAccessManager nam;
    QNetworkRequest req{ QUrl(url) };
    req.setHeader(QNetworkRequest::UserAgentHeader, kBrowserUA);
    req.setTransferTimeout(15000); // stall detection: no byte moves for 15s -> the reply errors out
    QSslConfiguration ssl = req.sslConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(ssl);
    QNetworkReply* reply = nam.get(req);
    reply->ignoreSslErrors(); // the CN mismatch would otherwise surface as an SSL error and abort the reply
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // Hard watchdog: even if the transfer-timeout somehow doesn't fire (e.g. a connection that
    // never reaches the transfer stage), abort at the deadline. abort() emits finished -> quits the
    // loop, and the aborted reply reports an error so we fall through to nullopt. The timer lives on
    // this worker thread, whose local QEventLoop drives it. Both feeds are tiny, so 20s is generous.
    QTimer::singleShot(20000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    // App-quit teardown: the RPCS3 update worker is interruption-requested on aboutToQuit. This wait
    // runs on that worker thread, so poll the flag and abort the reply — finished fires, the loop
    // quits, and the aborted reply reads as an error -> nullopt, exactly like any failed fetch.
    QTimer interruptPoll;
    interruptPoll.setInterval(500);
    QObject::connect(&interruptPoll, &QTimer::timeout, reply, [reply] {
        if (QThread::currentThread()->isInterruptionRequested() && reply->isRunning()) reply->abort();
    });
    interruptPoll.start();
    loop.exec();
    std::optional<QByteArray> out;
    if (reply->error() == QNetworkReply::NoError) out = reply->readAll();
    reply->deleteLater();
    return out;
}

// The per-title update feed (empty body is Sony's "no updates" signal, handled by the coordinator).
std::optional<QByteArray> fetchPs3VerXml(const QString& titleId)
{
    return fetchSonyTextFeed(
        QStringLiteral("https://a0.ww.np.dl.playstation.net/tpl/np/%1/%1-ver.xml").arg(titleId));
}

// The console-firmware update list: one ;-separated record per line, the CDN= field carrying the
// PS3UPDAT.PUP url. Same endpoint family and trust model as ver.xml. This feed offers no hash — RPCS3
// validates the PUP internally on --installfw, so a corrupt download fails the install and the boot
// falls through to RPCS3's own missing-firmware error.
std::optional<QByteArray> fetchPs3UpdateList()
{
    return fetchSonyTextFeed(
        QStringLiteral("https://fus01.ps3.update.playstation.net/update/ps3/list/us/ps3-updatelist.txt"));
}

// Streamed plain-HTTP GET of a package url to destPath (packages run into hundreds of MB, so stream to disk
// rather than buffer in RAM). true only on HTTP NoError; a failed transfer removes the partial file.
bool downloadPs3Pkg(const QString& url, const QString& destPath)
{
    QFile f(destPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QNetworkAccessManager nam;
    QNetworkRequest req{ QUrl(url) };
    req.setHeader(QNetworkRequest::UserAgentHeader, kBrowserUA);
    // Stall detection only: 30s of *inactivity* aborts, but a legitimately large download that keeps
    // making progress is never cut off (transferTimeout resets on each received chunk).
    req.setTransferTimeout(30000);
    QNetworkReply* reply = nam.get(req);
    // Hard byte ceiling: this feed is plain-HTTP with peer verification disabled, so a MITM/broken
    // mirror could otherwise stream unlimited data to disk (the SHA-1 gate only catches it after the
    // disk is full). 12 GB is well above any real PS3 update. Exceeding it aborts and fails.
    static constexpr qint64 kMaxBytes = 12LL * 1024 * 1024 * 1024;
    qint64 written = 0;
    bool overflow = false;
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&] {
        const QByteArray chunk = reply->readAll();
        written += chunk.size();
        f.write(chunk);
        if (written > kMaxBytes && reply->isRunning()) { overflow = true; reply->abort(); }
    });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // Last-resort cap on the whole transfer (15 min) in case something wedges the loop while bytes
    // keep trickling below the stall threshold. abort() emits finished -> quits -> error -> fail.
    QTimer::singleShot(900000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    // App-quit teardown: see fetchSonyTextFeed — an aborted reply reads as a failed download, and the
    // partial file is removed below.
    QTimer interruptPoll;
    interruptPoll.setInterval(500);
    QObject::connect(&interruptPoll, &QTimer::timeout, reply, [reply] {
        if (QThread::currentThread()->isInterruptionRequested() && reply->isRunning()) reply->abort();
    });
    interruptPoll.start();
    loop.exec();
    const bool ok = (!overflow && reply->error() == QNetworkReply::NoError);
    if (ok) f.write(reply->readAll());
    f.close();
    reply->deleteLater();
    if (!ok) QFile::remove(destPath);
    return ok;
}

} // namespace

// Run the PS3 pre-boot pipeline (console-firmware auto-install, then the game's update-PKG chain) for the
// RPCS3 rom on a worker thread, then finish the normal launch on the UI thread. Informational only: every
// internal failure falls through — the game always boots (worst case into RPCS3's own firmware error).
// The worker thread has no Qt event loop of its own, but each seam spins a local QEventLoop for its
// network wait, so QNetworkAccessManager works there.
void EmulatorManager::runPs3UpdateThenLaunch(const QString& program, const QStringList& args, const QString& binDir)
{
    const QString rom       = rom_;
    const QString rpcs3Exe  = program;
    const QString tmpDir    = binDir + QStringLiteral("/.eb-ps3-updates");
    const QString statePath = AppPaths::dataDir() + QStringLiteral("/ps3-updates.json");
    // Game updates keep their opt-out; the firmware step below has none (without firmware NOTHING boots).
    // Read the setting here on the UI thread — QSettings is not for cross-thread use — the worker only
    // sees the captured bool.
    const bool gameUpdates = Settings::ps3AutoUpdate() && !rom.isEmpty();

    if (gameUpdates) emit status(tr("Checking for PS3 game updates…"), -1);

    // Seed RPCS3's first-run config NOW, before the worker runs `--installfw` / `--installpkg`. On a fresh
    // RPCS3 install that is RPCS3's genuine first run, and without this seed its "Welcome to RPCS3" modal
    // (whose Exit button quits the app) would pop and block the bounded process wait for its full timeout.
    // The seed is idempotent (seed-if-absent), so finishLocalLaunch calling it again later is harmless.
    prepareFirstRunConfig(binDir);

    // Guard the cross-thread progress marshal against the manager being destroyed — or this launch being
    // superseded — mid-update: `ctx` is this launch's context object, cleared on the UI thread when a newer
    // launch/install retires it. The queued lambda checks both guards on the UI thread before touching
    // `this`, so a stale worker's leftover notes drop instead of overwriting the new launch's status line.
    QPointer<EmulatorManager> self(this);
    QPointer<QObject> ctx(launchCtx_);
    QThread* worker = QThread::create([self, ctx, rom, rpcs3Exe, binDir, tmpDir, statePath, gameUpdates] {
        if (QThread::currentThread()->isInterruptionRequested()) return; // app already quitting
        // Transient progress notes from both steps, marshalled to the UI thread via the existing status()
        // signal (both QPointers are captured by value and only dereferenced on the UI thread — the worker
        // itself just posts).
        auto note = [self, ctx](const QString& msg) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, ctx, msg] {
                if (self && ctx) emit self->status(msg, -1);
            }, Qt::QueuedConnection);
        };

        // Firmware first: RPCS3 cannot boot anything without dev_flash, and a fresh auto-downloaded
        // install has none. Result ignored — a failed fetch/download/install falls through and RPCS3
        // shows its own missing-firmware error, exactly like a failed game update falls through to an
        // unpatched boot.
        // The firmware root is resolved per-OS (Windows: portable, next to the exe; Linux/macOS: RPCS3's
        // user config dir) so installed() stays true across launches on every platform — a root that
        // never matches where --installfw actually wrote would re-pay the ~230MB PUP download per launch.
        Ps3Firmware::maybeInstall(Ps3Firmware::devFlashRoot(binDir), rpcs3Exe, tmpDir,
            [] { return fetchPs3UpdateList(); },
            [](const QString& url, const QString& dest) { return downloadPs3Pkg(url, dest); },
            [binDir](const QString& exe, const QString& pup) {
                // Wait on the RESULT, not the process: `rpcs3 --installfw` installs the firmware and then
                // simply STAYS OPEN as the normal GUI (verified on hardware 2026-08-19 — dev_flash landed,
                // the window sat at the main screen, and the old waitForFinished(10min) would have killed a
                // SUCCESSFUL install and stamped the 1h failure backoff). Poll for dev_flash appearing; when
                // it does, close the lingering GUI and report success. The 10-minute bound still covers a
                // truly wedged installer (unexpected modal, corrupt PUP): kill + fail, the game boots anyway.
                const QString fwRoot = Ps3Firmware::devFlashRoot(binDir);
                QProcess proc;
                proc.start(exe, { QStringLiteral("--installfw"), pup });
                if (!proc.waitForStarted(30000)) return -1;
                // Wait on the RESULT, in slices. rpcs3 --installfw installs the firmware and then simply
                // STAYS OPEN as the normal GUI (hardware, 2026-08-19: dev_flash landed while the window sat
                // at the main screen) — so waiting for process exit alone killed a SUCCESSFUL install after
                // ten minutes of "installing firmware" and stamped the 1h failure backoff. Poll for dev_flash
                // appearing: on success give the installer a beat to close its file handles, then close the
                // lingering GUI and report success. The waits are SLICED so an app-quit interruption request
                // kills the installer within ~500ms instead of blocking Qt teardown, and the 10-minute
                // deadline keeps the wedge protection (unexpected modal, corrupt PUP). A killed run returns
                // non-zero, and Ps3Firmware scrubs the half-written version.txt so installed() never reads
                // half-true; the success path returns 0 explicitly, so a good install is never scrubbed.
                QDeadlineTimer deadline(600000);
                for (;;)
                {
                    if (proc.waitForFinished(500)) // it DID exit on its own (a future RPCS3 might)
                        return Ps3Firmware::installed(fwRoot) ? 0 : (proc.exitCode() == 0 ? -1 : proc.exitCode());
                    if (Ps3Firmware::installed(fwRoot))
                    {
                        proc.waitForFinished(3000); // settle: version.txt lands late, let handles close
                        proc.kill();
                        proc.waitForFinished(5000);
                        return 0;
                    }
                    if (QThread::currentThread()->isInterruptionRequested() || deadline.hasExpired())
                    {
                        proc.kill(); proc.waitForFinished(5000);
                        return -1;
                    }
                }
            },
            note);

        // A quit-interrupted attempt is not a *failing* install: the interruption made the download or
        // installer seam abort, and maybeInstall filed that under its hourly retry backoff — which would
        // leave the next launch booting into RPCS3's missing-firmware error for an hour after an innocent
        // quit. Clear the marker so a quit-caused abort retries immediately (the pre-teardown behavior:
        // the old worker died with the process before the marker was written). A genuine network/installer
        // failure has no interruption request, so its backoff stands.
        if (QThread::currentThread()->isInterruptionRequested())
        {
            QFile::remove(QDir(tmpDir).filePath(QStringLiteral("fw-install-failed")));
            return;
        }

        if (!gameUpdates) return;

        Ps3UpdateState state(statePath);
        Ps3UpdateInstaller installer(
            rpcs3Exe, tmpDir,
            [](const QString& url, const QString& dest) { return downloadPs3Pkg(url, dest); },
            [binDir](const QString& exe, const QString& pkg, const QString& titleId,
                     const QString& version) {
                // Wait on the RESULT, not the process — same finding as the --installfw runner above:
                // `rpcs3 --installpkg` installs the package and then simply STAYS OPEN as the normal GUI
                // (hardware 2026-08-19, firmware twin), so waiting for exit killed a SUCCESSFUL install
                // after ten minutes and aborted the whole update chain on every launch. The result RPCS3
                // writes is APP_VER in dev_hdd0/game/<TITLEID>/PARAM.SFO.
                const QString gameDir =
                    Ps3InstalledVersion::gameDir(Ps3Firmware::devFlashRoot(binDir), titleId);
                // Already on disk: the disk state IS the result, so don't spawn at all. A lost or stale
                // ps3-updates.json re-runs an already-applied update, and spawning would risk killing a
                // reinstall mid-write; returning 0 lets installAll's markInstalled heal the state file.
                if (Ps3InstalledVersion::reachedTarget(gameDir, version)) return 0;
                QProcess proc;
                proc.start(exe, { QStringLiteral("--installpkg"), pkg });
                if (!proc.waitForStarted(30000)) return -1;
                // Sliced so an app-quit interruption request kills the installer within ~500ms instead of
                // blocking Qt teardown; the 10-minute deadline keeps the wedge protection (unexpected
                // modal, corrupt pkg): kill + fail, and the game boots unpatched anyway.
                //
                // Success needs BOTH halves: pkg entries extract IN PLACE in entry order (RPCS3's
                // Crypto/unpkg.cpp extract_worker), so PARAM.SFO can land long before the rest of the
                // update's files — version-at-target alone must never trigger the kill or we'd destroy a
                // mid-flight install and record it as applied. The quiescence check (nothing under the
                // game dir written for ~3s) is what makes killing the lingering GUI safe.
                QDeadlineTimer deadline(600000);
                for (;;)
                {
                    if (proc.waitForFinished(500)) // it DID exit on its own (a future RPCS3 might)
                        return Ps3InstalledVersion::reachedTarget(gameDir, version)
                                   ? 0
                                   : (proc.exitCode() == 0 ? -1 : proc.exitCode());
                    if (QThread::currentThread()->isInterruptionRequested() || deadline.hasExpired())
                    {
                        proc.kill(); proc.waitForFinished(5000);
                        return -1;
                    }
                    if (Ps3InstalledVersion::reachedTarget(gameDir, version)
                        && Ps3InstalledVersion::secsSinceNewestWrite(
                               gameDir, QDateTime::currentDateTimeUtc()) >= 3)
                    {
                        proc.waitForFinished(2000); // settle: let the installer close its handles
                        proc.kill();
                        proc.waitForFinished(5000);
                        return 0;
                    }
                }
            });
        Ps3UpdateCoordinator coord(
            [](const QString& p) { return Ps3TitleId::read(p); },
            [](const QString& titleId) { return fetchPs3VerXml(titleId); },
            &state, &installer, note);
        coord.maybeUpdate(rom); // result ignored — always fall through to a boot
    });
    // The thread frees itself when it finishes, regardless of the manager's lifetime — so if the
    // manager is destroyed mid-update (the continuation below auto-disconnects) the QThread doesn't leak.
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // App-quit teardown: without this, the worker runs on through Qt teardown (its local event loops
    // and QNetworkAccessManager outlive the Qt globals — deleteLater is never delivered once the loop
    // stops) and can leave a half-run --installfw behind. Request interruption — the network waits and
    // the sliced process waits above poll it — and join for a bounded interval so quit is never held
    // hostage by a slow kill (worst case ~5.6s: one 500ms slice + the 5s reap). The worker is the
    // connection context, so a finished-and-deleted worker drops its handler automatically and every
    // live worker (including one whose launch was superseded) gets its own.
    connect(qApp, &QCoreApplication::aboutToQuit, worker, [worker] {
        worker->requestInterruption();
        worker->wait(8000);
    });
    // finished() is emitted from the worker; delivered queued to the UI thread, where the launch must run.
    // Bound to this launch's context object — not to `this` — so a destroyed manager still skips it (the
    // context dies with the manager), and so does a superseded launch: play()/install() retiring the context
    // auto-disconnects this continuation, and a stale worker can never boot its game on top of the launch
    // that replaced it. `this` in the capture is safe: launchCtx_ is a child of the manager, so if this
    // lambda runs at all the manager is still alive.
    connect(worker, &QThread::finished, launchCtx_, [this, program, args, binDir] {
        finishLocalLaunch(program, args, binDir);
    });
    worker->start();
}

// The process half of launch(): spawn + monitor the emulator. Split out so it can run as the continuation
// of the async BIOS fetch above (and directly for Flatpak, which skips the on-disk prep).
void EmulatorManager::startGameProcess(const QString& program, const QStringList& args,
                                       const QString& binDir, bool isFlatpak)
{
    game_ = new QProcess(this);
    if (!isFlatpak) game_->setWorkingDirectory(binDir);
    connect(game_, &QProcess::started, this, [this] { emit launched(em_.displayName); });
    connect(game_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, binDir, isFlatpak](int code, QProcess::ExitStatus) {
        busy_ = false;
        if (game_) { game_->deleteLater(); game_ = nullptr; }
        if (!isFlatpak) backupSaves(binDir); // snapshot the saves the emulator just wrote into the central tree
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
    game_->start(program, args);
}

#endif // Q_OS_IOS
