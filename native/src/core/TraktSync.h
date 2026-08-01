// The Trakt READ layer, part two (issue #23): the watchlist/collection lists, and the watched-history
// backfill. TraktRead owns the calendar and the OAuth token reply; this owns everything the /sync/*
// endpoints hand back, plus the two rules that make a backfill safe to run twice.
//
// Pure, exactly like TraktRead: QByteArray/structs in, structs out. No network, no GUI, no ini, no
// ItemMarks — the one thing it needs to know about local state arrives through a callback. probe_trakt
// pins every table below with no I/O at all, which is the only way the rate-limit, partial-page and
// disagreement cases are reachable without a live account.
//
// ============================================================================================
// 1. THE LISTS  (/sync/watchlist, /sync/collection)
// ============================================================================================
// Both endpoints return a FLAT array of rows, each row carrying a `type` discriminator plus a nested
// `movie` or `show` object with the usual `ids` bag — so ONE parser reads both, and the two surfaces
// cannot drift apart in what they consider a row. The per-endpoint difference is the timestamp field
// name (`listed_at` vs `collected_at`), which is read tolerantly: whichever is present wins.
//
// ============================================================================================
// 2. THE BACKFILL  (/sync/watched/movies, /sync/watched/shows)
// ============================================================================================
// WHAT IT IS: an INCREMENTAL, ADDITIVE reconciliation — not a one-shot import and not a two-way sync.
//
//   * ADDITIVE: the only transition it ever performs is Unmarked -> Watched. It never clears a mark,
//     never downgrades one, and never touches `hidden` or tags. Trakt cannot delete local state.
//   * INCREMENTAL: a per-device WATERMARK (the newest `last_watched_at` a COMPLETE run has ever
//     observed) makes each run consider only what is newer than the last one.
//
// WHEN LOCAL AND TRAKT DISAGREE, LOCAL WINS — always, and permanently:
//
//   local Unmarked        + Trakt says watched  ->  MARK IT. This is the whole feature.
//   local Watched         + Trakt says watched  ->  no write at all. Not a no-op for politeness: a
//                                                   redundant write bumps ItemMarks::updatedAt, which
//                                                   re-arms the Drive push and makes an import that
//                                                   changed nothing look like a device-wide edit.
//   local OtherExplicit   + Trakt says watched  ->  KEEP LOCAL. "In progress", "abandoned" and
//                                                   "planned" are things the user said on purpose;
//                                                   an import must not overrule them.
//   local anything        + Trakt says nothing  ->  KEEP LOCAL. Absence from Trakt is not evidence:
//                                                   the account may simply predate the watch.
//
// WHY REPEATED RUNS CONVERGE (the property #58 lost, and the reason the comparison below is STRICT):
//
//   Eligibility is `lastWatchedAt > watermark`. Suppose the user unmarks something the backfill had
//   already marked. Its `last_watched_at` on Trakt is unchanged, so on every later run it fails the
//   test and is skipped — the unmark STICKS. With `>=` instead, the single newest entry of the
//   previous run is always exactly equal to the watermark, so it would be re-marked on EVERY run,
//   for ever, silently reverting the user each time. That is not a corner case: it is whichever
//   episode the user watched most recently, i.e. the one they are most likely to be correcting.
//
//   Nothing is lost to the strictness, and this is the part worth stating as a theorem. A complete
//   run advances the watermark to the maximum `last_watched_at` over EVERY entry it OBSERVED — not
//   just the ones it marked. So take any entry e with `e.lastWatchedAt == newWatermark`: e was
//   observed by that run, and therefore either it was eligible (and was marked), or it already
//   failed `> oldWatermark`, which means some earlier complete run had already observed and marked
//   it. Either way e has been offered at least once, so making it ineligible from now on discards
//   nothing. Advancing to the max of the OBSERVED set rather than of the MARKED set is what makes
//   that argument go through, and it is why planWatchedBackfill computes the two separately.
//
//   A PARTIAL run advances the watermark by ZERO (see 3). So a run that missed a page cannot make
//   the entries on that page permanently ineligible.
//
// ============================================================================================
// 3. PAGING, RATE LIMITS, AND WHAT A PARTIAL RUN REPORTS
// ============================================================================================
// Trakt paginates the list endpoints with `X-Pagination-Page` / `X-Pagination-Page-Count` and answers
// a rate-limited caller with HTTP 429 + `Retry-After`. Both are read here (parsePageInfo /
// classifyPage) so the fetch loop in TraktClient contains no wire knowledge of its own.
//
//   * A page is retried, with backoff, only for outcomes that CAN succeed later (429, 5xx, a dead
//     transport), and at most kMaxPageAttempts times.
//   * The next page is derived from the page WE ASKED FOR, never from the page the server echoed —
//     a server echoing the same number twice would otherwise loop the run for ever.
//   * kMaxPages bounds a run outright, so a `page_count` of a billion costs a bounded number of
//     requests instead of hanging the app until the account is rate-limited into the ground.
//   * When a page finally fails, the run stops and is INCOMPLETE. Everything already applied stays
//     applied (each write is idempotent, so re-running costs nothing), the watermark is NOT advanced,
//     and BackfillPlan::complete is false. The caller reports it AS incomplete — "imported N of M
//     pages, will continue later" — because a partial import that claims to be finished is the one
//     outcome worse than an obvious failure: the user stops looking for the missing half.
#pragma once
#include "TraktRead.h"   // TraktIds + the imdb id mappings, shared rather than restated

#include <QMap>
#include <QString>
#include <QVector>
#include <functional>

class QByteArray;

// One row of /sync/watchlist or /sync/collection.
struct TraktListEntry
{
    // "movie" or "show". EMPTY when the row named a type this app has no surface for (a season, an
    // episode, a person — all of which those endpoints really can return): such rows are DROPPED by the
    // parser rather than carried with a type nothing renders.
    QString  type;
    QString  title;
    int      year = 0;          // 0 = Trakt gave none
    TraktIds ids;
    // `listed_at` (watchlist) or `collected_at` (collection), unix seconds; 0 = absent/unparseable.
    // Used only for ORDER, never for a correctness decision — unlike the watched timestamps, whose
    // exactness the watermark depends on.
    qint64   addedAt = 0;
};

namespace trakt
{
    // ---- the lists ------------------------------------------------------------------------------
    // TOTAL and TOLERANT, on the same contract as parseMyShowsCalendar: a malformed row is skipped, a
    // missing `ids` yields an empty TraktIds, and non-array or non-JSON input returns empty. A row whose
    // `type` is neither movie nor show is skipped; so is a row whose nested object is missing entirely,
    // because a row with no title and no ids is not something a surface can draw.
    QVector<TraktListEntry> parseListPayload(const QByteArray& json);

    // The array-ness discriminator, for the same reason looksLikeCalendarPayload exists and with the
    // same subtlety: an EMPTY watchlist is a real answer that must be able to replace a stale cache
    // (the user emptied it), whereas an HTML interstitial carrying HTTP 200 must not. Row count is
    // therefore NOT part of the test.
    bool looksLikeListPayload(const QByteArray& json);

    // The on-disk cache for a list, versioned exactly like the calendar's. TOTAL on read: truncated,
    // garbage, wrong shape or an unknown version all yield an empty vector, so the caller re-fetches
    // rather than drawing half a row.
    QByteArray               serializeList(const QVector<TraktListEntry>& entries);
    QVector<TraktListEntry>  deserializeList(const QByteArray& json);

    // ---- the watched history --------------------------------------------------------------------
    // One thing the user has watched, already mapped onto the app's own key.
    struct WatchedMark
    {
        // "tt123" (movie) or "ttShow:season:episode" (episode) — the id the app's marks and stream
        // resolver already use. NEVER empty in a parser result: an entry that cannot be mapped is
        // dropped and counted, because a mark under an unresolvable key is a mark nothing can ever read.
        QString streamId;
        qint64  lastWatchedAt = 0;   // unix seconds; the watermark's unit
    };

    struct WatchedParse
    {
        QVector<WatchedMark> marks;
        // Rows Trakt sent that could not become a mark, split by CAUSE, because the two mean very
        // different things to a user and the run reports them:
        //
        // droppedNoKey — the row could not be mapped onto an app key at all. That is USUALLY "Trakt has
        // no IMDB id for this title" (the common case, and not the user's fault or ours), and also
        // covers an id the mapping rejects and a season/episode number outside the format's range. They
        // share a counter because they share the only consequence a user can act on: this title will not
        // be backfilled, ever, until Trakt learns its IMDB id.
        int droppedNoKey = 0;
        int droppedNoTimestamp = 0;  // watched, but with no usable `last_watched_at`
    };

    // Reads BOTH /sync/watched/movies and /sync/watched/shows — they are the same array-of-rows shape
    // with a different nested object, and one parser means one place to be wrong about it. A movie row
    // yields one mark; a show row yields one mark per episode listed under `seasons[].episodes[]`.
    //
    // AN EPISODE WITH NO PARSEABLE `last_watched_at` IS DROPPED, and deliberately does NOT inherit the
    // show-level one. Inheriting looks harmless and is the #58 failure mode wearing a different hat:
    // the show-level timestamp advances whenever ANY episode is watched, so an episode carrying it
    // would clear the watermark again on every later run and re-mark itself for ever — reverting the
    // user's unmark each time. Dropping it costs one un-backfilled episode, which the run REPORTS;
    // inheriting costs the user's control over their own library, silently.
    WatchedParse parseWatchedPayload(const QByteArray& json);

    // ---- the reconciliation ---------------------------------------------------------------------
    // What the app already knows about one item. Deliberately three-valued rather than a copy of
    // ItemMarks::Completion: this file must not depend on the marks store (which reads the ini), and
    // the only distinction the plan needs is "nothing said" / "already watched" / "the user said
    // something else". The caller maps its enum onto this at the call site.
    enum class LocalState { Unmarked, Watched, OtherExplicit };

    struct BackfillPlan
    {
        // The stream ids to mark watched, in first-seen order — stable, so the same input always
        // produces the same plan and a probe can assert it.
        QVector<QString> toMark;

        // The watermark to STORE — but only if `complete`. It is the max lastWatchedAt over every
        // OBSERVED mark, including ones that were skipped; see the theorem in the header comment.
        // 0 when nothing was observed, which correctly leaves the stored watermark alone.
        qint64 newWatermark = 0;

        // Set by the CALLER from the fetch loop, not by the planner: the planner sees only the marks it
        // was handed and cannot know whether a page was missed. It is carried here so the one struct
        // the caller reports from holds the whole story.
        bool complete = false;

        // The report. Every mark handed in falls into exactly one of these buckets, so
        // toMark.size() + the five counters equals marks.size() — a total that is itself worth
        // asserting, because a bucket quietly missing an entry is how an import comes to claim it did
        // more than it did.
        int skippedByWatermark = 0;  // older than the last complete run — already offered once
        int alreadyWatched = 0;      // local already says watched; no write, so no sync churn
        int keptLocal = 0;           // local says something else on purpose; Trakt loses
        int unusable = 0;            // handed in with an empty id or a non-positive timestamp
        // The SAME id seen again among the eligible marks. Re-fetching a run after a partial failure
        // replays whole pages, and an episode can legitimately appear on two of them, so this is the
        // normal case rather than a corruption signal. The first occurrence decided its bucket; every
        // later one lands here, so no store write and no localState call happens twice.
        int duplicates = 0;
    };

    // Pure: no store, no clock. `localState` is asked once per ELIGIBLE mark (never for one the
    // watermark already excluded), so a caller backed by a real store does the cheapest possible work.
    //
    // Duplicate ids collapse: the same episode can appear twice across pages of a re-fetched run, and
    // the plan must not ask the store to write it twice.
    BackfillPlan planWatchedBackfill(const QVector<WatchedMark>& marks, qint64 watermark,
                                     const std::function<LocalState(const QString&)>& localState);

    // ---- paging + failure -----------------------------------------------------------------------
    struct PageInfo
    {
        int page = 0;        // what the server SAYS it sent; 0 = header absent/unparseable
        int pageCount = 0;   // 0 = header absent, which means "not paginated" (see nextPageAfter)
        int itemCount = -1;  // -1 = absent. Reported to the user, never used to decide control flow.
    };

    // `headers` keys are LOWERCASED header names — HTTP header names are case-insensitive and Qt hands
    // them back in whatever case the server used, so normalising at the boundary is the caller's job
    // and asserting it here is not possible. Values that are not non-negative integers read as absent.
    PageInfo parsePageInfo(const QMap<QString, QString>& headers);

    enum class PageOutcome
    {
        Ok,          // 200 with a body that is a JSON array
        Malformed,   // 200 with a body that is NOT — a captive portal, an error object, a truncation.
                     // NOT retried: the transport succeeded, so trying again gets the same page back.
        AuthFailed,  // 401/403 — the token gate must act; hammering it cannot help and may lock out
        Retryable,   // 429, any 5xx, or a dead transport. Carries the wait.
        Fatal        // any other 4xx — a bad request stays bad
    };

    struct PageVerdict
    {
        PageOutcome outcome = PageOutcome::Fatal;
        // The SERVER'S hint, clamped to [1, kMaxBackoffSec]. 0 means "no usable hint" — including for a
        // Retryable outcome, which is the common case (a 500 carries no Retry-After) — and tells
        // backoffSecFor to use its own schedule. It is never a wait of zero.
        int retryAfterSec = 0;
    };

    // `httpStatus` <= 0 means "no HTTP response at all" (a transport error), which is Retryable.
    // A 429's `Retry-After` is honoured when it parses as a positive integer and is CLAMPED to
    // [1, kMaxBackoffSec]: a server (or a proxy) answering "Retry-After: 86400" must not park the app
    // on a timer a whole day long, and a "0" must not spin the loop.
    PageVerdict classifyPage(int httpStatus, const QMap<QString, QString>& headers,
                             const QByteArray& body);

    // The next page to ask for after successfully fetching `fetchedPage`, or 0 when the run is DONE.
    //
    // `fetchedPage` is the page WE ASKED FOR. info.page — what the server echoed — is deliberately
    // ignored for this decision: a server that echoes "1" for every page would otherwise hold the loop
    // on page 1 until kMaxPages, and one that echoes a page ahead would skip real rows.
    //
    // pageCount <= 0 means the pagination headers were absent, which for Trakt means the endpoint
    // returned everything in one body — so the run is complete, not broken.
    int nextPageAfter(const PageInfo& info, int fetchedPage);

    // Should attempt number `attempt` (1-based: 1 is the first try) be retried at all?
    bool shouldRetryAttempt(int attempt);

    // How long to wait before attempt `attempt`+1. Honours a server-supplied `retryAfterSec` when it is
    // positive; otherwise doubles from kBaseBackoffSec. Always within [1, kMaxBackoffSec].
    int backoffSecFor(int attempt, int retryAfterSec);

    // A run is bounded on both axes so a hostile or buggy `page_count` costs a bounded number of
    // requests. kMaxPages * 100 rows/page is 20k items, which is far past any real watchlist.
    constexpr int kMaxPages = 200;
    constexpr int kMaxPageAttempts = 4;
    constexpr int kBaseBackoffSec = 2;
    constexpr int kMaxBackoffSec = 300;
}
