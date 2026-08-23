// Per-item lyric OFFSET memory (issue #142). A community .lrc file that runs half a second ahead runs half a
// second ahead every time you play that track, on every device — the drift is a property of the CONTENT (of
// the lyric file that came with it), not of this machine and not of the listener — so the nudge is remembered
// per item, and it SYNCS.
//
// Layout mirrors speed/metaoverrides/launchopts (the GLOBAL per-item stores): a JSON blob per hash under
//   lyricoffset/items/<hash>   ->   {"offset": <double seconds>, "updatedAt": <epoch seconds>}
// <hash> is the 10-hex-char MD5 of the item's key, hashed exactly as ResumeStore and SpeedStore hash theirs.
// GLOBAL rather than per profile for the SpeedStore reason: how far out a lyric file is is not a per-viewer
// preference, it is a fact about the file.
//
// THE KEY IS THE TRACK, NOT THE ITEM, and that is the one place this deliberately differs from SpeedStore.
// A speed belongs to a BOOK — one narrator, one rate, across every file in the queue — so SpeedStore keys on
// the resume key of the thing being played. An offset belongs to ONE .lrc file, and an album queue is a
// dozen of them; keying on the item would make correcting track 3 shift the lyrics of track 4. So the caller
// passes the track's own identity — LyricFetch::cacheKey(path), the absolute cleaned path that the online
// lyric cache is already keyed by, so a track's fetched lyrics and its correction to them name the same
// thing.
//
// 0.0 IS "UNSET", AND THAT IS FINE HERE. Unlike a speed (where 0 would be a meaningless rate and so doubles as
// the unset sentinel), 0.0 is a perfectly legal offset — it just means "no nudge", which is also what no
// record at all means. The two are indistinguishable on purpose: clearing a nudge writes 0.0 rather than
// deleting the row, so a peer holding the old value is overwritten by a NEWER record instead of needing a
// tombstone (the husk shape metaoverrides uses, for the same reason).
//
// CLOUD SYNC. The "lyricoffset/" prefix is in CloudSync::isPerItemStoreKey (NOT isDeviceLocalKey): the value
// belongs to the content and should follow the user across devices, exactly like resume and speed. CloudMerge
// carries a "lyricoffset" section keyed by hash, newest-updatedAt-wins per item, no tombstones — the same
// shape and reasoning as speed. probe_cloudmerge pins the classification and the round-trip.
//
// The ARITHMETIC an offset takes part in is not here: LyricSeek owns the sign convention, the ±0.5 s step and
// the clamp, as pure functions, and this store only remembers a number LyricSeek has already sanitised.
#pragma once
#include <QString>

namespace LyricOffsetStore
{
    // The ini group the per-item offset blobs live under ("lyricoffset/items"), named here so the store and
    // CloudMerge's serializer/merger cannot drift on the spelling.
    QString itemsGroup();

    // The 10-hex-char MD5 leaf for `key` — the same hashing ResumeStore/SpeedStore use. What is HASHED is a
    // track identity (see the header note), not the item key those two hash.
    QString hashFor(const QString& key);

    // The stored offset in seconds for `key`, or 0.0 when none is stored (0.0 is also a legal stored value —
    // see the header note; "no nudge" and "no record" mean the same thing to every caller). An empty key, a
    // malformed blob or an out-of-range value all read back as 0.0, never as a wild offset.
    double forItem(const QString& key);

    // Remember `offsetSec` as this item's lyric offset (stamped now). The value is snapped to the ±0.5 s grid
    // and clamped by LyricSeek::clampOffset before it is written, so nothing off-grid can reach the store.
    // No-op for an empty key (an item with no identity has nowhere to remember anything).
    void setForItem(const QString& key, double offsetSec);
}
