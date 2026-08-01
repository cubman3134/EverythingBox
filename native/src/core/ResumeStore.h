// The resume-position key scheme, and the ONE spelling of "forget where you left off" (issue #150).
//
// Layout — GLOBAL, not per profile (same posture as metaoverrides/*, unlike marks/*): where a household member
// left off in a file is a property of the file.
//   resume/<hash>/pos     -> seconds into the media
//   resume/<hash>/dur     -> its duration (the Home progress bar needs both)
//   resume/<hash>/ts      -> when the position was saved (epoch seconds) — the merge's recency key
//   resume/<hash>/title   -> a display label
// <hash> is the first 10 hex chars of MD5-over-UTF8 of the item's stable key (its addon item id when it has
// one, else its url/path). PlaybackSession writes the group, HomeView reads it for the progress overlay, and
// CloudMerge merges it newest-ts-wins.
//
// A CLEAR RECORDS A TOMBSTONE, because a removed group is indistinguishable from "this device has never played
// that file" (issue #150, the shape of #132). CloudMerge's resume pass never deletes — an absence has no
// timestamp — so a peer still holding the pre-clear position won on presence rather than on time and put the
// position straight back. Worse than the marks case: the cloud document holds THIS device's own pre-finish row,
// so finishing an episode and syncing resurrected it with no second device involved at all.
//
// A tombstone, and NOT the stamped husk #132 gave ItemMarks, and the difference is the point. A resume clear
// fires on every finished episode, so husks would grow with PLAYBACK rather than with deliberate user actions
// and every one would ride the sync document for ever. Tombstones are bounded by Tombstones::compact(30)
// instead — which costs a resurrection if a peer is dormant for 31 days and comes back still holding the
// position. That is acceptable HERE and was not acceptable for a mark: a month-old playback point is stale
// anyway, while a hide/complete/tag is a deliberate statement with no expiry. The rule this choice is made
// against is written next to CloudMerge::remoteReplaces.
//
// Clearing is the only verb here. Writing a position stays with PlaybackSession (the throttled playback funnel,
// which has the duration and the stats accrual around it) — it calls noteResumed() so a fresh position undoes
// an earlier clear.
#pragma once
#include <QString>

class QSettings;

namespace ResumeStore
{
    // The ini group one item's resume state lives under: "resume/<hash>".
    QString groupFor(const QString& key);

    // The tombstone store name for resume clears ("resume"), and the tombstone KEY for one item (its <hash>,
    // which is exactly the key CloudMerge's resume document is indexed by). Named here so the merge and the
    // clear sites cannot drift apart on the spelling.
    QString tombStore();
    QString tombKey(const QString& key);

    // Forget the saved position for `key` in `s`, and record a DATED tombstone so the clear survives the merge.
    // `s` is the caller's QSettings on the shared ini (PlaybackSession owns an injectable one; the tombstone
    // always goes through Tombstones, i.e. the shared portable ini, which is the same file in the app).
    //
    // The tombstone is only recorded where a record existed to clear — clearing an item that carries no
    // position writes nothing, exactly as ItemMarks leaves no husk for a no-op unhide. "Never played" is then
    // the truth, and a tombstone there would both record an event that did not happen and grow deleted/* on
    // every finished file that was never resumable in the first place.
    void clear(QSettings& s, const QString& key);

    // A fresh position was just saved for `key`: drop any tombstone for it, because the clear has been undone
    // by the user resuming the item. Without this a re-watch that lands in the SAME second as the clear would
    // be suppressed by its own device's tombstone (the merge's `tomb >= item` rule, which favourites share).
    void noteResumed(const QString& key);
}
