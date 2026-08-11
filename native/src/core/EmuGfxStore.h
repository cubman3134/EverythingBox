// Per-game (and per-system-default) standalone-emulator graphics overrides (issue #103) — the STORE half.
// Holds an EmuGfx::Settings (the internal-resolution / aspect / vsync / renderer / MSAA quartet) keyed by the
// SAME stable identity every per-item store uses (the key GameLauncher::open() carries), hashed with the SAME
// full MD5-hex-over-UTF8 as LaunchOptionsStore / LaunchHooksStore / ItemMarks. The launch pipeline resolves a
// per-game override over a per-system default (EmuGfx::resolve) and EmulatorManager writes the resulting edits
// into the emulator's own config before boot.
//
// DEVICE-LOCAL, AND NOT SYNCED — the LaunchHooksStore posture, and for a sharper reason than hooks: a graphics
// override is inherently HARDWARE-specific. A renderer backend that works on the desktop's GPU may not exist on
// the laptop; a 6x internal resolution a strong card eats will crawl on a weak one. Syncing "run this game at 6x
// Vulkan" to every device is a footgun, so the emugfx prefix is device-local (it does NOT go in CloudMerge's
// per-item set). Because it never merges, there is no husk-on-clear dance: reset() is a plain row delete.
//
//   emugfx/items/<md5(key)>  -> compact JSON (EmuGfx::toJson: res / aspect / vsync / renderer / msaa, omit-empty)
//
// PER-SYSTEM DEFAULT. The same store also holds one default per SYSTEM, under a reserved key that cannot collide
// with a real game identity (systemKey(id) prefixes an ASCII-control byte). resolve() layers the per-game
// override over that default, so a system-wide "all PS2 games at 3x" plus a per-game exception both work through
// one store. QtCore-only (a QSettings wrapper over the portable everythingbox.ini), so it runs under the
// offscreen QPA in probe_emusettings.
#pragma once
#include <QString>
#include "EmuSettings.h"

namespace EmuGfxStore
{
    QString hashKey(const QString& key);                     // md5-hex over UTF-8 (the shared per-item key scheme)
    QString systemKey(const QString& systemId);              // reserved, collision-proof key for a system default

    EmuGfx::Settings get(const QString& key);                // absent/empty key -> an all-unset Settings
    bool             has(const QString& key);                // is any lever set for this game/system
    void             set(const QString& key, const EmuGfx::Settings& s); // persist on a real change; all-unset removes
    void             reset(const QString& key);              // delete any override for this game/system (plain remove)

    // Convenience: the per-system default for a system id (get(systemKey(id))).
    EmuGfx::Settings systemDefault(const QString& systemId);

    void invalidate();                                       // drop the cache (external ini change / test reset)
}
