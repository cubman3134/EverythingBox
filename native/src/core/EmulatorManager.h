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
#include <functional>
#include "EmulatorRegistry.h"
#include "EmuSettings.h"   // issue #103: the resolved graphics quartet written into the emulator before launch

class QNetworkAccessManager;
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
    bool busy() const { return busy_; }

signals:
    void status(const QString& text, int pct);  // pct < 0 => indeterminate (download/extract progress)
    void launched(const QString& displayName);  // the emulator process started
    void finished(int exitCode);                // the emulator process exited (return to the app)
    void installed(const QString& displayName); // install-only completed
    void failed(const QString& message);

private:
    void startInstall();
    void fetchArtifactList();
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
    bool busy_ = false;
};
