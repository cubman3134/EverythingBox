#include "MyAnimeListTracker.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "TrackerLinks.h"
#include "TrackerQueue.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace tracker;

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

MyAnimeListTracker::MyAnimeListTracker(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, [this] { drain(); });
}

MyAnimeListTracker::~MyAnimeListTracker() { closeLoopback(); }

// ---- configuration + credentials -------------------------------------------------------------------

QString MyAnimeListTracker::clientId()
{
    // THE #81 SEAM, the AniList one's twin: the zero-config follow-up replaces this body with "typed value,
    // else the embedded BuiltinSecrets slot" and touches nothing else in the feature.
    return store().value(clientIdKey(Id::MyAnimeList)).toString();
}

QString MyAnimeListTracker::clientSecret()
{
    return store().value(clientSecretKey(Id::MyAnimeList)).toString();
}

void MyAnimeListTracker::setClientId(const QString& v)
{
    store().setValue(clientIdKey(Id::MyAnimeList), v.trimmed());
    store().sync();
}

void MyAnimeListTracker::setClientSecret(const QString& v)
{
    store().setValue(clientSecretKey(Id::MyAnimeList), v.trimmed());
    store().sync();
}

// THE ID ALONE. MAL issues public clients with no secret at all, and demanding one would lock out exactly
// the registration the app's docs tell a user to make. AniList's isConfigured() needs both because AniList
// issues no public clients.
bool MyAnimeListTracker::isConfigured() { return !clientId().isEmpty(); }

bool MyAnimeListTracker::isConnected()
{
    return !store().value(accessKey(Id::MyAnimeList)).toString().isEmpty();
}

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
            const QByteArray req = sock->readAll();
            const QByteArray line = req.left(req.indexOf('\r'));
            const int sp1 = line.indexOf(' '), sp2 = line.indexOf(' ', sp1 + 1);
            const QString target = QString::fromUtf8(line.mid(sp1 + 1, sp2 - sp1 - 1));
            const QUrlQuery q(QUrl::fromEncoded(("http://localhost" + target.toUtf8())).query());
            const QString err = q.queryItemValue(QStringLiteral("error"));
            const QString code = q.queryItemValue(QStringLiteral("code"));
            // THE CSRF CHECK. A callback that does not carry our own state back is not our callback, and a
            // code from it is somebody else's — redeeming it would link the user's app to another account.
            const bool stateOk = !state_.isEmpty()
                              && q.queryItemValue(QStringLiteral("state")) == state_;

            // A plain body rather than a redirect: sending the browser anywhere would hand a third party a
            // request whose Referer names this loopback port. Content-Length is explicit so the browser does
            // not sit waiting on a connection close.
            const QByteArray page = err.isEmpty() && !code.isEmpty() && stateOk
                ? QByteArray("Signed in. You can close this tab and go back to the app.")
                : QByteArray("Sign-in was not completed. You can close this tab.");
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                        "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n"
                        "Content-Length: " + QByteArray::number(page.size()) + "\r\n\r\n" + page);
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
    store().setValue(accessKey(Id::MyAnimeList), r.accessToken);
    // MAL ROTATES the refresh token on every refresh, so this is normally present and normally NEW. It is
    // still guarded: a reply that omits it must leave the old one alone rather than blanking it, which
    // would unlink the account at the next expiry.
    if (!r.refreshToken.isEmpty()) store().setValue(refreshKey(Id::MyAnimeList), r.refreshToken);
    // MAL's access tokens last about a month. A missing expires_in stores 0, and 0 means "assume valid"
    // below rather than "expired": treating an unknown expiry as expired would refresh on every request.
    store().setValue(expiryKey(Id::MyAnimeList),
                     r.expiresInSec > 0 ? QDateTime::currentSecsSinceEpoch() + r.expiresInSec : 0);
    store().sync();
}

void MyAnimeListTracker::disconnectAccount()
{
    closeLoopback();
    codeVerifier_.clear();
    state_.clear();
    store().remove(accessKey(Id::MyAnimeList));
    store().remove(refreshKey(Id::MyAnimeList));
    store().remove(expiryKey(Id::MyAnimeList));
    store().sync();
    TrackerQueue::forgetAccount(Id::MyAnimeList);
    failures_ = 0;
    emit connectedChanged(false);
    emit queueChanged();
}

void MyAnimeListTracker::ensureValidToken(std::function<void(bool ok)> done)
{
    if (!isConfigured() || !isConnected()) { if (done) done(false); return; }
    const qint64 expiry = store().value(expiryKey(Id::MyAnimeList), 0).toLongLong();
    if (expiry <= 0 || expiry - 60 > QDateTime::currentSecsSinceEpoch()) { if (done) done(true); return; }
    const QString refresh = store().value(refreshKey(Id::MyAnimeList)).toString();
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

// Retry-After, in seconds, or 0 when it was absent or was the HTTP-date form (which this does not parse:
// answering an unparsed date with 0 falls back to our own base wait, which is never shorter than a minute).
static qint64 retryAfterOf(QNetworkReply* rep)
{
    bool ok = false;
    const qint64 secs = QString::fromLatin1(rep->rawHeader("Retry-After")).trimmed().toLongLong(&ok);
    return (ok && secs > 0) ? secs : 0;
}

void MyAnimeListTracker::get(const QString& url, std::function<void(int, qint64, QByteArray)> cb)
{
    if (url.isEmpty()) { if (cb) cb(0, 0, QByteArray()); return; }
    ensureValidToken([this, url, cb](bool ok) {
        if (!ok) { if (cb) cb(0, 0, QByteArray()); return; }
        QNetworkRequest req{ QUrl(url) };
        req.setRawHeader("Accept", "application/json");
        req.setRawHeader("Authorization",
                         "Bearer " + store().value(accessKey(Id::MyAnimeList)).toString().toUtf8());
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
                         "Bearer " + store().value(accessKey(Id::MyAnimeList)).toString().toUtf8());
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

void MyAnimeListTracker::drain()
{
    if (sending_ || !isConfigured() || !isConnected()) return;
    QVector<Update> q = TrackerQueue::load(Id::MyAnimeList);
    if (q.isEmpty()) { retry_->stop(); return; }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 soonest = -1;
    const int idx = TrackerQueue::nextSendable(q, Id::MyAnimeList, now, &soonest);
    if (idx < 0)
    {
        // Everything queued is inside its item's debounce window. Wake exactly when the earliest one opens
        // rather than polling: a binge-reader would otherwise have a timer firing every second.
        retry_->start(int(qBound<qint64>(1000, soonest, kDebounceMs)));
        return;
    }

    const Update u = q[idx];
    // The COMPLETED decision uses the TRACKER'S OWN unit count, captured when the link was made. Reading it
    // from the link rather than from the app's chapter list is what keeps a partial provider listing from
    // marking a running series finished.
    const int total = TrackerLinks::get(Id::MyAnimeList, u.itemKey).totalUnits;
    sending_ = true;
    patch(mal::saveUrl(apiUrl(), u.mediaId, u.kind), mal::saveBody(u, total),
          [this, u](int status, qint64 retryAfterSec, QByteArray body) {
        sending_ = false;
        Q_UNUSED(body);   // deliberately unread: nothing MAL echoes is worth quoting back at the user
        if (status >= 200 && status < 300)
        {
            failures_ = 0;
            TrackerQueue::removeDelivered(Id::MyAnimeList, u);
            TrackerQueue::noteSent(Id::MyAnimeList, u.itemKey, QDateTime::currentMSecsSinceEpoch());
            TrackerQueue::setLastError(Id::MyAnimeList, QString());
            emit progressPushed(u.itemKey, u.unit);
            emit queueChanged();
            drain();   // keep going; the next item's debounce is checked afresh
            return;
        }

        ++failures_;
        const mal::Backoff b = mal::backoffFor(status, retryAfterSec, failures_);
        if (b.permanent)
        {
            // MAL will never accept this row. DROPPED rather than left to wedge the head of the queue for
            // ever — every later chapter behind it would be lost too. Said out loud in the status line.
            TrackerQueue::removeDelivered(Id::MyAnimeList, u);
            TrackerQueue::setLastError(Id::MyAnimeList,
                tr("MyAnimeList refused one update and it has been dropped; the rest are still queued."));
            emit queueChanged();
            drain();
            return;
        }
        // A message ABOUT the failure, never the request — see the file header.
        TrackerQueue::setLastError(Id::MyAnimeList,
            b.reauth ? tr("MyAnimeList needs signing in again; updates are queued.")
                     : tr("MyAnimeList did not accept the update; it is queued and will be retried."));
        retry_->start(int(qBound<qint64>(1000, b.delayMs, mal::kBackoffMaxMs)));
        emit queueChanged();
    });
}
