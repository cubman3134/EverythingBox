#include "core/ps3/Ps3VerifyBackoff.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace Ps3VerifyBackoff {

QString markerPath(const QString& tmpDir, const QString& titleId)
{
    return QDir(tmpDir).filePath(QStringLiteral("ps3-verify-failed-") + titleId);
}

void record(const QString& tmpDir, const QString& titleId)
{
    QDir().mkpath(tmpDir);
    QFile f(markerPath(tmpDir, titleId));
    // Truncate rather than append: the mtime is the whole state, and a re-record must refresh it.
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                 + QLatin1String(" verification failed\n")).toUtf8());
}

bool inBackoff(const QString& tmpDir, const QString& titleId, const QDateTime& now)
{
    // tmpDir may not exist yet — QFileInfo on a missing path simply reports !exists().
    const QFileInfo markerInfo(markerPath(tmpDir, titleId));
    const qint64 age = markerInfo.lastModified().toUTC().secsTo(now);
    return markerInfo.exists() && age >= 0 && age < kRetryBackoffSecs;
}

} // namespace Ps3VerifyBackoff
