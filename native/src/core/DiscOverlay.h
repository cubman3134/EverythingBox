// Applies a Riivolution overlay to a disc tree that DolphinTool already extracted.
//
// Every path is CONTAINED: an `external` or `disc` attribute comes out of an archive we did not build, so
// a mapping of disc="/../../Windows" must be refused rather than obeyed. Containment is checked on the
// RESOLVED, canonical path, because "a/../../b" only escapes once resolved.
#pragma once
#include "RiivolutionPatch.h"
#include <QString>

namespace DiscOverlay
{
    struct Result
    {
        bool ok = false;
        QString error;      // non-empty iff !ok
        int filesWritten = 0;
    };

    // `discRoot` is the directory DolphinTool extracted into. `modRoot` is the directory holding the mod's
    // replacement tree. `parsed.root` names the subdirectory of `modRoot` the operations are relative to.
    // Returns ok=false, writing nothing further, on the first operation that cannot be applied safely.
    Result apply(const QString& discRoot, const QString& modRoot, const RiivolutionPatch::Parsed& parsed);

    // Where a disc path lands inside an extracted tree: the Wii layout ("DATA/files") when discRoot has a
    // DATA directory, the GameCube layout ("files") otherwise. Exposed for the probe.
    QString discFilesRoot(const QString& discRoot);
}
