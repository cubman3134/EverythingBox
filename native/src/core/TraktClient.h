// Trakt.tv scrobbling: keeps your Trakt profile in sync as you watch. Uses the OAuth device-code flow (no
// redirect URI needed for a desktop app): connectAccount() emits a short code + URL for the user to enter at
// trakt.tv/activate, then polls for the token. Once linked, scrobbleStart/Stop mark movies and episodes as
// watched (Trakt counts a stop at >80% as watched). Media is identified by IMDB id, which the app already has.
//
// The app has no built-in Trakt client id, so the user registers a free Trakt API app and pastes its
// client id + secret into Settings; tokens are stored + refreshed automatically. All empty => Trakt is off.
#pragma once
#include "TraktRead.h"   // CalendarEntry — the read layer's struct, returned by value below

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
    void disconnectAccount();   // forget the tokens

    // Scrobble the current media. imdbStreamId is "tt123" (movie) or "ttShow:season:episode" (episode);
    // progressPct is 0..100. No-op unless configured + connected and the id is usable.
    void scrobbleStart(const QString& imdbStreamId, double progressPct);
    void scrobblePause(const QString& imdbStreamId, double progressPct);
    void scrobbleStop(const QString& imdbStreamId, double progressPct);

    // Episodes airing in [today - daysBack, today + daysForward] for the shows this account follows.
    // Routed through the SAME ensureValidToken gate scrobbling uses, so refresh/expiry live in one
    // place — no read path may issue a raw request. Calls back with ok=false and an empty list when
    // Trakt is not configured or not connected: Trakt being off is not a failure.
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
    void ensureValidToken(std::function<void(bool ok)> done); // refresh if expired, then call done
    void scrobble(const QString& action, const QString& imdbStreamId, double pct);
    // Persist a freshly fetched calendar (+ the fetch time). Static and private: only the fetch writes
    // the cache, and it writes it only on a reply that actually parsed.
    static void writeCalendarCache(const QVector<CalendarEntry>& entries);

    QNetworkAccessManager* nam_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    QString pollDeviceCode_;
    int pollElapsed_ = 0;
    int pollExpiresIn_ = 600;
};
