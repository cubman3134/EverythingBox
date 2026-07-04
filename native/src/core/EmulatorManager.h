// Installs (download + extract) and runs standalone emulators, monitoring the child process until it
// exits - the RetroBat / ES-DE launcher model. One instance is reused; only one external game runs at a
// time. Auto-install is currently implemented for Windows (fetch the emulator's official archive and
// extract it with the bundled bsdtar); other OSes report a manual-install message.
#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QPair>
#include "EmulatorRegistry.h"

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

    void play(const ExternalEmulator& em, const QString& rom); // ensure installed, then boot + monitor
    void install(const ExternalEmulator& em);                  // download + extract only (Settings button)
    void terminateGame();                                      // force-close the running emulator
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
    void prepareBios(const QString& binDir); // fetch + wire up a BIOS for emulators that need one (PCSX2)
    void prepareFirstRunConfig(const QString& binDir); // pre-seed configs so emulators skip their first-run prompts
    void prepareControllerConfig(const QString& binDir); // auto-map a standard pad as Player 1 in each emulator
    void backupSaves(const QString& binDir);   // snapshot this emulator's saves into <app>/saves/emulators/<id>
    void restoreSaves(const QString& binDir);  // seed saves from that central copy when the emulator has none
    // Per-emulator save-data locations to back up: {absolute source dir, stable label under the central folder}.
    static QList<QPair<QString, QString>> emulatorSaveDirs(const QString& id, const QString& binDir);
    void prepareCemuConfig(const QString& binDir); // pre-seed settings.xml so Cemu skips its first-run wizard
    void prepareCemuKeys(const QString& binDir); // fetch Cemu's keys.txt into its folder if absent (Wii U)
    void prepareCemuDiscKey(const QString& binDir); // add a disc image's per-disc key to keys.txt (Wii U .wux/.wud)
    void launch(const QString& binary);
    QString platformArtifact() const;
    QString platformUpdateUrl() const; // per-OS update/release URL (override), else updateJsonUrl

    QNetworkAccessManager* nam_ = nullptr;
    QProcess* game_ = nullptr;
    ExternalEmulator em_;
    QString rom_;
    QString archivePath_;
    bool launchAfterInstall_ = false;
    bool busy_ = false;
};
