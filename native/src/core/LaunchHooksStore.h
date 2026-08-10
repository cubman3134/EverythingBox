// Per-game pre-launch / post-exit command hooks (issue #64) — the STORE half: two optional command lines per
// game, keyed the SAME stable identity every per-item store uses (the key GameLauncher::open() already
// carries), hashed with the SAME full MD5-hex-over-UTF8 as LaunchOptionsStore/ItemMarks/MetaOverrides.
//
// DEVICE-LOCAL, AND NOT SYNCED — the one deliberate difference from #51's LaunchOptionsStore, which DOES ride
// cloud sync. A synced command line executing on a different machine is a footgun (a path that exists on the
// desktop but not the laptop, a tool installed on one box only), so the hooks prefix goes in
// CloudSync::isDeviceLocalKey, NOT isPerItemStoreKey. Because it is device-local and never merges, there is no
// husk-on-clear dance (that idiom exists only to stop a peer resurrecting a cleared record across a merge):
// reset() is a plain row delete.
//
//   launchhooks/items/<md5(key)>  -> compact JSON { preLaunch, postExit }
//
// Values are trimmed at write time; a hook is "set" when present and non-empty. Setting both to empty on a game
// that had a record deletes the row (no husk needed — nothing to out-race). QtCore-only (a QSettings wrapper
// over the portable everythingbox.ini), so it runs under the offscreen QPA in probe_launchhooks.
#pragma once
#include <QString>

namespace LaunchHooksStore
{
    struct Hooks
    {
        QString preLaunch;   // command line run to completion before the game launches (empty = no pre-hook)
        QString postExit;    // command line run after the game/emulator exits (empty = no post-hook)

        bool isEmpty() const { return preLaunch.isEmpty() && postExit.isEmpty(); }
    };

    QString hashKey(const QString& key);                 // md5-hex over UTF-8 (the shared per-item key scheme)
    Hooks   get(const QString& key);                     // absent/empty key -> empty Hooks
    bool    has(const QString& key);                     // is either hook set for this game
    void    set(const QString& key, const Hooks& h);     // trim; persist on a real change; empty+empty removes
    void    reset(const QString& key);                   // delete any hooks for this game (device-local: plain remove)

    void invalidate();                                   // drop the cache (external ini change / test reset)
}
