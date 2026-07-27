#include "SaveMeta.h"

#include <QFileInfo>

QString SaveMeta::titleFor(const QString& relPath)
{
    // No sidecar yet (Task 4). Fall back to the file's own base name, which is also the permanent answer for
    // any save the sidecar never recorded. Guard against an empty result so callers can put this straight
    // into a user-facing sentence.
    const QString base = QFileInfo(relPath).completeBaseName();
    if (!base.isEmpty()) return base;
    return relPath.isEmpty() ? QStringLiteral("this save") : relPath;
}
