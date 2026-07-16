#include "GameLauncher.h"
#include "../emu/RetroView.h"        // openGame/stop/gamepad() + RETRO_DEVICE_ID_JOYPAD_* (via LibretroCore.h)
#include "../input/Gamepad.h"
#include "../core/AppPaths.h"
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
#include "../core/CoreManager.h"
#include "../core/ArchiveRom.h"
#include "../core/EmulatorRegistry.h"
#include "../core/EmulatorManager.h"
#include "../core/RecentStore.h"
#include "../core/PlayStats.h"
#include "../core/BiosCatalog.h"
#include <QTimer>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QSet>
#include <QRegularExpression>
#include <QDateTime>

// Standalone-emulator exit hotkey (Windows): read the global Esc key state while the app is minimized.
// Included last so <windows.h>'s macros don't clobber the Qt headers above.
#ifdef Q_OS_WIN
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

// One-line append to <app>/stream_debug.log, shared with the addon stream/manga resolution tracing.
// A local copy of MainWindow.cpp's mwLog so the launch pipeline keeps logging to the same file.
static void glLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
}

// A disc dumped as a descriptor + raw tracks (Redump: "Game.cue" + "Game (Track N).bin"; or a GDI dump: a
// ".gdi" + "trackNN.bin/.raw") must be booted via the .cue/.gdi — handing the emulator a raw data track mounts
// nothing and it exits immediately (the Flycast "process exited (code 0)" symptom). If `rom` is such a track,
// return its descriptor; otherwise return `rom` unchanged. Safe for direct images (.iso/.chd/.cdi) and for a
// lone .bin with no descriptor beside it (e.g. an Atari 2600 cart), which are left untouched.
static QString resolveDiscDescriptor(const QString& rom)
{
    const QFileInfo fi(rom);
    static const QSet<QString> trackExts = { QStringLiteral("bin"), QStringLiteral("img"), QStringLiteral("raw") };
    if (!trackExts.contains(fi.suffix().toLower())) return rom;

    const QFileInfoList descs = fi.absoluteDir().entryInfoList(
        { QStringLiteral("*.cue"), QStringLiteral("*.gdi") }, QDir::Files);
    if (descs.isEmpty()) return rom;

    const QString binName = fi.fileName();
    // 1) A descriptor that textually references this exact track file is definitive (handles GDI dumps whose
    //    track names — track03.bin — don't resemble the .gdi's name, and multi-.cue folders).
    for (const QFileInfo& d : descs)
    {
        QFile f(d.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        if (QString::fromUtf8(f.read(1 << 20)).contains(binName, Qt::CaseInsensitive))
            return d.absoluteFilePath();
    }
    // 2) Else match by name: strip a trailing " (Track N)" and compare base names (Redump layout).
    QString base = fi.completeBaseName();
    base.remove(QRegularExpression(QStringLiteral("\\s*\\(Track\\s*\\d+\\)\\s*$"),
                                   QRegularExpression::CaseInsensitiveOption));
    for (const QFileInfo& d : descs)
        if (d.completeBaseName().compare(base, Qt::CaseInsensitive) == 0) return d.absoluteFilePath();

    // No positive evidence this .bin belongs to any descriptor here (it may be a cart, e.g. an Atari 2600
    // .bin sitting next to an unrelated .cue) — leave it as-is rather than redirect to the wrong disc.
    return rom;
}

GameLauncher::GameLauncher(RetroView* retro, QObject* parent)
    : QObject(parent), retro_(retro)
{
    // Bank the elapsed session whenever the full-screen libretro game is torn down (the RetroView Esc-menu Exit,
    // or switching to other content). Symmetric to beginPlaySession() in the retro branch of open(); the session
    // state lives here, so this class owns the end trigger too.
    connect(retro_, &RetroView::gameStopped, this, [this] { endPlaySession(); });
}

GameLauncher::CorePlan GameLauncher::prepareCore(const QString& rom, const QString& systemHint)
{
    CorePlan plan;

    const QString ext = QFileInfo(rom).suffix().toLower();
    // Prefer the console/platform the game was opened from (when known): it disambiguates extensions shared
    // across systems (PSP .iso vs GameCube .iso, PSP .pbp vs PlayStation .pbp). Fall back to the extension.
    const GameSystem* sys = nullptr;
    if (!systemHint.isEmpty())
    {
        sys = SystemCatalog::byId(systemHint);
        if (!sys) sys = SystemCatalog::forConsoleName(systemHint);
    }
    const bool byHint = sys != nullptr;
    if (!sys) sys = SystemCatalog::forExtension(ext);
    glLog(QStringLiteral("game: open \"%1\" (.%2)%3 -> system %4")
              .arg(QFileInfo(rom).fileName(), ext,
                   systemHint.isEmpty() ? QString() : QStringLiteral(" hint=\"%1\"%2").arg(systemHint,
                       byHint ? QString() : QStringLiteral("(unmatched)")),
                   sys ? sys->id : QStringLiteral("(none)")));
    if (!sys)
    {
        glLog(QStringLiteral("game: no system for .%1 — aborting").arg(ext));
        plan.error = tr("No system is configured for .%1 files.").arg(ext);
        return plan;
    }
    plan.systemId = sys->id;

    // If the user opened a raw disc track (a "(Track N).bin" / GDI track), boot its .cue/.gdi descriptor instead —
    // the emulator can't mount a bare track. No-op for direct images and lone .bin carts. Covers cores + externals.
    const QString launchRom = resolveDiscDescriptor(rom);
    if (launchRom != rom)
        glLog(QStringLiteral("game: track \"%1\" -> disc descriptor \"%2\"")
                  .arg(QFileInfo(rom).fileName(), QFileInfo(launchRom).fileName()));
    plan.launchRom = launchRom;

    // Standalone-emulator systems (GameCube/Wii → Dolphin) have no libretro core to prepare — open() routes them
    // to a child-process emulator instead. A split pane can't host one, so leave corePath empty with a message
    // the split-pane caller surfaces.
    if (!sys->externalEmulator.isEmpty())
    {
        const ExternalEmulator* em = EmulatorRegistry::byId(sys->externalEmulator);
        plan.error = em ? tr("%1 opens in its own window, not a split pane.").arg(em->displayName)
                        : tr("No emulator is configured for %1.").arg(sys->name);
        return plan;
    }

    QString core = Settings::coreFor(sys->id);
    if (core.isEmpty())
        core = sys->cores.value(0); // catalog default
    glLog(QStringLiteral("game: core '%1' for system %2 (configured=%3)")
              .arg(core, sys->id, Settings::coreFor(sys->id).isEmpty() ? QStringLiteral("no, default") : QStringLiteral("yes")));

    // No prompt: use the configured core, downloading it from the buildbot if it isn't installed. Progress
    // shows inline in the status bar; failures report there too.
    QString dlErr;
    const QString corePath = CoreManager::ensureCore(core, nullptr, &dlErr, [this, core](int pct) {
        emit statusMessage(tr("Downloading core ‘%1’… %2%").arg(core).arg(pct), 0);
    });
    if (corePath.isEmpty())
    {
        glLog(QStringLiteral("game: core '%1' unavailable: %2").arg(core, dlErr.isEmpty() ? QStringLiteral("download failed") : dlErr));
        plan.error = dlErr.isEmpty() ? tr("Couldn't download core ‘%1’.").arg(core) : dlErr;
        return plan;
    }
    glLog(QStringLiteral("game: core ready at %1").arg(QFileInfo(corePath).fileName()));

    plan.core = core;
    plan.corePath = corePath;
    return plan;
}

void GameLauncher::open(const QString& rom, const QString& title, const QString& thumb, const QString& key,
                        const QString& systemHint)
{
    // Archived ROM (.zip / .7z): extract the inner ROM once and open that. Every libretro core and external
    // emulator loads from a path, so this single spot handles archives for all of them. If we know the
    // system (from the hint), narrow the member pick to its extensions; otherwise take the largest file.
    // The hint carries through the re-entry so disambiguation still works.
    if (ArchiveRom::isArchive(rom))
    {
        QStringList wanted;
        if (!systemHint.isEmpty())
        {
            const GameSystem* hs = SystemCatalog::byId(systemHint);
            if (!hs) hs = SystemCatalog::forConsoleName(systemHint);
            if (hs)
                for (const QString& e : hs->extensions)
                    wanted << (QStringLiteral(".") + e);
        }
        QString aerr;
        const QString extracted = ArchiveRom::extractToTemp(rom, wanted, &aerr);
        if (extracted.isEmpty())
        {
            glLog(QStringLiteral("game: archive extract failed for \"%1\": %2").arg(QFileInfo(rom).fileName(), aerr));
            emit notifyUser(tr("Couldn't open the archived game: %1").arg(aerr), 7000);
            return;
        }
        glLog(QStringLiteral("game: extracted \"%1\" from \"%2\"")
                  .arg(QFileInfo(extracted).fileName(), QFileInfo(rom).fileName()));
        open(extracted, title, thumb, key, systemHint);
        return;
    }

    // Resolve the system, disc descriptor, and (for a libretro system) the core. prepareCore short-circuits for
    // standalone-emulator systems, which have no core — we route those to the external-emulator branch below.
    const CorePlan plan = prepareCore(rom, systemHint);

    // The Recent entry shows the catalog item's name/cover when we have them; otherwise the descriptor's file
    // name. A remote ROM is cached under a hashed file name, so without the passed title it would show as that hash.
    const QString launchRom = plan.launchRom.isEmpty() ? rom : plan.launchRom;
    const QString recentTitle = title.isEmpty() ? QFileInfo(launchRom).completeBaseName() : title;

    // Standalone-emulator systems (GameCube/Wii → Dolphin) launch an external process instead of a core.
    // Not possible on Android (the sandbox can't spawn downloaded desktop executables - see android-port.md).
    const GameSystem* sys = plan.systemId.isEmpty() ? nullptr : SystemCatalog::byId(plan.systemId);
    if (sys && !sys->externalEmulator.isEmpty())
    {
#if defined(Q_OS_ANDROID)
        emit statusMessage(tr("“%1” needs a standalone emulator, which isn't supported on Android.")
                               .arg(sys->name), 6000);
#else
        launchExternalGame(sys, launchRom, recentTitle, thumb, key);
#endif
        return;
    }

    if (plan.corePath.isEmpty())
    {
        emit notifyUser(plan.error.isEmpty() ? tr("Can't run game.") : plan.error, 6000);
        return;
    }

    // Some systems (3DO, Saturn, PlayStation) need a BIOS in the libretro system folder. Fetch any that
    // are missing before the core loads — best-effort, so a failure just falls back to the core's own
    // "BIOS not found" message rather than blocking the launch.
    CoreManager::ensureBios(plan.systemId, CoreManager::systemDir(),
                            [this](const QString& s) { emit statusMessage(s, 0); });

    emit aboutToLaunch();
    QString err;
    if (retro_->openGame(plan.corePath, launchRom, plan.core, &err))
    {
        glLog(QStringLiteral("game: running \"%1\"").arg(recentTitle));
        emit showRetroRequested();
        RecentStore::add({ launchRom, recentTitle, QStringLiteral("game"), thumb, key, plan.systemId });
        beginPlaySession(PlayStats::identity(key, launchRom));
    }
    else
    {
        glLog(QStringLiteral("game: openGame failed: %1").arg(err));
        emit notifyUser(tr("Can't run game: %1").arg(err), 7000);
    }
}

// ---- External (standalone) emulators: the RetroBat / ES-DE launch-and-monitor model -----------------

void GameLauncher::ensureEmu()
{
    if (emu_) return;
    emu_ = new EmulatorManager(this);

    connect(emu_, &EmulatorManager::status, this, [this](const QString& t, int pct) {
        const QString line = pct >= 0 ? tr("%1  %2%").arg(t).arg(pct) : t;
        emit statusMessage(line, 0);
        if (emu_->busy())
            emit waitPage(line, false);
    });
    connect(emu_, &EmulatorManager::launched, this, [this](const QString& name) {
        emit waitPage(tr("Playing in %1.\n\nClose the %1 window — or press Start+Select on your controller, "
                         "or Esc — to return to My Media Vault.").arg(name), true);
        if (!pendingEmuRom_.isEmpty()) // record now that it actually started
        {
            RecentStore::add({ pendingEmuRom_, pendingEmuTitle_, QStringLiteral("game"),
                               pendingEmuThumb_, pendingEmuKey_, pendingEmuSystem_ });
            beginPlaySession(PlayStats::identity(pendingEmuKey_, pendingEmuRom_));
        }
        // Step aside so the emulator is unobstructed and in front; we restore when it exits. (Our window is
        // often full screen and would otherwise sit on top of the freshly-launched emulator.)
        emit minimizeRequested();
        emuDisplayName_ = name;
        emuUserClosing_ = false;
        emuRunClock_.start();  // to spot a boot that fails and exits instantly (missing BIOS/firmware)
        startEmuHotkeyWatch(); // Start+Select / Esc closes the standalone emulator back to MMV
    });
    connect(emu_, &EmulatorManager::finished, this, [this](int code) {
        glLog(QStringLiteral("emu: process exited (code %1)").arg(code));
        stopEmuHotkeyWatch();
        endPlaySession(); // bank the external emulator's play time

        emit restoreRequested(); // come back to where we were before handing off to the emulator
        emit waitPageDone();

        // Closed within a couple of seconds, and we didn't ask it to? That's a failed boot, not a play session —
        // most often a missing console BIOS/firmware, which -batch-style launches quit on silently. Tell the user
        // what happened instead of just bouncing back to the home screen with no explanation.
        if (!emuUserClosing_ && emuRunClock_.isValid() && emuRunClock_.elapsed() < 4000)
        {
            const QString emuName = emuDisplayName_.isEmpty() ? tr("The emulator") : emuDisplayName_;
            const bool needsBios = !pendingEmuSystem_.isEmpty()
                                   && !BiosCatalog::forSystem(pendingEmuSystem_).isEmpty();
            const GameSystem* sys = pendingEmuSystem_.isEmpty() ? nullptr : SystemCatalog::byId(pendingEmuSystem_);
            const QString sysName = sys ? sys->name : tr("This system");
            if (needsBios)
                emit notifyUser(tr("%1 closed immediately. %2 games need a console BIOS to boot. I try to fetch it "
                                   "automatically — if it still won’t start, the BIOS couldn’t be downloaded and you’ll "
                                   "need to place it in the emulator’s “bios” folder yourself.").arg(emuName, sysName), 12000);
            else
                emit notifyUser(tr("%1 closed immediately — the game may be missing files it needs to boot, or the "
                                   "emulator needs firmware set up.").arg(emuName), 9000);
        }
        emuRunClock_.invalidate();
    });
    connect(emu_, &EmulatorManager::installed, this, [this](const QString& name) {
        emit statusMessage(tr("%1 is installed.").arg(name), 5000);
    });
    connect(emu_, &EmulatorManager::failed, this, [this](const QString& msg) {
        glLog(QStringLiteral("emu: failed: %1").arg(msg));
        stopEmuHotkeyWatch();
        emit statusMessage(msg, 9000);
        emit waitPageDone();
    });
}

// ---- Standalone-emulator exit hotkey: close melonDS/Dolphin/etc. back to MMV on Start+Select or Esc ---------
// A libretro core shows MMV's own pause menu on Start+Select; a standalone emulator is a separate process we
// can't inject a menu into, so the RetroBat-equivalent is to close it and come back. We poll while MMV is
// minimized (Qt gets no input then): the pad works because SDL keeps device state live in the background
// (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS), and Esc is read from the global key state on Windows.
void GameLauncher::startEmuHotkeyWatch()
{
    if (!emuHotkeyTimer_)
    {
        emuHotkeyTimer_ = new QTimer(this);
        emuHotkeyTimer_->setInterval(60);
        connect(emuHotkeyTimer_, &QTimer::timeout, this, &GameLauncher::pollEmuExitHotkey);
    }
    // Prime the edge-detectors as "held" so a combo/Esc still down from the moment of launch doesn't instantly
    // close the emulator — we only act on a fresh press.
    emuComboPrev_ = true;
    emuEscPrev_ = true;
    emuHotkeyTimer_->start();
}

void GameLauncher::stopEmuHotkeyWatch()
{
    if (emuHotkeyTimer_) emuHotkeyTimer_->stop();
}

void GameLauncher::pollEmuExitHotkey()
{
    if (!emu_ || !emu_->busy()) { stopEmuHotkeyWatch(); return; }
    bool exitNow = false;

    // Controller: Start+Select on any connected pad. Reuse RetroView's Gamepad (idle while a standalone emulator
    // runs, so borrowing it to poll is free and avoids opening the device twice).
    if (retro_ && retro_->gamepad() && retro_->gamepad()->available())
    {
        Gamepad* pad = retro_->gamepad();
        pad->poll();
        bool combo = false;
        for (unsigned p = 0; p < Gamepad::kMaxPlayers && !combo; ++p)
            combo = pad->button(p, RETRO_DEVICE_ID_JOYPAD_START) && pad->button(p, RETRO_DEVICE_ID_JOYPAD_SELECT);
        if (combo && !emuComboPrev_) exitNow = true;
        emuComboPrev_ = combo;
    }

#if defined(Q_OS_WIN)
    // Keyboard: Qt can't see Esc while the emulator owns focus, so read the global key state.
    const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (esc && !emuEscPrev_) exitNow = true;
    emuEscPrev_ = esc;
#endif

    if (exitNow)
    {
        glLog(QStringLiteral("emu: exit hotkey (Start+Select / Esc) — closing the standalone emulator"));
        stopEmuHotkeyWatch();   // one shot: don't fire again while it's tearing down
        emuUserClosing_ = true; // a deliberate close — don't mistake it for a failed boot
        emu_->closeGame();
    }
}

void GameLauncher::launchExternalGame(const GameSystem* sys, const QString& rom, const QString& title,
                                      const QString& thumb, const QString& key)
{
    const ExternalEmulator* em = EmulatorRegistry::byId(sys->externalEmulator);
    if (!em)
    {
        glLog(QStringLiteral("game: external emulator '%1' not registered").arg(sys->externalEmulator));
        emit statusMessage(tr("No emulator is configured for %1.").arg(sys->name), 6000);
        return;
    }
    runEmulator(*em, rom, title, thumb, key, sys->id);
}

void GameLauncher::runEmulator(const ExternalEmulator& em, const QString& rom, const QString& title,
                               const QString& thumb, const QString& key, const QString& system)
{
    ensureEmu();
    if (emu_->busy())
    {
        emit statusMessage(tr("An emulator is already running."), 4000);
        return;
    }
    // Hand the screen + audio to the external emulator: stop our own playback first.
    emit aboutToLaunch();
    retro_->stop();

    pendingEmuRom_ = rom; pendingEmuTitle_ = title; pendingEmuThumb_ = thumb; pendingEmuKey_ = key; pendingEmuSystem_ = system;
    // Tell the emulator's SDL to ignore any phantom controller (e.g. a Keychron HE keyboard that presents a
    // gamepad interface). Otherwise it can take the first device slot and the emulator, bound to "SDL-0", listens
    // to the keyboard instead of the real pad. The child QProcess inherits these; we set both the SDL2 and SDL3
    // hint names since standalone emulators use either.
    if (retro_ && retro_->gamepad())
    {
        const std::string ignore = retro_->gamepad()->phantomControllerIgnoreList();
        if (!ignore.empty())
        {
            const QByteArray v = QByteArray::fromStdString(ignore);
            qputenv("SDL_JOYSTICK_IGNORE_DEVICES", v);       // SDL3 (DuckStation, current PCSX2…)
            qputenv("SDL_GAMECONTROLLER_IGNORE_DEVICES", v); // SDL2-based emulators
            glLog(QStringLiteral("emu: ignoring phantom controller(s) for the emulator: %1").arg(QString::fromUtf8(v)));
        }
        else { qunsetenv("SDL_JOYSTICK_IGNORE_DEVICES"); qunsetenv("SDL_GAMECONTROLLER_IGNORE_DEVICES"); }
    }
    emit waitPage(EmulatorManager::isInstalled(em)
                      ? tr("Starting %1…").arg(em.displayName)
                      : tr("%1 isn't installed yet — downloading it…").arg(em.displayName),
                  false);
    glLog(QStringLiteral("emu: run %1 \"%2\"")
              .arg(em.displayName, rom.isEmpty() ? QStringLiteral("(no game)") : QFileInfo(rom).fileName()));
    emu_->play(em, rom);
}

void GameLauncher::install(const ExternalEmulator& em)
{
    ensureEmu();
    emu_->install(em);
}

bool GameLauncher::emulatorBusy() const
{
    return emu_ && emu_->busy();
}

void GameLauncher::forceCloseEmulator()
{
    emuUserClosing_ = true;
    if (emu_) emu_->terminateGame();
}

// Start timing a game session: close any session still open, stamp last-played, and note the start time.
void GameLauncher::beginPlaySession(const QString& identity)
{
    endPlaySession();
    if (identity.isEmpty()) return;
    PlayStats::markPlayed(identity);
    activePlayId_ = identity;
    activePlayStart_ = QDateTime::currentSecsSinceEpoch();
}

// End the active session (if any) and bank its elapsed time into the game's total.
void GameLauncher::endPlaySession()
{
    if (activePlayId_.isEmpty()) return;
    const qint64 secs = QDateTime::currentSecsSinceEpoch() - activePlayStart_;
    PlayStats::addSession(activePlayId_, secs);
    activePlayId_.clear();
    activePlayStart_ = 0;
}
