// JELLYFIN QUICK CONNECT (issue #83) — signing in to a Jellyfin server by approving a six-digit code on an
// app that is already signed in, instead of typing a password with a D-pad.
//
// ==================================================================================================
// 1. THE PROTOCOL, AS THE SERVER PUBLISHES IT
// ==================================================================================================
// Jellyfin's own OpenAPI (api.jellyfin.org/openapi/jellyfin-openapi-stable.json, and the controller source
// for 10.8.13 / 10.9.11 / 10.10.7) spells four calls, all under the same `Authorization: MediaBrowser ...`
// client/device header the password sign-in already sends, with no token:
//
//   GET  /QuickConnect/Enabled                  -> a bare JSON bool. 10.7 has no such route (it had
//                                                  /QuickConnect/Status) and answers 404.
//   POST /QuickConnect/Initiate                 -> QuickConnectResult { Secret, Code, Authenticated, … }.
//                                                  10.8 spells it GET; 10.9 and 10.10 accept both (the GET is
//                                                  marked Obsolete); the current stable schema has POST only.
//                                                  So: POST first, and a 404/405 retries once as a GET.
//   GET  /QuickConnect/Connect?secret=<Secret>  -> the same QuickConnectResult, `Authenticated: true` once the
//                                                  user has approved the code. 404 "Unknown secret" once the
//                                                  server has expired the request (its own limit is ten
//                                                  minutes); 401 when Quick Connect was switched off.
//   POST /Users/AuthenticateWithQuickConnect    -> body { "Secret": … }; answers the SAME AuthenticationResult
//                                                  as /Users/AuthenticateByName, so it is read by the same
//                                                  Jellyfin::readAuthResult and stored through the same
//                                                  JellyfinServerStore::fromSignIn as a password sign-in.
//
// ==================================================================================================
// 2. THE DECISIONS ARE PURE; THE SOCKETS ARE A THIN SHELL
// ==================================================================================================
// "Offer Quick Connect or go straight to the password?" and "what does this poll answer mean?" are the two
// questions the feature turns on, and both are pure functions here (routeFor, stateFor) so probe_jellyfin
// drives every arm of them over a table. JellyfinQuickConnectSession is the socket half: it asks, and hands
// each answer to those functions. It takes its QNetworkAccessManager and a request DECORATOR rather than
// reaching for JellyfinClient, so the probe drives the real session against a fake server without linking
// the client's settings and identity plumbing — JellyfinClient supplies the decorator the app uses.
//
// ==================================================================================================
// 3. THE SECRET IS A CREDENTIAL UNTIL IT EXPIRES
// ==================================================================================================
// Whoever holds an approved Secret can exchange it for an access token. So it is held by the session for as
// long as the session lives, sent to the server that issued it and to nobody else (SameOriginRedirectPolicy
// comes with the decorator), and never logged, rendered or stored. The CODE is not a credential — it is
// what the user reads off the screen — and it is the only value this file hands to the UI.
#pragma once
#include "Jellyfin.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QTimer;

namespace JellyfinQuickConnect
{
    // Poll every two seconds; give up after five minutes. The server keeps a request for ten, but nobody is
    // still standing in front of a television waiting for a code after five, and a panel that polls for ten
    // is a panel that talks to a server long after its user has gone.
    constexpr int kPollIntervalMs = 2000;
    constexpr int kLifetimeMs     = 5 * 60 * 1000;
    // Each individual request's own deadline. A poll that does not answer in this long is a MISSED poll,
    // not the end of the flow (see stateFor).
    constexpr int kRequestBudgetMs = 10000;

    // The one spelling of each path.
    QString enabledPath();
    QString initiatePath();
    QString connectPath();          // without the query
    QString authenticatePath();
    QString connectQuery(const QString& secret);          // "secret=<percent-encoded>"
    QByteArray authenticateBody(const QString& secret);   // {"Secret":"…"} — carries the secret; never held

    // ---- Offer Quick Connect, or go straight to the password? ------------------------------------------
    // Quick Connect ONLY when the server answered 200 with the JSON literal `true`. Everything else — false,
    // a 404 from a server too old to have the route, a proxy's HTML page, a timeout, a refused connection —
    // is the password flow, which is exactly what signing in looked like before this feature. The asymmetry
    // is deliberate: wrongly offering Quick Connect costs a panel that can never be approved, while wrongly
    // offering the password costs nothing the user did not already have.
    enum class Route { QuickConnect, Password };
    Route routeFor(bool transportOk, int httpStatus, const QByteArray& body);

    // The socket half of that question: GET /QuickConnect/Enabled under `decorate`'s headers, answered
    // through routeFor, calling back EXACTLY ONCE (a timeout is an answer: Password). `context` guards the
    // callback — if it is gone, nothing is called.
    using Decorate = std::function<void(QNetworkRequest&)>;
    void fetchRoute(QNetworkAccessManager* nam, const QString& root, const Decorate& decorate, int budgetMs,
                    QObject* context, std::function<void(Route)> done);

    // ---- Initiate --------------------------------------------------------------------------------------
    struct Code
    {
        QString secret;   // THE CREDENTIAL HALF. See section 3.
        QString code;     // what the user types into the other app
        bool    ok = false;
    };
    Code readInitiate(const QByteArray& body);
    // A server that has the route under the other verb (10.8's GET) answers 405, or 404 from a router that
    // matches on method. Either is worth exactly one retry as a GET; nothing else is.
    bool initiateRetryAsGet(int httpStatus);

    // ---- What a poll answer means ----------------------------------------------------------------------
    //   Authorized — 200 and `Authenticated: true`.
    //   Waiting    — 200 and `Authenticated: false`; OR the poll itself did not get an answer (a timeout, a
    //                dropped connection, a 5xx). A missed poll on a living-room Wi-Fi link is not a verdict,
    //                and the five-minute lifetime already bounds how long a dead server can keep us waiting.
    //   Expired    — 404: the server no longer knows this secret. Its own ten-minute expiry, or a restart.
    //   Error      — 401/403 (Quick Connect was switched off while we waited), any other 4xx, or a 200 whose
    //                body is not a QuickConnectResult at all.
    enum class State { Waiting, Authorized, Expired, Error };
    State stateFor(bool transportOk, int httpStatus, const QByteArray& body);

    // Has this code outlived the panel's patience?
    bool timedOut(qint64 elapsedMs, qint64 lifetimeMs = kLifetimeMs);
}

// ONE Quick Connect attempt: one code, polled until it is approved, expires, fails or is cancelled.
//
// EVERY OUTCOME IS REPORTED EXACTLY ONCE, and after it the session is inert: the poll timer is stopped, any
// in-flight request is aborted and its answer ignored. cancel() is the same stop with no signal at all, and
// the destructor calls it — so a panel that owns the session (as its QObject child) stops the polling simply
// by closing, however it closes.
class JellyfinQuickConnectSession : public QObject
{
    Q_OBJECT
public:
    // Applies the Authorization header, Accept and the redirect policy. JellyfinClient passes its own; the
    // probe passes one built from the same Jellyfin::authHeader.
    using Decorate = JellyfinQuickConnect::Decorate;

    // `root` is an already-normalised server root (Jellyfin::normalizeRoot), no trailing slash.
    JellyfinQuickConnectSession(QNetworkAccessManager* nam, const QString& root, Decorate decorate,
                                QObject* parent = nullptr);
    ~JellyfinQuickConnectSession() override;

    // Test-sized timings. The app never calls these; the defaults are the section-1 numbers.
    void setPollIntervalMs(int ms) { pollMs_ = ms; }
    void setLifetimeMs(int ms)     { lifetimeMs_ = ms; }

    void start();                   // initiate; then codeReady, then polling
    void cancel();                  // stop, silently
    bool isActive() const { return active_; }
    int  pollCount() const { return polls_; }   // Connect requests SENT — diagnostic and probe use

signals:
    void codeReady(const QString& code);
    void authorized(const Jellyfin::AuthResult& result);   // result.ok is true; carries the token
    void expired();                                        // the server forgot it, or kLifetimeMs passed
    void failed(const QString& message);                   // one of our own sentences; never a url

private:
    void initiate(bool asGet);
    void schedulePoll();
    void poll();
    void exchange();
    void finish();                  // stop everything; after this no signal is emitted
    QNetworkReply* track(QNetworkReply* reply);

    QNetworkAccessManager* nam_ = nullptr;
    QString                root_;
    Decorate               decorate_;
    QString                secret_;
    QTimer*                timer_ = nullptr;
    QPointer<QNetworkReply> inFlight_;
    QElapsedTimer          clock_;
    int                    pollMs_     = JellyfinQuickConnect::kPollIntervalMs;
    int                    lifetimeMs_ = JellyfinQuickConnect::kLifetimeMs;
    int                    polls_      = 0;
    bool                   active_     = false;
};
