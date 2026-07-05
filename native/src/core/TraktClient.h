// Trakt.tv scrobbling: keeps your Trakt profile in sync as you watch. Uses the OAuth device-code flow (no
// redirect URI needed for a desktop app): connectAccount() emits a short code + URL for the user to enter at
// trakt.tv/activate, then polls for the token. Once linked, scrobbleStart/Stop mark movies and episodes as
// watched (Trakt counts a stop at >80% as watched). Media is identified by IMDB id, which the app already has.
//
// The app has no built-in Trakt client id, so the user registers a free Trakt API app and pastes its
// client id + secret into Settings; tokens are stored + refreshed automatically. All empty => Trakt is off.
#pragma once
#include <QObject>
#include <QString>
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

signals:
    void deviceCode(const QString& userCode, const QString& verificationUrl); // show these to the user
    void connectedChanged(bool connected);
    void connectError(const QString& message);
    void log(const QString& line);

private:
    void pollDeviceToken(const QString& deviceCode, int intervalSec);
    void ensureValidToken(std::function<void(bool ok)> done); // refresh if expired, then call done
    void scrobble(const QString& action, const QString& imdbStreamId, double pct);

    QNetworkAccessManager* nam_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    QString pollDeviceCode_;
    int pollElapsed_ = 0;
    int pollExpiresIn_ = 600;
};
