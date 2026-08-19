#include "GameLauncher.h"
#include "../emu/RetroView.h"        // openGame/stop/gamepad() + RETRO_DEVICE_ID_JOYPAD_* (via LibretroCore.h)
#include "../emu/RetroParkView.h"    // Slice 2a: the RetroPark backend's play surface (driven refcore)
#include "../input/Gamepad.h"
#include "../core/AppPaths.h"
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
#include "../core/CoreManager.h"
#include "../core/ArchiveRom.h"
#include "../core/RomPatch.h"
#include "../core/EmulatorRegistry.h"
#include "../core/EmulatorManager.h"
#include "../core/LaunchOptionsStore.h"   // per-game core/emulator/args override (issue #51)
#include "../core/EmulationTarget.h"      // unified engine resolution: resolveLaunch (Unified Emulation Picker)
#include "../core/LaunchHooks.h"          // pure argv tokenizer + {rom} substitution (issue #64)
#include "../core/LaunchHooksStore.h"     // per-game pre-launch / post-exit command hooks (issue #64)
#include "../core/EmuGfxStore.h"          // per-game standalone-emulator graphics quartet override (issue #103)
#include "../core/DeviceProfileDetect.h"  // per-device tuned graphics defaults (weakest layer) (issue #119)
#include "../core/Pad2KeyStore.h"         // per-game pad-to-keyboard enable + profile (issue #105)
#include "../input/Pad2KeyRuntime.h"      // the SendInput injector that runs while a keyboard-only PC game holds focus
#include "../core/RecentStore.h"
#include "../core/PlayStats.h"
#include "../core/BiosCatalog.h"
#include "../core/PerfTrace.h"
#include <QTimer>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QSet>
#include <QRegularExpression>
#include <QDateTime>
#include <QProcess>
#include <QThread>
#include <QPointer>

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

// Per-game launch-hook timeouts (issue #64). A pre-hook runs to completion BEFORE the game launches, so it
// blocks the GUI thread — 30s is generous for the intended use (start a controller profile, mount a disc,
// toggle a resolution) yet bounds a runaway command so a launch can't hang for ever. The post-hook shares the
// bound; it is log-only, so a timeout is noted and dropped.
static constexpr int kPreHookTimeoutMs  = 30000;
static constexpr int kPostHookTimeoutMs = 30000;

// Run a user-authored hook command line to completion, argv-not-shell (issue #64). Desktop-only: standalone
// hooks are a desktop feature (the same posture as the external-emulator path), and on Android/iOS the sandbox
// can't spawn arbitrary child processes anyway. Returns true on a clean exit-0; on failure fills *err.
//
// The command line is tokenized (LaunchHooks::parseCommandLine) and {rom} is substituted AFTER tokenizing
// (LaunchHooks::substituteRom) so a spaced ROM path stays one argument, then run via QProcess::start(program,
// args) — the argv overload, NEVER the single-string overload that would re-parse through a shell-like split.
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
static bool runLaunchHook(const QString& commandLine, const QString& romPath, int timeoutMs, QString* err)
{
    QStringList argv = LaunchHooks::substituteRom(LaunchHooks::parseCommandLine(commandLine), romPath);
    if (argv.isEmpty()) return true;                  // nothing to run == nothing to fail
    const QString program = argv.takeFirst();
    QProcess proc;
    proc.start(program, argv);                        // argv overload: tokens are literal, no shell parsing
    if (!proc.waitForStarted(5000))
    {
        if (err) *err = QStringLiteral("couldn't start '%1'").arg(program);
        return false;
    }
    if (!proc.waitForFinished(timeoutMs))
    {
        proc.kill();
        proc.waitForFinished(2000);
        if (err) *err = QStringLiteral("timed out after %1 ms").arg(timeoutMs);
        return false;
    }
    if (proc.exitStatus() != QProcess::NormalExit)
    {
        if (err) *err = QStringLiteral("the command crashed");
        return false;
    }
    if (proc.exitCode() != 0)
    {
        if (err) *err = QStringLiteral("exit code %1").arg(proc.exitCode());
        return false;
    }
    return true;
}
#endif

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

GameLauncher::GameLauncher(RetroView* retro, RetroParkView* retroPark, QObject* parent)
    : QObject(parent), retro_(retro), retroPark_(retroPark)
{
    // Bank the elapsed session whenever the full-screen libretro game is torn down (the RetroView Esc-menu Exit,
    // or switching to other content). Symmetric to beginPlaySession() in the retro branch of open(); the session
    // state lives here, so this class owns the end trigger too.
    // The shared teardown handler for BOTH play surfaces: bank the elapsed session, then fire the post-exit hook
    // and clear its context. RetroView and RetroParkView are mutually exclusive (a game runs on exactly one
    // backend), and each fires gameStopped exactly once per game, so one lambda wired to both is correct.
    auto onGameStopped = [this] {
        endPlaySession();
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
        // Post-exit hook (issue #64) for the session that just ended. Fired here — the single teardown point for
        // the full-screen game (an Esc-menu Exit, or switching content) — then the context is cleared so a later
        // stop with no game loaded is a no-op.
        firePostHook(hookKey_, hookRom_);
        hookKey_.clear();
        hookRom_.clear();
#endif
    };
    connect(retro_, &RetroView::gameStopped, this, onGameStopped);
    if (retroPark_) connect(retroPark_, &RetroParkView::gameStopped, this, onGameStopped);
}

// Run the game's post-exit hook (issue #64), log-only: a failing post-hook is recorded and never blocks. No-op
// for an empty key or an unset hook. See runLaunchHook for the argv-not-shell contract.
void GameLauncher::firePostHook(const QString& key, const QString& rom)
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    if (key.isEmpty()) return;
    const QString post = LaunchHooksStore::get(key).postExit;
    if (post.isEmpty()) return;
    QString herr;
    if (!runLaunchHook(post, rom, kPostHookTimeoutMs, &herr))
        glLog(QStringLiteral("hook: post-exit failed (ignored): %1").arg(herr));
    else
        glLog(QStringLiteral("hook: post-exit ran OK"));
#else
    Q_UNUSED(key);
    Q_UNUSED(rom);
#endif
}

// Resolve an archived ROM (.zip/.7z) to a launchable path, looping in case an extracted member is itself an
// archive. Folder-tree systems (PS3 → RPCS3) extract the WHOLE archive and boot the game root (PS3_GAME dir /
// EBOOT.BIN); every other system extracts the single inner ROM. This is the ONE slow step in a launch — for a
// multi-GB game the LZMA decode takes a long time — so open() runs it OFF the GUI thread. Extraction is cached
// by archive path, so a second (warm) call from prepareCore on the GUI thread returns instantly. Returns the
// resolved path (a file, or a PS3 game folder); empty + *err on failure. `game` non-archive => returned as-is.
QString GameLauncher::resolveArchiveForLaunch(const QString& rom, const QString& systemHint, QString* err)
{
    QString game = rom;
    while (ArchiveRom::isArchive(game))
    {
        const GameSystem* hs = nullptr;
        if (!systemHint.isEmpty())
        {
            hs = SystemCatalog::byId(systemHint);
            if (!hs) hs = SystemCatalog::forConsoleName(systemHint);
        }
        QString extracted, aerr;
        if (hs && hs->externalEmulator == QStringLiteral("rpcs3")) // PS3: a whole game folder tree, not one file
            extracted = ArchiveRom::extractGameTree(game, &aerr);
        else
        {
            QStringList wanted;
            if (hs)
                for (const QString& e : hs->extensions)
                    wanted << (QStringLiteral(".") + e);
            extracted = ArchiveRom::extractToTemp(game, wanted, &aerr);
        }
        if (extracted.isEmpty()) { if (err) *err = aerr; return QString(); }
        game = extracted;
    }
    return game;
}

GameLauncher::CorePlan GameLauncher::prepareCore(const QString& rom, const QString& systemHint, const QString& key)
{
    CorePlan plan;
    // The reopenable source: what the user actually opened (an archive, a promoted ROMs-folder file, a plain
    // ROM). Recent/relaunch must remember THIS, not the extracted temp file below — otherwise reopening a
    // .zip/.7z game skips extraction and boots a lone .cue whose .bin siblings were never unpacked.
    plan.sourceRom = rom;
    // The per-game override (issue #51). Empty for an empty key (the split-pane libretro branch) — so
    // resolution below is byte-for-byte today's when there is nothing to override.
    const LaunchOpts::Override ov = key.isEmpty() ? LaunchOpts::Override{} : LaunchOpts::get(key);

    // Archived ROM (.zip / .7z): resolve to the inner ROM (or, for PS3, the extracted game folder). Every libretro
    // core and external emulator loads from a path, so this single spot handles archives for all of them. The heavy
    // extraction runs OFF the GUI thread in open() before we get here, so this call is normally a warm cache hit;
    // it still works (synchronously) on the split-pane path and any caller that reaches prepareCore directly.
    QString aerr;
    const QString game = resolveArchiveForLaunch(rom, systemHint, &aerr);
    if (game.isEmpty())
    {
        glLog(QStringLiteral("game: archive extract failed for \"%1\": %2").arg(QFileInfo(rom).fileName(), aerr));
        plan.error = tr("Couldn't open the archived game: %1").arg(aerr);
        plan.errorMs = kFeedbackLong;
        return plan;
    }
    if (game != rom)
        glLog(QStringLiteral("game: resolved \"%1\" from \"%2\"")
                  .arg(QFileInfo(game).fileName(), QFileInfo(rom).fileName()));

    const QString ext = QFileInfo(game).suffix().toLower();
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
              .arg(QFileInfo(game).fileName(), ext,
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
    plan.sys = sys;    // borrowed static-catalog pointer; lets callers skip a second SystemCatalog::byId lookup

    // If the user opened a raw disc track (a "(Track N).bin" / GDI track), boot its .cue/.gdi descriptor instead —
    // the emulator can't mount a bare track. No-op for direct images and lone .bin carts. Covers cores + externals.
    QString launchRom = resolveDiscDescriptor(game);
    if (launchRom != game)
        glLog(QStringLiteral("game: track \"%1\" -> disc descriptor \"%2\"")
                  .arg(QFileInfo(game).fileName(), QFileInfo(launchRom).fileName()));

    // Soft-patch: if a sidecar IPS/BPS/UPS patch sits beside the ROM, apply it into a derived cache file and
    // launch THAT — the original ROM on disk stays byte-for-byte untouched. This is the one seam every launch
    // (libretro core and standalone emulator alike) funnels through, so both get patching for free. A patch
    // that is present but bad (wrong magic, or a checksum that says it was built for a different ROM) is a hard
    // error here: we must not fall through and boot the unpatched ROM as if nothing were wrong, because the
    // user put that patch there deliberately. Disc descriptors are left alone — CD-image patching is a
    // separate, streaming path (issue #128 defers it), and sidecarPatchFor keys on the ROM's own base name.
    QString perr;
    const QString patched = RomPatch::resolvePatchedRom(launchRom, &perr);
    if (patched.isEmpty())
    {
        glLog(QStringLiteral("game: soft-patch failed for \"%1\": %2").arg(QFileInfo(launchRom).fileName(), perr));
        plan.error = perr.isEmpty() ? tr("Couldn't apply the ROM patch.") : perr;
        plan.errorMs = kFeedbackLong;
        return plan;
    }
    if (patched != launchRom)
    {
        glLog(QStringLiteral("game: soft-patched \"%1\" -> \"%2\"")
                  .arg(QFileInfo(launchRom).fileName(), QFileInfo(patched).fileName()));
        launchRom = patched;
    }
    plan.launchRom = launchRom;

    // ---- Unified Emulation Picker (Task 3): source the effective engine through resolveEmulationTarget --------
    // ONE resolver now produces the effective run-target — per-game override → per-system default → system
    // built-in — reusing the SAME LaunchOpts::resolveBackend / resolveCore / resolveEmulatorId resolvers this
    // used to call inline on each arm. resolveLaunch composes them with the two launch-time RetroPark gates the
    // pure model leaves to us — `retroParkAvailable` (the cross-platform clamp) and `dolphinVehiclePresent` (the
    // Slice-3b local-only vehicle gate for the PRESENTING gc/Dolphin core) — and hands back the CorePlan-relevant
    // fields. Every un-opted launch is byte-for-byte identical to the pre-Task-3 arms: a default NES resolves to
    // libretro/fceumm (backend Libretro, no externalEmulatorId), a default gc to externalEmulatorId=dolphin.
#ifdef EB_HAVE_RETROPARK
    const bool retroParkAvailable = true;
    // The Dolphin vehicle is LOCAL-ONLY: dolphin_present.dll is git-ignored, unbuildable by EB, and absent on most
    // machines (only core.json is tracked). It is the same <coresDir>/dolphin_present/dolphin_present.dll that
    // RetroParkView loads the presenting core from — a PRESENTING RetroPark target (gc) is honoured only when it is
    // actually staged, otherwise resolveLaunch degrades to the external-Dolphin launch (the 3b clamp, no brick).
    const bool dolphinVehiclePresent = QFileInfo::exists(
        CoreManager::coresDir() + QStringLiteral("/dolphin_present/dolphin_present.dll"));
#else
    // No RetroParkView on this build and no on-device picker to change the setting (both #ifdef EB_HAVE_RETROPARK):
    // a synced backend=retropark degrades to the underlying engine so it can never route open() to an inert surface.
    const bool retroParkAvailable = false;
    const bool dolphinVehiclePresent = false;
#endif
    const ResolvedLaunch rl = resolveLaunch(sys, ov, Settings::coreFor(sys->id), Settings::emulatorFor(sys->id),
                                            Settings::backendFor(sys->id), retroParkAvailable, dolphinVehiclePresent);

    // Standalone-emulator engine (GameCube/Wii → Dolphin, or a presenting RetroPark target whose vehicle was
    // absent) has no libretro core to prepare — resolution stops here with the emulator id set and no
    // error/corePath. open() routes these to a child-process emulator. Byte-for-byte today's for an un-opted game
    // (externalEmulatorId == sys->externalEmulator).
    if (rl.engine == EmuEngine::Standalone)
    {
        plan.externalEmulatorId = rl.externalEmulatorId;
        if (plan.externalEmulatorId != sys->externalEmulator)
            glLog(QStringLiteral("game: standalone emulator '%1' for system %2 (system default '%3')")
                      .arg(plan.externalEmulatorId, sys->id, sys->externalEmulator));
        return plan;
    }

    // A PRESENTING RetroPark target (gc → Dolphin, in-process on a Vulkan runtime) replaces the external emulator
    // entirely: externalEmulatorId is left empty so open() skips the external branch and reaches
    // finishRetroParkLaunch; launchRom (set above) carries the ISO, and retroparkPresenting tells the view to
    // create a Vulkan runtime. There is no libretro core to resolve or download, so resolution stops here.
    if (rl.engine == EmuEngine::RetroPark && rl.retroparkPresenting)
    {
        plan.backend = EmuBackend::RetroPark;
        plan.retroparkPresenting = true;
        glLog(QStringLiteral("game: system %1 opted onto RetroPark presenting backend — in-process path, "
                             "not external %2").arg(sys->id, sys->externalEmulator));
        return plan;
    }

    // Libretro engine, OR a DRIVEN RetroPark target (the built-in NES shim) which still resolves + carries
    // plan.core (finishRetroParkLaunch passes it to RetroParkView::openGame). rl.backend is RetroPark only for an
    // honoured driven RetroPark target, Libretro otherwise — so an un-opted game stays on today's libretro path.
    const QString core = rl.core;
    glLog(QStringLiteral("game: core '%1' for system %2 (configured=%3, backend=%4)")
              .arg(core, sys->id,
                   Settings::coreFor(sys->id).isEmpty() ? QStringLiteral("no, default") : QStringLiteral("yes"),
                   backendToString(rl.backend)));
    if (core.isEmpty())
    {
        plan.error = tr("No core is available for %1.").arg(sys->name);
        return plan;
    }

    // Resolution only: a missing core is NOT an error here — corePath stays empty with `core` set, and the
    // caller downloads it asynchronously (ensureCoreAsync) so the GUI thread never waits on the buildbot.
    plan.core = core;
    if (CoreManager::isInstalled(core))
        plan.corePath = CoreManager::corePath(core);
    plan.backend = rl.backend;
    return plan;
}

// Resolve plan.corePath — immediately when the core is installed, else via an async buildbot download with
// progress on the Notifier toast (the status bar is hidden app-wide) — then hand the completed plan to
// onReady. On failure onReady never runs; the error surfaces on the toast. Parented to `context` like the
// BIOS fetch: a superseded/dead launch cancels the download and drops the continuation.
void GameLauncher::ensureCoreThen(const CorePlan& plan, QObject* context,
                                  const std::function<void(const CorePlan&)>& onReady)
{
    CoreManager::ensureCoreAsync(plan.core, context,
        [this, core = plan.core](int pct) {
            const QString s = tr("Downloading core ‘%1’… %2%").arg(core).arg(pct);
            emit statusMessage(s, 0);
            emit notifyUser(s, 8000); // bounded, renewed per update: clears itself shortly after the launch proceeds
        },
        [this, plan, onReady](const QString& corePath, const QString& error) {
            if (corePath.isEmpty())
            {
                glLog(QStringLiteral("game: core '%1' unavailable: %2")
                          .arg(plan.core, error.isEmpty() ? QStringLiteral("download failed") : error));
                emit waitPageDone(); // clear the "Preparing…" page if this was an archived launch (no-op otherwise)
                emit notifyUser(error.isEmpty() ? tr("Couldn't download core ‘%1’.").arg(plan.core) : error,
                                kFeedbackLong);
                return;
            }
            glLog(QStringLiteral("game: core ready at %1").arg(QFileInfo(corePath).fileName()));
            CorePlan ready = plan;
            ready.corePath = corePath;
            onReady(ready);
        });
}

void GameLauncher::open(const QString& rom, const QString& title, const QString& thumb, const QString& key,
                        const QString& systemHint)
{
    // A new launch supersedes any pending async extraction from a prior open(): destroying extractCtx_ drops
    // its queued continuation (Qt removes the receiver-side connection). Done for BOTH paths — even a plain
    // non-archive launch must cancel a prior archive's still-running extraction so it can't boot on top later.
    delete extractCtx_;
    extractCtx_ = new QObject(this);

    // A non-archive ROM needs no extraction — launch it directly (byte-for-byte the old synchronous path).
    if (!ArchiveRom::isArchive(rom)) { openResolved(rom, title, thumb, key, systemHint); return; }

    // An archived ROM is extracted first. For a multi-GB game (a PS3 folder, a big disc image) the LZMA decode
    // takes many seconds — doing it inline froze the whole UI. Run it on a worker thread, show a "Preparing…"
    // wait page, and continue the launch once it lands.
    QObject* ectx = extractCtx_; // set above for BOTH paths; a superseding open() destroys it -> continuation dropped
    const QString label = title.isEmpty() ? QFileInfo(rom).completeBaseName() : title;
    emit waitPage(tr("Preparing “%1”…\n\nLarge games can take a minute to unpack.").arg(label), false);

    // The worker lambda captures NO `this` (resolveArchiveForLaunch is static), so it stays safe even if
    // GameLauncher is destroyed mid-extraction; the result lands in shared state the continuation reads.
    auto resultPath = std::make_shared<QString>();
    auto resultErr  = std::make_shared<QString>();
    QThread* worker = QThread::create([rom, systemHint, resultPath, resultErr]() {
        *resultPath = resolveArchiveForLaunch(rom, systemHint, resultErr.get());
    });
    // Continuation runs on the GUI thread (ectx lives there). Tying it to ectx makes Qt drop it if ectx is
    // destroyed — by a superseding open() (fixes booting a stale game) OR by GameLauncher's own destruction
    // (ectx is its child), so the captured `this` is never used after free.
    QObject::connect(worker, &QThread::finished, ectx,
        [this, rom, title, thumb, key, systemHint, resultPath, resultErr]() {
            if (resultPath->isEmpty())
            {
                glLog(QStringLiteral("game: archive extract failed for \"%1\": %2")
                          .arg(QFileInfo(rom).fileName(), *resultErr));
                emit waitPageDone();
                emit notifyUser(tr("Couldn't open the archived game: %1").arg(*resultErr), kFeedbackLong);
                return;
            }
            // prepareCore (inside openResolved) re-runs resolveArchiveForLaunch, now a warm cache hit — instant.
            // openResolved manages launchCtx_ (a DIFFERENT object than ectx), so it never deletes its own caller.
            openResolved(rom, title, thumb, key, systemHint);
        });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void GameLauncher::openResolved(const QString& rom, const QString& title, const QString& thumb, const QString& key,
                                const QString& systemHint)
{
    // Resolve the system, disc descriptor, and (for a libretro system) the core. The archive (if any) is already
    // extracted, so prepareCore's resolve is a warm cache hit. prepareCore short-circuits for standalone-emulator
    // systems (externalEmulatorId set) — we route those to the external-emulator branch below.
    const CorePlan plan = prepareCore(rom, systemHint, key);

    // The Recent entry shows the catalog item's name/cover when we have them; otherwise the descriptor's file
    // name. A remote ROM is cached under a hashed file name, so without the passed title it would show as that hash.
    const QString launchRom = plan.launchRom.isEmpty() ? rom : plan.launchRom;
    const QString recentTitle = title.isEmpty() ? QFileInfo(launchRom).completeBaseName() : title;

    // Pre-launch hook (issue #64): a user-authored local command run to COMPLETION before the game launches —
    // start a controller-mapping profile, mount a disc image, toggle a resolution. A failing pre-hook (non-zero
    // exit, crash, or timeout) ABORTS the launch with a visible error and we do not proceed. No key => no hook
    // (the split-pane branch has none); an empty pre-hook is byte-for-byte today's launch. Desktop-only.
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    if (!key.isEmpty())
    {
        const QString pre = LaunchHooksStore::get(key).preLaunch;
        if (!pre.isEmpty())
        {
            QString herr;
            if (!runLaunchHook(pre, launchRom, kPreHookTimeoutMs, &herr))
            {
                glLog(QStringLiteral("hook: pre-launch failed, aborting launch: %1").arg(herr));
                emit waitPageDone(); // clear the "Preparing…" page (no-op if it wasn't showing)
                emit statusMessage(tr("Pre-launch command failed — launch aborted."), kFeedbackLong);
                emit notifyUser(tr("Pre-launch command failed: %1").arg(herr), kFeedbackLong);
                return;
            }
            glLog(QStringLiteral("hook: pre-launch ran OK"));
        }
    }
#endif

    // Standalone-emulator systems (GameCube/Wii → Dolphin) launch an external process instead of a core.
    // Not possible on Android (the sandbox can't spawn downloaded desktop executables - see android-port.md).
    if (!plan.externalEmulatorId.isEmpty())
    {
        const GameSystem* sys = plan.sys; // resolved in prepareCore (borrowed static-catalog pointer)
#if defined(Q_OS_ANDROID)
        emit statusMessage(tr("“%1” needs a standalone emulator, which isn't supported on Android.")
                               .arg(sys ? sys->name : plan.systemId), 6000);
#else
        launchExternalGame(sys, plan.externalEmulatorId, launchRom, recentTitle, thumb, key, plan.sourceRom);
#endif
        return;
    }

    if (!plan.error.isEmpty())
    {
        emit waitPageDone(); // clear the "Preparing…" page (no-op if it wasn't showing)
        emit notifyUser(plan.error, plan.errorMs);
        return;
    }

    // The third launch branch (Slice 2a): a libretro system the user (or its per-system default) opted onto the
    // RetroPark backend runs in RetroParkView, not RetroView + a libretro core. Only libretro systems reach here
    // (standalone-emulator systems returned above), and only when the resolved backend is RetroPark — every other
    // game (backend == Libretro, the default) falls through to the unchanged libretro tail below. RetroPark ignores
    // corePath/BIOS, so it does NOT go through ensureCoreThen/ensureBiosAsync; the plan is already fully resolved.
    if (plan.backend == EmuBackend::RetroPark)
    {
        finishRetroParkLaunch(plan, launchRom, recentTitle, thumb, key);
        return;
    }

    // Download the core (if missing), then any BIOS the system needs, then run the launch tail. Both
    // fetches are asynchronous (no nested event loop, so nothing on the GUI thread waits on the network):
    // open() returns, progress shows on the Notifier toast, and finishLibretroLaunch runs once the files
    // land. With everything already on disk the whole chain completes synchronously right here — a warm
    // launch is unchanged. The chain is parented to a per-launch context, so a newer open() supersedes
    // (cancels) a still-downloading one instead of both booting when their downloads finish.
    delete launchCtx_;
    launchCtx_ = new QObject(this);
    ensureCoreThen(plan, launchCtx_, [this, launchRom, recentTitle, thumb, key](const CorePlan& ready) {
        // Some systems (3DO, Saturn, PlayStation) need a BIOS in the libretro system folder. Fetch any
        // that are missing before the core loads — best-effort, so a failure just falls back to the
        // core's own "BIOS not found" message rather than blocking the launch.
        CoreManager::ensureBiosAsync(ready.systemId, CoreManager::systemDir(), launchCtx_,
            [this](const QString& s) {
                emit statusMessage(s, 0);
                // The main window's status bar is hidden app-wide, so surface the wait visibly: the Notifier
                // toast is the app's download-progress channel. Each file's message renews it; the bounded
                // duration lets it clear itself shortly after the launch proceeds.
                emit notifyUser(s, 8000);
            },
            [this, ready, launchRom, recentTitle, thumb, key] {
                finishLibretroLaunch(ready, launchRom, recentTitle, thumb, key);
            });
    });
}

void GameLauncher::finishLibretroLaunch(const CorePlan& plan, const QString& launchRom, const QString& recentTitle,
                                        const QString& thumb, const QString& key)
{
    // This surface is about to own the screen, so an external launch still waiting on an install/firmware
    // update must not boot over it minutes from now. Done here in the tail rather than at open()'s top so
    // external launches keep hitting runEmulator's busy-refusal instead of superseding each other.
    cancelPendingEmulatorLaunch();
    emit aboutToLaunch();
    QString err;
    // recentTitle + plan.systemId travel into the view so its save files can be named to the user later and
    // filed under the console the item was actually opened from (a shared extension resolves ambiguously).
    // `key` (the catalog item's stable id, else empty) keys this game's per-game overrides (#95) — the same
    // identity RecentStore de-dups on and PlayStats accrues under, so overrides follow the game, not the path.
    if (retro_->openGame(plan.corePath, launchRom, plan.core, &err, recentTitle, plan.systemId, key))
    {
        glLog(QStringLiteral("game: running \"%1\"").arg(recentTitle));
        emit showRetroRequested();
        RecentStore::add({ plan.sourceRom.isEmpty() ? launchRom : plan.sourceRom, recentTitle,
                           QStringLiteral("game"), thumb, key, plan.systemId });
        beginPlaySession(PlayStats::identity(key, launchRom));
        // Remember this game so its post-exit hook (issue #64) can fire when RetroView::gameStopped ends the
        // session. Captured only on a successful load; cleared in the gameStopped handler.
        hookKey_ = key;
        hookRom_ = launchRom;
        PerfTrace::end(QStringLiteral("open.game"), QFileInfo(launchRom).fileName()); // libretro: measured to core load
    }
    else
    {
        glLog(QStringLiteral("game: openGame failed: %1").arg(err));
        emit waitPageDone(); // clear the "Preparing…" page (no-op if it wasn't showing)
        emit notifyUser(tr("Can't run game: %1").arg(err), kFeedbackLong);
    }
}

// The RetroPark launch tail (Slice 2a) — the third branch beside finishLibretroLaunch, taken when a libretro
// system's resolved backend is RetroPark. It drives RetroParkView exactly as finishLibretroLaunch drives
// RetroView: stop outgoing playback, load the game (2a loads the driven reference core — the ROM is ignored),
// and on success signal the host to show the RetroPark page + record the Recent entry and play session. The
// backend is the ONLY difference from the libretro tail; the Recent/PlayStats/hook bookkeeping is identical, so
// a RetroPark game behaves like any other game everywhere outside the emulator itself.
void GameLauncher::finishRetroParkLaunch(const CorePlan& plan, const QString& launchRom, const QString& recentTitle,
                                         const QString& thumb, const QString& key)
{
    // Build-time precondition FIRST, above the supersession below: on a build without RetroPark this tail can
    // never own the screen, so cancelling the user's pending external launch here would destroy that launch and
    // then tell them this one isn't available either — they'd lose both. Bail before touching anything.
    if (!retroPark_)
    {
        glLog(QStringLiteral("game: RetroPark backend requested but no RetroParkView on this build"));
        emit waitPageDone(); // clear the "Preparing…" page (no-op if it wasn't showing)
        emit notifyUser(tr("RetroPark is not available in this build."), kFeedbackLong);
        return;
    }
    // Same supersession as the libretro tail: an external launch pending behind an install/firmware update
    // must be cancelled before this surface takes the screen, or it boots on top of the game minutes later.
    cancelPendingEmulatorLaunch();
    emit aboutToLaunch();
    // Tear down the OTHER play surface before starting, mirroring how finishLibretroLaunch's aboutToLaunch path
    // stops outgoing playback: a launch while libretro was still running would otherwise leave RetroView's timer
    // and bookkeeping live behind the hidden page.
    retro_->stop();
    QString err;
    // Mirrors finishLibretroLaunch's identity set: plan.core is the resolved core id, recentTitle/systemId name
    // the game + the console it was opened from, key is its stable per-game override id. plan.retroparkPresenting
    // (Slice 3b) tells the view whether to create a headless VULKAN runtime for the Dolphin presenting core (gc)
    // vs the D3D11 driven core (NES) — chosen at rp_runtime_create, so it must be passed BEFORE the core loads.
    retroPark_->openGame(plan.core, launchRom, recentTitle, plan.systemId, key, &err, plan.retroparkPresenting);
    if (retroPark_->running())
    {
        glLog(QStringLiteral("game: running \"%1\" on RetroPark").arg(recentTitle));
        // Diagnostic (Slice 3b input fix): RetroParkView now feeds input off the app's SHARED gamepad
        // (retro_->gamepad()). Log what that shared pad sees at launch so a failed controller feed is diagnosable
        // from stream_debug.log alone — connected count + port-0 name (no credential/identifier values).
        if (retro_ && retro_->gamepad()) {
            Gamepad* pad = retro_->gamepad();
            glLog(QStringLiteral("retropark: gamepad \"%1\" connected=%2")
                      .arg(QString::fromStdString(pad->name(0)))
                      .arg(pad->connectedCount()));
        }
        emit showRetroParkRequested();
        RecentStore::add({ plan.sourceRom.isEmpty() ? launchRom : plan.sourceRom, recentTitle,
                           QStringLiteral("game"), thumb, key, plan.systemId });
        beginPlaySession(PlayStats::identity(key, launchRom));
        // Post-exit hook context (issue #64), captured on a successful load, cleared in the gameStopped handler —
        // the same wiring as the libretro tail (RetroParkView::gameStopped ends the session).
        hookKey_ = key;
        hookRom_ = launchRom;
        PerfTrace::end(QStringLiteral("open.game"), QFileInfo(launchRom).fileName());
    }
    else
    {
        glLog(QStringLiteral("game: RetroPark openGame failed: %1").arg(err));
        emit waitPageDone(); // clear the "Preparing…" page (no-op if it wasn't showing)
        emit notifyUser(tr("Can't run game: %1").arg(err.isEmpty() ? tr("RetroPark failed to start") : err),
                        kFeedbackLong);
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
        // Update the wait-page label only if it's already the current view — an install-only flow (Settings ▸
        // Emulators) must NOT be yanked onto the wait page and stranded there. runEmulator/launched switch views.
        emit waitPageStatus(line);
        emit emulatorInstallProgress(t, pct);   // themed Emulators panel: tick the emulator's status row in place
    });
    // The pre-boot prep phase began (BIOS/keys prep, the RPCS3 firmware/update worker — worst case
    // ~30 min): everything pending is now cancellable, so put the Stop button up. Back/Esc and Stop
    // route through forceCloseEmulator -> cancelPendingLaunch until the process actually starts.
    connect(emu_, &EmulatorManager::bootPending, this, [this](const QString& name) {
        emit waitPage(tr("Starting %1…").arg(name), true);
    });
    connect(emu_, &EmulatorManager::launched, this, [this](const QString& name) {
        emit waitPage(tr("Playing in %1.\n\nClose the %1 window — or press Start+Select on your controller, "
                         "or Esc — to return to EverythingBox.").arg(name), true);
        if (!pendingEmuRom_.isEmpty()) // record now that it actually started
        {
            RecentStore::add({ pendingEmuSource_.isEmpty() ? pendingEmuRom_ : pendingEmuSource_, pendingEmuTitle_,
                               QStringLiteral("game"), pendingEmuThumb_, pendingEmuKey_, pendingEmuSystem_ });
            beginPlaySession(PlayStats::identity(pendingEmuKey_, pendingEmuRom_));
        }
        // Step aside so the emulator is unobstructed and in front; we restore when it exits. (Our window is
        // often full screen and would otherwise sit on top of the freshly-launched emulator.)
        emit minimizeRequested();
        emuDisplayName_ = name;
        emuUserClosing_ = false;
        emuRunClock_.start();  // to spot a boot that fails and exits instantly (missing BIOS/firmware)
        startEmuHotkeyWatch(); // Start+Select / Esc closes the standalone emulator back to EB
        startPad2Key();        // issue #105: synthesise keystrokes from the pad IF this game has pad2key enabled
    });
    connect(emu_, &EmulatorManager::finished, this, [this](int code) {
        glLog(QStringLiteral("emu: process exited (code %1)").arg(code));
        stopEmuHotkeyWatch();
        stopPad2Key();    // release any key pad2key was holding BEFORE we return focus to the app (issue #105)
        endPlaySession(); // bank the external emulator's play time
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
        // Post-exit hook (issue #64): the standalone emulator's QProcess has finished — including a Stop-button
        // or exit-hotkey close, which route here the same way — so fire the game's post-exit command, log-only.
        firePostHook(pendingEmuKey_, pendingEmuRom_);
#endif

        emit restoreRequested(); // come back to where we were before handing off to the emulator
        emit waitPageDone();

        // Closed within a couple of seconds, and we didn't ask it to? That's a failed boot, not a play session —
        // most often a missing console BIOS/firmware, which -batch-style launches quit on silently. Tell the user
        // what happened instead of just bouncing back to the home screen with no explanation.
        if (!emuUserClosing_ && emuRunClock_.isValid() && emuRunClock_.elapsed() < 4000)
        {
            const QString emuName = emuDisplayName_.isEmpty() ? tr("The emulator") : emuDisplayName_;
            const GameSystem* sys = pendingEmuSystem_.isEmpty() ? nullptr : SystemCatalog::byId(pendingEmuSystem_);
            // This is the standalone-emulator exit path, so the relevant question is whether THAT emulator
            // needs a console BIOS to boot (BiosCatalog maps emulator id -> the system whose BIOS it needs).
            const bool needsBios = sys && !sys->externalEmulator.isEmpty()
                                   && !BiosCatalog::forExternalEmulator(sys->externalEmulator).systemId.isEmpty();
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
        emit statusMessage(tr("%1 is installed.").arg(name), kFeedbackShort);
        emit emulatorInstallFinished(name);
    });
    connect(emu_, &EmulatorManager::failed, this, [this](const QString& msg) {
        glLog(QStringLiteral("emu: failed: %1").arg(msg));
        stopEmuHotkeyWatch();
        stopPad2Key();    // issue #105: never leave a synthesised key held if the launch failed mid-run
        emit statusMessage(msg, kFeedbackLong);
        emit waitPageDone();
        emit emulatorInstallFailed(msg);
    });
}

// ---- Standalone-emulator exit hotkey: close melonDS/Dolphin/etc. back to EB on Start+Select or Esc ---------
// A libretro core shows EB's own pause menu on Start+Select; a standalone emulator is a separate process we
// can't inject a menu into, so the RetroBat-equivalent is to close it and come back. We poll while EB is
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

// ---- Pad-to-keyboard (issue #105): synthesise keystrokes for a keyboard-only PC game while it holds focus -----
// Gated ENTIRELY on the per-game enable bit: an ordinary emulator (which reads the pad itself) is never touched,
// so double-input is impossible unless the user deliberately flags a game. The profile is the game's custom one
// if it has authored one, else the per-system default (the DOS default for msdos). Borrows RetroView's Gamepad,
// which is idle while a standalone emulator owns the screen — the same device the exit-hotkey watch polls.
void GameLauncher::startPad2Key()
{
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    if (pendingEmuKey_.isEmpty() || !Pad2KeyStore::enabled(pendingEmuKey_)) return;
    Gamepad* pad = retro_ ? retro_->gamepad() : nullptr;
    if (!pad || !pad->available()) { glLog(QStringLiteral("pad2key: enabled but no controller available")); return; }
    const pad2key::Profile prof = Pad2KeyStore::effectiveProfile(pendingEmuKey_, pendingEmuSystem_);
    if (prof.isEmpty()) { glLog(QStringLiteral("pad2key: enabled but no profile resolves for this game/system")); return; }
    if (!pad2key_) pad2key_ = new Pad2KeyRuntime(this);
    pad2key_->start(pad, prof);
    glLog(QStringLiteral("pad2key: injecting '%1' (%2 mappings)").arg(prof.name).arg(prof.map.size()));
#endif
}

void GameLauncher::stopPad2Key()
{
    if (pad2key_) pad2key_->stop();   // idempotent; releases every held key
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

void GameLauncher::launchExternalGame(const GameSystem* sys, const QString& emulatorId, const QString& rom,
                                      const QString& title, const QString& thumb, const QString& key,
                                      const QString& sourceRom)
{
    // emulatorId is the RESOLVED id (sys->externalEmulator, or a per-game override #51) — use it, not the
    // system's default, so the override actually reaches the launch. A bogus/retired override id (or the
    // ordinary missing-registration case) surfaces the same error.
    const ExternalEmulator* em = EmulatorRegistry::byId(emulatorId);
    if (!em)
    {
        glLog(QStringLiteral("game: external emulator '%1' not registered").arg(emulatorId));
        emit waitPageDone(); // clear the "Preparing…" page (no-op if it wasn't showing)
        emit statusMessage(tr("No emulator is configured for %1.").arg(sys->name), kFeedbackLong);
        return;
    }
    runEmulator(*em, rom, title, thumb, key, sys->id, sourceRom);
}

void GameLauncher::runEmulator(const ExternalEmulator& em, const QString& rom, const QString& title,
                               const QString& thumb, const QString& key, const QString& system,
                               const QString& sourceRom)
{
    ensureEmu();
    if (emu_->busy())
    {
        emit statusMessage(tr("An emulator is already running."), kFeedbackLong);
        // The status bar is hidden app-wide, so that message reaches nobody on its own. After a demote (the
        // superseded launch's download still running) this refusal is the ONLY thing telling the user why
        // pressing play did nothing, so it has to reach the toast as well.
        emit notifyUser(tr("An emulator is already running."), kFeedbackLong);
        return;
    }
    // Hand the screen + audio to the external emulator: stop our own playback first.
    emit aboutToLaunch();
    retro_->stop();

    pendingEmuRom_ = rom; pendingEmuTitle_ = title; pendingEmuThumb_ = thumb; pendingEmuKey_ = key; pendingEmuSystem_ = system;
    pendingEmuSource_ = sourceRom.isEmpty() ? rom : sourceRom; // Recent stores the reopenable source, not the boot file
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
    // Per-game extra command-line args (issue #51), appended to the emulator's resolved argsTemplate at launch.
    // Only meaningful with a game key (a bare "open the emulator UI" run has none) and empty unless the user set
    // an override, so a launch with no override passes the empty string and the args are byte-for-byte today's.
    const QString extraArgs = key.isEmpty() ? QString() : LaunchOpts::get(key).extraArgs;
    // Graphics quartet, resolved as a THREE-layer chain: per-game (issue #103) over per-system default (#103)
    // over this DEVICE's tuned profile default (issue #119). Precedence is per-game > per-system > per-device >
    // unset — the device layer is the WEAKEST, so it only fills levers neither the game nor the system set, and
    // an Unknown / no-opinion machine yields an all-unset device default (a no-op: launches byte-for-byte as
    // before). The device default is keyed by the emulator id and applies even to a bare "open the UI" run,
    // since a hardware-appropriate resolution is a property of the machine, not of a particular game.
    const EmuGfx::Settings deviceDefault = DeviceProfileDetect::defaultsForEmulator(em.id);
    const EmuGfx::Settings gfx = key.isEmpty()
        ? deviceDefault
        : EmuGfx::resolve(EmuGfxStore::get(key),
                          EmuGfx::resolve(EmuGfxStore::systemDefault(system), deviceDefault));
    emu_->play(em, rom, extraArgs, gfx);
    PerfTrace::end(QStringLiteral("open.game"), em.displayName); // external: measured to process handoff
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
    if (!emu_) return;
    emu_->terminateGame();        // running game: hard kill (no-op pre-boot, game_ is null)
    // Pre-boot: retire the launch instead (no-op once game_ exists). A cancel during the install/download
    // phase is a DEMOTE — the download deliberately runs to completion — and the failed() message saying so
    // lands on the app-wide-hidden status bar, so surface that one fact on the toast: without it the user who
    // pressed Stop/Back sees an unexplained "<emulator> is installed." minutes later.
    if (emu_->cancelPendingLaunch() == EmulatorManager::PendingCancel::CancelledDownloadContinues)
        emit notifyUser(tr("The emulator download that launch started will finish in the background."),
                        kFeedbackStandard);
}

bool GameLauncher::cancelPendingEmulatorLaunch()
{
    // No manager yet => nothing was ever launched externally, so there is nothing pending. ensureEmu() is
    // deliberately NOT called: creating the manager just to ask it whether it is idle would be backwards.
    const EmulatorManager::PendingCancel cancelled = emu_ ? emu_->cancelPendingLaunch()
                                                          : EmulatorManager::PendingCancel::None;
    if (cancelled == EmulatorManager::PendingCancel::None) return false;
    glLog(QStringLiteral("emu: pending launch superseded by another frontend"));
    // The failed() path's statusMessage lands on the app-wide-hidden status bar, so the toast is the visible
    // channel. pendingEmuTitle_ is accurate here: a true cancel means a launch really was pending, and the
    // runEmulator that started it set these. An empty title is a bare "open the emulator's own UI" run.
    QString msg = pendingEmuTitle_.isEmpty()
                      ? tr("Cancelled the pending emulator launch.")
                      : tr("Cancelled the pending launch of “%1”.").arg(pendingEmuTitle_);
    // Being the only visible channel, the toast must also carry the demote arm's one important fact: the
    // download the cancelled launch started is NOT cancelled with it and runs to completion. Without this
    // sentence the user sees an unexplained "<emulator> is installed." minutes later with nothing that
    // connects it to the launch they superseded.
    if (cancelled == EmulatorManager::PendingCancel::CancelledDownloadContinues)
        msg += QStringLiteral(" ") + tr("The emulator download it started will finish in the background.");
    emit notifyUser(msg, kFeedbackStandard);
    return true;
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
