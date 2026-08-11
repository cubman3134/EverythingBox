// Per-item playback-speed memory (issue #140). A narrator's ideal speed is a property of the CONTENT, not the
// device — re-setting the speed every session is the annoyance every audiobook listener knows — so a chosen
// speed is remembered per item, keyed by the same stable resume key the resume position uses, and it SYNCS.
//
// Layout mirrors metaoverrides/launchopts (the GLOBAL per-item stores): a JSON blob per hash under
//   speed/items/<hash>   ->   {"rate": <double>, "updatedAt": <epoch seconds>}
// <hash> is the 10-hex-char MD5 of the item's stable key (its addon item id, else its url/path) — the exact
// leaf ResumeStore uses, so one book's resume and speed share an identity. GLOBAL rather than per profile for
// the metaoverrides reason: which speed a given narrator wants is not a per-viewer preference.
//
// CLOUD SYNC. The "speed/" prefix is in CloudSync::isPerItemStoreKey (NOT isDeviceLocalKey): the value belongs
// to the content and should follow the user across devices, exactly like resume. CloudMerge carries a "speed"
// section keyed by hash, newest-updatedAt-wins per item, no tombstones — the same shape and reasoning as
// metaoverrides (there is no "clear speed" verb that must survive a merge; changing the speed writes a newer
// record that propagates). probe_cloudmerge pins that the prefix is per-item-synced and not device-local, and
// that the section round-trips newest-wins both merge directions.
//
// The resolve rule lives here as a PURE function so probe_listening can pin it without a store: the stored
// per-item speed wins when set; else the global default; MUSIC is forced to 1x unless a speed was explicitly
// stored for it (genre-appropriate default — you do not want your music sped up because your audiobook was).
#pragma once
#include <QString>

namespace SpeedStore
{
    // The ini group the per-item speed blobs live under ("speed/items"), named here so the store and
    // CloudMerge's serializer/merger cannot drift on the spelling.
    QString itemsGroup();

    // The 10-hex-char MD5 leaf for `key` — the same identity ResumeStore uses, so a book's resume and speed
    // records name the same hash.
    QString hashFor(const QString& key);

    // The stored per-item speed for `key`, or 0.0 when none is stored (a real rate is always > 0, so 0.0 is an
    // unambiguous "unset" the resolve below keys on). An empty key is unset.
    double storedForItem(const QString& key);

    // Remember `rate` as this item's speed (stamped now). No-op for an empty key or a non-positive rate (a
    // rate <= 0 is meaningless and would be read back as "unset").
    void setForItem(const QString& key, double rate);

    // The speed to actually play at: the stored per-item value when one is set (any content type), else the
    // global default — except MUSIC, which stays 1x unless a per-item speed was explicitly stored. A
    // non-positive globalDefault falls back to 1x so a corrupt/absent setting never yields a zero rate.
    double speedForItem(double storedForItem, double globalDefault, bool isMusic);
}
