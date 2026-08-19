#include "core/ps3/Ps3InstalledVersion.h"
#include "core/ps3/Ps3Sfo.h"
#include "core/ps3/Ps3Version.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <functional>
#include <utility>

namespace Ps3InstalledVersion {

QString gameDir(const QString& rpcs3Root, const QString& titleId)
{
    return QDir(rpcs3Root).filePath(QStringLiteral("dev_hdd0/game/") + titleId);
}

std::optional<QString> installedVersion(const QString& gameDir)
{
    QFile f(QDir(gameDir).filePath(QStringLiteral("PARAM.SFO")));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray sfo = f.readAll();
    if (auto app = Ps3Sfo::stringValue(sfo, "APP_VER")) return app;
    return Ps3Sfo::stringValue(sfo, "VERSION");
}

bool reachedTarget(const QString& gameDir, const QString& targetVersion)
{
    const auto have = installedVersion(gameDir);
    return have && !Ps3Version::less(*have, targetVersion);
}

std::optional<QByteArray> dirFingerprint(const QString& gameDir, const std::function<bool()>& abort)
{
    const QDir root(gameDir);
    QStringList paths;
    QDirIterator it(gameDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) paths << it.next();
    paths.sort(); // stable ordering: iteration order is not guaranteed across scans

    static const QByteArray sep("\0", 1); // delimit the fields, or a length shift in one is absorbed by the next
    QCryptographicHash h(QCryptographicHash::Sha1);
    for (const QString& p : std::as_const(paths))
    {
        if (abort && abort()) return std::nullopt; // caller wants out; "busy" is the safe way to say so
        QFile f(p);
        // Cannot open == the writer holds it exclusively == definitely busy. Not "unchanged".
        if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
        // A path-based QFileInfo::size() passes every probe case (in-process, Qt stats it through a
        // handle and flushes first) — the open is here for the CROSS-process writer, where NTFS may
        // serve a stale directory entry, and for the cannot-open==busy signal. Do not remove it.
        h.addData(root.relativeFilePath(p).toUtf8());                              h.addData(sep);
        h.addData(QByteArray::number(f.size()));                                   h.addData(sep); // handle-based: real-time
        h.addData(QByteArray::number(QFileInfo(p).lastModified().toUTC().toMSecsSinceEpoch()));
        h.addData(sep);                                                                            // mtime: lazy on NTFS
    }
    return h.result();
}

QByteArray snapshotSfo(const QString& gameDir)
{
    QFile f(QDir(gameDir).filePath(QStringLiteral("PARAM.SFO")));
    if (!f.open(QIODevice::ReadOnly)) return {}; // null, not empty: "there was no file here"
    const QByteArray bytes = f.readAll();
    return bytes.isNull() ? QByteArray("") : bytes; // a zero-byte file is EMPTY, and must restore as one
}

bool completedDespiteKill(const QString& gameDir, const QString& targetVersion,
                          const std::optional<QByteArray>& lastPrint,
                          const std::function<bool()>& abort)
{
    if (!lastPrint) return false; // no mid-run quiescence evidence: nothing to verify against
    if (!reachedTarget(gameDir, targetVersion)) return false;
    // No writer is left post-kill, so a fresh scan against ITSELF proves nothing — the evidence is
    // agreement with the last scan taken while the installer still lived: the tree was already done then.
    const std::optional<QByteArray> now = dirFingerprint(gameDir, abort);
    return now && *now == *lastPrint;
}

void restoreSfo(const QString& gameDir, const QByteArray& prior)
{
    const QString path = QDir(gameDir).filePath(QStringLiteral("PARAM.SFO"));
    if (prior.isNull()) { QFile::remove(path); return; }
    QDir().mkpath(gameDir); // a killed run can be interrupted before the dir exists
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(prior);
}

} // namespace Ps3InstalledVersion
