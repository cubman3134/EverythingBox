// Turning a FILE the user pointed at into a REGISTERED custom core (issue #98). The one place that composes
// the two halves — CoreInspect (what does this file say it is?) and CustomCores (the registry) — so the
// settings surfaces, both of them, are a button and nothing else.
//
// WHAT LOADING DOES, IN ORDER:
//   1. Inspect the file where it lies. A file that is not a loadable library, is not a libretro core, or
//      speaks a libretro API version we do not, stops HERE with a sentence and nothing is written. We never
//      copy first and ask questions after — a rejected file leaves no trace.
//   2. Derive the id from the core's OWN library_name (sanitised), falling back to the file's base name. The
//      id is what "custom:<id>" is built from, so it has to come from the core's identity rather than the
//      path: the same core loaded twice from two folders is ONE registration, updated, not two.
//   3. Copy the library into <data>/cores/custom/. The user's copy may live in a downloads folder they clear,
//      on a stick they unplug, or beside an installer they delete; a registry pointing at that is a launch
//      that fails weeks later for no visible reason. A file ALREADY inside that folder is left where it is
//      (this is also what makes "drop a core in <data>/cores/custom/ and scan" work with no copy at all).
//   4. Register it. Re-loading a core that is already registered REPLACES the record, which is how a user
//      updates a core they rebuilt.
//
// Nothing here downloads anything, and nothing here refuses a core for being "known bad": #98's posture is
// advisory. The only refusals are the ones in step 1, where the file genuinely cannot be hosted.
#pragma once
#include <QString>
#include <QStringList>
#include "CustomCores.h"

namespace CustomCoreInstall
{
    // The platform's shared-library suffix (".dll" / ".dylib" / ".so"). Exposed so the file picker's filter and
    // the folder scan agree with what can actually be loaded.
    QString librarySuffix();

    // PURE. The id to register a core under: its library_name sanitised, else the file's base name sanitised
    // (with a trailing "_libretro" dropped — every buildbot core file carries it and it is noise in an id).
    // Empty only when BOTH are unusable, which the caller reports rather than registering an unnamed core.
    QString idFor(const QString& libraryName, const QString& filePath);

    // Inspect + copy + register, as described above. On success *out (when given) holds the registered record
    // and the registry has been written. On failure returns false with a sentence in *error and NOTHING has
    // been written or copied.
    bool loadFromFile(const QString& file, CustomCore* out = nullptr, QString* error = nullptr);

    // Every library file sitting in <data>/cores/custom/ that is NOT already registered — the "drop it in the
    // folder" half of the feature. Absolute paths, in name order. Loading them is the caller's call, one by
    // one through loadFromFile, so a bad file in the folder reports itself instead of failing the whole scan.
    QStringList unregisteredInCustomDir();
}
