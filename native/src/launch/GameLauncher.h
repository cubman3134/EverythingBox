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

class RetroView;
class EmulatorManager;
class QTimer;
struct GameSystem;
struct ExternalEmulator;

class GameLauncher : public QObject
{
    Q_OBJECT
public:
    explicit GameLauncher(RetroView* retro, QObject* parent = nullptr);

    // The full launch pipeline: archive → system → disc descriptor → core/BIOS → RetroView or external emulator.
    // title/thumb/key carry the catalog item's display name + cover + stable id for the Recent entry; systemHint
    // is the console/platform the game was opened from, to disambiguate extensions shared across systems.
    void open(const QString& rom, const QString& title = QString(), const QString& thumb = QString(),
              const QString& key = QString(), const QString& systemHint = QString());

    // The pipeline's resolution half (system + disc descriptor + core lookup), reused by MainWindow's split-pane
    // branch to run a ROM in the focused pane's own emulator. Resolution only — no network: `error` non-empty =>
    // couldn't resolve; corePath empty with `core` set => the core isn't installed yet, and the caller downloads
    // it via ensureCoreThen before launching.
    struct CorePlan { QString corePath; QString core; QString launchRom; QString systemId; QString error;
                      int errorMs = kFeedbackLong;        // error-class toast duration (J06 policy: all errors kFeedbackLong)
                      const GameSystem* sys = nullptr;    // the resolved system (borrowed; SystemCatalog entries are static)
                      QString externalEmulatorId; }; // non-empty => a standalone-emulator system (no libretro core)
    CorePlan prepareCore(const QString& rom, const QString& systemHint);

    // Fill plan.corePath — immediately when installed, else via an async buildbot download (progress on the
    // Notifier toast) — then run onReady with the completed plan. On failure onReady never runs; the error
    // shows on the toast. Parented to `context`: destroying it cancels the download and the continuation.
    void ensureCoreThen(const CorePlan& plan, QObject* context,
                        const std::function<void(const CorePlan&)>& onReady);

    // Run a standalone emulator: stop our playback, show the wait page, minimise, and launch (auto-installing if
    // needed). rom empty => open the emulator's own UI (e.g. TeknoParrot, or another emulator for setup).
    void runEmulator(const ExternalEmulator& em, const QString& rom = QString(), const QString& title = QString(),
                     const QString& thumb = QString(), const QString& key = QString(), const QString& system = QString());
    void install(const ExternalEmulator& em);  // download + extract only (Settings ▸ Emulators button)
    bool emulatorBusy() const;                 // an emulator run/install is in progress
    void forceCloseEmulator();                 // wait-page Stop button: hard-kill the running emulator

signals:
    void aboutToLaunch();        // host stops the player/readers and clears the audio queue
    void showRetroRequested();   // host shows the RetroView page (a libretro game started)
    void waitPage(const QString& text, bool stopVisible); // host builds/updates the emu wait page + shows it
    void waitPageStatus(const QString& text); // install/launch progress: update the wait-page label IF it's showing, never switch to it
    void waitPageDone();         // host returns Home if the wait page is the current view
    void minimizeRequested();    // host saves its window state + minimises (step aside for the emulator)
    void restoreRequested();     // host restores the saved window state (the emulator exited)
    void statusMessage(const QString& text, int ms); // status-bar message (ms 0 = no timeout)
    void notifyUser(const QString& text, int ms);    // user-facing notice (→ Notifier)

private:
    // The libretro launch tail — stop playback, load the core into RetroView, record the Recent entry and
    // play session. Split out of open() so a missing BIOS can download asynchronously (UI responsive,
    // progress in the status bar) with this tail running as the continuation once the files land.
    void finishLibretroLaunch(const CorePlan& plan, const QString& launchRom, const QString& recentTitle,
                              const QString& thumb, const QString& key);
    void ensureEmu();            // lazily create EmulatorManager + wire its signals
    // Systems flagged as external (GameCube/Wii via Dolphin) run in a standalone emulator launched as a child
    // process: ensure it's installed (auto-download), boot the ROM, and show a wait page until it exits.
    void launchExternalGame(const GameSystem* sys, const QString& rom, const QString& title,
                            const QString& thumb, const QString& key);
    void startEmuHotkeyWatch();
    void stopEmuHotkeyWatch();
    void pollEmuExitHotkey();
    // Play-time tracking for the full-screen emulator / external-emulator flow: stamp last-played + start the
    // clock when a game begins, and bank the elapsed session when it ends. beginPlaySession auto-closes any
    // session still open.
    void beginPlaySession(const QString& identity);
    void endPlaySession();

    RetroView* retro_ = nullptr;
    EmulatorManager* emu_ = nullptr;
    // Per-launch context the async core + BIOS fetches are parented to: recreated on every open(), so a newer
    // launch supersedes (cancels) a still-downloading one instead of both booting when their downloads finish.
    QObject* launchCtx_ = nullptr;
    QString pendingEmuRom_, pendingEmuTitle_, pendingEmuThumb_, pendingEmuKey_, pendingEmuSystem_; // Recent entry, added on launch
    // While a standalone emulator (melonDS, Dolphin…) owns the screen, watch for a global exit hotkey — Start+Select
    // on a pad, or Esc on the keyboard — and close it back to the app. Runs only between the emulator's launched
    // and finished signals (the app is minimized then, so Qt can't see the input itself).
    QTimer* emuHotkeyTimer_ = nullptr;
    bool emuComboPrev_ = false;          // edge-detect: Start+Select was held last poll
    bool emuEscPrev_ = false;            // edge-detect: Esc was held last poll
    // Detect a standalone emulator that closes almost immediately (a failed boot — usually a missing BIOS/firmware,
    // which -batch-style launches exit silently on). Only warn when the user didn't close it themselves.
    QElapsedTimer emuRunClock_;
    QString emuDisplayName_;              // the running emulator's display name (from the launched signal)
    bool emuUserClosing_ = false;         // set when WE ask it to close (exit hotkey / force-close), to suppress the warning
    QString activePlayId_;                // identity of the game currently being timed ("" = none)
    qint64  activePlayStart_ = 0;         // epoch seconds the active session began
};
