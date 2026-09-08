// THE MYANIMELIST TRACKER (issue #156, increment 2) — the SECOND implementation of the Tracker seam, and
// the second file in this feature that owns a socket. Everything it decides is decided in TrackerRules
// (namespace tracker::mal); this file is the impure half: OAuth, the network access manager, the backoff
// timer and the ini.
//
// THE SEAM DID NOT MOVE TO ADMIT IT. Tracker.h is byte-for-byte what increment 1 shipped: Id::MyAnimeList
// was already reserved with a stable token, fetchEntry already carried the Kind that MAL needs and AniList
// ignores, and Match/Entry/Update already spelled everything MAL says. The only thing increment 2 had to
// share rather than duplicate was the offline QUEUE, and that moved out of AniListTracker into TrackerQueue
// (same keys, same rules, one implementation) rather than being written a second time.
//
// AUTH is OAuth AUTHORIZATION CODE WITH PKCE. MAL requires PKCE and accepts only the `plain` method, so the
// code challenge IS the verifier: it is generated per attempt from the system CSPRNG, held in memory for
// the length of one sign-in, and never written to disk. A `state` value is carried and COMPARED on the way
// back — without it, anything able to reach the loopback listener could feed us a code of its choosing.
//
// CREDENTIALS live in the device-local secrets carve-out (tracker::settingsKeyPrefix()) and are NEVER
// logged. Nothing here builds a diagnostic message out of a REQUEST: QNetworkReply::errorString() embeds
// the URL, and a MAL request URL carries the media id and — on the token endpoint — would sit one edit away
// from carrying the grant. The `log` signal and the last-error line are fed sentences of our own.
//
// PUSHING is debounced and QUEUED through TrackerQueue, exactly as AniList's is — and since issue #326 the
// DRAIN LOOP is shared too (TrackerQueue::Sender), along with the credential store and the loopback request
// parser this file used to keep its own copy of. The BACKOFF that increment 2 wrote here turned out to be
// the thing AniList was missing rather than a MAL speciality, so it moved up as well: 429/5xx/a dead socket
// are answered by waiting (doubling, capped at 30 minutes, honouring Retry-After when it asks for longer),
// 401 by refreshing the token, and 400/404/422 by DROPPING the row — see tracker::classifySend for why a
// permanently-refused row cannot be left to wedge the head of the queue. What is still MAL's own is the
// STATUS SET (mal::sendPolicy(); 422 is the one code AniList's GraphQL endpoint cannot answer with), PKCE,
// and REST.
//
// NO MYANIMELIST ACCOUNT WAS CREATED and no API client was registered for this work. Every shape here is
// written from MAL's published API v2 reference, and the probe and live drive were answered by a local
// fixture server (EB_MAL_ENDPOINT / EB_MAL_AUTH).
#pragma once
#include "SingleFlight.h"   // ensureValidToken's one-refresh-many-waiters queue, shared with TraktClient
#include "Tracker.h"
#include "TrackerQueue.h"   // the shared queue, credential store and drain loop (#326)
#include "TrackerRules.h"

#include <memory>

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QTimer;

class MyAnimeListTracker : public QObject, public tracker::Tracker
{
    Q_OBJECT
public:
    explicit MyAnimeListTracker(QObject* parent = nullptr);
    ~MyAnimeListTracker() override;

    // ---- statics, for the settings surfaces (which have no instance to ask) --------------------------
    static bool isConfigured();   // a client id is present
    static bool isConnected();    // an access token is stored
    // The user's typed client id / secret. MAL issues PUBLIC clients as well as confidential ones, and a
    // public one has NO secret at all — so `configured` is the id alone, and an empty secret is omitted
    // from the grant rather than sent blank (tracker::mal::tokenExchangeBody).
    static QString clientId();
    static QString clientSecret();
    static void setClientId(const QString& v);
    static void setClientSecret(const QString& v);

    // ---- the Tracker seam -----------------------------------------------------------------------------
    tracker::Id id() const override { return tracker::Id::MyAnimeList; }
    QString displayName() const override { return QStringLiteral("MyAnimeList"); }
    bool configured() const override { return isConfigured(); }
    bool connected() const override { return isConnected(); }

    void search(const QString& title, int year, tracker::Kind kind,
                std::function<void(QVector<tracker::Match>)> cb) override;
    void fetchEntry(const QString& mediaId, tracker::Kind kind,
                    std::function<void(bool ok, tracker::Entry)> cb) override;
    void pushProgress(const tracker::Update& u) override;
    void flushQueue() override;

    // ---- account linking ------------------------------------------------------------------------------
    void connectAccount();
    // Forget the tokens AND this device's pending queue; the per-item LINKS are kept, for the reason
    // AniListTracker::disconnectAccount documents.
    void disconnectAccount();

    static int queuedCount();
    static QString lastError();

signals:
    void authUrlReady(const QString& url);
    void connectedChanged(bool connected);
    void connectError(const QString& message);
    void progressPushed(const QString& itemKey, int unit);
    void queueChanged();
    void log(const QString& line);

private:
    void ensureValidToken(std::function<void(bool ok)> done);
    void exchangeCode(const QString& code);
    void storeTokenReply(const tracker::mal::TokenReply& r);
    void closeLoopback();

    // One authenticated GET. `cb` receives the HTTP status (0 for "no reply at all"), the Retry-After
    // header in seconds (0 when absent), and the body. The status and the header are carried rather than
    // collapsed into an ok/fail because the BACKOFF is a decision about which failure this was.
    void get(const QString& url, std::function<void(int status, qint64 retryAfterSec, QByteArray)> cb);
    // One authenticated PATCH with a form body — the only write this tracker performs.
    void patch(const QString& url, const QByteArray& form,
               std::function<void(int status, qint64 retryAfterSec, QByteArray)> cb);

    static QString apiUrl();
    static QString authBase();

    // THE LOOP ITSELF is TrackerQueue::Sender (#326); this is the one line that starts it.
    void drain();

    QNetworkAccessManager* nam_ = nullptr;
    QTcpServer*            loopback_ = nullptr;
    QString                redirectUri_;
    // PKCE + CSRF, for ONE sign-in attempt. In memory only: the verifier is the whole of the proof that the
    // code coming back belongs to the request that went out, and a copy of it on disk would outlive the
    // thirty seconds it is worth anything for.
    QString                codeVerifier_;
    QString                state_;
    QTimer*                retry_ = nullptr;
    // THE ONE DRAIN LOOP, shared with AniList. It holds the re-entrancy guard this class used to spell
    // `sending_` and the consecutive-failure count it used to spell `failures_` — both still in memory and
    // not on disk, because a restart is exactly the moment it is worth trying again immediately.
    std::unique_ptr<TrackerQueue::Sender> sender_;
    SingleFlight           tokenRefresh_;
};
