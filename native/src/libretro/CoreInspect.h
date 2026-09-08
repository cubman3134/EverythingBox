// Read a libretro core file WITHOUT running it — issue #98's "a core that needs something we do not provide
// fails in a sentence, not a crash".
//
// WHAT THE LIBRETRO API LETS US KNOW UP FRONT, AND WHAT IT DOES NOT. There is no manifest. A core declares its
// identity in retro_get_system_info (name, version, the extensions it accepts, whether it wants a file path)
// and declares everything ELSE by CALLING the frontend back during retro_set_environment — SET_SUPPORT_NO_GAME,
// SET_HW_RENDER, GET_CAMERA_INTERFACE and the rest. So inspection is exactly: resolve the exports, check the
// API version, read the system info, install a RECORDING environment callback, call retro_set_environment, and
// unload. Anything the core only asks for later — inside retro_init, or at retro_load_game, or on the first
// frame — cannot be seen from here, and this header does not pretend otherwise: `needs` is what the core told
// us before it ran, not a guarantee that it will run.
//
// WHY NOT retro_init. retro_init allocates, spawns threads and (in a few cores) touches hardware; it is where
// the frontend stops inspecting and starts hosting. Every fact this feature needs is available before it, and
// the libretro docs put SET_SUPPORT_NO_GAME in retro_set_environment specifically so a frontend can list
// no-content cores without booting them. So the inspection never calls it, never calls retro_load_game, and
// closes the library again before returning.
//
// The one call that DOES enter the core (retro_set_environment) runs under the same structured-exception guard
// LibretroCore::loadCore uses, so a core that faults on contact becomes a sentence rather than a process death
// (on Windows; on POSIX a fault is still a fault, and the honest statement is in the report, not in a promise).
#pragma once
#include <QString>
#include <QStringList>

// Everything a core says about itself before it runs.
struct CoreInspection
{
    QString     libraryName;          // retro_get_system_info().library_name
    QString     libraryVersion;       // retro_get_system_info().library_version ("" when the core gives none)
    QStringList extensions;           // valid_extensions split on '|', lowercased, no leading dots
    bool        needFullpath = false; // the core wants a PATH, not the bytes
    bool        blockExtract = false; // do not hand it an archive's contents
    bool        supportsNoGame = false; // declared SET_SUPPORT_NO_GAME during retro_set_environment
    unsigned    apiVersion = 0;       // retro_api_version()
    // The environment requests we had to refuse, as human phrases ("a Vulkan renderer", "camera access").
    // EMPTY is the overwhelmingly common case. Advisory: a core can ask for something, be refused, and run
    // perfectly well on its fallback path — so this is reported, never used to block a load.
    QStringList unmet;
};

namespace CoreInspect
{
    // Load `path` far enough to answer the questions above, then unload it. Returns false with a SENTENCE in
    // *error when the file is not a loadable library, is missing the libretro exports, or reports a libretro
    // API version this frontend does not speak. Never runs the core.
    bool inspect(const QString& path, CoreInspection* out, QString* error);

    // PURE. The advisory sentence for a core whose declared needs we cannot serve — "" when there are none.
    // Separate from inspect() so it can be pinned without a core file, and so the UI has exactly one wording.
    QString unmetSentence(const CoreInspection& in);

    // PURE. Split a libretro valid_extensions string ("gba|agb|bin") into lowercase, dotless extensions.
    // Tolerates leading dots, spaces and empty segments, all of which appear in real cores.
    QStringList splitExtensions(const QString& validExtensions);
}
