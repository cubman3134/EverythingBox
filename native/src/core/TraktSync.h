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
//   * INCREMENTAL: a per-device, PER-PROFILE WATERMARK (the newest `last_watched_at` a COMPLETE run has
//     ever observed for that profile) makes each run consider only what is newer than the last one.
//     Per-profile is a correctness rule, not tidiness — see backfillThroughKey below.
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
// WHAT THE THEOREM DOES NOT COVER, AND THE ESCAPE HATCH THAT ANSWERS IT:
//
//   The argument quantifies over entries the run OBSERVED. It says nothing about an entry that was not
//   on Trakt yet — and Trakt gains entries with OLD `last_watched_at` all the time: a backdated manual
//   check-in, a Letterboxd or Netflix import, another device syncing plays it made while offline. Every
//   one of those arrives BELOW the watermark a completed run already stored, so it is skipped for ever
//   and never imported. Nothing is damaged and nothing re-marks the user's corrections; the import is
//   simply, silently, not there. `WatchedParse::droppedNoKey` has the same shape: a title Trakt later
//   learns an IMDB id for keeps its old timestamp, so it stays below the watermark after it becomes
//   importable.
//
//   That is why `skippedByWatermark` is documented as "older than the last complete run" and NOT as
//   "already offered" — the second is a claim this code cannot make — and why the surface must offer a
//   RE-IMPORT that clears the watermark outright. Re-import is not free: it re-offers everything, so
//   anything the user has unmarked since gets marked again, which is precisely what the strict `>`
//   exists to prevent on a NORMAL run. It therefore belongs behind a deliberate, separate action that
//   says so, never as the behaviour of the ordinary one.
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
//     requests instead of hanging the app until the account is rate-limited into the ground. Hitting
//     that bound is a TRUNCATION, and nextPageAfter reports it as its own step rather than as "the last
//     page" — see NextPage. A run that stopped early because the list was longer than the bound is
//     INCOMPLETE by exactly the same rule as one whose page failed: it did not read everything, so it
//     must not cache a partial list as whole and must not advance the watermark over a tail it never
//     saw. Before that distinction existed, both answers were the integer 0 and a truncated run
//     reported COMPLETE.
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
        // Not newer than the last complete run's watermark. NOT "already offered once": for an entry
        // that run OBSERVED that is exactly what it means (the theorem), but an entry Trakt gained
        // AFTERWARDS with an older stamp — a backdated check-in, a Letterboxd import — lands here
        // having never been offered at all. See "WHAT THE THEOREM DOES NOT COVER" above; a re-import
        // is the only thing that reaches those.
        int skippedByWatermark = 0;
        int alreadyWatched = 0;      // local already says watched; no write, so no sync churn
        int keptLocal = 0;           // local says something else on purpose; Trakt loses
        int unusable = 0;            // handed in with an empty id or a non-positive timestamp
        // The SAME id seen again among the eligible marks. Re-fetching a run after a partial failure
        // replays whole pages, and an episode can legitimately appear on two of them, so this is the
        // normal case rather than a corruption signal. The first occurrence decided its bucket; every
        // later one lands here, so no store write and no localState call happens twice.
        int duplicates = 0;
    };

    // ---- where a run's progress is stored ---------------------------------------------------------
    // The watermark and the "a complete run has happened" flag are PER PROFILE, and that is a
    // correctness rule rather than tidiness. The marks a run writes land under "marks/<profileId>/items"
    // — ItemMarks resolves the ACTIVE profile — so a watermark shared by every profile on one box lets
    // the parent's completed import make the kid's FIRST one skip every entry it has: the kid's run
    // reports "0 newly marked watched", is indistinguishable from "nothing to do", and can never be
    // repaired except by unlinking Trakt, which is the only thing that clears the cursor. Namespacing
    // the cursor the way the marks are namespaced keeps "what this profile has imported" and "what this
    // profile has marked" the same claim. (It is DEVICE-local as well as profile-local: CloudSync keeps
    // it off Drive for the same reason, one install's run must not suppress another's.)
    //
    // PURE, and here rather than beside the QSettings that holds it, so a probe can pin the shape — and
    // pin that the cloud/transaction predicates really do match it — with no store anywhere near it.
    // An EMPTY profileId means "no profile selected yet" and maps to "default", which is exactly
    // ItemMarks' own rule, so the cursor and the marks agree about which bucket that is.
    QString backfillThroughKey(const QString& profileId);
    QString backfillDoneKey(const QString& profileId);
    // The prefix every key above starts with. CloudSync::isDeviceLocalKey and SettingsTxn::inScope match
    // on THIS rather than on a list of exact keys, because the set of keys grows with the set of
    // profiles and a list would be one profile behind for ever. It ends in '/', so it cannot also
    // swallow a sibling like "trakt/backfillx".
    QString backfillKeyPrefix();

    // ---- what a finished run should SAY ------------------------------------------------------------
    // The one thing a user reads. Pure and here, rather than an if-chain in the surface, because the
    // distinction that matters most is invisible from the counters at a glance: "marked nothing because
    // there is nothing new" and "marked nothing because Trakt had nothing" are the same `marked == 0`,
    // and the first of them is also what a PROFILE whose watermark was wrongly shared would report. A
    // toast that cannot tell those apart is how the Critical stayed invisible.
    enum class BackfillHeadline
    {
        Abandoned,       // discarded before it read or wrote anything (the profile changed mid-run)
        Incomplete,      // pages were missed or the bound truncated it; what was marked stays marked
        Marked,          // it marked something
        NothingNew,      // complete, marked nothing, and the WATERMARK is why — re-import reaches these
        AlreadyKnown,    // complete, marked nothing, and the app already knew about everything eligible
        NothingToImport  // complete, and Trakt had nothing this app could use
    };
    // Precedence is top to bottom in the order above, which is what makes the classification total and
    // a probe able to pin it. `alreadyKnown` is alreadyWatched + keptLocal: from the user's side both
    // mean "we looked at it and left your library alone".
    BackfillHeadline backfillHeadlineFor(bool complete, bool abandoned, int marked,
                                         int skippedByWatermark, int alreadyKnown);

    // The "what have I got from Trakt, and how old is it" line the settings surfaces show. Pure, and
    // shared by BOTH settings builders so the themed and the classic one cannot drift into telling the
    // user different things.
    //
    // It exists because the watermark is otherwise an INVISIBLE limit: a user whose import "finished:
    // 0 newly marked" has no way to see that the run considered nothing older than some date, or which
    // date. Stating it is what makes the re-import action beside it mean something. Every stamp is unix
    // seconds with 0 = never; `everImported` is separate from `importedThrough` because a complete run
    // that observed nothing new leaves the cursor at 0 and has still happened.
    QString importStatusLine(qint64 watchlistAt, qint64 collectionAt,
                             bool everImported, qint64 importedThrough, qint64 nowUnix);

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

    // Why the paging loop stops, or that it does not. THREE terminal answers, not one: the loop's
    // caller has to tell a run that READ EVERYTHING from one that merely STOPPED, and an integer 0
    // cannot carry that. It used to: `nextPageAfter` returned 0 for "last page" and 0 for "the bound
    // cut this off", the caller mapped every 0 to complete=true, and a list longer than kMaxPages was
    // therefore cached truncated-as-whole and let the backfill advance its watermark over a tail no run
    // had ever observed. Both are the exact outcomes this file's rules exist to forbid.
    enum class PageStep
    {
        Next,        // ask for NextPage::page
        LastPage,    // the server's last page has been read — the run is COMPLETE
        BoundHit,    // there are more pages, but kMaxPages stops the run here — INCOMPLETE
        Unusable     // `fetchedPage` was not a page number, so nothing at all was established —
                     // INCOMPLETE, because "we cannot tell" is not "we are done"
    };
    struct NextPage
    {
        PageStep step = PageStep::Unusable;   // the safe default: a caller that forgets to switch stops
        int      page = 0;                    // meaningful ONLY for Next; 0 otherwise
    };

    // What to do after successfully fetching `fetchedPage`.
    //
    // `fetchedPage` is the page WE ASKED FOR. info.page — what the server echoed — is deliberately
    // ignored for this decision: a server that echoes "1" for every page would otherwise hold the loop
    // on page 1 until kMaxPages, and one that echoes a page ahead would skip real rows.
    //
    // pageCount <= 0 means the pagination headers were absent, which for Trakt means the endpoint
    // returned everything in one body — so the run is LastPage, complete, not broken.
    NextPage nextPageAfter(const PageInfo& info, int fetchedPage);

    // Should attempt number `attempt` (1-based: 1 is the first try) be retried at all?
    bool shouldRetryAttempt(int attempt);

    // How long to wait before attempt `attempt`+1. Honours a server-supplied `retryAfterSec` when it is
    // positive; otherwise doubles from kBaseBackoffSec. Always within [1, kMaxBackoffSec].
    //
    // TOTAL over a nonsense `attempt` too: 0 and negatives yield kBaseBackoffSec, because the doubling
    // runs `attempt - 1` times and that is already none of them. There is deliberately no `attempt < 1`
    // clamp in front of it — one was there, and it was a guard nothing could distinguish from its
    // absence, on the same footing as the one removed from nextPageAfter.
    int backoffSecFor(int attempt, int retryAfterSec);

    // A run is bounded on both axes so a hostile or buggy `page_count` costs a bounded number of
    // requests. kMaxPages * 100 rows/page is 20k items, which is far past any real watchlist.
    constexpr int kMaxPages = 200;
    constexpr int kMaxPageAttempts = 4;
    constexpr int kBaseBackoffSec = 2;
    constexpr int kMaxBackoffSec = 300;
}
