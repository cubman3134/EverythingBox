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

    // Waiters on the one in-flight /oauth/token refresh. Never more than one request; every caller
    // is answered exactly once, on failure as well as success.
    SingleFlight tokenRefresh_;

    QNetworkAccessManager* nam_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    QString pollDeviceCode_;
    int pollElapsed_ = 0;
    int pollExpiresIn_ = 600;
};
