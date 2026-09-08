#include "AniListTracker.h"
#include "TrackerLinks.h"
#include "TrackerQueue.h"   // the ONE queue, credential store and drain loop, shared with MyAnimeList

#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace tracker;

// The ini lives in TrackerQueue now (#326): this file had its own `static QSettings& store()` and its own
// five credential accessors, and MyAnimeListTracker had the identical pair. Both call the shared ones.

AniListTracker::AniListTracker(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, [this] { drain(); });

    // THE SHARED DRAIN LOOP (#326). Everything below is what is genuinely AniList's: whether the tracker is
    // usable, how one row is put on the wire, what its answer MEANS, and three sentences that name it. The
    // loop itself — read the queue, respect the debounce, remove on success, drop what can never succeed,
    // back off on what can — is TrackerQueue::Sender, and MyAnimeList runs the same one.
    TrackerQueue::SendSpec spec;
    spec.id = Id::AniList;
    // 400/404 drop; 401 refreshes; 429 backs off honouring Retry-After; everything else waits. The base is
    // the 60 seconds increment 1 already waited, to the millisecond — see anilist::sendPolicy().
    spec.policy = anilist::sendPolicy();
    spec.ready = [] { return isConfigured() && isConnected(); };
    spec.send = [this](const Update& u, std::function<void(TrackerQueue::Reply)> done) {
        // The COMPLETED decision uses the tracker's OWN unit count, captured when the link was made — see
        // anilist::saveBody. Reading it from the link rather than from the app's chapter list is what keeps
        // a partial provider listing from marking a running series finished.
        const int total = TrackerLinks::get(Id::AniList, u.itemKey).totalUnits;
        post(anilist::saveBody(u, total),
             [done](bool netOk, int status, qint64 retryAfterSec, QByteArray body) {
            TrackerQueue::Reply r;
            // A GraphQL error arrives as HTTP 200 with an `errors` array and no `data`, so transport
            // success is not acceptance. Anything that is not a SaveMediaListEntry payload is a failure,
            // and the status the POLICY judges is then the one AniList put inside that error object.
            r.accepted      = netOk && anilist::saveAccepted(status, body);
            r.status        = anilist::effectiveStatus(status, body);
            r.retryAfterSec = retryAfterSec;
            if (done) done(r);
        });
    };
    spec.droppedMessage = [](const Update& u) {
        // WHICH update, by the title the link store already holds — no request, and nothing out of a
        // response body. An unlinked or untitled row falls back to the sentence increment 2 shipped.
        const QString title = TrackerLinks::get(Id::AniList, u.itemKey).title;
        return title.isEmpty()
            ? tr("AniList refused one update and it has been dropped; the rest are still queued.")
            : tr("AniList refused the update for %1 and it has been dropped; "
                 "the rest are still queued.").arg(title);
    };
    spec.reauthMessage = tr("AniList needs signing in again; updates are queued.");
    // A message ABOUT the failure. Never the body, never the request — see the file header.
    spec.retryMessage = tr("AniList did not accept the update; it is queued and will be retried.");
    spec.pushed = [this](const QString& itemKey, int unit) { emit progressPushed(itemKey, unit); };
    spec.changed = [this] { emit queueChanged(); };
    spec.wait = [this](int delayMs) { if (delayMs > 0) retry_->start(delayMs); else retry_->stop(); };
    sender_ = std::make_unique<TrackerQueue::Sender>(std::move(spec));
}

AniListTracker::~AniListTracker() { closeLoopback(); }

// ---- configuration + credentials -------------------------------------------------------------------

QString AniListTracker::clientId()
{
    // THE #81 SEAM. The zero-config follow-up replaces this body with "typed value, else the embedded
    // BuiltinSecrets slot" and touches nothing else in the feature. See tracker::builtinSecretIdSlot().
    // The ini access underneath moved to TrackerQueue in #326; the SEAM did not move, because this is still
    // the one place in the app that decides what "the AniList client id" means.
    return TrackerQueue::clientId(Id::AniList);
}

QString AniListTracker::clientSecret() { return TrackerQueue::clientSecret(Id::AniList); }

void AniListTracker::setClientId(const QString& v) { TrackerQueue::setClientId(Id::AniList, v); }

void AniListTracker::setClientSecret(const QString& v) { TrackerQueue::setClientSecret(Id::AniList, v); }

bool AniListTracker::isConfigured() { return !clientId().isEmpty() && !clientSecret().isEmpty(); }

bool AniListTracker::isConnected() { return TrackerQueue::hasAccessToken(Id::AniList); }

QString AniListTracker::apiUrl()
{
    // The stub hook for a live drive. Read per call rather than cached so a rig can point the app at a
    // fixture without a rebuild; absent, this is the real service.
    return qEnvironmentVariable("EB_ANILIST_ENDPOINT", anilist::defaultApiUrl());
}

QString AniListTracker::authBase()
{
    return qEnvironmentVariable("EB_ANILIST_AUTH", anilist::defaultAuthBase());
}

// ---- linking ----------------------------------------------------------------------------------------

void AniListTracker::closeLoopback()
{
    if (!loopback_) return;
    loopback_->close();
    loopback_->deleteLater();
    loopback_ = nullptr;
}

void AniListTracker::connectAccount()
{
    if (!isConfigured()) { emit connectError(tr("Enter your AniList Client ID and Secret first.")); return; }

    closeLoopback();
    loopback_ = new QTcpServer(this);
    if (!loopback_->listen(QHostAddress::LocalHost, 0))
    {
        closeLoopback();
        emit connectError(tr("Couldn't open a local port for sign-in."));
        return;
    }
    redirectUri_ = QStringLiteral("http://127.0.0.1:%1").arg(loopback_->serverPort());

    connect(loopback_, &QTcpServer::newConnection, this, [this] {
        QTcpSocket* sock = loopback_->nextPendingConnection();
        if (!sock) return;
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            // The request parsing and the reply bytes are tracker::parseLoopbackRequest /
            // tracker::loopbackResponse (#326) — both trackers had written them out identically, and the
            // Referer/Cache-Control posture they encode is now stated in one place a probe can read.
            const LoopbackCallback cb = parseLoopbackRequest(sock->readAll());
            const QString err = cb.error;
            const QString code = cb.code;

            sock->write(loopbackResponse(err.isEmpty() && !code.isEmpty()));
            sock->flush();
            sock->disconnectFromHost();
            closeLoopback();

            if (!code.isEmpty()) { exchangeCode(code); return; }
            // The error CODE only — AniList's `error_description` is free text echoed from the request and
            // has been observed to quote parameters back. Nothing that could carry a secret is surfaced.
            emit connectError(err.isEmpty() ? tr("Sign-in was cancelled.")
                                            : tr("AniList refused the sign-in (%1).").arg(err));
        });
    });

    const QString url = anilist::authorizeUrl(authBase(), clientId(), redirectUri_);
    // Emitted BEFORE the browser is asked to open, so a surface that has to show the URL (a TV, where the
    // browser may open somewhere the user cannot see) always gets it — even if openUrl fails outright.
    emit authUrlReady(url);
    QDesktopServices::openUrl(QUrl(url));
}

void AniListTracker::exchangeCode(const QString& code)
{
    QNetworkRequest req{ QUrl(authBase() + QStringLiteral("/token")) };
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    QNetworkReply* rep = nam_->post(req,
        anilist::tokenExchangeBody(clientId(), clientSecret(), redirectUri_, code));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const anilist::TokenReply r = anilist::parseTokenReply(rep->readAll());
        if (!r.ok)
        {
            // The EXCEPTION, not the request: rep->errorString() names the transport failure and never the
            // body we sent. A 200 carrying an error object lands here too, with no token to leak.
            emit connectError(tr("AniList did not return a token (%1).").arg(rep->errorString()));
            return;
        }
        storeTokenReply(r);
        emit connectedChanged(true);
        flushQueue();   // an account linked after an offline session delivers what was queued
    });
}

void AniListTracker::storeTokenReply(const anilist::TokenReply& r)
{
    if (!r.ok) return;   // never write an unsuccessful reply over live tokens — see TrackerRules
    // AniList's access tokens are long-lived (a year). A missing expires_in stores 0, and 0 means "assume
    // valid" rather than "expired": treating an unknown expiry as expired would refresh on every request
    // against a token that is fine. The omitted-refresh-token guard is TrackerQueue's, for both trackers.
    TrackerQueue::storeTokens(Id::AniList, r.accessToken, r.refreshToken, r.expiresInSec,
                              QDateTime::currentSecsSinceEpoch());
}

void AniListTracker::disconnectAccount()
{
    closeLoopback();
    TrackerQueue::clearTokens(Id::AniList);
    // The pending queue is this account's progress; the next account has not agreed to receive it.
    TrackerQueue::forgetAccount(Id::AniList);
    // ...and the consecutive-failure count goes with it: a fresh link is exactly the moment it is worth
    // trying again immediately rather than half an hour from now.
    if (sender_) sender_->reset();
    emit connectedChanged(false);
    emit queueChanged();
}

void AniListTracker::ensureValidToken(std::function<void(bool ok)> done)
{
    if (!isConfigured() || !isConnected()) { if (done) done(false); return; }
    // 0 = unknown expiry (see storeTokenReply). A 60-second skew keeps a token that expires mid-flight from
    // being used for the request it would fail.
    if (TrackerQueue::tokenFresh(Id::AniList, QDateTime::currentSecsSinceEpoch(), 60))
    { if (done) done(true); return; }
    const QString refresh = TrackerQueue::refreshToken(Id::AniList);
    if (refresh.isEmpty()) { if (done) done(false); return; }

    if (!tokenRefresh_.join(std::move(done))) return;   // one refresh in flight; this caller joined it

    QNetworkRequest req{ QUrl(authBase() + QStringLiteral("/token")) };
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    QNetworkReply* rep = nam_->post(req, anilist::tokenRefreshBody(clientId(), clientSecret(), refresh));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const anilist::TokenReply r = anilist::parseTokenReply(rep->readAll());
        if (r.ok) storeTokenReply(r);
        tokenRefresh_.settle(r.ok);
    });
}

// ---- requests ---------------------------------------------------------------------------------------

void AniListTracker::post(const QByteArray& body,
                          std::function<void(bool ok, int status, qint64 retryAfterSec, QByteArray)> cb)
{
    ensureValidToken([this, body, cb](bool ok) {
        if (!ok) { if (cb) cb(false, 0, 0, QByteArray()); return; }
        QNetworkRequest req{ QUrl(apiUrl()) };
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("Accept", "application/json");
        req.setRawHeader("Authorization",
                         "Bearer " + TrackerQueue::accessToken(Id::AniList).toUtf8());
        QNetworkReply* rep = nam_->post(req, body);
        connect(rep, &QNetworkReply::finished, this, [rep, cb] {
            rep->deleteLater();
            const bool netOk = rep->error() == QNetworkReply::NoError;
            // The STATUS and the Retry-After are carried rather than collapsed into ok/fail, because since
            // #326 the retry decision is a decision about WHICH failure this was. `ok` is untouched, so
            // search() and fetchEntry() below read exactly what they always did.
            const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (cb) cb(netOk, status, retryAfterSeconds(rep->rawHeader("Retry-After")), rep->readAll());
        });
    });
}

void AniListTracker::search(const QString& title, int year, Kind kind,
                            std::function<void(QVector<Match>)> cb)
{
    if (title.trimmed().isEmpty() || !isConfigured() || !isConnected())
    { if (cb) cb({}); return; }   // the tracker being off is not a failure — an empty result covers both
    post(anilist::searchBody(title, year, kind), [cb](bool ok, int, qint64, QByteArray body) {
        if (cb) cb(ok ? anilist::parseSearch(body) : QVector<Match>{});
    });
}

void AniListTracker::fetchEntry(const QString& mediaId, Kind, std::function<void(bool, Entry)> cb)
{
    if (mediaId.isEmpty() || !isConfigured() || !isConnected()) { if (cb) cb(false, Entry{}); return; }
    post(anilist::entryBody(mediaId), [cb, mediaId](bool ok, int, qint64, QByteArray body) {
        Entry e;
        const bool parsed = ok && anilist::parseEntry(body, mediaId, e);
        if (cb) cb(parsed, e);
    });
}

// ---- the queue --------------------------------------------------------------------------------------

// The queue plumbing moved to TrackerQueue in increment 2 and is now SHARED with MyAnimeList - the same
// keys, the same rules, one implementation. #326 moved the DRAIN LOOP up beside it, which left the three
// private forwarders here with no callers; these two stayed, because the settings surfaces call them.
int AniListTracker::queuedCount() { return TrackerQueue::count(Id::AniList); }

QString AniListTracker::lastError() { return TrackerQueue::lastError(Id::AniList); }

void AniListTracker::pushProgress(const Update& in)
{
    if (in.mediaId.isEmpty() || in.itemKey.isEmpty()) return;   // no link, no push (issue's rule)
    Update u = in;
    if (u.atMs <= 0) u.atMs = QDateTime::currentMSecsSinceEpoch();
    // an earlier unit arriving late changes nothing and writes nothing
    if (!TrackerQueue::enqueue(Id::AniList, u)) return;
    emit queueChanged();
    drain();
}

void AniListTracker::flushQueue() { drain(); }

// THE LOOP MOVED (#326). What used to be thirty lines here — and thirty near-identical lines in
// MyAnimeListTracker — is TrackerQueue::Sender, built in the constructor out of the four things that really
// are AniList's. The bug this fixes lived in the removed lines: every failure took the `!accepted` branch
// and was retried for ever, so a row AniList would never accept sat at the head of an ordered queue and
// blocked every update behind it. The shared loop drops that row, says which one it was, and carries on.
void AniListTracker::drain() { if (sender_) sender_->drain(); }
