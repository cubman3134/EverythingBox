// The game-launch pipeline + external-emulator lifecycle, carved out of MainWindow. Given a ROM path it
// resolves the system, disc descriptor, and libretro core (downloading the core/BIOS if needed) and either
// loads it into the shared RetroView or hands the game to a standalone emulator (Dolphin/PCSX2/…), which it
// installs, launches, and monitors — minimising the app while it runs and watching a global exit hotkey
// (Start+Select on a pad, or Esc) to close the emulator back to the app, the way RetroBat does. The host
// (MainWindow) owns the actual window state + the "playing in <emulator>" wait page; this class drives them
// via signals so the touchy process/window bits stay observable and testable.
#pragma once
#include <QObject>
#include <QString>
#include <QElapsedTimer>
#include <functional>
#include "../ui/FeedbackPolicy.h"   // kFeedbackLong — error-class notice duration
#include "../core/EmuBackend.h"     // CorePlan::backend — which engine a resolved game launches on (Slice 2a)
#include "LaunchContexts.h"         // the per-launch continuation contexts + who retires which, and when

class RetroView;
class RetroParkView;   // Slice 2a: the RetroPark backend's play surface (sibling of RetroView)
class EmulatorManager;
class Pad2KeyRuntime;
class QTimer;
struct GameSystem;
struct ExternalEmulator;

class GameLauncher : public QObject
{
    Q_OBJECT
public:
    // `retroPark` is the RetroPark play surface (Slice 2a), the sibling page a game opts onto the RetroPark
    // backend runs in. Owned by the host (MainWindow), like `retro`; may be null on a build without RetroPark.
    explicit GameLauncher(RetroView* retro, RetroParkView* retroPark, QObject* parent = nullptr);

    // The full launch pipeline: archive → system → disc descriptor → core/BIOS → RetroView or external emulator.
    // title/thumb/key carry the catalog item's display name + cover + stable id for the Recent entry; systemHint
    // is the console/platform the game was opened from, to disambiguate extensions shared across systems.
    void open(const QString& rom, const QString& title = QString(), const QString& thumb = QString(),
              const QString& key = QString(), const QString& systemHint = QString());

    // Resolve an archived ROM (.zip/.7z) to a launchable path — the inner ROM, or for PS3 the extracted game
    // folder. The ONE slow step in a launch (a multi-GB LZMA decode); open() runs it on a worker thread so the
    // UI can't freeze, then re-uses the warm extraction cache. Empty + *err on failure. Public + static so it
    // has no instance state and can run safely off the GUI thread.
    static QString resolveArchiveForLaunch(const QString& rom, const QString& systemHint, QString* err);

    // The pipeline's resolution half (system + disc descriptor + core lookup), reused by MainWindow's split-pane
    // branch to run a ROM in the focused pane's own emulator. Resolution only — no network: `error` non-empty =>
    // couldn't resolve; corePath empty with `core` set => the core isn't installed yet, and the caller downloads
    // it via ensureCoreThen before launching.
    struct CorePlan { QString corePath; QString core; QString launchRom; QString sourceRom; QString systemId; QString error;
                      int errorMs = kFeedbackLong;        // error-class toast duration (J06 policy: all errors kFeedbackLong)
                      const GameSystem* sys = nullptr;    // the resolved system (borrowed; SystemCatalog entries are static)
                      QString externalEmulatorId; // non-empty => a standalone-emulator system (no libretro core)
                      // Which engine this system launches on. Slice 2a resolved this only for LIBRETRO systems
                      // (from Settings::backendFor + the per-game override). Slice 3b extends it to the STANDALONE
                      // arm: a standalone system RetroPark supports (gc → Dolphin) whose resolved backend is
                      // RetroPark routes to the in-process presenting path instead of the external emulator (in
                      // that case externalEmulatorId is left empty). Every other case leaves this at Libretro —
                      // keeping un-opted games byte-for-byte on today's path (open()'s libretro / external branches
                      // are unchanged when backend==Libretro).
                      EmuBackend backend = EmuBackend::Libretro;
                      // Slice 3b: does the RetroPark-backed target run on a PRESENTING core (Dolphin/gc — GPU
                      // render read back on a headless Vulkan runtime) rather than a DRIVEN core (NES shim, D3D11)?
                      // Derived from retroParkSystemIsPresenting(systemId) at resolution and threaded to
                      // RetroParkView::openGame so the runtime graphics API is chosen (rpapi::runtimeApiForCore)
                      // BEFORE the core loads. Only meaningful when backend==RetroPark; false for every other plan
                      // (incl. every driven/NES RetroPark plan), so the 2b caller is byte-behaviourally unchanged.
                      bool retroparkPresenting = false;
                      // #190 (folder games, MS-DOS): the folder held SEVERAL plausible programs and the
                      // recipe gave no basis to choose one, so the launch stops here and asks. Non-empty =>
                      // `bootFolder` is the game folder and these are the candidate programs, relative to it,
                      // in the order to offer them. Empty on every other launch, which is every launch that
                      // is not a folder game with an unresolved pick — so nothing else changes shape.
                      QStringList bootChoices;
                      QString bootFolder; };
    // `key` is the game's stable id (the same one open() carries). When non-empty its per-game launch override
    // (LaunchOptionsStore, issue #51) is consulted: the preferred core (libretro) / emulator (standalone) is
    // applied to the resolved plan. Empty key => no override, byte-for-byte today's resolution — which is what
    // MainWindow's split-pane branch relies on (it has no key). The extra-args lever is applied later, at the
    // standalone-emulator launch (runEmulator), not here — a CorePlan carries no args.
    CorePlan prepareCore(const QString& rom, const QString& systemHint, const QString& key = QString());

    // Fill plan.corePath — immediately when installed, else via an async buildbot download (progress on the
    // Notifier toast) — then run onReady with the completed plan. On failure onReady never runs; the error
    // shows on the toast. Parented to `context`: destroying it cancels the download and the continuation.
    void ensureCoreThen(const CorePlan& plan, QObject* context,
                        const std::function<void(const CorePlan&)>& onReady);

    // Run a standalone emulator: stop our playback, show the wait page, minimise, and launch (auto-installing if
    // needed). rom empty => open the emulator's own UI (e.g. TeknoParrot, or another emulator for setup).
    void runEmulator(const ExternalEmulator& em, const QString& rom = QString(), const QString& title = QString(),
                     const QString& thumb = QString(), const QString& key = QString(), const QString& system = QString(),
                     const QString& sourceRom = QString()); // sourceRom = the reopenable source (archive) for Recent; rom = the boot path
    void install(const ExternalEmulator& em);  // download + extract only (Settings ▸ Emulators button)
    bool emulatorBusy() const;                 // an emulator run/install is in progress
    void forceCloseEmulator();                 // wait-page Stop button: hard-kill the running emulator
    // Supersede a pending external-emulator launch — one that is still installing/updating and has not spawned
    // a process yet — because another frontend is about to own the screen. Called from the in-app launch tails
    // and from MainWindow's split-pane branch; without it, a launch started while the RPCS3 firmware/update
    // worker runs would find the emulator booting full-screen on top of it minutes later, and the stale
    // pending-emulator entry recorded into Recents over the game actually being played. Returns true if there
    // was such a launch. A RUNNING external game is untouched (that is forceCloseEmulator's job), and an
    // external-over-external launch stays on runEmulator's unchanged busy-refusal.
    bool cancelPendingEmulatorLaunch();

signals:
    void aboutToLaunch();        // host stops the player/readers and clears the audio queue
    void showRetroRequested();   // host shows the RetroView page (a libretro game started)
    void showRetroParkRequested(); // host shows the RetroParkView page (a RetroPark-backend game started) — Slice 2a
    // Host builds/updates the emu wait page + shows it. `stopLabel` names what the Stop button does in THIS
    // phase — it is a cancel before the emulator process exists ("Cancel download" / "Cancel launch") and a
    // hard close after ("Force-close emulator") — because one control spans both and the wrong verb reads as
    // a threat to a game that is merely downloading. Empty leaves the host's default label.
    void waitPage(const QString& text, bool stopVisible, const QString& stopLabel = QString());
    void waitPageStatus(const QString& text); // install/launch progress: update the wait-page label IF it's showing, never switch to it
    void waitPageDone();         // host returns Home if the wait page is the current view
    void minimizeRequested();    // host saves its window state + minimises (step aside for the emulator)
    void restoreRequested();     // host restores the saved window state (the emulator exited)
    // Discord presence: a game session opened / closed. Emitted from begin/endPlaySession, which is the ONE
    // point every emulator launch path already funnels through - hooking the individual open() arms instead
    // would miss whichever one is added next.
    void playSessionBegan(const QString& title, const QString& system, const QString& artPath);
    void playSessionEnded();
    void statusMessage(const QString& text, int ms); // status-bar message (ms 0 = no timeout)
    void notifyUser(const QString& text, int ms);    // user-facing notice (→ Notifier)
    // #190: a folder game whose program could not be picked automatically. The host shows a nav-kit menu of
    // `choices` (paths relative to the game folder), records the answer in the game's launch override, and
    // re-opens the game — which then resolves with nothing to ask. Emitted QUEUED from the host's connect, so
    // the menu is never opened inside this emission (the #28 / #211 nested-loop family); the launch that
    // emitted it simply ends. The original open() arguments travel with it because re-opening is how the
    // answer is applied — there is no half-finished launch parked anywhere waiting to be resumed.
    void chooseBootProgram(const QString& title, const QStringList& choices,
                           const QString& rom, const QString& thumb, const QString& key,
                           const QString& systemHint);
    // Standalone-emulator install stream (Settings ▸ Emulators), forwarded from the private EmulatorManager so
    // the themed panel can tick the emulator's status row in place. pct < 0 = indeterminate (extract phase).
    void emulatorInstallProgress(const QString& text, int pct);
    void emulatorInstallFinished(const QString& displayName); // install-only completed (the binary is now present)
    void emulatorInstallFailed(const QString& message);       // download/extract failed

private:
    // The libretro launch tail — stop playback, load the core into RetroView, record the Recent entry and
    // play session. Split out of open() so a missing BIOS can download asynchronously (UI responsive,
    // progress in the status bar) with this tail running as the continuation once the files land.
    void finishLibretroLaunch(const CorePlan& plan, const QString& launchRom, const QString& recentTitle,
                              const QString& thumb, const QString& key);
    // The RetroPark launch tail (Slice 2a) — the third branch beside finishLibretroLaunch, taken when a libretro
    // system's resolved backend is RetroPark. For now a STUB that only signals the host to show the RetroPark page;
    // Task 4 wires the live RetroParkView (driven refcore surface) in here. Mirrors finishLibretroLaunch's shape so
    // the Task-4 body can slot in without touching open()'s routing.
    void finishRetroParkLaunch(const CorePlan& plan, const QString& launchRom, const QString& recentTitle,
                               const QString& thumb, const QString& key);
    // The launch tail: everything after the archive has been extracted (resolve system/core, hooks, route to
    // libretro / RetroPark / external). open() calls this directly for a non-archive, or from the worker-thread
    // continuation once extraction finishes. It re-enters prepareCore, whose archive resolve is a warm cache hit.
    void openResolved(const QString& rom, const QString& title, const QString& thumb,
                      const QString& key, const QString& systemHint);
    void ensureEmu();            // lazily create EmulatorManager + wire its signals
    // #190: the recipe's firmware check for a resolved plan — "" when everything the core needs is present
    // (which is every system without a recipe, and every recipe whose firmware is there), otherwise the
    // message naming the exact file(s) and the folder they go in. Called AFTER the BIOS fetch, so a file the
    // app can legitimately fetch for itself is never reported as the user's problem.
    QString firmwareBlocker(const CorePlan& plan, const QString& title) const;
    // Systems flagged as external (GameCube/Wii via Dolphin) run in a standalone emulator launched as a child
    // process: ensure it's installed (auto-download), boot the ROM, and show a wait page until it exits.
    // `emulatorId` is the resolved standalone-emulator id — sys->externalEmulator by default, or a per-game
    // override (issue #51) — so the override reaches the actual launch instead of being re-read off the system.
    void launchExternalGame(const GameSystem* sys, const QString& emulatorId, const QString& rom,
                            const QString& title, const QString& thumb, const QString& key,
                            const QString& sourceRom = QString()); // sourceRom = reopenable source (archive) for Recent
    void startEmuHotkeyWatch();
    void stopEmuHotkeyWatch();
    void pollEmuExitHotkey();
    void startPad2Key();   // issue #105: begin pad-to-keyboard injection if enabled for the launched game
    void stopPad2Key();    // issue #105: stop injection and release every held key (footgun guard)
    // Play-time tracking for the full-screen emulator / external-emulator flow: stamp last-played + start the
    // clock when a game begins, and bank the elapsed session when it ends. beginPlaySession auto-closes any
    // session still open.
    // `title`/`system`/`artPath` are carried purely so the Discord presence card can name the game. They
    // default to empty so the call sites that have nothing to say keep compiling, and an empty title simply
    // means no card. Nothing here looks anything up.
    void beginPlaySession(const QString& identity, const QString& title = QString(),
                          const QString& system = QString(), const QString& artPath = QString());
    void endPlaySession();
    // Post-exit hook (issue #64): run the game's user-authored post-exit command after its session ends. Reads
    // the device-local LaunchHooksStore for `key`; a set command runs log-only (a failure never blocks). No-op
    // for an empty key or an empty hook. Desktop-only (gated off Android/iOS in the .cpp).
    void firePostHook(const QString& key, const QString& rom);

    RetroView* retro_ = nullptr;
    RetroParkView* retroPark_ = nullptr;   // Slice 2a: the RetroPark-backend play surface (borrowed; host-owned)
    EmulatorManager* emu_ = nullptr;
    // The per-launch contexts every async launch continuation is parented to — the archive-extraction worker's
    // continuation and the core + BIOS fetch chain — plus the rules for which of them a newly committed launch
    // retires, so a superseded launch drops instead of booting a stale game on top of its replacement minutes
    // later. Read LaunchContexts.h: the reason there are two contexts, and the reason the extraction one is
    // consumed by its own continuation rather than deleted at the retire site, are both load-bearing.
    LaunchContexts contexts_{this};
    QString pendingEmuRom_, pendingEmuTitle_, pendingEmuThumb_, pendingEmuKey_, pendingEmuSystem_; // Recent entry, added on launch
    QString pendingEmuSource_; // the reopenable source path (archive) recorded in Recent — NOT the extracted boot file
    // While a standalone emulator (melonDS, Dolphin…) owns the screen, watch for a global exit hotkey — Start+Select
    // on a pad, or Esc on the keyboard — and close it back to the app. Runs only between the emulator's launched
    // and finished signals (the app is minimized then, so Qt can't see the input itself).
    QTimer* emuHotkeyTimer_ = nullptr;
    bool emuComboPrev_ = false;          // edge-detect: Start+Select was held last poll
    bool emuEscPrev_ = false;            // edge-detect: Esc was held last poll
    // Pad-to-keyboard injector (issue #105): synthesises keystrokes from the pad while a standalone/PC game we
    // launched holds focus, for keyboard-only games. Started only when Pad2KeyStore says pad2key is ENABLED for
    // the launched game (so an ordinary emulator, which has its own pad support, is never touched), and stopped —
    // releasing every held key — the instant the emulator exits or fails. Reuses retro_'s idle Gamepad.
    Pad2KeyRuntime* pad2key_ = nullptr;
    // Detect a standalone emulator that closes almost immediately (a failed boot — usually a missing BIOS/firmware,
    // which -batch-style launches exit silently on). Only warn when the user didn't close it themselves.
    QElapsedTimer emuRunClock_;
    QString emuDisplayName_;              // the running emulator's display name (from the launched signal)
    bool emuUserClosing_ = false;         // set when WE ask it to close (exit hotkey / force-close), to suppress the warning
    QString activePlayId_;                // identity of the game currently being timed ("" = none)
    qint64  activePlayStart_ = 0;         // epoch seconds the active session began
    // Post-exit hook context for the LIBRETRO session (issue #64): the key + launch rom of the game currently
    // loaded in RetroView, captured at launch and consumed when RetroView::gameStopped fires. The external-
    // emulator path uses pendingEmuKey_/pendingEmuRom_ at its own QProcess::finished instead.
    QString hookKey_, hookRom_;
};
