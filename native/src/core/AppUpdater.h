// Checks GitHub Releases for a newer My Media Vault build and, on desktop, installs it in place. The app is a
// portable folder (exe + DLLs + resources next to it), and Windows can't overwrite a running exe, so applying an
// update downloads the release's platform archive, extracts it, and hands off to a tiny helper that waits for
// this process to exit, swaps the files over the install dir, and relaunches. The version check is cross-
// platform; the in-place apply is implemented for Windows (the packaged-zip target). Mirrors the addon
// self-update model (versionCompare + download), just for the app itself.
#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;

class AppUpdater : public QObject
{
    Q_OBJECT
public:
    explicit AppUpdater(QObject* parent = nullptr);

    void checkForUpdate();   // query the Releases API; emits updateAvailable / upToDate / checkFailed
    void downloadAndApply(); // download the pending release archive, swap it in, relaunch (desktop)

    bool updatePending() const { return !pendingUrl_.isEmpty(); }
    QString latestVersion() const { return latestVersion_; }
    static QString currentVersion(); // this build's version (QApplication::applicationVersion)

signals:
    void updateAvailable(const QString& version, const QString& notes); // a newer release exists
    void upToDate();                                                    // already on the latest
    void checkFailed(const QString& why);                              // offline / API error (silent by default)
    void progress(const QString& text, int pct);                       // download/apply feedback (pct<0 = busy)
    void applyFailed(const QString& why);

private:
    QNetworkAccessManager* nam_ = nullptr;
    QString pendingUrl_;      // browser_download_url of this platform's asset for the newer release
    QString pendingAsset_;    // its file name (e.g. MyMediaVault-windows-x64.zip)
    QString latestVersion_;
    QString latestNotes_;
    bool applyArchive(const QString& archivePath, QString* error); // extract + spawn the swap helper, then quit
};
