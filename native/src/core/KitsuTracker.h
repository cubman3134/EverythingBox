// THE KITSU TRACKER (issue #156, increment 3) — the THIRD implementation of the Tracker seam, and the test
// of what issue #326 hoisted. Everything it DECIDES is decided in TrackerRules (namespace tracker::kitsu);
// this file is the impure half: the token exchange, the network access manager, the backoff timer and the
// ini. There is no loopback listener and no browser here, because Kitsu's sign-in needs neither.
//
// WHAT THIS PROVIDER SUPPLIES, AND WHAT IT CONSUMES. That split IS the increment:
//
//   SUPPLIED (genuinely Kitsu's own): its status codes (kitsu::sendPolicy), its wire format (the whole of
//   the kitsu namespace), its auth (the password grant below), and three status-line sentences that name
//   it. Roughly two hundred lines.
//
//   CONSUMED UNCHANGED from the shared layer: the offline queue (TrackerQueue, keyed by tracker::Id), the
//   credential store (TrackerQueue::accessToken/storeTokens/clearTokens/tokenFresh), the drain loop
//   (TrackerQueue::Sender) and — the one that matters — the SINGLE classification of a failed push
//   (tracker::classifySend). Not one line of any of them was forked to admit a third provider, and none of
//   them needed a parameter added.
//
//   OFFERED AND DECLINED: tracker::parseLoopbackRequest / tracker::loopbackResponse. Kitsu's flow has no
//   redirect, so this file opens no port. They are shared machinery, not mandatory machinery.
//
// AUTH IS THE OAUTH 2 PASSWORD GRANT, and it is the one place Kitsu is genuinely unlike the other two.
// There is no client to register, so there is no client id and no client secret to type; the user gives
// the app their own Kitsu email and password ONCE, the app exchanges them for a token pair, and the
// credentials themselves are never written anywhere. They live in two statics for the length of one
// sign-in and are cleared the moment the exchange answers, success or failure. probe_tracker §20 asserts
// the fixture password reaches the ini ZERO times — the other two secrets are asserted to appear exactly
// once, and zero is the stronger and correct claim for a credential that is never stored.
//
// BECAUSE THERE IS NOTHING TO CONFIGURE, `configured()` AND `connected()` ARE THE SAME QUESTION here: the
// only credential Kitsu has is the token. AniList needs an id and a secret before a sign-in can even be
// attempted and MyAnimeList needs an id, so for them the two states are distinct; for Kitsu the
// "set up, but not connected" state does not exist, and reporting it would be describing a state a user
// can never be in. TrackerFanout::active asks for both, so this is also what keeps a signed-out Kitsu out
// of the fan-out.
//
// THE SIGNED-IN USER'S ID is needed to read a library entry (it is a filter) and to create one (it is a
// relationship). It is derived from the token at first use, held in memory for the session behind the same
// SingleFlight the token refresh uses, and NEVER written to disk — a cached-on-disk user id is the one way
// it could go stale against a re-linked account, and it is not a credential, so there is nothing to gain
// by persisting it.
//
// CREDENTIALS live in the device-local carve-out (tracker::settingsKeyPrefix()) and are NEVER logged.
// Nothing here builds a diagnostic message out of a REQUEST: QNetworkReply::errorString() embeds the URL,
// and this URL is the token endpoint. The `log` signal and the last-error line are fed sentences of our own.
//
// PUSHING is debounced and QUEUED through TrackerQueue, and drained by TrackerQueue::Sender, exactly as
// the other two are. Kitsu's write is the one thing that takes TWO round trips — read the library entry to
// learn whether it exists and what its id is, then PATCH it or POST a new one — and that is also what
// makes a replayed update idempotent: after a restart the queue's row finds the entry it created last
// time and updates it rather than creating a second.
//
// NO KITSU ACCOUNT WAS CREATED and no API client was registered for this work. Every shape here is written
// from Kitsu's published JSON:API reference, and the probe and live drive were answered by a local fixture
// server (EB_KITSU_ENDPOINT / EB_KITSU_AUTH).
#pragma once
#include "SingleFlight.h"   // ensureValidToken's (and ensureSelfId's) one-in-flight-many-waiters queue
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
class QTimer;

class KitsuTracker : public QObject, public tracker::Tracker
{
    Q_OBJECT
public:
    explicit KitsuTracker(QObject* parent = nullptr);
    ~KitsuTracker() override;

    // ---- statics, for the settings surfaces (which have no instance to ask) --------------------------
    // BOTH are "is a token stored" — see the header note. There is no client id and no client secret for
    // Kitsu, so there is no third state between "nothing set up" and "signed in".
    static bool isConfigured();
    static bool isConnected();

    // The sign-in credentials, held IN MEMORY ONLY for the length of one sign-in. The settings surfaces
    // write them as the user types and the exchange clears them; neither is ever written to the ini, and
    // there is deliberately NO getter for the password — a masked field that reads its own value back is
    // one screenshot away from being read out loud on a television.
    static QString email();
    static void setEmail(const QString& v);
    static void setPassword(const QString& v);
    // Has the user typed enough to attempt a sign-in? What the Connect button asks before calling
    // connectAccount(), so "you have not typed anything" is not reported as "Kitsu refused you".
    static bool hasSignInCredentials();
    // Forget them without signing in — what a settings surface calls when it goes away, so a password
    // typed and then abandoned does not sit in memory for the rest of the session.
    static void forgetSignInCredentials();

    // ---- the Tracker seam -----------------------------------------------------------------------------
    tracker::Id id() const override { return tracker::Id::Kitsu; }
    QString displayName() const override { return QStringLiteral("Kitsu"); }
    bool configured() const override { return isConfigured(); }
    bool connected() const override { return isConnected(); }

    void search(const QString& title, int year, tracker::Kind kind,
                std::function<void(QVector<tracker::Match>)> cb) override;
    void fetchEntry(const QString& mediaId, tracker::Kind kind,
                    std::function<void(bool ok, tracker::Entry)> cb) override;
    void pushProgress(const tracker::Update& u) override;
    void flushQueue() override;

    // ---- account linking ------------------------------------------------------------------------------
    // Exchanges the email/password held in memory for a token pair and clears them. NO BROWSER OPENS: this
    // is the whole of Kitsu's sign-in.
    void connectAccount();
    // Forget the tokens, this device's pending progress and the cached user id. The per-item LINKS are
    // kept, for the reason AniListTracker::disconnectAccount documents.
    void disconnectAccount();

    static int queuedCount();
    static QString lastError();

signals:
    // DECLARED, NEVER EMITTED for this tracker, and that is deliberate: the settings surfaces connect the
    // same three signals for all three trackers, and a Kitsu that simply never has a URL to show is less
    // code than a Kitsu the surfaces have to special-case. There is no browser step to show a URL for.
    void authUrlReady(const QString& url);
    void connectedChanged(bool connected);
    void connectError(const QString& message);
    void progressPushed(const QString& itemKey, int unit);
    void queueChanged();
    void log(const QString& line);

private:
    void ensureValidToken(std::function<void(bool ok)> done);
    // The signed-in user's id, resolved once per session from the token. `cb` gets "" when it could not be
    // asked for or the reply was not one; a caller treats that as a failed request and leaves the row
    // queued, because it is a waiting problem and not a refusal.
    void ensureSelfId(std::function<void(QString)> done);
    void storeTokenReply(const tracker::kitsu::TokenReply& r);

    // One authenticated GET. `cb` receives the HTTP status (0 for "no reply at all"), the Retry-After
    // header in seconds (0 when absent), and the body — carried rather than collapsed into ok/fail because
    // the BACKOFF is a decision about which failure this was.
    void get(const QString& url, std::function<void(int status, qint64 retryAfterSec, QByteArray)> cb);
    // One authenticated JSON:API write. `verb` is "PATCH" or "POST" (tracker::kitsu::saveMethod).
    void write(const QString& url, const QByteArray& verb, const QByteArray& body,
               std::function<void(int status, qint64 retryAfterSec, QByteArray)> cb);

    static QString apiUrl();
    static QString authBase();

    // THE LOOP ITSELF is TrackerQueue::Sender (#326); this is the one line that starts it.
    void drain();

    QNetworkAccessManager* nam_ = nullptr;
    QTimer*                retry_ = nullptr;
    // The session's user id. In memory only — see the header.
    QString                selfId_;
    std::unique_ptr<TrackerQueue::Sender> sender_;
    SingleFlight           tokenRefresh_;
    SingleFlight           selfLookup_;
};
