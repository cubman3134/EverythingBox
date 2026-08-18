#pragma once
#include "core/ps3/Ps3UpdateFeed.h"
#include <QString>
#include <QVector>
#include <functional>

class Ps3UpdateInstaller {
public:
    using Downloader = std::function<bool(const QString& url, const QString& destPath)>;
    using Installer  = std::function<int(const QString& rpcs3Exe, const QString& pkgPath)>;

    Ps3UpdateInstaller(QString rpcs3Exe, QString tmpDir, Downloader dl, Installer run);
    bool installAll(const QString& titleId, const QVector<Ps3UpdatePackage>& pkgs);

private:
    QString    rpcs3Exe_;
    QString    tmpDir_;
    Downloader download_;
    Installer  install_;
};
