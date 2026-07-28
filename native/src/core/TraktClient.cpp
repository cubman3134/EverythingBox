#include "TraktClient.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "Settings.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QByteArray>
#include <QDate>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>
#include <QDateTime>
#include <QUrl>

namespace {
constexpr const char* kBase = "https://api.trakt.tv";

// The calendar cache lives in the shared portable ini, alongside everything else this app persists.
// Opened here rather than routed through Settings:: because these two keys are not settings — nothing in
// the Settings UI reads or writes them, they are a background fetch's output, and SettingsTxn excludes
// them from the save/discard transaction for exactly that reason (see SettingsTxn::inScope).
//
// A second QSettings on the same path is safe and is the established idiom in this tree (Settings.cpp and
// SettingsTxn.cpp each hold their own): QSettings shares one QConfFile per path process-wide, so a synced
// write through this handle is visible through the others.
constexpr const char* kCacheKey   = "trakt/calendarCache";
constexpr const char* kCacheAtKey = "trakt/calendarCachedAt";

QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QNetworkRequest req(const QString& path, bool auth)
{
    QNetworkRequest r{ QUrl(QString::fromLatin1(kBase) + path) };
    r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    r.setRawHeader("trakt-api-version", "2");
    r.setRawHeader("trakt-api-key", Settings::traktClientId().toUtf8());
    r.setTransferTimeout(20000);
    if (auth) r.setRawHeader("Authorization", QByteArray("Bearer ") + Settings::traktAccessToken().toUtf8());
    return r;
}

// Build the scrobble media object from an IMDB stream id:
//   "tt123"        -> { "movie": { "ids": { "imdb": "tt123" } } }
//   "ttShow:s:e"   -> { "show": { "ids": { "imdb": "ttShow" } }, "episode": { "season": s, "number": e } }
QJsonObject mediaJson(const QString& imdbStreamId)
{
    const QStringList p = imdbStreamId.split(QLatin1Char(':'));
    if (p.size() >= 3)
        return { { QStringLiteral("show"), QJsonObject{ { QStringLiteral("ids"), QJsonObject{ { QStringLiteral("imdb"), p.value(0) } } } } },
                 { QStringLiteral("episode"), QJsonObject{ { QStringLiteral("season"), p.value(1).toInt() },
                                                           { QStringLiteral("number"), p.value(2).toInt() } } } };
    return { { QStringLiteral("movie"), QJsonObject{ { QStringLiteral("ids"), QJsonObject{ { QStringLiteral("imdb"), imdbStreamId } } } } } };
}
} // namespace

TraktClient::TraktClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

bool TraktClient::configured()
{
    return !Settings::traktClientId().isEmpty() && !Settings::traktClientSecret().isEmpty();
}

bool TraktClient::connected() { return !Settings::traktAccessToken().isEmpty(); }

void TraktClient::connectAccount()
{
    if (!configured()) { emit connectError(tr("Enter your Trakt client id and secret first.")); return; }
    const QJsonObject body{ { QStringLiteral("client_id"), Settings::traktClientId() } };
    QNetworkReply* r = nam_->post(req(QStringLiteral("/oauth/device/code"), false),
                                  QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { emit connectError(tr("Couldn't reach Trakt (%1).").arg(r->errorString())); return; }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        const QString code = o.value(QStringLiteral("device_code")).toString();
        const QString userCode = o.value(QStringLiteral("user_code")).toString();
        const QString url = o.value(QStringLiteral("verification_url")).toString();
        pollExpiresIn_ = o.value(QStringLiteral("expires_in")).toInt(600);
        const int interval = o.value(QStringLiteral("interval")).toInt(5);
        if (code.isEmpty() || userCode.isEmpty()) { emit connectError(tr("Trakt didn't return a device code.")); return; }
        emit deviceCode(userCode, url.isEmpty() ? QStringLiteral("https://trakt.tv/activate") : url);
        pollDeviceCode_ = code; pollElapsed_ = 0;
        pollDeviceToken(code, qMax(2, interval));
    });
}

void TraktClient::pollDeviceToken(const QString& deviceCode, int intervalSec)
{
    if (!pollTimer_) { pollTimer_ = new QTimer(this); pollTimer_->setSingleShot(true); }
    pollTimer_->disconnect();
    connect(pollTimer_, &QTimer::timeout, this, [this, deviceCode, intervalSec] {
        pollElapsed_ += intervalSec;
        if (pollElapsed_ > pollExpiresIn_) { emit connectError(tr("Trakt activation timed out — try again.")); return; }
        const QJsonObject body{ { QStringLiteral("code"), deviceCode },
                                { QStringLiteral("client_id"), Settings::traktClientId() },
                                { QStringLiteral("client_secret"), Settings::traktClientSecret() } };
        QNetworkReply* r = nam_->post(req(QStringLiteral("/oauth/device/token"), false),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [this, r, deviceCode, intervalSec] {
            r->deleteLater();
            const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code == 200)
            {
                const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
                const qint64 created = o.value(QStringLiteral("created_at")).toVariant().toLongLong();
                const qint64 exp = (created ? created : QDateTime::currentSecsSinceEpoch())
                                 + o.value(QStringLiteral("expires_in")).toVariant().toLongLong();
                Settings::setTraktTokens(o.value(QStringLiteral("access_token")).toString(),
                                         o.value(QStringLiteral("refresh_token")).toString(), exp);
                emit connectedChanged(true);
                return;
            }
            if (code == 400) { pollDeviceToken(deviceCode, intervalSec); return; } // still pending -> keep polling
            emit connectError(code == 409 ? tr("This code was already used.")
                            : code == 410 ? tr("The code expired — try again.")
                            : code == 418 ? tr("Activation was denied.")
                                          : tr("Trakt activation failed (%1).").arg(code));
        });
    });
    pollTimer_->start(intervalSec * 1000);
}

void TraktClient::disconnectAccount()
{
    Settings::clearTraktTokens();
    if (pollTimer_) pollTimer_->stop();
    emit connectedChanged(false);
}

// Refresh the access token if it has (nearly) expired, then invoke done(ok).
//
// SINGLE-FLIGHT, and not as an optimisation. Trakt ROTATES the refresh token: the reply that hands
// back a new access token also invalidates the refresh token that asked for it. So two callers that
// both reach the POST below before either reply lands present the SAME refresh token, the second
// presents one Trakt has already consumed, and — if the replies interleave — the later
// setTraktTokens() writes the older pair over the newer one. The account link is then permanently
// broken: every later refresh presents a token that no longer exists and the user has to re-link by
// hand. That is precisely the failure SettingsTxn's scope exclusion is written to avoid, and it costs
// far more than the duplicate request that caused it.
//
// It takes a fetch racing a scrobble today, which is rare. It stops being rare the moment two surfaces
// both call fetchMyShowsCalendar at startup, so the guard belongs here, in the gate every path already
// funnels through, rather than in any one caller. `tokenRefresh_` holds the waiters; the first caller
// in issues the one request and settle() fans the result out to all of them exactly once — on failure
// as well as success, since a queue that only drains on success would strand every joiner forever.
// SingleFlight.h documents the drain ordering; probe_trakt §12 pins it.
void TraktClient::ensureValidToken(std::function<void(bool)> done)
{
    if (!connected()) { if (done) done(false); return; }
    if (QDateTime::currentSecsSinceEpoch() < Settings::traktTokenExpiry() - 60) { if (done) done(true); return; }

    // Queued either way; a false return means a refresh is already in flight and this caller has
    // joined it, so there is nothing more to do but wait for that reply.
    if (!tokenRefresh_.join(std::move(done))) return;

    const QJsonObject body{ { QStringLiteral("refresh_token"), Settings::traktRefreshToken() },
                            { QStringLiteral("client_id"), Settings::traktClientId() },
                            { QStringLiteral("client_secret"), Settings::traktClientSecret() },
                            { QStringLiteral("redirect_uri"), QStringLiteral("urn:ietf:wg:oauth:2.0:oob") },
                            { QStringLiteral("grant_type"), QStringLiteral("refresh_token") } };
    QNetworkReply* r = nam_->post(req(QStringLiteral("/oauth/token"), false),
                                  QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError)
        {
            // Status + the QNetworkReply error ORDINAL, and nothing else. Every transport failure
            // reports HTTP 0 — DNS, timeout and a rejected TLS handshake are one indistinguishable
            // line without the ordinal — and an enum ordinal cannot carry a token the way
            // errorString() can.
            emit log(QStringLiteral("trakt: token refresh failed (HTTP %1, net %2)")
                         .arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                         .arg(static_cast<int>(r->error())));
            tokenRefresh_.settle(false);
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        const qint64 exp = QDateTime::currentSecsSinceEpoch() + o.value(QStringLiteral("expires_in")).toVariant().toLongLong();
        Settings::setTraktTokens(o.value(QStringLiteral("access_token")).toString(),
                                 o.value(QStringLiteral("refresh_token")).toString(), exp);
        tokenRefresh_.settle(true);
    });
}

void TraktClient::scrobble(const QString& action, const QString& imdbStreamId, double pct)
{
    if (!configured() || !connected() || imdbStreamId.isEmpty()) return;
    ensureValidToken([this, action, imdbStreamId, pct](bool ok) {
        if (!ok) return;
        QJsonObject body = mediaJson(imdbStreamId);
        body.insert(QStringLiteral("progress"), qBound(0.0, pct, 100.0));
        QNetworkReply* r = nam_->post(req(QStringLiteral("/scrobble/") + action, true),
                                      QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [this, r, action] {
            r->deleteLater();
            emit log(QStringLiteral("trakt: scrobble %1 -> HTTP %2").arg(action)
                         .arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
        });
    });
}

// ---- the read side: the "my shows" calendar (#23) ------------------------------------------------

bool TraktClient::calendarAvailable() { return configured() && connected(); }

void TraktClient::writeCalendarCache(const QVector<CalendarEntry>& entries)
{
    store().setValue(QLatin1String(kCacheKey), trakt::serializeCalendar(entries));
    store().setValue(QLatin1String(kCacheAtKey), QDateTime::currentSecsSinceEpoch());
    store().sync();
}

QVector<CalendarEntry> TraktClient::cachedCalendar()
{
    // No guard needed for an absent key: value() yields an invalid QVariant, toByteArray() an empty
    // one, and deserializeCalendar is total over that — "never cached" and "cache is garbage" are the
    // same empty result to the caller, which is the honest answer in both cases.
    return trakt::deserializeCalendar(store().value(QLatin1String(kCacheKey)).toByteArray());
}

qint64 TraktClient::cachedCalendarAt()
{
    return store().value(QLatin1String(kCacheAtKey)).toLongLong();   // 0 when absent or unparseable
}

void TraktClient::fetchMyShowsCalendar(int daysBack, int daysForward,
                                       std::function<void(bool, QVector<CalendarEntry>)> cb)
{
    // Trakt being off is NOT a failure — it is the default state of an install nobody linked. The
    // callback still fires (ok=false, empty) so a caller can fall back to the cache and finish its
    // layout instead of waiting on a reply that will never come.
    if (!calendarAvailable()) { if (cb) cb(false, {}); return; }

    // Every read goes through the SAME gate the scrobbler uses. Refresh, expiry and Trakt's ROTATED
    // refresh token are handled in exactly one place; a read path that built its own request would
    // duplicate that logic and would eventually get the rotation wrong, which permanently breaks the
    // account link rather than failing one call.
    ensureValidToken([this, daysBack, daysForward, cb](bool ok) {
        if (!ok) { if (cb) cb(false, {}); return; }
        const int back = qMax(0, daysBack);
        const int fwd  = qMax(0, daysForward);
        // The UTC date, not the local one. Every time on this calendar is UTC — Trakt states it, the
        // parser normalises to it, and CalendarEntry::airsAtUtc says so in its name — so a window
        // whose start came from QDate::currentDate() would be expressed in a different clock than the
        // rows it selects. East of UTC near midnight (UTC+13 is the extreme) the local date is already
        // TOMORROW, so `today - back` silently starts the window a day late and drops the oldest day;
        // west of UTC it starts a day early. Whatever bucketing a caller layers on top of these
        // entries must likewise be done in UTC, or a row lands on the wrong day.
        const QString start = QDateTime::currentDateTimeUtc().date().addDays(-back).toString(Qt::ISODate);
        // Trakt's `days` COUNTS the start date, so a window inclusive of both today-back and
        // today+fwd is back + fwd + 1 days, not back + fwd — the latter silently drops the last
        // day forward, which is the one a "what's on this week" surface cares most about.
        const int days = back + fwd + 1;
        QNetworkReply* r = nam_->get(req(QStringLiteral("/calendars/my/shows/") + start
                                         + QStringLiteral("/") + QString::number(days), true));
        connect(r, &QNetworkReply::finished, this, [this, r, cb] {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError)
            {
                // The HTTP status and the QNetworkReply error ORDINAL, and nothing else. No token, no
                // client id, no secret, and not errorString() either — a diagnostic string is exactly
                // where a credential leaks into a log by accident, while an enum ordinal cannot carry
                // one. The status distinguishes "not authorised" from "no network"; the ordinal is
                // what distinguishes the transport failures FROM EACH OTHER, since DNS failure, a
                // timeout and a rejected TLS handshake all report HTTP 0 and this line is the only
                // diagnostic this path has.
                emit log(QStringLiteral("trakt: calendar fetch failed (HTTP %1, net %2)")
                             .arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                             .arg(static_cast<int>(r->error())));
                if (cb) cb(false, {});
                return;
            }
            // An HTTP 200 is NOT on its own evidence that Trakt answered. A captive portal or a
            // corporate proxy happily returns 200 with an HTML sign-in interstitial: no transport
            // error, a tolerant parser that finds no rows in it, and — before this check — a cache
            // overwritten with nothing. The cache exists precisely for the offline launch, i.e. for
            // the network conditions that produce that interstitial, so it would be blanked exactly
            // when it is needed and stay blank until the next successful fetch.
            //
            // The discriminator is whether the body IS a calendar (a JSON array), never whether it
            // has rows: a real empty calendar ("nothing airs this week") must still be able to
            // replace a stale non-empty one, or the user keeps seeing last month's forever. The
            // array test lives in TraktRead with the rest of the wire-format knowledge; probe §11.
            const QByteArray body = r->readAll();
            if (!trakt::looksLikeCalendarPayload(body))
            {
                emit log(QStringLiteral("trakt: calendar reply was not a JSON array (HTTP %1) — cache kept")
                             .arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
                if (cb) cb(false, {});   // ok=false, so a caller falls back to the cache it still has
                return;
            }
            // The parser is tolerant, so a partly-malformed body still yields the entries it could
            // read. Cached on any reply that really was a calendar — including an empty one.
            const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(body);
            writeCalendarCache(e);
            if (cb) cb(true, e);
        });
    });
}

void TraktClient::scrobbleStart(const QString& id, double pct) { scrobble(QStringLiteral("start"), id, pct); }
void TraktClient::scrobblePause(const QString& id, double pct) { scrobble(QStringLiteral("pause"), id, pct); }
void TraktClient::scrobbleStop(const QString& id, double pct)  { scrobble(QStringLiteral("stop"),  id, pct); }
