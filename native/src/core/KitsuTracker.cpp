#include "KitsuTracker.h"
#include "TrackerLinks.h"
#include "TrackerQueue.h"   // the ONE queue, credential store and drain loop, shared with the other two

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

using namespace tracker;

// The ini access lives in TrackerQueue (#326). This file adds no store of its own — the whole point of
// the increment is that a third provider needed none.

// THE SIGN-IN CREDENTIALS, IN MEMORY ONLY. Statics rather than members because the settings surfaces are
// static-facing (they have no instance to ask) and because there is exactly one sign-in at a time. They
// are cleared by connectAccount() the moment the exchange answers, either way.
//
// Never written to the ini, never logged, never put in a message. probe_tracker §20 scans the ini bytes
// for the fixture password and asserts ZERO occurrences.
static QString& signInEmail()
{
    static QString e;
    return e;
}

static QString& signInPassword()
{
    static QString p;
    return p;
}

KitsuTracker::KitsuTracker(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, [this] { drain(); });

    // THE SHARED DRAIN LOOP (#326) — the same one AniList and MyAnimeList run, with nothing added to it
    // for a third provider. What is supplied here is only what is really Kitsu's: whether the tracker is
    // usable, how one row is written, and three sentences that name it.
    TrackerQueue::SendSpec spec;
    spec.id = Id::Kitsu;
    spec.policy = kitsu::sendPolicy();
    spec.ready = [] { return isConfigured() && isConnected(); };
    spec.send = [this](const Update& u, std::function<void(TrackerQueue::Reply)> done) {
        // TWO ROUND TRIPS, and the first one is what makes the write idempotent: a library entry is its
        // own resource on Kitsu, so we have to know whether the account already has one before we can say
        // whether this is a PATCH or a create. A replayed queue row after a restart finds the entry it
        // made last time and updates it rather than creating a second.
        ensureSelfId([this, u, done](QString userId) {
            if (userId.isEmpty())
            {
                // Could not ask. That is a WAITING problem, not a refusal: status 0 falls through
                // classifySend's default and the row stays queued.
                TrackerQueue::Reply r;
                if (done) done(r);
                return;
            }
            get(kitsu::entryUrl(apiUrl(), userId, u.mediaId, u.kind),
                [this, u, userId, done](int status, qint64 retryAfterSec, QByteArray body) {
                if (status < 200 || status >= 300)
                {
                    // The READ failed. Answer with its own status so the shared classification decides:
                    // a 401 refreshes, a 404 on the read really is a media that is gone, a 5xx waits.
                    TrackerQueue::Reply r;
                    r.status = status;
                    r.retryAfterSec = retryAfterSec;
                    if (done) done(r);
                    return;
                }
                Entry e;
                QString entryId;
                kitsu::parseEntry(body, u.mediaId, u.kind, e, &entryId);
                // THE COMPLETED RULE'S TOTAL. Kitsu's own count when the read carried one, else the count
                // captured when the link was made; the LARGER of the two when both are known, because a
                // bigger total makes "this was the last unit" HARDER to claim, and that is the safe
                // direction — marking a running series finished is the one push a later push cannot undo.
                const int linked = TrackerLinks::get(Id::Kitsu, u.itemKey).totalUnits;
                const int total = qMax(e.totalUnits, linked);
                write(kitsu::saveUrl(apiUrl(), entryId), kitsu::saveMethod(entryId),
                      kitsu::saveBody(u, total, entryId, userId),
                      [done](int st, qint64 ra, QByteArray reply) {
                    Q_UNUSED(reply);   // nothing Kitsu echoes is worth quoting back at the user
                    TrackerQueue::Reply r;
                    // A PATCH answers 200 and a create answers 201; both are inside the 2xx test, and
                    // unlike AniList there is no body-level refusal hiding under a 200 here.
                    r.accepted      = st >= 200 && st < 300;
                    r.status        = st;
                    r.retryAfterSec = ra;
                    if (done) done(r);
                });
            });
        });
    };
    spec.droppedMessage = [](const Update& u) {
        // WHICH update, by the title the link store already holds — no request, and nothing out of a
        // response body.
        const QString title = TrackerLinks::get(Id::Kitsu, u.itemKey).title;
        return title.isEmpty()
            ? tr("Kitsu refused one update and it has been dropped; the rest are still queued.")
            : tr("Kitsu refused the update for %1 and it has been dropped; "
                 "the rest are still queued.").arg(title);
    };
    spec.reauthMessage = tr("Kitsu needs signing in again; updates are queued.");
    // A message ABOUT the failure, never the request — see the file header.
    spec.retryMessage = tr("Kitsu did not accept the update; it is queued and will be retried.");
    spec.pushed = [this](const QString& itemKey, int unit) { emit progressPushed(itemKey, unit); };
    spec.changed = [this] { emit queueChanged(); };
    spec.wait = [this](int delayMs) { if (delayMs > 0) retry_->start(delayMs); else retry_->stop(); };
    sender_ = std::make_unique<TrackerQueue::Sender>(std::move(spec));
}

KitsuTracker::~KitsuTracker() = default;

// ---- configuration + credentials -------------------------------------------------------------------

QString KitsuTracker::email() { return signInEmail(); }

void KitsuTracker::setEmail(const QString& v) { signInEmail() = v.trimmed(); }

// NOT trimmed. Leading or trailing whitespace is legal in a password, and trimming it would sign in with
// something the user did not type.
void KitsuTracker::setPassword(const QString& v) { signInPassword() = v; }

bool KitsuTracker::hasSignInCredentials()
{
    return !signInEmail().isEmpty() && !signInPassword().isEmpty();
}

void KitsuTracker::forgetSignInCredentials()
{
    signInEmail().clear();
    signInPassword().clear();
}

// BOTH ARE THE TOKEN. Kitsu has no client id and no client secret, so there is no "set up but not
// connected" state to report — see the header.
bool KitsuTracker::isConfigured() { return TrackerQueue::hasAccessToken(Id::Kitsu); }

bool KitsuTracker::isConnected() { return TrackerQueue::hasAccessToken(Id::Kitsu); }

QString KitsuTracker::apiUrl()
{
    // The stub hook for a live drive, read per call so a rig can point the app at a fixture with no rebuild.
    return qEnvironmentVariable("EB_KITSU_ENDPOINT", kitsu::defaultApiUrl());
}

QString KitsuTracker::authBase()
{
    return qEnvironmentVariable("EB_KITSU_AUTH", kitsu::defaultAuthBase());
}

// ---- linking ----------------------------------------------------------------------------------------

void KitsuTracker::connectAccount()
{
    if (!hasSignInCredentials())
    {
        emit connectError(tr("Enter your Kitsu email and password first."));
        return;
    }
    QNetworkRequest req{ QUrl(authBase() + QStringLiteral("/token")) };
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Accept", "application/json");
    const QByteArray body = kitsu::passwordGrantBody(signInEmail(), signInPassword());
    // THE CREDENTIALS ARE GONE THE MOMENT THE REQUEST HOLDS THEM. Not on the reply — a request that never
    // answers would otherwise leave a password in memory for the rest of the session.
    forgetSignInCredentials();
    QNetworkReply* rep = nam_->post(req, body);
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const kitsu::TokenReply r = kitsu::parseTokenReply(rep->readAll());
        if (!r.ok)
        {
            // A SENTENCE OF OUR OWN. rep->errorString() embeds the URL, and this URL is the token
            // endpoint — one edit away from carrying the grant. The HTTP status is the whole of what we
            // say about it, and 401 is said plainly because "wrong password" is the answer the user needs.
            const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 400 || status == 401)
                emit connectError(tr("Kitsu did not accept that email and password."));
            else
                emit connectError(status > 0
                    ? tr("Kitsu did not return a token (HTTP %1).").arg(status)
                    : tr("Kitsu did not return a token; the connection failed."));
            return;
        }
        storeTokenReply(r);
        selfId_.clear();   // a new account: the cached user id belonged to the old one
        emit connectedChanged(true);
        flushQueue();      // an account linked after an offline session delivers what was queued
    });
}

void KitsuTracker::storeTokenReply(const kitsu::TokenReply& r)
{
    if (!r.ok) return;   // never write an unsuccessful reply over live tokens — see TrackerRules
    // Kitsu's access tokens last about a month and it issues a fresh refresh token with each grant. The
    // omitted-refresh-token guard is TrackerQueue's, for all three trackers.
    TrackerQueue::storeTokens(Id::Kitsu, r.accessToken, r.refreshToken, r.expiresInSec,
                              QDateTime::currentSecsSinceEpoch());
}

void KitsuTracker::disconnectAccount()
{
    forgetSignInCredentials();
    selfId_.clear();
    TrackerQueue::clearTokens(Id::Kitsu);
    TrackerQueue::forgetAccount(Id::Kitsu);
    // ...and the consecutive-failure count goes with it: a fresh link is exactly the moment it is worth
    // trying again immediately rather than half an hour from now.
    if (sender_) sender_->reset();
    emit connectedChanged(false);
    emit queueChanged();
}

void KitsuTracker::ensureValidToken(std::function<void(bool ok)> done)
{
    if (!isConnected()) { if (done) done(false); return; }
    if (TrackerQueue::tokenFresh(Id::Kitsu, QDateTime::currentSecsSinceEpoch(), 60))
    { if (done) done(true); return; }
    const QString refresh = TrackerQueue::refreshToken(Id::Kitsu);
    if (refresh.isEmpty()) { if (done) done(false); return; }

    // SINGLE-FLIGHT, and on Kitsu it matters for MAL's reason: it rotates the refresh token, so two
    // overlapping refreshes race to invalidate each other's and can break the link permanently.
    if (!tokenRefresh_.join(std::move(done))) return;

    QNetworkRequest req{ QUrl(authBase() + QStringLiteral("/token")) };
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Accept", "application/json");
    QNetworkReply* rep = nam_->post(req, kitsu::tokenRefreshBody(refresh));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const kitsu::TokenReply r = kitsu::parseTokenReply(rep->readAll());
        if (r.ok) storeTokenReply(r);
        tokenRefresh_.settle(r.ok);
    });
}

void KitsuTracker::ensureSelfId(std::function<void(QString)> done)
{
    if (!selfId_.isEmpty()) { if (done) done(selfId_); return; }
    if (!isConnected()) { if (done) done(QString()); return; }
    // SINGLE-FLIGHT again: a drain and a fetchEntry starting together would otherwise both ask, and the
    // second answer would overwrite the first for no benefit. The waiter takes a bool and reads selfId_
    // afterwards, so the id itself does not have to travel through SingleFlight's signature.
    if (!selfLookup_.join([this, done](bool) { if (done) done(selfId_); })) return;
    get(kitsu::selfUrl(apiUrl()), [this](int status, qint64, QByteArray body) {
        if (status >= 200 && status < 300) selfId_ = kitsu::parseSelfId(body);
        selfLookup_.settle(!selfId_.isEmpty());
    });
}

// ---- requests ---------------------------------------------------------------------------------------

// The status a reply really carried. 0 means "there was no HTTP answer at all" — a dead socket, a DNS
// failure, a cancelled request — which the backoff treats as a waiting problem rather than a verdict.
static int kitsuStatusOf(QNetworkReply* rep)
{
    return rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// Retry-After, in seconds. The PARSING is tracker::retryAfterSeconds (#326) — all three trackers read the
// delta-seconds form and none of them parses the HTTP-date one, so there is one reader rather than three.
static qint64 kitsuRetryAfterOf(QNetworkReply* rep)
{
    return retryAfterSeconds(rep->rawHeader("Retry-After"));
}

void KitsuTracker::get(const QString& url, std::function<void(int, qint64, QByteArray)> cb)
{
    if (url.isEmpty()) { if (cb) cb(0, 0, QByteArray()); return; }
    ensureValidToken([this, url, cb](bool ok) {
        if (!ok) { if (cb) cb(0, 0, QByteArray()); return; }
        QNetworkRequest req{ QUrl(url) };
        // JSON:API's own media type. Kitsu answers a plain application/json Accept, but sending the right
        // one is what keeps a future content negotiation from silently changing the shape we parse.
        req.setRawHeader("Accept", "application/vnd.api+json");
        req.setRawHeader("Authorization", "Bearer " + TrackerQueue::accessToken(Id::Kitsu).toUtf8());
        QNetworkReply* rep = nam_->get(req);
        connect(rep, &QNetworkReply::finished, this, [rep, cb] {
            rep->deleteLater();
            if (cb) cb(kitsuStatusOf(rep), kitsuRetryAfterOf(rep), rep->readAll());
        });
    });
}

void KitsuTracker::write(const QString& url, const QByteArray& verb, const QByteArray& body,
                         std::function<void(int, qint64, QByteArray)> cb)
{
    if (url.isEmpty() || verb.isEmpty()) { if (cb) cb(0, 0, QByteArray()); return; }
    ensureValidToken([this, url, verb, body, cb](bool ok) {
        if (!ok) { if (cb) cb(0, 0, QByteArray()); return; }
        QNetworkRequest req{ QUrl(url) };
        // JSON:API REQUIRES this exact content type on a write; Kitsu answers 415 to anything else.
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/vnd.api+json"));
        req.setRawHeader("Accept", "application/vnd.api+json");
        req.setRawHeader("Authorization", "Bearer " + TrackerQueue::accessToken(Id::Kitsu).toUtf8());
        // PATCH and POST both go through sendCustomRequest: QNetworkAccessManager has no PATCH
        // convenience, and using one call for both keeps the two paths from drifting on headers.
        QNetworkReply* rep = nam_->sendCustomRequest(req, verb, body);
        connect(rep, &QNetworkReply::finished, this, [rep, cb] {
            rep->deleteLater();
            if (cb) cb(kitsuStatusOf(rep), kitsuRetryAfterOf(rep), rep->readAll());
        });
    });
}

void KitsuTracker::search(const QString& title, int year, Kind kind,
                          std::function<void(QVector<Match>)> cb)
{
    if (!isConnected()) { if (cb) cb({}); return; }
    // Shorter than Kitsu will answer usefully, so we do not ask. The tracker being off and a query too
    // short to be meaningful are both an EMPTY result, never an error.
    const QString url = kitsu::searchUrl(apiUrl(), title, year, kind, 8);
    if (url.isEmpty()) { if (cb) cb({}); return; }
    const QString query = title.trimmed();
    get(url, [cb, kind, query](int status, qint64, QByteArray body) {
        if (status < 200 || status >= 300) { if (cb) cb({}); return; }
        // RANKED, and the noise dropped — the MAL treatment, for the same reason: filter[text] is a fuzzy
        // full-text search that answers a title it does not have with loosely-related rows, and a
        // controller user scrolling that list is one press from linking the wrong series.
        if (cb) cb(rankMatches(query, kitsu::parseSearch(body, kind)));
    });
}

void KitsuTracker::fetchEntry(const QString& mediaId, Kind kind,
                              std::function<void(bool, Entry)> cb)
{
    if (mediaId.isEmpty() || !isConnected()) { if (cb) cb(false, Entry{}); return; }
    ensureSelfId([this, mediaId, kind, cb](QString userId) {
        if (userId.isEmpty()) { if (cb) cb(false, Entry{}); return; }
        get(kitsu::entryUrl(apiUrl(), userId, mediaId, kind),
            [cb, mediaId, kind](int status, qint64, QByteArray body) {
            Entry e;
            const bool parsed = status >= 200 && status < 300
                             && kitsu::parseEntry(body, mediaId, kind, e, nullptr);
            if (cb) cb(parsed, e);
        });
    });
}

// ---- the queue --------------------------------------------------------------------------------------

int KitsuTracker::queuedCount() { return TrackerQueue::count(Id::Kitsu); }

QString KitsuTracker::lastError() { return TrackerQueue::lastError(Id::Kitsu); }

void KitsuTracker::pushProgress(const Update& in)
{
    if (in.mediaId.isEmpty() || in.itemKey.isEmpty()) return;   // no link, no push (issue's rule)
    Update u = in;
    if (u.atMs <= 0) u.atMs = QDateTime::currentMSecsSinceEpoch();
    // THE SHARED QUEUE, keyed by Id::Kitsu — so a chapter this account refused stays pending here and not
    // on AniList's or MyAnimeList's.
    if (!TrackerQueue::enqueue(Id::Kitsu, u)) return;
    emit queueChanged();
    drain();
}

void KitsuTracker::flushQueue() { drain(); }

void KitsuTracker::drain() { if (sender_) sender_->drain(); }
