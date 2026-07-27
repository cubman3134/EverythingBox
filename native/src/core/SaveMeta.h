// Per-save metadata (title/system/rom, hash and mtime bookkeeping) backing the save-sync baseline.
// Stub: Task 4 of the trustworthy-save-sync track fills it in with the <data>/saves-meta.json sidecar and a
// put() called from RetroView's save paths.
//
// titleFor() exists NOW because SaveSync (Task 2) names the game in its conflictKept notice, and a conflict
// notice that says "Zelda.conflict-devA-20260727-141530.srm" instead of "The Legend of Zelda" is a notice the
// user cannot act on. Until Task 4 lands there is no sidecar to consult, so it returns the file's own base
// name — which is what it must return for any save with no sidecar entry anyway (most real .srm files are
// named for a cached ROM's 40-hex filename). It is NEVER empty.
#pragma once
#include <QString>

namespace SaveMeta
{
    // The display title for a save file (path relative to the saves root), or the bare file name when
    // nothing is recorded. Never empty.
    QString titleFor(const QString& relPath);
}
