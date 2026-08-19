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

std::optional<QByteArray> dirFingerprint(const QString& gameDir)
{
    const QDir root(gameDir);
    QStringList paths;
    QDirIterator it(gameDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) paths << it.next();
    paths.sort(); // stable ordering: iteration order is not guaranteed across scans

    QCryptographicHash h(QCryptographicHash::Sha1);
    for (const QString& p : std::as_const(paths))
    {
        QFile f(p);
        // Cannot open == the writer holds it exclusively == definitely busy. Not "unchanged".
        if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
        h.addData(root.relativeFilePath(p).toUtf8());
        h.addData(QByteArray::number(f.size()));                                   // handle-based: real-time
        h.addData(QByteArray::number(QFileInfo(p).lastModified().toUTC().toMSecsSinceEpoch())); // lazy on NTFS
    }
    return h.result();
}

} // namespace Ps3InstalledVersion
