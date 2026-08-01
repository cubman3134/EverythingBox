// The app's whole Trakt.tv link — both directions. Uses the OAuth device-code flow (no redirect URI needed
// for a desktop app): connectAccount() emits a short code + URL for the user to enter at trakt.tv/activate,
// then polls for the token.
//
// WRITE half — scrobbling: once linked, scrobbleStart/Pause/Stop keep your Trakt profile in sync as you
// watch (Trakt counts a stop at >80% as watched). Media is identified by IMDB id, which the app already has.
//
// READ half (#23) — fetchMyShowsCalendar pulls the "my shows" calendar for the followed shows, and the last
// good result is cached on disk (cachedCalendar/cachedCalendarAt) so an offline launch still draws something.
// Both halves funnel through the SAME ensureValidToken gate, so refresh and Trakt's rotated refresh token
// live in exactly one place; the wire FORMAT of everything read — the calendar, the cache, and the OAuth
// token reply — lives in TraktRead, which is pure and probe-covered.
//
// The app has no built-in Trakt client id, so the user registers a free Trakt API app and pastes its
// client id + secret into Settings; tokens are stored + refreshed automatically. All empty => Trakt is off.
#pragma once
#include "SingleFlight.h" // the token-refresh queue: one /oauth/token refresh, however many callers
#include "TraktRead.h"    // CalendarEntry — the read layer's struct, returned by value below
#include "TraktSync.h"    // TraktListEntry + the paging/reconciliation rules the fetch loop runs on

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QTimer;

class TraktClient : public QObject
{
    Q_OBJECT
public:
    explicit TraktClient(QObject* parent = nullptr);

    static bool configured();   // client id + secret present
    static bool connected();    // an access token is stored

    void connectAccount();      // begin the device-code flow (emits deviceCode, then connected/connectError)
    // Forget the tokens AND the cached calendar. The cache is per-ACCOUNT, so it cannot outlive the link
    // that produced it: leaving it would show the previous account's shows to the next one that links.
    void disconnectAccount();

    // Scrobble the current media. imdbStreamId is "tt123" (movie) or "ttShow:season:episode" (episode);
    // progressPct is 0..100. No-op unless configured + connected and the id is usable.
    void scrobbleStart(const QString& imdbStreamId, double progressPct);
    void scrobblePause(const QString& imdbStreamId, double progressPct);
    void scrobbleStop(const QString& imdbStreamId, double progressPct);

    // Episodes airing in [today - daysBack, today + daysForward] for the shows this account follows.
    // "today" and every entry's air time are UTC, not local — see CalendarEntry::airsAtUtc — so any
    // day-bucketing a caller layers on top of the result must be done in UTC too.
    //
    // Routed through the SAME ensureValidToken gate scrobbling uses, so refresh/expiry live in one
    // place — no read path may issue a raw request. Calls back with ok=false and an empty list when
    // Trakt is not configured or not connected: Trakt being off is not a failure. ok=false ALSO
    // covers a reply that was not a calendar at all (a captive-portal HTML page carrying HTTP 200);
    // in that case the existing cache is deliberately left intact for the caller to fall back on.
    //
    // The callback may NEVER ARRIVE if this TraktClient is destroyed while the request is in flight:
    // the reply is parented to it and the connection is context-bound, so both die with it and no
    // final call is made. That is intentional — a callback firing into a half-destroyed owner is the
    // worse outcome — but it means a caller must not park UI state (a spinner, a "loading" flag) on
    // this callback always firing. Own the lifetime, or give the UI a state that survives silence.
    void fetchMyShowsCalendar(int daysBack, int daysForward,
                              std::function<void(bool ok, QVector<CalendarEntry> entries)> cb);

    // The last successfully fetched calendar, persisted so an offline launch still shows something.
    // A stale calendar is far more useful than an empty one. Empty when nothing was ever cached, or
    // when the stored cache is unreadable — see trakt::deserializeCalendar's totality contract.
    static QVector<CalendarEntry> cachedCalendar();

    // When cachedCalendar() was written, in unix seconds; 0 = never. Lets a caller decide whether to
    // show the cache at all, or to label it — a calendar is a claim about "this week", and one from
    // three weeks ago is not the same claim.
    static qint64 cachedCalendarAt();

    // configured() && connected() — the one predicate every surface gates on.
    static bool calendarAvailable();

    // ---- the watchlist and the collection (#23) --------------------------------------------------
    // Both are fetched, cached and surfaced identically; only the endpoint and the cache key differ.
    // Same contract as fetchMyShowsCalendar throughout: ok=false when Trakt is off, when the token
    // gate refuses, or when the reply was not the payload — and in every one of those cases the
    // existing cache is left INTACT for the caller to fall back on. Same lifetime warning too: the
    // callback may never arrive if this TraktClient is destroyed mid-flight.
    //
    // These page. The loop honours Trakt's pagination headers and its 429 + Retry-After, retries only
    // what can succeed later, and gives up after a bounded number of attempts — all of it decided by
    // the pure classifiers in TraktSync, so the wire rules live in one probe-covered place. A run that
    // could not read every page reports ok=false and does NOT overwrite the cache: a watchlist missing
    // its second page, cached and drawn as if it were the whole thing, is worse than a stale one,
    // because nothing about it looks wrong.
    void fetchWatchlist(std::function<void(bool ok, QVector<TraktListEntry> entries)> cb);
    void fetchCollection(std::function<void(bool ok, QVector<TraktListEntry> entries)> cb);

    static QVector<TraktListEntry> cachedWatchlist();
    static QVector<TraktListEntry> cachedCollection();
    // When either list was last written, unix seconds; 0 = never. One stamp for both, because they are
    // refreshed together and a surface only ever asks "how old is what I am showing".
    static qint64 cachedListsAt();

    // ---- the watched-history backfill (#23) -------------------------------------------------------
    // What a run did, in the terms the user is told. Every field is a count of something that really
    // happened, and `complete` is the one that decides whether the watermark advances at all.
    struct BackfillReport
    {
        bool    complete = false;
        int     marked = 0;              // items that went from unmarked to watched
        int     alreadyWatched = 0;      // local already said so; not written, so no sync churn
        int     keptLocal = 0;           // local said something else on purpose; Trakt lost
        int     skippedByWatermark = 0;  // already offered by an earlier complete run
        int     droppedNoKey = 0;        // Trakt has no IMDB id for it — this app cannot key on it
        int     droppedNoTimestamp = 0;  // watched, but Trakt sent no usable watch time
        QString stopReason;              // "" when complete; otherwise WHY it stopped, for the user
    };

    // Import /sync/watched into the app's own marks. ADDITIVE and INCREMENTAL — see TraktSync.h for
    // the rules and for why repeated runs converge instead of fighting the user.
    //
    // The two callbacks are how this stays out of the marks store: `localState` answers what the app
    // already knows about one stream id, and `markWatched` performs the one write this feature is
    // allowed to make. TraktClient therefore never includes ItemMarks, never learns the profile, and
    // the caller owns the mapping from a stream id onto its own key — which is the part that differs
    // per catalogue and must not be guessed here.
    //
    // markWatched is called ONLY for items the plan chose, so a run that changes nothing performs no
    // writes at all and cannot re-arm the Drive push.
    void runWatchedBackfill(std::function<trakt::LocalState(const QString&)> localState,
                            std::function<void(const QString&)> markWatched,
                            std::function<void(BackfillReport)> cb);

    // Has a COMPLETE backfill ever run for this account? The surface uses it to decide whether to
    // offer the import at all; disconnectAccount clears it with the rest of the account's state.
    static bool backfillEverCompleted();

signals:
    void deviceCode(const QString& userCode, const QString& verificationUrl); // show these to the user
    void connectedChanged(bool connected);
    void connectError(const QString& message);
    void log(const QString& line);

private:
    void pollDeviceToken(const QString& deviceCode, int intervalSec);
    // Refresh if expired, then call done. SINGLE-FLIGHT: callers arriving while a refresh is in
    // flight join it instead of issuing their own, because Trakt rotates the refresh token and two
    // overlapping refreshes can permanently break the account link. See TraktClient.cpp.
    void ensureValidToken(std::function<void(bool ok)> done);
    void scrobble(const QString& action, const QString& imdbStreamId, double pct);
    // Persist a freshly fetched calendar (+ the fetch time). Static and private: only the fetch writes
    // the cache, and it writes it only on a reply that actually parsed.
    static void writeCalendarCache(const QVector<CalendarEntry>& entries);
    // Drop the cached calendar (both keys). Only disconnectAccount calls it: the cache is discarded when
    // the account it describes is, and at no other time — a failed fetch deliberately KEEPS it.
    static void clearCalendarCache();
    // The same rule for everything the second read slice persists: the lists describe an ACCOUNT'S
    // library and the watermark describes what has been imported FROM that account, so both die with
    // the link. A watermark that outlived it would make the next account's first backfill skip
    // everything older than the previous account's newest watch.
    static void clearListCaches();
    static void clearBackfillState();

    // One run of a paged GET over an endpoint that answers with a JSON array. Held in a shared_ptr and
    // carried through the reply lambdas, because the loop is asynchronous and re-entrant: the state has
    // to outlive each individual request without belonging to the client (two runs can be in flight).
    struct PagedRun
    {
        QString             path;             // "/sync/watchlist" — no query; the loop adds page+limit
        QVector<QByteArray> bodies;           // one raw body per page actually read
        int                 page = 1;         // the page being asked for now
        int                 attempt = 1;      // 1-based attempt at THIS page
        int                 pagesFetched = 0;
        int                 pagesExpected = 0;  // 0 until a reply carries the headers
        bool                complete = false;
        QString             stopReason;       // "" only when complete
    };
    // Fetch `run->page` and either recurse, retry after a backoff, or finish. `done` is called EXACTLY
    // once per run, complete or not.
    void fetchPage(std::shared_ptr<PagedRun> run, std::function<void(std::shared_ptr<PagedRun>)> done);
    // The whole of one paged endpoint, behind the shared token gate.
    void fetchAllPages(const QString& path, std::function<void(std::shared_ptr<PagedRun>)> done);
    // Shared by fetchWatchlist and fetchCollection: run the pages, parse, and cache ONLY on a complete
    // run. `cacheKey` names which of the two caches this fills.
    void fetchListInto(const QString& path, const char* cacheKey,
                       std::function<void(bool, QVector<TraktListEntry>)> cb);

    // Waiters on the one in-flight /oauth/token refresh. Never more than one request; every caller
    // is answered exactly once, on failure as well as success.
    SingleFlight tokenRefresh_;

    QNetworkAccessManager* nam_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    QString pollDeviceCode_;
    int pollElapsed_ = 0;
    int pollExpiresIn_ = 600;
};
