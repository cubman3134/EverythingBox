// SEVERAL TRACKERS AT ONCE (issue #156, increment 2).
//
// With AniList alone there was nothing to decide: one tracker, one link, one push. With MyAnimeList beside
// it there is, and the issue is explicit about what the answer has to be — "push to both and let each own
// its own state". This is that rule, written once, off the pure seam (tracker::Tracker) so a probe can drive
// it with fakes and no socket.
//
// THE THREE PROPERTIES IT EXISTS TO HOLD:
//
//  1. EACH TRACKER RESOLVES ITS OWN LINK. AniList media 30002 and MyAnimeList media 2 are the same series
//     under two ids on two accounts, and TrackerLinks is already keyed by (Id, itemKey) for exactly that.
//     Nothing here ever hands one tracker another's media id.
//
//  2. ONE FAILING MUST NOT BLOCK ANOTHER. Every tracker in the list is visited, in order, and no tracker's
//     answer is allowed to end the loop: being switched off, being unlinked, being rate-limited or having a
//     dead socket are all conditions of ONE account. The queues are per tracker (TrackerQueue is keyed by
//     Id), so a chapter one account refused stays pending on that one and is gone from the other.
//
//  3. NO DOUBLE COUNTING. A progress event produces at most ONE update per tracker, and the debounce and
//     the coalescing that stop a binge-read spending a rate limit are the shared ones from increment 1 —
//     applied per tracker, because two accounts do not share a rate limit either.
//
// AND ONE THING IT DELIBERATELY DOES NOT DO: it never links anything. Prompting is the caller's, and the
// caller prompts for at most one tracker per event — see `needLink`.
#pragma once
#include "Tracker.h"

#include <QString>
#include <QVector>

namespace TrackerFanout
{
    // The trackers that are ON: configured AND connected. Order is preserved, and a null pointer in `all`
    // is skipped rather than dereferenced — the caller's list is built from members that may not have been
    // constructed yet.
    QVector<tracker::Tracker*> active(const QVector<tracker::Tracker*>& all);

    struct Result
    {
        int pushed = 0;      // trackers that were handed an update for this item
        int unlinked = 0;    // active, but this item has no link on them
        int declined = 0;    // active and unlinked, and the user has said not to ask about this item
        // Active, unlinked, and NOT declined: the ones that would like the user asked. The caller prompts
        // for AT MOST ONE of these per progress event — two nested pick loops stacked on one page turn is
        // both the #28/#211 crash family and an unusable interruption. The next event offers the next one.
        QVector<tracker::Tracker*> needLink;
    };

    // Fan one completion event out. For each ACTIVE tracker: if the item is linked there, raise that
    // tracker's own local progress (monotonically) and hand it an Update built from ITS link; otherwise
    // record it under unlinked/declined/needLink. Returns what happened, which is what lets a caller say
    // "sent to 2 trackers" rather than guessing.
    //
    // `unit` <= 0 or an empty `itemKey` is a no-op: an item with no identity has nowhere to remember
    // anything, and a non-positive unit is not progress.
    Result push(const QVector<tracker::Tracker*>& all, const QString& itemKey, tracker::Kind kind,
                int unit, bool completes);
}
