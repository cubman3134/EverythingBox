#include "core/ps3/Ps3InstalledVersion.h"
#include "core/ps3/Ps3Sfo.h"
#include "core/ps3/Ps3Version.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

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

qint64 secsSinceNewestWrite(const QString& gameDir, const QDateTime& nowUtc)
{
    QDateTime newest;
    QDirIterator it(gameDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        const QDateTime m = it.fileInfo().lastModified().toUTC();
        if (!newest.isValid() || m > newest) newest = m;
    }
    if (!newest.isValid()) return -1;
    // Clamped: a clock-skewed future mtime must not come back as the missing-dir sentinel.
    return qMax<qint64>(0, newest.secsTo(nowUtc));
}

} // namespace Ps3InstalledVersion
