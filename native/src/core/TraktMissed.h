// "You missed" (issue #25): the backward-looking half of the Trakt calendar. #23 built the forward half —
// episodes of your followed shows still to air. This is the complement: episodes that ALREADY aired, on
// shows you follow, that you have not watched and have not waved away.
//
// It is a JOIN over things the app already holds, not a new source of data:
//
//     the CALENDAR   says what aired and when   (CalendarEntry, TraktRead.h — one already-cached fetch)
//   x the MARKS      say what you have watched  (ItemMarks, through the LocalState callback below)
//   x the DISMISSALS say what you waved away    (MissedDismiss, through the second callback)
//
// Pure, exactly like TraktRead and TraktSync: structs + callbacks in, structs out. No network, no GUI, no
// ini, no ItemMarks — the two things it needs to know about local state arrive through std::function, the
// same seam planWatchedBackfill uses. probe_trakt pins every rule below with no I/O and no account.
//
// ============================================================================================
// WHAT COUNTS AS "MISSED" — the four clauses, and why each is drawn where it is
// ============================================================================================
//
// 1. IT AIRED.  `airedAtUtc <= nowUtc`, CLOSED on the recent side. traktCalendarCatalog's future test is
//    `airsAtUtc > nowUtc`, strictly, so the two surfaces PARTITION the calendar at the tick: an episode
//    airing exactly now belongs here and to nothing else. SyntheticCatalogs.h already promised that
//    ("the boundary is CLOSED on the past side — airsAtUtc <= nowUtc is excluded, so an episode airing
//    exactly now belongs to #25"); this is the other half of that promise, and probe_browse pins both.
//
// 2. IT IS STILL IN THE WINDOW.  `airedAtUtc >= nowUtc - lookbackDays`, CLOSED on the old side. Thirty
//    days is the default (kMissedLookbackDays) because it is the "I have been away/busy for a month"
//    horizon this surface exists to answer, and because a month of a followed-show list is one calendar
//    request — the same request #23 already makes, asked for a wider span.
//
//    The window is re-applied HERE, at render time, and not left to the fetch. The fetch's window is
//    anchored at the moment it ran; the cache it wrote outlives that by hours or (offline) days, so a
//    cached calendar routinely holds entries that have since fallen out. Without this clause a set-top box
//    left on would slowly accumulate a surface that is older than it claims to be. The fetch bound and this
//    bound are therefore NOT redundant: the fetch bounds the request, this bounds the claim.
//
// 3. YOU HAVE NOT WATCHED IT.  `localState(streamId) == LocalState::Unmarked`.
//
//    BOTH non-Unmarked states clear the row, and that is where this rule DIFFERS from the backfill's use of
//    the same enum. planWatchedBackfill has to tell Watched from OtherExplicit because the two lead to
//    different writes. Here they lead to the same answer for one reason: this surface is about things you
//    do not KNOW about. "In progress", "abandoned" and "planned" are all statements the user has already
//    made about that exact episode, so it is not a surprise to them, and re-raising it is nagging. The
//    caller folds `hidden` into OtherExplicit for the same reason.
//
//    An episode with NO usable stream id is DROPPED, not carried. That is the opposite of what the calendar
//    does with the same entry — traktCalendarCatalog keeps it, unplayable, rather than silently losing a
//    third of the user's week — and the asymmetry is deliberate. A calendar row is a statement about the
//    WORLD ("this airs Tuesday") and needs no local knowledge to be true. A "you missed this" row is a
//    statement about the USER, and without a key there is no marks lookup, so the claim rests on no
//    evidence: it would show to someone who watched the episode, be unplayable, and stay for the whole
//    window. A false accusation you cannot act on is worse than an absent row.
//
// 4. YOU HAVE NOT WAVED IT AWAY.  `airedAtUtc <= dismissedThrough(showKey)` drops the episode. See the
//    dismissal note below for why that is a per-show WATERMARK rather than a per-episode flag.
//
// ============================================================================================
// ONE ROW PER SHOW — the clause that keeps this surface small
// ============================================================================================
// A show you follow and have never started is not one missed episode, it is every episode it aired in the
// window: four or five for a weekly show, more for a daily. Forty such shows is two hundred rows, and the
// user who has been away for a month — the exact user this feature is for — is the one who gets all of
// them. Listed per episode this surface would be unreadable precisely when it matters.
//
// So a show contributes AT MOST ONE row, and the surface is bounded by the size of your followed list
// rather than by the length of your absence. The row still carries the whole group:
//
//   * the episode it PLAYS is the OLDEST unwatched one in the window. Not the newest: you resume a show
//     where you left off, and handing Play the newest would skip the four episodes underneath it.
//   * the episode it SORTS by is the NEWEST, so a show that aired last night leads a show that stopped
//     three weeks ago. That is the issue's "most-recent first, for actionability".
//   * `count` is how many are waiting, so the row can say "and 3 more" instead of pretending there is one.
//
// A show that has never been started therefore looks exactly like a show you are one episode behind on,
// which is the honest rendering: in both cases the next thing to do is play the oldest one.
//
// ============================================================================================
// DISMISSAL IS A PER-SHOW HIGH-WATER MARK, NOT A FLAG AND NOT A TOMBSTONE
// ============================================================================================
// "I am not going to catch up on this" is stored as one number per show: the air time THROUGH which that
// show is dismissed. Every clause of that shape is load-bearing.
//
//   * PER SHOW, because the row is per show. A per-episode flag would need one write per episode behind
//     the row, and would grow with the length of the backlog rather than with the number of shows.
//   * A WATERMARK, not a boolean, because dismissing must not also dismiss the FUTURE. A flag would
//     suppress next week's episode too, which turns "I am caught up" into "unfollow", silently.
//   * MONOTONE (merge by MAX), which is what makes it safe across devices with no tombstones and no
//     tie-break. #132 and #150 hold that in a store merged by timestamp, "cleared" and "never known" must
//     not share a representation — the husk idiom exists because a REMOVED row reads to the merge as
//     "this device never saw that item" and a peer resurrects what the user just cleared. That hazard
//     needs a clear to be expressible as a removal. Here it is not: 0/absent means "never dismissed",
//     any positive stamp means "dismissed through it", and the only mutation is raising it. The store is
//     a join-semilattice — order-independent, idempotent, convergent — so neither a husk nor a tombstone
//     has anything to represent.
//
//     The price is that a dismissal cannot be UNDONE by a lower write, and that is accepted rather than
//     worked around: the show returns to the surface on its own the moment it airs something newer, which
//     is the only undo a user actually asks for here, and it costs nothing to wait for.
//
//   * IT EXPIRES, which is the tombstone half of the answer applied to garbage collection rather than to
//     correctness. A dismissal can only be made while its episodes are in the window, so
//     `airedAt <= dismissedAt`, so every episode a stamp can suppress has left the window once
//     `now - dismissedAt > lookbackDays`. Past that point the record is INERT: dropping it changes no
//     rendering, and a peer that still holds it and re-propagates it changes no rendering either. That is
//     exactly what MetaOverrides' husks can never claim — a husk has to outlive any peer's stale copy for
//     ever, because the thing it suppresses never expires — and it is why this store can be collected and
//     that one cannot. kMissedDismissTtlDays keeps a wide margin over the tight bound anyway; see it.
//
// ============================================================================================
// WHAT THIS DELIBERATELY DOES NOT COVER
// ============================================================================================
// WATCHLIST AND COLLECTION MOVIES. The issue asks for them and they are not here, because nothing the app
// holds says WHEN a watchlisted movie was released: TraktListEntry carries `addedAt` (when YOU listed it)
// and a year, and neither is a release date. "Already released" would therefore be a guess, and #23's own
// rule — a row that looks actionable and cannot be is worse than an absent row — applies with full force to
// a surface whose entire claim is "this is out now and you have not seen it". Supporting them properly
// means /calendars/my/movies, its own cache, and its own staleness rules; that is a second feature, not a
// clause of this one.
#pragma once
#include "TraktRead.h"    // CalendarEntry + TraktIds + the imdb id mappings
#include "TraktSync.h"    // LocalState — the three-valued "what the app already knows" the backfill uses

#include <QDateTime>
#include <QString>
#include <QVector>
#include <functional>

namespace trakt
{
    // How far back "missed" reaches, in days. See clause 2 above for why thirty.
    constexpr int kMissedLookbackDays = 30;

    // How long a dismissal record is kept before it is collected, in days. The TIGHT bound is
    // kMissedLookbackDays — at exactly that age the newest episode a stamp can suppress has just left the
    // window — and this is deliberately three times it. The margin is not superstition: the stamps are
    // written on whichever device the user pressed the button on, merged into another device's ini, and
    // compared against THAT device's clock, so a household whose boxes disagree by a day would otherwise be
    // collecting records that are still doing work. Three times the window is far past any real skew, and
    // the record it keeps alive is one integer.
    constexpr int kMissedDismissTtlDays = 3 * kMissedLookbackDays;

    // How many rows the HOME SHELF shows. The FOLDER is uncapped — it is where you go to deal with the
    // whole backlog — but a shelf is a strip you scan on the way past, and a strip long enough to need
    // scrolling has stopped being a glance. Eight is what fits without the surface below it moving off
    // screen on a TV. The cap belongs here, next to the rule, so both surfaces read the same number.
    constexpr int kMissedShelfMax = 8;

    // The identity a dismissal is filed under: the SHOW's usable IMDB id, and there is deliberately no
    // fallback for a show that has none.
    //
    // The fallback would be unreachable, which is worse than missing. A show reaches this surface only if
    // at least one of its episodes produced a stream id, and imdbStreamIdFor produces one only from a
    // usable show IMDB id — so a show with no usable id contributes no episodes, no group, and no row, and
    // a title-based fallback here could never be exercised by anything. probe_trakt pins the agreement
    // between this key and the show half of the stream ids in the same group, so the two cannot drift.
    //
    // Returns "" for a show this surface cannot key, which is also the store's "no such record".
    QString missedShowKey(const TraktIds& showIds);

    // One show's worth of missed episodes, collapsed. See "ONE ROW PER SHOW" above.
    struct MissedRow
    {
        QString   showKey;        // missedShowKey — what a dismissal is filed under
        QString   showTitle;
        TraktIds  showIds;

        // The OLDEST unwatched, in-window, undismissed episode — the one to PLAY.
        int       season = -1;
        int       episode = -1;
        QString   episodeTitle;
        QString   streamId;       // NEVER empty in a result: an unkeyable episode is not a row (clause 3)
        QString   posterUrl;      // "" when no episode in the group carried one
        QDateTime airedAtUtc;     // that oldest episode's air time

        // The NEWEST one's air time. Two jobs, and both need it: it is the sort key (most-recent first),
        // and it is the stamp a "dismiss" writes, so pressing dismiss covers exactly the episodes the row
        // was speaking for and nothing that has not aired yet.
        QDateTime latestAiredUtc;

        // How many episodes this row stands for, always >= 1. Deduplicated by stream id first: a calendar
        // that lists an episode twice must not make the row claim two.
        int       count = 0;
    };

    // The selection rule. Pure: no store, no clock — `nowUtc` is a parameter for the reason
    // traktCalendarCatalog's is, that the boundary is the one rule worth pinning and a function that read
    // the clock itself could only be tested by waiting.
    //
    // `localState` is asked once per episode that passed clauses 1, 2 and the keyability test — never for
    // one already excluded by the window — so a caller backed by the real marks store does the cheapest
    // work the rule allows. `dismissedThrough` is asked once per SHOW that has at least one episode past
    // those clauses, never once per episode, and returns unix seconds with 0 meaning "never dismissed".
    //
    // A non-positive `lookbackDays` yields NO rows, because the window it describes is empty. That is
    // deliberately not clamped to the default: a caller that passes 0 by mistake gets an empty surface,
    // which is visible, rather than a silently substituted window, which is not.
    //
    // ORDER is total and therefore reproducible over the same input regardless of how the entries arrived:
    // newest-aired first, then show title (case-insensitive), then show key.
    QVector<MissedRow> planMissed(const QVector<CalendarEntry>& entries,
                                  const QDateTime& nowUtc,
                                  int lookbackDays,
                                  const std::function<LocalState(const QString&)>& localState,
                                  const std::function<qint64(const QString&)>& dismissedThrough);

    // Is a dismissal stamp old enough to collect? See the expiry argument in the header. TOTAL over
    // nonsense: a non-positive stamp is not a record at all and reads as expired, and a stamp in the FUTURE
    // (a peer whose clock is ahead) is not expired — it is doing work.
    bool missedDismissExpired(qint64 dismissedAt, qint64 nowUnix);
}
