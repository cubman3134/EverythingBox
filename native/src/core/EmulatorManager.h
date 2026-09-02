// Installs (download + extract) and runs standalone emulators, monitoring the child process until it
// exits - the RetroBat / ES-DE launcher model. One instance is reused; only one external game runs at a
// time. Auto-install is currently implemented for Windows (fetch the emulator's official archive and
// extract it with the bundled bsdtar); other OSes report a manual-install message.
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QSet>
#include <QPointer>
#include <functional>
#include "EmulatorRegistry.h"
#include "EmuSettings.h"   // issue #103: the resolved graphics quartet written into the emulator before launch

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

class EmulatorManager : public QObject
{
    Q_OBJECT
public:
    explicit EmulatorManager(QObject* parent = nullptr);

    // Where emulators live, "<root>/<id>/". Configurable so it can point at an existing RetroBat/ES-DE
    // "emulators" folder; defaults to <app>/emulators.
    static QString emulatorsRoot();
    static void setEmulatorsRoot(const QString& dir);
    static QString installDir(const ExternalEmulator& em);
    static QString resolveBinary(const ExternalEmulator& em); // existing binary path, or "" if not installed
    static bool isInstalled(const ExternalEmulator& em) { return !resolveBinary(em).isEmpty(); }

    // The PATCHED Dolphin disc tool the app ships (composing a disc from an extracted tree is the one thing
    // the stock DolphinTool in Dolphin's own download cannot do). The name is deliberately not upstream's:
    // both builds are called DolphinTool.exe there, so the app tells them apart by where they came from.
    static QString discToolName();
    // Copy the shipped tool (and the GPL material that must travel with it) into a Dolphin install, once.
    // Returns the tool's path inside that install, or "" when nothing was shipped with this build. A no-op
    // when it is already there — safe to call on every launch and at every point of use.
    static QString seedDiscTool(const QString& binDir);

    static bool launchFullscreen();          // launch emulators full screen (default true)
    static void setLaunchFullscreen(bool on);

    // ensure installed, then boot + monitor. `extraArgs` (issue #51) are the game's per-game extra CLI args,
    // appended to the emulator's resolved argsTemplate at launch; empty => today's args exactly. `gfx` (issue
    // #103) is the resolved graphics quartet (per-game override already layered over the per-system default by
    // the caller); an all-unset gfx writes nothing, so the emulator's own config is left exactly as it was.
    void play(const ExternalEmulator& em, const QString& rom, const QString& extraArgs = QString(),
              const EmuGfx::Settings& gfx = EmuGfx::Settings{});
    void install(const ExternalEmulator& em);                  // download + extract only (Settings button)
    void terminateGame();                                      // force-close the running emulator (hard kill)
    void closeGame();                                          // ask it to close (WM_CLOSE), force-kill if it lingers
    // What cancelPendingLaunch() did. CancelledInstallContinues = the cancel landed in the short window where
    // the download had already finished and the archive was being extracted/installed: there is no request left
    // to abort, so that step runs to completion (seconds) as an install-only one and nothing boots. Every other
    // cancel is Cancelled — including one mid-download, which aborts the transfer (see LaunchCancel.h).
    enum class PendingCancel { None, Cancelled, CancelledInstallContinues };
    // Cancel a launch that is pending but has NOT yet spawned the emulator process. Two callers share it: an
    // in-app frontend (libretro/RetroPark/split-pane launch) superseding a still-installing or still-updating
    // external launch, and the wait page's Stop/Back route (forceCloseEmulator tries terminateGame() for a
    // running process, then this for the pre-boot window where game_ is still null). Never touches a RUNNING
    // game — that stays closeGame()/terminateGame() territory. During the install/download machinery (whose
    // continuations are bound to `this`, not the context) the cancel aborts the in-flight request but leaves
    // busy_ alone: releasing it here would let a new launch interleave with a download continuation over shared
    // members, so the aborted request's own finished handler frees it instead. See LaunchCancel.h for the two
    // phases.
    PendingCancel cancelPendingLaunch();
    bool busy() const { return busy_; }

signals:
    void status(const QString& text, int pct);  // pct < 0 => indeterminate (download/extract progress)
    void launched(const QString& displayName);  // the emulator process started
    void finished(int exitCode);                // the emulator process exited (return to the app)
    void installed(const QString& displayName); // install-only completed
    void failed(const QString& message);
    // The context-gated pre-boot phase began (everything pending now hangs off launchCtx_), so a
    // cancelPendingLaunch() from here on is safe and complete — the host can offer a Stop control.
    void bootPending(const QString& displayName);

private:
    void startInstall();
    void fetchArtifactList();
    void fetchArtifactListFrom(const QString& url);   // the request itself; fetchArtifactList picks the URL
    void downloadArchive(const QString& url);
    void installDownloaded();   // dispatch the downloaded artifact by format (per OS)
    void extractArchive();      // .zip / .7z  (per-OS extractor candidates)
    void tryExtract(const QList<QPair<QString, QStringList>>& cmds, int index); // run candidates until one works
    void installDmg();          // macOS .dmg  (hdiutil mount -> copy .app)
    void installAppImage();     // Linux .AppImage (move + chmod +x)
    void installFlatpak();      // Linux .flatpak (flatpak install --user)
    void finishInstall();       // common tail: locate the binary, then launch or report "installed"
    // Fetch + wire up a BIOS for emulators that need one (PCSX2/DuckStation). Asynchronous: onDone runs once
    // the files have settled (immediately when nothing is missing), parented to launchCtx_ for cancellation.
    void prepareBios(const QString& binDir, const std::function<void()>& onDone);
    void prepareFirstRunConfig(const QString& binDir); // pre-seed configs so emulators skip their first-run prompts
    void prepareControllerConfig(const QString& binDir); // auto-map a standard pad as Player 1 in each emulator
    void prepareAchievements(const QString& binDir); // sync EB's RetroAchievements login into the emulator's own RA client
    void prepareGraphicsSettings(const QString& binDir); // write the resolved graphics quartet (issue #103) into the emulator's config
    void backupSaves(const QString& binDir);   // snapshot this emulator's saves into <app>/saves/emulators/<id>
    void restoreSaves(const QString& binDir);  // seed saves from that central copy when the emulator has none
    // Per-emulator save-data locations to back up: {absolute source dir, stable label under the central folder}.
    static QList<QPair<QString, QString>> emulatorSaveDirs(const QString& id, const QString& binDir);
    void prepareCemuConfig(const QString& binDir); // pre-seed settings.xml so Cemu skips its first-run wizard
    // Fetch Cemu's keys.txt into its folder(s) if absent (Wii U). Asynchronous: onDone runs once the file
    // has settled (immediately for non-Cemu emulators or when keys are present), parented to launchCtx_.
    void prepareCemuKeys(const QString& binDir, const std::function<void()>& onDone);
    void prepareCemuDiscKey(const QString& binDir); // add a disc image's per-disc key to keys.txt (Wii U .wux/.wud)
    void launch(const QString& binary);
    // The on-disk prep + process start for a locally-installed emulator (the async BIOS/keys chain then boot).
    // Extracted from launch() so the normal path and the RPCS3 post-update path share one launch codepath.
    void finishLocalLaunch(const QString& program, const QStringList& args, const QString& binDir);
    // RPCS3 only: on a worker thread, auto-install the PS3 console firmware (if dev_flash is missing) and then the
    // game's official Sony update PKG chain, then finishLocalLaunch on the UI thread. Informational only — it never
    // blocks the boot: any failure falls through and the game boots (unpatched, or into RPCS3's firmware error).
    void runPs3UpdateThenLaunch(const QString& program, const QStringList& args, const QString& binDir);
    // The process half of launch(): spawn + monitor the emulator, run as the async BIOS fetch's continuation.
    void startGameProcess(const QString& program, const QStringList& args, const QString& binDir, bool isFlatpak);
    QString platformArtifact() const;
    QString platformUpdateUrl() const; // per-OS update/release URL (override), else updateJsonUrl

    QNetworkAccessManager* nam_ = nullptr;
    // The install chain's one in-flight request (the artifact-list lookup, then the archive download), so a
    // cancel arriving mid-download can abort it instead of leaving a ~100-500MB transfer running with the
    // manager wedged busy behind it. Held as a QPointer and cleared by each finished handler, so it is never a
    // dangling pointer to abort: outside those two windows it is simply null and the cancel has nothing to
    // stop. NOT part of the launchCtx_ regime — this chain is `this`-bound (LaunchCancel.h).
    QPointer<QNetworkReply> installReply_;
    QProcess* game_ = nullptr;
    // Per-launch context every async step of a launch hangs off: the BIOS/keys fetch chains parent to it, and
    // the RPCS3 update worker's boot continuation binds to it as its connect context. Recreated when play() or
    // install() takes ownership of the manager, so a dying manager or a superseding launch/install cancels all
    // of it — pending downloads abort, and a stale continuation auto-disconnects instead of booting its game
    // on top of the launch that replaced it.
    // For supersession only the continuation is gated (the worker runs to completion; its installs are
    // idempotent). App quit is the exception: aboutToQuit interruption-requests and joins every live
    // worker (bounded), killing an in-flight installer child — see runPs3UpdateThenLaunch.
    QObject* launchCtx_ = nullptr;
    ExternalEmulator em_;
    QString rom_;
    QString extraArgs_;   // per-game extra CLI args appended to the resolved argsTemplate at launch (issue #51)
    EmuGfx::Settings gfx_; // resolved graphics quartet written into the emulator's config at launch (issue #103)
    QString archivePath_;
    bool launchAfterInstall_ = false;
    // Which of the two ownership regimes currently owns the flow, i.e. which cancel is correct (LaunchCancel.h):
    // true from startInstall() until the top of launch(), where both of launch()'s callers — play()'s direct
    // route and finishInstall's launch-after-install route — converge. An install-chain failure path leaves it
    // stale-true, which is harmless: those paths clear busy_, and the decision checks busy_ first. The same
    // goes for BETWEEN flows — after an install-only completes the flag can sit stale-true until the next flow
    // rewrites it — and that is unobservable too: play() reaches launch() or startInstall() SYNCHRONOUSLY, and
    // both write installing_ before any event-loop turn can run decide() against the stale value.
    bool installing_ = false;
    bool busy_ = false;
    // Issue #233: this install has already retried its release lookup against the `/releases` list after a
    // 404 on `/releases/latest`. Per-flow (reset in startInstall), and one retry only — a second 404 is a
    // real failure and must reach the user rather than loop.
    bool releasesFallbackTried_ = false;
    // True while the PS3 pre-boot update worker thread is alive — including after a cancel orphans it; external
    // launches must not start while it can still be mutating the emulator's install dir. Cleared by a
    // `this`-bound queued connection so it survives supersession — unlike the launchCtx_-bound boot
    // continuation, surviving is the point. This refusal subsumes the per-binDir "skip the update step and
    // boot plain" guard the wait-page-cancel branch carried: refusing the launch outright also prevents
    // booting RPCS3 while an orphaned worker's --installfw child is still rewriting dev_flash, which the
    // skip-and-boot fallback did not.
    bool updateWorkerLive_ = false;
};
