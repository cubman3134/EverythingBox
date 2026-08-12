// Per-game (and per-system-default) slang-shader PRESET override (issue #99, SLICE 1) — the STORE half.
// The "third payload type" after #51 launchopts and #103 emugfx: it holds a preset id (a ShaderPreset id string
// — a curated builtin like "crt", the explicit "off", or a "custom:<abs.slangp>" path) keyed by the SAME stable
// identity every per-item store uses (the key GameLauncher::open() carries), hashed with the SAME MD5-hex-over-
// UTF8 as LaunchOptionsStore / EmuGfxStore / ItemMarks. A LATER render slice resolves per-game over per-system
// over the global default (ShaderPreset::resolvePreset) and hands the winning id to librashader; THIS slice only
// stores and reads it — nothing renders yet.
//
// DEVICE-LOCAL, AND NOT SYNCED — the EmuGfxStore posture, for the SAME reason: a shader's cost is HARDWARE-
// dependent. A Mega-Bezel chain a strong desktop GPU eats will crawl on a weak handheld, so syncing "run this
// game under Mega-Bezel" to every device is a footgun. The shaderpreset prefix is therefore device-local (it is
// carved out in CloudSync::isDeviceLocalKey and does NOT enter CloudMerge's per-item set). Because it never
// merges, there is no husk-on-clear dance: reset() is a plain row delete, exactly like EmuGfxStore.
//
//   shaderpreset/items/<md5(key)>  -> the preset id string (empty id removes the row)
//
// PER-SYSTEM DEFAULT. The same store also holds one default per SYSTEM, under a reserved key that cannot collide
// with a real game identity (systemKey(id) prefixes an ASCII-control byte, like EmuGfxStore). QtCore-only (a
// QSettings wrapper over the portable everythingbox.ini), so it runs under the offscreen QPA in probe_shaderpreset.
#pragma once
#include <QString>

namespace ShaderPresetStore
{
    QString hashKey(const QString& key);                 // md5-hex over UTF-8 (the shared per-item key scheme)
    QString systemKey(const QString& systemId);          // reserved, collision-proof key for a system default

    QString get(const QString& key);                     // absent/empty key -> "" (no override at this scope)
    bool    has(const QString& key);                     // is any preset set for this game/system
    void    set(const QString& key, const QString& presetId); // persist; an empty id removes the row
    void    reset(const QString& key);                   // delete any override for this game/system (plain remove)

    // Convenience: the per-system default for a system id (get(systemKey(id))).
    QString systemDefault(const QString& systemId);

    void    invalidate();                                // drop the cache (external ini change / test reset)
}
