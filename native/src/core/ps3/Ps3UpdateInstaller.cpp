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
// The digest Sony's ver.xml `sha1sum` attribute holds is NOT the SHA-1 of the whole file: every
// retail pkg ends with a 0x20-byte footer whose first 20 bytes are that very digest (so a whole-file
// hash can never equal it), and the attribute covers the file MINUS that footer. Verified against a
// live download (BCUS98148 update 01.02, 2026-08-19): SHA1(file[0 .. size-0x20)) == the attribute ==
// the file's own tail bytes. Hashing the whole file — the previous behavior — therefore failed EVERY
// genuine package and aborted the chain before rpcs3 ever ran, which is exactly the hardware failure
// this comment dates. A file too small to carry the footer is hashed whole; it can only be garbage,
// and garbage merely needs to keep failing the comparison.
QString sha1Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    qint64 remaining = f.size() > 0x20 ? f.size() - 0x20 : f.size();
    QCryptographicHash h(QCryptographicHash::Sha1);
    char buf[65536];
    while (remaining > 0)
    {
        const qint64 n = f.read(buf, qMin<qint64>(remaining, qint64(sizeof buf)));
        if (n <= 0) return {}; // short read: not a verdict, fail the verification
        h.addData(QByteArrayView(buf, n));
        remaining -= n;
    }
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
