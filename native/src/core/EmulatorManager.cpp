#include "EmulatorManager.h"
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
#include <QEventLoop>
#include <cctype>
#include <QProcess>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTextStream>
#include "CoreManager.h"
#include "BiosCatalog.h"

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
    return QSettings(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
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
// image so -batch boots without its first-run wizard.
static void wireBiosConfig(const BiosCatalog::ExternalBios& b, const QString& binDir)
{
    const QList<BiosFile>& bios = BiosCatalog::forSystem(b.systemId);
    if (b.portable && !bios.isEmpty())
    {
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
                   << "[Filenames]\n" << "BIOS = " << bios.first().fileName << "\n";
                f.close();
            }
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
                                 [b, binDir, onDone] { wireBiosConfig(b, binDir); onDone(); });
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
// which can change on re-login and must stay in sync with MMV.
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

// Feed MMV's RetroAchievements login into a standalone emulator's own RA client, so it unlocks natively against
// the same account (a standalone emulator is a separate process — MMV can't run rcheevos against its memory the
// way it does for in-process cores). Runs every launch to keep the token fresh; does nothing (leaves the config
// untouched) when MMV isn't signed into RA.
//
// Only emulators that accept a RAW rcheevos token in their config qualify. VERIFIED LIVE:
//   • PCSX2 ([Achievements] Username/Token) — logs in successfully with MMV's token.
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
    if (user.isEmpty() || token.isEmpty()) return; // not signed into RetroAchievements in MMV

    QString path, section;
    if (em_.id == QStringLiteral("pcsx2")) { path = binDir + QStringLiteral("/inis/PCSX2.ini"); section = QStringLiteral("Achievements"); }
    else return;

    setIniKey(path, section, QStringLiteral("Enabled"), QStringLiteral("true"));
    setIniKey(path, section, QStringLiteral("Username"), user);
    setIniKey(path, section, QStringLiteral("Token"), token); // credential — never logged
}

// Auto-map the player's controller inside each standalone emulator so a game boots with working input — the
// thing RetroBat/ES-DE do that makes a pad "just work". Without this, most standalone emulators launch with no
// binding and the user has to open each emulator's input menu and hand-map every button. We seed a config for a
// standard XInput/SDL pad as Player 1 (only when absent, so a user's own mapping is never overwritten). The
// bodies mirror RetroBat's input-config generators; on Windows they key on device index 0 / XInput, so no
// per-controller GUID is needed. Emulators that already auto-map a standard pad (DuckStation, PPSSPP) are seeded
// too for a guaranteed result; Ryujinx (needs the live SDL GUID) and Flycast (keys on the controller name, and
// ships a working default) are left to their own detection.
void EmulatorManager::prepareControllerConfig(const QString& binDir)
{
    const QString& id = em_.id;

    // ---- Cemu: controllerProfiles/controllerN.xml (N = player-1 = 0). Auto-loaded on start, no settings.xml
    // reference needed. XInput api with <uuid>0</uuid> binds device 0 generically (no GUID). ----------------
#ifdef Q_OS_WIN
    if (id == QStringLiteral("cemu"))
    {
        static const QByteArray kCemuPad =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<emulated_controller>\n\t<type>Wii U GamePad</type>\n"
            "\t<controller>\n\t\t<api>XInput</api>\n\t\t<uuid>0</uuid>\n\t\t<display_name>Controller 0</display_name>\n"
            "\t\t<rumble>0</rumble>\n\t\t<axis><deadzone>0.25</deadzone><range>1</range></axis>\n"
            "\t\t<rotation><deadzone>0.25</deadzone><range>1</range></rotation>\n"
            "\t\t<trigger><deadzone>0.25</deadzone><range>1</range></trigger>\n\t\t<mappings>\n"
            "\t\t\t<entry><mapping>1</mapping><button>13</button></entry>\n"   // A  <- XInput A
            "\t\t\t<entry><mapping>2</mapping><button>12</button></entry>\n"   // B  <- XInput B
            "\t\t\t<entry><mapping>3</mapping><button>15</button></entry>\n"   // X
            "\t\t\t<entry><mapping>4</mapping><button>14</button></entry>\n"   // Y
            "\t\t\t<entry><mapping>5</mapping><button>8</button></entry>\n"    // L
            "\t\t\t<entry><mapping>6</mapping><button>9</button></entry>\n"    // R
            "\t\t\t<entry><mapping>7</mapping><button>42</button></entry>\n"   // ZL (left trigger)
            "\t\t\t<entry><mapping>8</mapping><button>43</button></entry>\n"   // ZR (right trigger)
            "\t\t\t<entry><mapping>9</mapping><button>4</button></entry>\n"    // + (start)
            "\t\t\t<entry><mapping>10</mapping><button>5</button></entry>\n"   // - (back)
            "\t\t\t<entry><mapping>11</mapping><button>0</button></entry>\n"   // dpad up
            "\t\t\t<entry><mapping>12</mapping><button>1</button></entry>\n"   // dpad down
            "\t\t\t<entry><mapping>13</mapping><button>2</button></entry>\n"   // dpad left
            "\t\t\t<entry><mapping>14</mapping><button>3</button></entry>\n"   // dpad right
            "\t\t\t<entry><mapping>15</mapping><button>6</button></entry>\n"   // L3
            "\t\t\t<entry><mapping>16</mapping><button>7</button></entry>\n"   // R3
            "\t\t\t<entry><mapping>17</mapping><button>39</button></entry>\n"  // left stick up
            "\t\t\t<entry><mapping>18</mapping><button>45</button></entry>\n"  // left stick down
            "\t\t\t<entry><mapping>19</mapping><button>44</button></entry>\n"  // left stick left
            "\t\t\t<entry><mapping>20</mapping><button>38</button></entry>\n"  // left stick right
            "\t\t\t<entry><mapping>21</mapping><button>41</button></entry>\n"  // right stick up
            "\t\t\t<entry><mapping>22</mapping><button>47</button></entry>\n"  // right stick down
            "\t\t\t<entry><mapping>23</mapping><button>46</button></entry>\n"  // right stick left
            "\t\t\t<entry><mapping>24</mapping><button>40</button></entry>\n"  // right stick right
            "\t\t</mappings>\n\t</controller>\n</emulated_controller>\n";
        seedFileIfAbsent(binDir + QStringLiteral("/controllerProfiles/controller0.xml"), kCemuPad);
        const QString appdata = qEnvironmentVariable("APPDATA");
        if (!appdata.isEmpty())
            seedFileIfAbsent(appdata + QStringLiteral("/Cemu/controllerProfiles/controller0.xml"), kCemuPad);
        return;
    }

    // ---- Dolphin: GCPadNew.ini [GCPad1] + Dolphin.ini [Core] SIDevice0=6 (standard controller in port 1). ----
    if (id == QStringLiteral("dolphin"))
    {
        seedFileIfAbsent(binDir + QStringLiteral("/User/Config/GCPadNew.ini"),
            "[GCPad1]\nDevice = XInput/0/Gamepad\n"
            "Buttons/A = `Button B`\nButtons/B = `Button A`\nButtons/X = `Button Y`\nButtons/Y = `Button X`\n"
            "Buttons/Z = `Shoulder R`\nButtons/Start = `Start`\n"
            "Main Stick/Up = `Left Y+`\nMain Stick/Down = `Left Y-`\nMain Stick/Left = `Left X-`\n"
            "Main Stick/Right = `Left X+`\nC-Stick/Up = `Right Y+`\nC-Stick/Down = `Right Y-`\n"
            "C-Stick/Left = `Right X-`\nC-Stick/Right = `Right X+`\n"
            "Triggers/L = `Trigger L`\nTriggers/R = `Trigger R`\nTriggers/L-Analog = `Trigger L`\n"
            "Triggers/R-Analog = `Trigger R`\nD-Pad/Up = `Pad N`\nD-Pad/Down = `Pad S`\nD-Pad/Left = `Pad W`\n"
            "D-Pad/Right = `Pad E`\nMain Stick/Dead Zone = 15.0\nC-Stick/Dead Zone = 15.0\n"
            "Rumble/Motor = `Motor L`|`Motor R`\n");
        appendIniSectionIfAbsent(binDir + QStringLiteral("/User/Config/Dolphin.ini"),
            "SIDevice0", "\n[Core]\nSIDevice0 = 6\n");
        return;
    }
#endif // Q_OS_WIN

    // ---- PCSX2: [Pad1] appended to the inis/PCSX2.ini prepareBios wrote. SDL-0 works cross-platform; PCSX2 has
    // NO default binding, so this is the one that's outright broken without a seed. ----
    if (id == QStringLiteral("pcsx2"))
    {
        appendIniSectionIfAbsent(binDir + QStringLiteral("/inis/PCSX2.ini"), "[Pad1]",
            "\n[InputSources]\nSDL = true\nSDLControllerEnhancedMode = false\nSDLRawInput = true\n"
            "XInput = false\nDInput = false\n\n[Pad1]\nType = DualShock2\n"
            "Up = SDL-0/DPadUp\nRight = SDL-0/DPadRight\nDown = SDL-0/DPadDown\nLeft = SDL-0/DPadLeft\n"
            "Triangle = SDL-0/FaceNorth\nCircle = SDL-0/FaceEast\nCross = SDL-0/FaceSouth\nSquare = SDL-0/FaceWest\n"
            "Select = SDL-0/Back\nStart = SDL-0/Start\nL1 = SDL-0/LeftShoulder\nR1 = SDL-0/RightShoulder\n"
            "L2 = SDL-0/+LeftTrigger\nR2 = SDL-0/+RightTrigger\nL3 = SDL-0/LeftStick\nR3 = SDL-0/RightStick\n"
            "Analog = SDL-0/Guide\nLUp = SDL-0/-LeftY\nLRight = SDL-0/+LeftX\nLDown = SDL-0/+LeftY\n"
            "LLeft = SDL-0/-LeftX\nRUp = SDL-0/-RightY\nRRight = SDL-0/+RightX\nRDown = SDL-0/+RightY\n"
            "RLeft = SDL-0/-RightX\nLargeMotor = SDL-0/LargeMotor\nSmallMotor = SDL-0/SmallMotor\n");
        return;
    }

    // ---- DuckStation: [Pad1] appended to settings.ini. It self-maps a standard pad, but seeding guarantees it. ----
    if (id == QStringLiteral("duckstation"))
    {
        appendIniSectionIfAbsent(binDir + QStringLiteral("/settings.ini"), "[Pad1]",
            "\n[ControllerPorts]\nMultitapMode = Disabled\nControllerSettingsMigrated = true\n\n"
            "[InputSources]\nSDL = true\nSDLControllerEnhancedMode = false\nXInput = false\nDInput = false\n\n"
            "[Pad1]\nType = AnalogController\n"
            "Up = SDL-0/DPadUp\nDown = SDL-0/DPadDown\nLeft = SDL-0/DPadLeft\nRight = SDL-0/DPadRight\n"
            "Triangle = SDL-0/Y\nCircle = SDL-0/B\nCross = SDL-0/A\nSquare = SDL-0/X\n"
            "Select = SDL-0/Back\nStart = SDL-0/Start\nL1 = SDL-0/LeftShoulder\nR1 = SDL-0/RightShoulder\n"
            "L2 = SDL-0/+LeftTrigger\nR2 = SDL-0/+RightTrigger\nL3 = SDL-0/LeftStick\nR3 = SDL-0/RightStick\n"
            "Analog = SDL-0/Guide\nLLeft = SDL-0/-LeftX\nLRight = SDL-0/+LeftX\nLDown = SDL-0/+LeftY\n"
            "LUp = SDL-0/-LeftY\nRLeft = SDL-0/-RightX\nRRight = SDL-0/+RightX\nRDown = SDL-0/+RightY\n"
            "RUp = SDL-0/-RightY\n");
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
        // (PS3 firmware is still required to actually boot games — a separate one-time user step.)
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
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
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

    // Emulators that can't boot without a copyrighted BIOS (PCSX2) / decryption keys (Cemu): make sure they're
    // in place next to the binary before launching. Best-effort and only on local installs we control on disk.
    // The BIOS fetch is asynchronous, so the GUI thread never waits on the network: the rest of the pre-launch
    // prep and the process start run as its continuation — the launch still happens only once the BIOS has
    // settled. The chain is parented to a per-launch context object, recreated here, so a torn-down manager
    // (or a launch superseded before its download finished) cancels it and the process never starts.
    delete launchCtx_;
    launchCtx_ = new QObject(this);
    prepareBios(binDir, [this, program, args, binDir] {
        prepareFirstRunConfig(binDir);
        prepareCemuConfig(binDir);
        prepareControllerConfig(binDir); // after the above wrote the base inis to append to
        prepareAchievements(binDir);     // sync MMV's RetroAchievements login into the emulator
        prepareCemuKeys(binDir, [this, program, args, binDir] { // async too (gist fetch, Cemu only)
            prepareCemuDiscKey(binDir); // appends to the keys.txt the fetch may have just (over)written
            restoreSaves(binDir); // seed saves from the central backup if this install has none
            startGameProcess(program, args, binDir, false);
        });
    });
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
