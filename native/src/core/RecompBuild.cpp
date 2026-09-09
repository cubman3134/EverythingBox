// Where a build happens on disk (issue #248, increment c). The rest of the model is pure and lives in
// RecompBuild.h; this file exists only because these four answers need AppPaths.
//
// ONE ROOT, AND EVERYTHING UNDER IT. A build writes to <data>/recomps/builds/<id> and nowhere else: the
// downloaded source, the C++ the recompiler emits, the object files and build.log are all inside it, so
// "remove this build" is one directory and "what did this feature put on my disk" has one answer. The id is
// sanitised on the way in (RecompBuild.h's sanitiseId) because a catalogue is data and an id is whatever a
// feed said it was.
#include "RecompBuild.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "AppPaths.h"

namespace recompbuild {

QString buildsRoot() { return AppPaths::dataDir() + QStringLiteral("/recomps/builds"); }

QString workspaceDir(const QString& portId)
{
    const QString slug = sanitiseId(portId);
    // An id that sanitises to nothing gets no workspace at all rather than the builds root itself — which,
    // handed to a recursive delete, would be every build on the machine.
    if (slug.isEmpty()) return QString();
    return buildsRoot() + QLatin1Char('/') + slug;
}

QString sourceDir(const QString& portId)
{
    const QString ws = workspaceDir(portId);
    return ws.isEmpty() ? QString() : ws + QStringLiteral("/source");
}

QString cmakeBuildDir(const QString& portId, const QString& relativeBuildDir)
{
    const QString src = sourceDir(portId);
    if (src.isEmpty()) return QString();
    const QString rel = relativeBuildDir.trimmed().isEmpty() ? QStringLiteral("build")
                                                             : relativeBuildDir.trimmed();
    // The recipe's `cmake.build_dir` is a catalogue field, so it is checked the same way an archive member
    // is: an absolute path or a `..` segment names somewhere outside the workspace and is refused back to
    // the default rather than honoured.
    const QString safe = safeMemberPath(src, rel);
    return safe.isEmpty() ? src + QStringLiteral("/build") : safe;
}

QString logPath(const QString& portId)
{
    const QString ws = workspaceDir(portId);
    return ws.isEmpty() ? QString() : ws + QStringLiteral("/build.log");
}

QString enginesRoot() { return AppPaths::dataDir() + QStringLiteral("/recomps/engines"); }

QString engineDir(const QString& engineId)
{
    const QString slug = sanitiseId(engineId);
    return slug.isEmpty() ? QString() : enginesRoot() + QLatin1Char('/') + slug;
}

bool writeEngineNotice(const QString& dir, const QString& engineId, const QString& license,
                       const QString& homepage)
{
    if (dir.trimmed().isEmpty()) return false;
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("WHERE-THIS-CAME-FROM.txt")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray body = engineNoticeText(engineId, license, homepage).toUtf8();
    return f.write(body) == body.size();
}

}  // namespace recompbuild
