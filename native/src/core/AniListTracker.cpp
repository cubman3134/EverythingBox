#include "AniListTracker.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "TrackerLinks.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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

// How long to wait before retrying a queued update whose send failed. Deliberately long: a failure here is
// almost always "no network" or "rate limited", and both are answered by waiting, not by trying harder.
static constexpr int kRetryMs = 60000;

AniListTracker::AniListTracker(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, [this] { drain(); });
}

AniListTracker::~AniListTracker() { closeLoopback(); }

// ---- configuration + credentials -------------------------------------------------------------------

QString AniListTracker::clientId()
{
    // THE #81 SEAM. The zero-config follow-up replaces this body with "typed value, else the embedded
    // BuiltinSecrets slot" and touches nothing else in the feature. See tracker::builtinSecretIdSlot().
    return store().value(clientIdKey(Id::AniList)).toString();
}

QString AniListTracker::clientSecret()
{
    return store().value(clientSecretKey(Id::AniList)).toString();
}

void AniListTracker::setClientId(const QString& v)
{
    store().setValue(clientIdKey(Id::AniList), v.trimmed());
    store().sync();
}

void AniListTracker::setClientSecret(const QString& v)
{
    store().setValue(clientSecretKey(Id::AniList), v.trimmed());
    store().sync();
}

bool AniListTracker::isConfigured() { return !clientId().isEmpty() && !clientSecret().isEmpty(); }

bool AniListTracker::isConnected()
{
    return !store().value(accessKey(Id::AniList)).toString().isEmpty();
}

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
            const QByteArray req = sock->readAll();
            const QByteArray line = req.left(req.indexOf('\r'));
            const int sp1 = line.indexOf(' '), sp2 = line.indexOf(' ', sp1 + 1);
            const QString target = QString::fromUtf8(line.mid(sp1 + 1, sp2 - sp1 - 1));
            const QUrlQuery q(QUrl::fromEncoded(("http://localhost" + target.toUtf8())).query());
            const QString err = q.queryItemValue(QStringLiteral("error"));
            const QString code = q.queryItemValue(QStringLiteral("code"));

            // A plain body rather than a redirect to the project website: unlike the Drive flow there is no
            // branded landing page for this, and sending the browser anywhere would hand a third party a
            // request whose Referer names this loopback port. Content-Length is explicit so the browser does
            // not sit waiting on a connection close.
            const QByteArray page = err.isEmpty() && !code.isEmpty()
                ? QByteArray("Signed in. You can close this tab and go back to the app.")
                : QByteArray("Sign-in was not completed. You can close this tab.");
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                        "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n"
                        "Content-Length: " + QByteArray::number(page.size()) + "\r\n\r\n" + page);
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
    store().setValue(accessKey(Id::AniList), r.accessToken);
    // A refresh reply may legitimately omit the refresh token; keeping the old one is correct, blanking it
    // would unlink the account on the next expiry.
    if (!r.refreshToken.isEmpty()) store().setValue(refreshKey(Id::AniList), r.refreshToken);
    // AniList's access tokens are long-lived (a year). A missing expires_in stores 0, and 0 means "assume
    // valid" below rather than "expired": treating an unknown expiry as expired would refresh on every
    // request against a token that is fine.
    store().setValue(expiryKey(Id::AniList),
                     r.expiresInSec > 0 ? QDateTime::currentSecsSinceEpoch() + r.expiresInSec : 0);
    store().sync();
}

void AniListTracker::disconnectAccount()
{
    closeLoopback();
    store().remove(accessKey(Id::AniList));
    store().remove(refreshKey(Id::AniList));
    store().remove(expiryKey(Id::AniList));
    // The pending queue is this account's progress; the next account has not agreed to receive it.
    store().remove(queueKey(ProfileStore::currentId(), Id::AniList));
    store().remove(lastErrorKey(ProfileStore::currentId(), Id::AniList));
    store().sync();
    emit connectedChanged(false);
    emit queueChanged();
}

void AniListTracker::ensureValidToken(std::function<void(bool ok)> done)
{
    if (!isConfigured() || !isConnected()) { if (done) done(false); return; }
    const qint64 expiry = store().value(expiryKey(Id::AniList), 0).toLongLong();
    // 0 = unknown expiry (see storeTokenReply). A 60-second skew keeps a token that expires mid-flight from
    // being used for the request it would fail.
    if (expiry <= 0 || expiry - 60 > QDateTime::currentSecsSinceEpoch()) { if (done) done(true); return; }
    const QString refresh = store().value(refreshKey(Id::AniList)).toString();
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

void AniListTracker::post(const QByteArray& body, std::function<void(bool ok, QByteArray)> cb)
{
    ensureValidToken([this, body, cb](bool ok) {
        if (!ok) { if (cb) cb(false, QByteArray()); return; }
        QNetworkRequest req{ QUrl(apiUrl()) };
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("Accept", "application/json");
        req.setRawHeader("Authorization",
                         "Bearer " + store().value(accessKey(Id::AniList)).toString().toUtf8());
        QNetworkReply* rep = nam_->post(req, body);
        connect(rep, &QNetworkReply::finished, this, [rep, cb] {
            rep->deleteLater();
            const bool netOk = rep->error() == QNetworkReply::NoError;
            if (cb) cb(netOk, rep->readAll());
        });
    });
}

void AniListTracker::search(const QString& title, int year, Kind kind,
                            std::function<void(QVector<Match>)> cb)
{
    if (title.trimmed().isEmpty() || !isConfigured() || !isConnected())
    { if (cb) cb({}); return; }   // the tracker being off is not a failure — an empty result covers both
    post(anilist::searchBody(title, year, kind), [cb](bool ok, QByteArray body) {
        if (cb) cb(ok ? anilist::parseSearch(body) : QVector<Match>{});
    });
}

void AniListTracker::fetchEntry(const QString& mediaId, Kind, std::function<void(bool, Entry)> cb)
{
    if (mediaId.isEmpty() || !isConfigured() || !isConnected()) { if (cb) cb(false, Entry{}); return; }
    post(anilist::entryBody(mediaId), [cb, mediaId](bool ok, QByteArray body) {
        Entry e;
        const bool parsed = ok && anilist::parseEntry(body, mediaId, e);
        if (cb) cb(parsed, e);
    });
}

// ---- the queue --------------------------------------------------------------------------------------

QVector<Update> AniListTracker::loadQueue()
{
    return decodeQueue(store().value(queueKey(ProfileStore::currentId(), Id::AniList)).toString().toUtf8());
}

void AniListTracker::saveQueue(const QVector<Update>& q)
{
    store().setValue(queueKey(ProfileStore::currentId(), Id::AniList),
                     QString::fromUtf8(encodeQueue(q)));
    store().sync();
}

int AniListTracker::queuedCount() { return loadQueue().size(); }

QString AniListTracker::lastError()
{
    return store().value(lastErrorKey(ProfileStore::currentId(), Id::AniList)).toString();
}

void AniListTracker::setLastError(const QString& message)
{
    const QString key = lastErrorKey(ProfileStore::currentId(), Id::AniList);
    if (message.isEmpty()) store().remove(key); else store().setValue(key, message);
    store().sync();
}

void AniListTracker::pushProgress(const Update& in)
{
    if (in.mediaId.isEmpty() || in.itemKey.isEmpty()) return;   // no link, no push (issue's rule)
    Update u = in;
    if (u.atMs <= 0) u.atMs = QDateTime::currentMSecsSinceEpoch();
    QVector<Update> q = loadQueue();
    if (!coalesce(q, u)) return;   // an earlier unit arriving late changes nothing and writes nothing
    applyQueueCap(q);
    saveQueue(q);
    emit queueChanged();
    drain();
}

void AniListTracker::flushQueue() { drain(); }

void AniListTracker::drain()
{
    if (sending_ || !isConfigured() || !isConnected()) return;
    QVector<Update> q = loadQueue();
    if (q.isEmpty()) { retry_->stop(); return; }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString profile = ProfileStore::currentId();
    int idx = -1;
    qint64 soonest = -1;
    for (int i = 0; i < q.size(); ++i)
    {
        const qint64 last = store().value(lastSentKey(profile, Id::AniList, q[i].itemKey), 0).toLongLong();
        if (debounceAllows(last, now)) { idx = i; break; }
        const qint64 waitMs = kDebounceMs - (now - last);
        if (soonest < 0 || waitMs < soonest) soonest = waitMs;
    }
    if (idx < 0)
    {
        // Everything queued is inside its item's debounce window. Wake exactly when the earliest one opens,
        // rather than polling: a binge-reader would otherwise have a timer firing every second.
        retry_->start(int(qBound<qint64>(1000, soonest, kDebounceMs)));
        return;
    }

    const Update u = q[idx];
    // The COMPLETED decision uses the tracker's OWN unit count, captured when the link was made — see
    // anilist::saveBody. Reading it from the link rather than from the app's chapter list is what keeps a
    // partial provider listing from marking a running series finished.
    const int total = TrackerLinks::get(Id::AniList, u.itemKey).totalUnits;
    sending_ = true;
    post(anilist::saveBody(u, total), [this, u](bool ok, QByteArray body) {
        sending_ = false;
        // A GraphQL error arrives as HTTP 200 with an `errors` array and no `data`, so transport success is
        // not acceptance. Anything that is not a SaveMediaListEntry payload leaves the row queued.
        const bool accepted = ok && body.contains("SaveMediaListEntry");
        if (!accepted)
        {
            // A message ABOUT the failure. Never the body, never the request — see the file header.
            setLastError(tr("AniList did not accept the update; it is queued and will be retried."));
            retry_->start(kRetryMs);
            emit queueChanged();
            return;
        }
        QVector<Update> q2 = loadQueue();
        // Remove by IDENTITY, not by index: the queue is re-read here and a page turn during the request may
        // have coalesced a FURTHER update onto this item. Dropping index 0 would then throw that away.
        for (int i = 0; i < q2.size(); ++i)
            if (q2[i].itemKey == u.itemKey && q2[i].mediaId == u.mediaId && q2[i].unit <= u.unit)
            { q2.remove(i); break; }
        saveQueue(q2);
        store().setValue(lastSentKey(ProfileStore::currentId(), Id::AniList, u.itemKey),
                         QDateTime::currentMSecsSinceEpoch());
        store().sync();
        setLastError(QString());
        emit progressPushed(u.itemKey, u.unit);
        emit queueChanged();
        drain();   // keep going; the next item's debounce is checked afresh
    });
}
