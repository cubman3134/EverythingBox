// A Riivolution document reduced to the overlay it describes. Pure: no file system, no network.
//
// Riivolution is a runtime loader — it substitutes files as the game reads them. We are composing a disc
// instead, so only the parts of the format that describe FILE SUBSTITUTION can be honoured. The two that
// cannot are handled explicitly rather than ignored quietly:
//
//   <savegame> redirects saves at runtime and has no meaning in a composed disc. Ignored, and the fact is
//   reported in `savegameIgnored` so a caller can say so rather than pretend the document was fully applied.
//
//   <memory> writes to RAM while the game runs and CANNOT be represented in a disc image at all. A document
//   using it is REFUSED. Composing it would produce a disc that builds, boots, and is subtly wrong, which is
//   worse than not installing it — so the refusal is the feature, not a limitation to work around later.
//
// Options are refused rather than guessed: see `parse`.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

namespace RiivolutionPatch
{
    struct Op
    {
        enum Kind { Folder, File };
        Kind kind = Folder;
        QString discPath;      // where it goes on the disc, e.g. "/StageData"
        QString externalPath;  // where it comes from, relative to the patch root, e.g. "StageData"
        bool create = false;   // the document's create= attribute
    };

    struct Parsed
    {
        bool ok = false;
        QString refusal;           // non-empty iff !ok; names the element that caused it
        QString root;              // the <patch root=> value, e.g. "/SuperMarioGravity_Demo"
        QVector<Op> ops;
        bool savegameIgnored = false;
    };

    // Parses a Riivolution document. Refuses — ok=false, with `refusal` naming why — when the document
    // contains a <memory> patch, or when it offers a CHOICE we cannot make: more than one <option>, or an
    // <option> with more than one <choice>. There is no UI for choosing, and picking silently would install
    // a different mod from the one the user believes they chose. A single-option, single-choice document
    // (the shape measured on the mod at hand) composes without asking anything.
    Parsed parse(const QByteArray& xml);
}
