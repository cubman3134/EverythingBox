// Per-game pad2key record (issue #105) — the STORE half. Whether pad2key is ENABLED for one game, and an
// optional CUSTOM profile for it; when no custom profile is stored the effective profile is the per-system
// default (pad2key::defaultProfile — the DOS default is the one that matters). Keyed by the SAME stable identity
// every per-item store uses (the key GameLauncher::open() carries), hashed with the SAME full MD5-hex-over-UTF8
// as LaunchOptionsStore / EmuGfxStore / ItemMarks. QtCore-only (a QSettings wrapper over the portable
// everythingbox.ini), so it runs under the offscreen QPA in probe_pad2key.
//
// SYNCED, HUSK-ON-CLEAR — the LaunchOptionsStore posture, NOT EmuGfxStore's device-local one. The issue says
// per-game pad2key profiles "ride the same sync category as #51's overrides", and unlike a graphics override a
// pad→key mapping is NOT hardware-specific: a profile that makes a keyboard-only game couch-playable is right on
// every device. So the layout is a flat hash under a synced prefix and CloudMerge carries it:
//   pad2key/items/<md5(key)>  -> compact JSON { enabled, profile{name,map}, updatedAt }
// and reset() writes a timestamp-only HUSK rather than deleting the row, so a peer still holding the old record
// can't resurrect a mapping the user just cleared (issue #132's idiom, LaunchOptionsStore verbatim).
//
// GLOBAL, not per profile (same posture as launchopts/*): whether a game needs pad2key is a property of the
// game, not the viewer. A byte-equal write is a no-op that does NOT refresh the stamp (issue #167). "Not
// overridden" has exactly one spelling — an absent row, or a husk that reads as empty — never a choice.
#pragma once
#include <QString>
#include <functional>
#include "Pad2Key.h"

namespace Pad2KeyStore
{
    struct Entry
    {
        bool             enabled = false;   // synthesise keystrokes for this game while it holds focus
        pad2key::Profile profile;           // a CUSTOM profile; empty => use the per-system default
        qint64           updatedAt = 0;

        // Nothing to apply: pad2key off AND no custom profile. Ignores updatedAt, so a clear husk is empty (=
        // "no override") while still being a real, newer, propagating record. A disabled entry that still holds
        // a custom profile is NOT empty — the user's authored mapping survives being toggled off.
        bool isEmpty() const { return !enabled && profile.isEmpty(); }
    };

    QString hashKey(const QString& key);              // md5-hex over UTF-8 (the shared per-item key scheme)

    Entry get(const QString& key);                    // absent/empty key -> a default (off, no profile) Entry
    bool  has(const QString& key);                    // is anything set for this game (enabled or a custom profile)
    bool  enabled(const QString& key);                // the runtime gate: is pad2key on for this game
    void  set(const QString& key, const Entry& e);    // normalize; stamp+persist only on a real change; husk-on-clear
    void  setEnabled(const QString& key, bool on);    // convenience: flip the enable bit, keep any custom profile
    void  reset(const QString& key);                  // clear: a newer, empty, still-propagating husk

    // The profile the runtime should actually inject: the game's custom profile when it has one, else the
    // per-system default for `systemId` (the DOS default for "msdos"). Empty when neither exists.
    pad2key::Profile effectiveProfile(const QString& key, const QString& systemId);

    void invalidate();                                // drop the cache (external ini change / after a cloud merge)

    // Multi-device sync trigger, same contract as LaunchOptionsStore::setChangeHook: fired after every mutation
    // so MainWindow can (re)arm the debounced push. Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);
}
