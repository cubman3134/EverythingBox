#include "MyAnimeListTracker.h"
#include "TrackerLinks.h"
#include "TrackerQueue.h"   // the ONE queue, credential store and drain loop, shared with AniList

#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace tracker;

// The ini lives in TrackerQueue now (#326): this file and AniListTracker had the identical
// `static QSettings& store()` and the identical five credential accessors. Both call the shared ones.

MyAnimeListTracker::MyAnimeListTracker(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, [this] { drain(); });

    // THE SHARED DRAIN LOOP (#326) — the same one AniList runs. What is supplied here is only what is
    // really MyAnimeList's: whether the tracker is usable, how one row is PATCHed, and three sentences that
    // name it. The classification that increment 2 wrote as mal::backoffFor is now the policy below, asked
    // by tracker::classifySend, so the two trackers cannot drift apart on what a failure means.
    TrackerQueue::SendSpec spec;
    spec.id = Id::MyAnimeList;
    spec.policy = mal::sendPolicy();
    spec.ready = [] { return isConfigured() && isConnected(); };
    spec.send = [this](const Update& u, std::function<void(TrackerQueue::Reply)> done) {
        // The COMPLETED decision uses the TRACKER'S OWN unit count, captured when the link was made.
        // Reading it from the link rather than from the app's chapter list is what keeps a partial provider
        // listing from marking a running series finished.
        const int total = TrackerLinks::get(Id::MyAnimeList, u.itemKey).totalUnits;
        patch(mal::saveUrl(apiUrl(), u.mediaId, u.kind), mal::saveBody(u, total),
              [done](int status, qint64 retryAfterSec, QByteArray body) {
            Q_UNUSED(body);   // deliberately unread: nothing MAL echoes is worth quoting back at the user
            TrackerQueue::Reply r;
            r.accepted      = status >= 200 && status < 300;
            r.status        = status;
            r.retryAfterSec = retryAfterSec;
            if (done) done(r);
        });
    };
    spec.droppedMessage = [](const Update& u) {
        // WHICH update, by the title the link store already holds — no request, and nothing out of a
        // response body. An unlinked or untitled row falls back to the sentence increment 2 shipped.
        const QString title = TrackerLinks::get(Id::MyAnimeList, u.itemKey).title;
        return title.isEmpty()
            ? tr("MyAnimeList refused one update and it has been dropped; the rest are still queued.")
            : tr("MyAnimeList refused the update for %1 and it has been dropped; "
                 "the rest are still queued.").arg(title);
    };
    spec.reauthMessage = tr("MyAnimeList needs signing in again; updates are queued.");
    // A message ABOUT the failure, never the request — see the file header.
    spec.retryMessage = tr("MyAnimeList did not accept the update; it is queued and will be retried.");
    spec.pushed = [this](const QString& itemKey, int unit) { emit progressPushed(itemKey, unit); };
    spec.changed = [this] { emit queueChanged(); };
    spec.wait = [this](int delayMs) { if (delayMs > 0) retry_->start(delayMs); else retry_->stop(); };
    sender_ = std::make_unique<TrackerQueue::Sender>(std::move(spec));
}

MyAnimeListTracker::~MyAnimeListTracker() { closeLoopback(); }

// ---- configuration + credentials -------------------------------------------------------------------

QString MyAnimeListTracker::clientId()
{
    // THE #81 SEAM, the AniList one's twin: the zero-config follow-up replaces this body with "typed value,
    // else the embedded BuiltinSecrets slot" and touches nothing else in the feature. The ini access
    // underneath moved to TrackerQueue in #326; the seam did not.
    return TrackerQueue::clientId(Id::MyAnimeList);
}

QString MyAnimeListTracker::clientSecret() { return TrackerQueue::clientSecret(Id::MyAnimeList); }

void MyAnimeListTracker::setClientId(const QString& v)
{
    TrackerQueue::setClientId(Id::MyAnimeList, v);
}

void MyAnimeListTracker::setClientSecret(const QString& v)
{
    TrackerQueue::setClientSecret(Id::MyAnimeList, v);
}

// THE ID ALONE. MAL issues public clients with no secret at all, and demanding one would lock out exactly
// the registration the app's docs tell a user to make. AniList's isConfigured() needs both because AniList
// issues no public clients.
bool MyAnimeListTracker::isConfigured() { return !clientId().isEmpty(); }

bool MyAnimeListTracker::isConnected() { return TrackerQueue::hasAccessToken(Id::MyAnimeList); }

QString MyAnimeListTracker::apiUrl()
{
    // The stub hook for a live drive, read per call so a rig can point the app at a fixture with no rebuild.
    return qEnvironmentVariable("EB_MAL_ENDPOINT", mal::defaultApiUrl());
}

QString MyAnimeListTracker::authBase()
{
    return qEnvironmentVariable("EB_MAL_AUTH", mal::defaultAuthBase());
}

// ---- linking ----------------------------------------------------------------------------------------

void MyAnimeListTracker::closeLoopback()
{
    if (!loopback_) return;
    loopback_->close();
    loopback_->deleteLater();
    loopback_ = nullptr;
}

void MyAnimeListTracker::connectAccount()
{
    if (!isConfigured()) { emit connectError(tr("Enter your MyAnimeList Client ID first.")); return; }

    closeLoopback();
    loopback_ = new QTcpServer(this);
    if (!loopback_->listen(QHostAddress::LocalHost, 0))
    {
        closeLoopback();
        emit connectError(tr("Couldn't open a local port for sign-in."));
        return;
    }
    redirectUri_ = QStringLiteral("http://127.0.0.1:%1").arg(loopback_->serverPort());
    // Fresh per attempt. The verifier is the only thing tying the code that comes back to the request that
    // went out; reusing one across attempts would make a code captured from an abandoned sign-in redeemable.
    codeVerifier_ = mal::makeCodeVerifier();
    state_ = QString::number(QRandomGenerator::system()->generate64(), 16);

    connect(loopback_, &QTcpServer::newConnection, this, [this] {
        QTcpSocket* sock = loopback_->nextPendingConnection();
        if (!sock) return;
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            // The request parsing and the reply bytes are tracker::parseLoopbackRequest /
            // tracker::loopbackResponse (#326) — both trackers had written them out identically. THE CSRF
            // CHECK STAYED HERE, because it is not shared: AniList's flow carries no state, and a rule the
            // shared parser enforced would be a rule about a value one of the two providers never sends.
            const LoopbackCallback cb = parseLoopbackRequest(sock->readAll());
            const QString err = cb.error;
            const QString code = cb.code;
            // A callback that does not carry our own state back is not our callback, and a code from it is
            // somebody else's — redeeming it would link the user's app to another account.
            const bool stateOk = !state_.isEmpty() && cb.state == state_;

            sock->write(loopbackResponse(err.isEmpty() && !code.isEmpty() && stateOk));
            sock->flush();
            sock->disconnectFromHost();
            closeLoopback();

            if (!code.isEmpty() && stateOk) { exchangeCode(code); return; }
            if (!code.isEmpty() && !stateOk)
            {
                emit connectError(tr("That sign-in didn't come from this app; nothing was linked."));
                return;
            }
            // The error CODE only. MAL's `message` is free text echoed from the request and has been
            // observed to quote parameters back, so nothing that could carry a secret is surfaced.
            emit connectError(err.isEmpty() ? tr("Sign-in was cancelled.")
                                            : tr("MyAnimeList refused the sign-in (%1).").arg(err));
        });
    });

    const QString url = mal::authorizeUrl(authBase(), clientId(), redirectUri_, codeVerifier_, state_);
    // Emitted BEFORE the browser is asked to open, so a surface that has to show the URL (a TV, where the
    // browser may open somewhere the user cannot see) always gets it. It carries the PKCE challenge, which
    // is public by construction in the `plain` method and is single-use; it carries no token and no secret.
    emit authUrlReady(url);
    QDesktopServices::openUrl(QUrl(url));
}

void MyAnimeListTracker::exchangeCode(const QString& code)
{
    QNetworkRequest req{ QUrl(authBase() + QStringLiteral("/token")) };
    // FORM, not JSON. MAL's token endpoint refuses a JSON body outright.
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Accept", "application/json");
    QNetworkReply* rep = nam_->post(req, mal::tokenExchangeBody(clientId(), clientSecret(),
                                                                redirectUri_, code, codeVerifier_));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        // The verifier has done its one job either way; it must not survive to be reused.
        codeVerifier_.clear();
        state_.clear();
        const mal::TokenReply r = mal::parseTokenReply(rep->readAll());
        if (!r.ok)
        {
            // A SENTENCE OF OUR OWN. rep->errorString() embeds the URL, and this URL is the token endpoint;
            // the HTTP status is the whole of what we say about it.
            const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            emit connectError(status > 0
                ? tr("MyAnimeList did not return a token (HTTP %1).").arg(status)
                : tr("MyAnimeList did not return a token; the connection failed."));
            return;
        }
        storeTokenReply(r);
        emit connectedChanged(true);
        flushQueue();   // an account linked after an offline session delivers what was queued
    });
}

void MyAnimeListTracker::storeTokenReply(const mal::TokenReply& r)
{
    if (!r.ok) return;   // never write an unsuccessful reply over live tokens — see TrackerRules
    // MAL ROTATES the refresh token on every refresh, so it is normally present and normally NEW. The
    // omitted-refresh-token guard is TrackerQueue's, for both trackers: a reply that omits it must leave the
    // old one alone rather than blanking it, which would unlink the account at the next expiry.
    //
    // MAL's access tokens last about a month. A missing expires_in stores 0, and 0 means "assume valid"
    // rather than "expired": treating an unknown expiry as expired would refresh on every request.
    TrackerQueue::storeTokens(Id::MyAnimeList, r.accessToken, r.refreshToken, r.expiresInSec,
                              QDateTime::currentSecsSinceEpoch());
}

void MyAnimeListTracker::disconnectAccount()
{
    closeLoopback();
    codeVerifier_.clear();
    state_.clear();
    TrackerQueue::clearTokens(Id::MyAnimeList);
    TrackerQueue::forgetAccount(Id::MyAnimeList);
    // ...and the consecutive-failure count goes with it: a fresh link is exactly the moment it is worth
    // trying again immediately rather than half an hour from now.
    if (sender_) sender_->reset();
    emit connectedChanged(false);
    emit queueChanged();
}

void MyAnimeListTracker::ensureValidToken(std::function<void(bool ok)> done)
{
    if (!isConfigured() || !isConnected()) { if (done) done(false); return; }
    if (TrackerQueue::tokenFresh(Id::MyAnimeList, QDateTime::currentSecsSinceEpoch(), 60))
    { if (done) done(true); return; }
    const QString refresh = TrackerQueue::refreshToken(Id::MyAnimeList);
    if (refresh.isEmpty()) { if (done) done(false); return; }

    // SINGLE-FLIGHT, and on MAL it matters more than it does on AniList: MAL rotates the refresh token, so
    // two overlapping refreshes race to invalidate each other's and can break the link permanently.
    if (!tokenRefresh_.join(std::move(done))) return;

    QNetworkRequest req{ QUrl(authBase() + QStringLiteral("/token")) };
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Accept", "application/json");
    QNetworkReply* rep = nam_->post(req, mal::tokenRefreshBody(clientId(), clientSecret(), refresh));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const mal::TokenReply r = mal::parseTokenReply(rep->readAll());
        if (r.ok) storeTokenReply(r);
        tokenRefresh_.settle(r.ok);
    });
}

// ---- requests ---------------------------------------------------------------------------------------

// The status a reply really carried. 0 means "there was no HTTP answer at all" — a dead socket, a DNS
// failure, a cancelled request — which the backoff treats as a waiting problem rather than a verdict.
static int statusOf(QNetworkReply* rep)
{
    return rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// Retry-After, in seconds. The PARSING is tracker::retryAfterSeconds (#326) — both trackers read the
// delta-seconds form and neither parses the HTTP-date one, so there is one reader rather than two.
static qint64 retryAfterOf(QNetworkReply* rep)
{
    return retryAfterSeconds(rep->rawHeader("Retry-After"));
}

void MyAnimeListTracker::get(const QString& url, std::function<void(int, qint64, QByteArray)> cb)
{
    if (url.isEmpty()) { if (cb) cb(0, 0, QByteArray()); return; }
    ensureValidToken([this, url, cb](bool ok) {
        if (!ok) { if (cb) cb(0, 0, QByteArray()); return; }
        QNetworkRequest req{ QUrl(url) };
        req.setRawHeader("Accept", "application/json");
        req.setRawHeader("Authorization",
                         "Bearer " + TrackerQueue::accessToken(Id::MyAnimeList).toUtf8());
        QNetworkReply* rep = nam_->get(req);
        connect(rep, &QNetworkReply::finished, this, [rep, cb] {
            rep->deleteLater();
            if (cb) cb(statusOf(rep), retryAfterOf(rep), rep->readAll());
        });
    });
}

void MyAnimeListTracker::patch(const QString& url, const QByteArray& form,
                               std::function<void(int, qint64, QByteArray)> cb)
{
    if (url.isEmpty()) { if (cb) cb(0, 0, QByteArray()); return; }
    ensureValidToken([this, url, form, cb](bool ok) {
        if (!ok) { if (cb) cb(0, 0, QByteArray()); return; }
        QNetworkRequest req{ QUrl(url) };
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
        req.setRawHeader("Accept", "application/json");
        req.setRawHeader("Authorization",
                         "Bearer " + TrackerQueue::accessToken(Id::MyAnimeList).toUtf8());
        // MAL's list write is a PATCH; QNetworkAccessManager has no convenience for it.
        QNetworkReply* rep = nam_->sendCustomRequest(req, "PATCH", form);
        connect(rep, &QNetworkReply::finished, this, [rep, cb] {
            rep->deleteLater();
            if (cb) cb(statusOf(rep), retryAfterOf(rep), rep->readAll());
        });
    });
}

void MyAnimeListTracker::search(const QString& title, int year, Kind kind,
                                std::function<void(QVector<Match>)> cb)
{
    Q_UNUSED(year);   // MAL's search takes no year filter; the year is shown in the picker instead
    if (!isConfigured() || !isConnected()) { if (cb) cb({}); return; }
    // Shorter than MAL will answer, so we do not ask. The tracker being off and a query too short to be
    // meaningful are both an EMPTY result, never an error — the caller's "no matches" path covers both.
    const QString url = mal::searchUrl(apiUrl(), title, kind, 8);
    if (url.isEmpty()) { if (cb) cb({}); return; }
    const QString query = title.trimmed();
    get(url, [cb, kind, query](int status, qint64, QByteArray body) {
        if (status < 200 || status >= 300) { if (cb) cb({}); return; }
        // RANKED, and the noise dropped. MAL's `q=` is a fuzzy full-text search that answers a title it does
        // not have with five loosely-related shows, and a controller user scrolling that list is one press
        // from linking the wrong series — see tracker::rankMatches for what "noise" means and for the case
        // where every row scores zero and they are all offered unchanged.
        if (cb) cb(rankMatches(query, mal::parseSearch(body, kind)));
    });
}

void MyAnimeListTracker::fetchEntry(const QString& mediaId, Kind kind,
                                    std::function<void(bool, Entry)> cb)
{
    if (mediaId.isEmpty() || !isConfigured() || !isConnected()) { if (cb) cb(false, Entry{}); return; }
    // KIND-DEPENDENT, which is why the seam carries a Kind here at all: MAL's path and its field list both
    // differ between anime and manga, and asking for the wrong one answers 404.
    get(mal::entryUrl(apiUrl(), mediaId, kind), [cb, mediaId, kind](int status, qint64, QByteArray body) {
        Entry e;
        const bool parsed = status >= 200 && status < 300 && mal::parseEntry(body, mediaId, kind, e);
        if (cb) cb(parsed, e);
    });
}

// ---- the queue --------------------------------------------------------------------------------------

int MyAnimeListTracker::queuedCount() { return TrackerQueue::count(Id::MyAnimeList); }

QString MyAnimeListTracker::lastError() { return TrackerQueue::lastError(Id::MyAnimeList); }

void MyAnimeListTracker::pushProgress(const Update& in)
{
    if (in.mediaId.isEmpty() || in.itemKey.isEmpty()) return;   // no link, no push (issue's rule)
    Update u = in;
    if (u.atMs <= 0) u.atMs = QDateTime::currentMSecsSinceEpoch();
    // THE SHARED QUEUE. Same rules, same keys, one implementation — and keyed by Id::MyAnimeList, so a
    // chapter this account refused stays pending here and not on AniList's.
    if (!TrackerQueue::enqueue(Id::MyAnimeList, u)) return;
    emit queueChanged();
    drain();
}

void MyAnimeListTracker::flushQueue() { drain(); }

// THE LOOP MOVED (#326). Increment 2 wrote these thirty lines here and AniList had thirty near-identical
// ones; both are now TrackerQueue::Sender, built in the constructor. Nothing MAL answers changed: the same
// policy, the same doubling, the same drop-and-carry-on — it is simply the only copy of them now.
void MyAnimeListTracker::drain() { if (sender_) sender_->drain(); }
