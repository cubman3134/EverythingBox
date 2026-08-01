// "I'm caught up on this" — the per-show dismissal watermarks behind the "You missed" surface (issue #25).
//
// One number per (profile, show): the air time THROUGH which that show's missed episodes have been waved
// away. `trakt::planMissed` reads it through a callback and drops every episode that aired at or before it.
// The SHAPE of that number is argued at length in TraktMissed.h — read that first; the short version is:
//
//   * 0 / absent = never dismissed, any positive stamp = dismissed through it. Two distinct spellings, so
//     the #132 hazard ("cleared" and "never known" sharing a representation) cannot arise: nothing here is
//     ever expressed as a REMOVAL, so there is no husk and no tombstone to get wrong.
//   * The only mutation is RAISING it (dismissThrough takes the max). That makes the store a
//     join-semilattice, which is why CloudMerge can carry it with a plain per-key max and needs neither a
//     tombstone space nor the equal-timestamp tie-break every other per-item store needs.
//   * It EXPIRES. Unlike a husk, a stamp older than the lookback window can suppress nothing that is still
//     on screen, so collecting it is invisible — and so is a peer re-propagating one, which is what makes
//     the collection safe in a store that syncs. trakt::missedDismissExpired owns that rule; this owns
//     the sweep.
//
// LAYOUT, per profile, exactly the namespacing ItemMarks uses and for the same reason — the rows this
// suppresses are one viewer's, so one profile's "I'm caught up" must not silence another's:
//
//   missed/<profileId>/shows/<md5(showKey)>  -> unix seconds
//
// The show key is trakt::missedShowKey (the show's IMDB id). It is hashed before use as an ini leaf with the
// SAME MD5-hex-over-UTF8 as ItemMarks and MetaOverrides — no fourth scheme — even though today's keys happen
// to be leaf-safe: the store should not be the thing that breaks if the key space ever widens.
//
// IT SYNCS, through the CloudMerge progress document rather than the heavy settings bundle (CloudSync::
// isPerItemStoreKey matches "missed/"). That is a deliberate answer to "should a dismissal follow you": yes.
// Waving away a month of a show on the TV and being nagged about it on the phone an hour later is the same
// complaint the marks sync exists to answer. The per-item channel rather than the bundle for the ordinary
// reason — a dismissal is a per-item tick, and riding the bundle would flip its stateHash and re-upload the
// whole zip for one button press.
//
// Backed by the portable everythingbox.ini, QtCore only (QSettings), same AppPaths::dataDir() posture as
// ItemMarks/MetaOverrides — so it links into headless probes with no UI anywhere near it.
#pragma once
#include <QString>
#include <functional>

namespace MissedDismiss
{
    // The stamp for one show, unix seconds; 0 = never dismissed (also the answer for an empty key and for a
    // corrupt row). Cached per active profile, like ItemMarks — planMissed asks once per show per rebuild.
    qint64 through(const QString& showKey);

    // Raise the stamp. MONOTONE: a value at or below the stored one is a NO-OP — no write, no timestamp
    // bump, no sync churn — which is what lets two devices apply each other's dismissals in either order and
    // land in the same place. There is deliberately no way to lower or clear one; see TraktMissed.h for why
    // the undo a user actually wants (the show returning when it airs something new) needs no such API.
    void dismissThrough(const QString& showKey, qint64 throughUnix);

    // Drop every stamp that can no longer suppress anything, across EVERY profile — this is a sweep of the
    // ini, not of the active profile's cache, because a profile nobody has selected for a year is exactly
    // the one accumulating records nothing ever collects. Returns how many rows it removed.
    //
    // Invisible by construction: trakt::missedDismissExpired only says yes once every episode a stamp could
    // suppress has left the lookback window, so a collected record and a kept one render identically. It is
    // therefore safe to run on a schedule, and safe for a peer that still holds the record to hand it back.
    int prune(qint64 nowUnix);

    // Drop the cache (profile switch, or an external ini change — CloudMerge calls it after a merge, the way
    // it already does for ItemMarks and MetaOverrides).
    void invalidate();

    // The multi-device sync trigger, the same std::function seam ItemMarks and MetaOverrides carry: fired
    // after a mutation that actually wrote, so a no-op dismissThrough arms nothing. Unset in probes.
    void setChangeHook(std::function<void()> hook);
}
