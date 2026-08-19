#include "core/ps3/Ps3UpdateInstaller.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <utility>

Ps3UpdateInstaller::Ps3UpdateInstaller(QString rpcs3Exe, QString tmpDir, Downloader dl, Installer run,
                                       AlreadyApplied applied)
    : rpcs3Exe_(std::move(rpcs3Exe)), tmpDir_(std::move(tmpDir)), download_(std::move(dl)),
      install_(std::move(run)), applied_(std::move(applied)) {}

namespace {
QString sha1Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    if (!h.addData(&f)) return {};
    return QString::fromLatin1(h.result().toHex());
}
}

bool Ps3UpdateInstaller::installAll(const QString& titleId, const QVector<Ps3UpdatePackage>& pkgs)
{
    QDir().mkpath(tmpDir_);
    QStringList temps;
    auto cleanup = [&] { for (const QString& t : temps) QFile::remove(t); };

    int n = 0;
    for (const Ps3UpdatePackage& p : pkgs)
    {
        // Before the download, not after: an already-applied package is hundreds of megabytes we would
        // otherwise fetch only for the installer to short-circuit on it.
        if (applied_ && applied_(titleId, p.version)) continue;
        const QString dest = QDir(tmpDir_).filePath(QStringLiteral("%1_%2_%3.pkg").arg(titleId, p.version).arg(n++));
        temps << dest;
        if (!download_(p.url, dest)) { cleanup(); return false; }
        if (!p.sha1.isEmpty() && sha1Of(dest).compare(p.sha1, Qt::CaseInsensitive) != 0) { cleanup(); return false; }
        if (install_(rpcs3Exe_, dest, titleId, p.version) != 0) { cleanup(); return false; }
    }
    cleanup();
    return true;
}
