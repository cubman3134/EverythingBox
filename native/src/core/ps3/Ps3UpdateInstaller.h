#pragma once
#include "core/ps3/Ps3UpdateFeed.h"
#include <QString>
#include <QVector>
#include <functional>

class Ps3UpdateInstaller {
public:
    using Downloader = std::function<bool(const QString& url, const QString& destPath)>;
    // Returns 0 iff the title's installed version reached `version` — however the installer determines
    // that. RPCS3's `--installpkg` stays open as the normal GUI after installing, so a process exit is
    // no proof of anything; titleId/version are the context a disk-state check needs. Non-zero aborts
    // the rest of the chain.
    using Installer  = std::function<int(const QString& rpcs3Exe, const QString& pkgPath,
                                         const QString& titleId, const QString& version)>;
    // Optional: "is this package already on disk?", asked BEFORE the package is downloaded. A retry
    // after a partially applied chain (or after a lost ps3-updates.json) must not re-pay hundreds of
    // megabytes just to have the installer discover the version is already there. Absent = never skip.
    using AlreadyApplied = std::function<bool(const QString& titleId, const QString& version)>;

    Ps3UpdateInstaller(QString rpcs3Exe, QString tmpDir, Downloader dl, Installer run,
                       AlreadyApplied applied = {});
    bool installAll(const QString& titleId, const QVector<Ps3UpdatePackage>& pkgs);

private:
    QString        rpcs3Exe_;
    QString        tmpDir_;
    Downloader     download_;
    Installer      install_;
    AlreadyApplied applied_;
};
