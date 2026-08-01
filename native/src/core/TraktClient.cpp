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

// The watchlist/collection caches, and the backfill's progress cursor (#23). Same posture as the two
// keys above, and for the same three reasons: they are written from a reply lambda rather than from a
// settings row (so SettingsTxn excludes them), nothing in the Settings UI reads them, and — the one
// that is new here — they are DEVICE-LOCAL, so CloudSync::isDeviceLocalKey excludes them from the
// Drive bundle. That last exclusion is deliberate on both counts. The caches would flip the bundle's
// stateHash on every refresh and re-upload the whole zip for data the other device can fetch in one
// request; and the WATERMARK is a statement about what THIS install has already imported, so syncing
// it would let one device's completed run suppress another's first one.
constexpr const char* kWatchlistKey  = "trakt/watchlistCache";
constexpr const char* kCollectionKey = "trakt/collectionCache";
constexpr const char* kListsAtKey    = "trakt/listsCachedAt";
constexpr const char* kBackfillThroughKey = "trakt/backfillThrough";
constexpr const char* kBackfillDoneKey    = "trakt/backfillDone";

// How many rows to ask for per page. Trakt only emits its pagination headers when a page size is
// requested, so asking for one is what makes the loop's paging visible at all; 100 is Trakt's own
// documented maximum, which is the fewest requests a large list can be read in.
constexpr int kPageLimit = 100;

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
                // Same guard as the refresh leg, same reason: an HTTP 200 that is not a token reply
                // would otherwise store an empty pair and announce a link that does not exist
                // (connected() reads the access token, so it would immediately disagree with the
                // signal). Attended rather than unattended, so the user sees a real error instead.
                const trakt::TokenReply t = trakt::parseTokenReply(r->readAll());
                if (!t.valid) { emit connectError(tr("Trakt didn't return a usable token — try again.")); return; }
                // created_at is Trakt's own issue time and is the better base HERE: the device flow
                // has been polling for up to ten minutes, so "now" can be well after the token was
                // minted. Absent -> fall back to the local clock.
                const qint64 exp = (t.createdAtUnix ? t.createdAtUnix : QDateTime::currentSecsSinceEpoch())
                                 + t.expiresInSec;
                Settings::setTraktTokens(t.accessToken, t.refreshToken, exp);
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
    // The cached calendar belongs to the ACCOUNT, not to the install, so unlinking must take it with
    // the tokens. Leaving it behind means the next account to link inherits the previous one's shows:
    // every surface reads the cache directly, so until a fresh fetch lands the user is looking at
    // somebody else's calendar — and MainWindow's 15-minute refresh cooldown can swallow exactly the
    // fetch that would have corrected it. (That cooldown is reset on the same signal, in
    // MainWindow's connectedChanged handler; both halves are needed — see the comment there.)
    //
    // Cleared BEFORE the signal, because the handler's onTraktCalendarChanged() re-reads the store.
    clearCalendarCache();
    // The same rule, for the same reason, for everything the second read slice keeps. The lists are the
    // previous account's library; the WATERMARK is the sharper hazard — left behind, the next account's
    // very first backfill would skip every watch older than the previous account's newest one, silently
    // importing a fraction of the history and reporting that it was complete.
    clearListCaches();
    clearBackfillState();
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
        // A NoError reply is not on its own evidence that TRAKT answered — the same fact the calendar
        // fetch learned below. A TLS-intercepting proxy, a captive-portal interstitial, or a 200 that
        // simply omits the token all arrive here with no transport error, and the previous code parsed
        // any of them to an empty object and wrote setTraktTokens("", "", exp).
        //
        // That is the worst write in the app. It blanks the access token AND the refresh token, and
        // the refresh token is the only credential that can mint another one: the account is unlinked
        // permanently and the user must re-link by hand. The calendar cache is guarded against exactly
        // this class of body (looksLikeCalendarPayload) and its loss was repaired by the next fetch;
        // this one is not repairable at all, so it gets the stronger guard, not the weaker.
        //
        // It matters more on this branch than it did before it: the 30-minute top-up timer and the
        // startup fetch now drive refreshes UNATTENDED, at every expiry, on whatever network the box
        // happens to be on — which is precisely the population of networks that produce these bodies.
        const trakt::TokenReply t = trakt::parseTokenReply(r->readAll());
        if (!t.valid)
        {
            // No token, no id, no secret, and no body excerpt either — a proxy interstitial is
            // attacker-influenced text and this line goes to a log the user can paste anywhere.
            emit log(QStringLiteral("trakt: token refresh reply was not a token (HTTP %1) — tokens kept")
                         .arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
            tokenRefresh_.settle(false);   // stored tokens UNTOUCHED; the next attempt can still work
            return;
        }
        // An ABSENT refresh token PRESERVES the stored one rather than rejecting the reply. Trakt
        // rotates refresh tokens, so "" must never be written — but the two ways not to write it are
        // not equal. Rejecting would also throw away a good access token, leaving the link just as
        // broken as it would be otherwise; preserving keeps a working session now, and if Trakt did
        // rotate, the stale refresh token simply fails the NEXT refresh — a recoverable ok=false on a
        // path that already handles it. Preserving therefore never loses and sometimes wins.
        const QString refresh = t.refreshToken.isEmpty() ? Settings::traktRefreshToken() : t.refreshToken;
        // now + expires_in, not created_at + expires_in: created_at is Trakt's clock and this expiry
        // is compared against ours, so basing it on the local clock keeps a skewed box honest.
        Settings::setTraktTokens(t.accessToken, refresh,
                                 QDateTime::currentSecsSinceEpoch() + t.expiresInSec);
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

void TraktClient::clearCalendarCache()
{
    store().remove(QLatin1String(kCacheKey));
    store().remove(QLatin1String(kCacheAtKey));
    store().sync();
    // Both keys, together. cachedCalendarAt() is what a surface uses to decide whether to LABEL the
    // calendar as stale, so a timestamp left behind without its entries would describe a calendar
    // that no longer exists.
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

// ---- the read side, part two: the lists and the watched-history backfill (#23) --------------------

namespace {
// Qt hands back header names in whatever case the server used, and HTTP says they are
// case-insensitive; TraktSync's readers take a lowercased map, so the normalisation happens here, at
// the boundary, once. A pure function cannot do it for itself and a probe cannot assert it — which is
// exactly why it is one line in one place rather than a lookup repeated per header.
QMap<QString, QString> lowercasedHeaders(QNetworkReply* r)
{
    QMap<QString, QString> h;
    const auto pairs = r->rawHeaderPairs();
    for (const auto& p : pairs)
        h.insert(QString::fromLatin1(p.first).trimmed().toLower(), QString::fromLatin1(p.second));
    return h;
}
} // namespace

void TraktClient::clearListCaches()
{
    store().remove(QLatin1String(kWatchlistKey));
    store().remove(QLatin1String(kCollectionKey));
    store().remove(QLatin1String(kListsAtKey));
    store().sync();
}

void TraktClient::clearBackfillState()
{
    store().remove(QLatin1String(kBackfillThroughKey));
    store().remove(QLatin1String(kBackfillDoneKey));
    store().sync();
}

QVector<TraktListEntry> TraktClient::cachedWatchlist()
{ return trakt::deserializeList(store().value(QLatin1String(kWatchlistKey)).toByteArray()); }

QVector<TraktListEntry> TraktClient::cachedCollection()
{ return trakt::deserializeList(store().value(QLatin1String(kCollectionKey)).toByteArray()); }

qint64 TraktClient::cachedListsAt()
{ return store().value(QLatin1String(kListsAtKey)).toLongLong(); }

bool TraktClient::backfillEverCompleted()
{ return store().value(QLatin1String(kBackfillDoneKey), false).toBool(); }

void TraktClient::fetchPage(std::shared_ptr<PagedRun> run,
                            std::function<void(std::shared_ptr<PagedRun>)> done)
{
    // The outright page bound, checked before the request rather than after it: kMaxPages is there to
    // cap the number of REQUESTS a hostile or buggy page_count can cost, which a check on the way out
    // would not do.
    if (run->page > trakt::kMaxPages)
    {
        run->stopReason = tr("Stopped after %1 pages.").arg(trakt::kMaxPages);
        if (done) done(run);
        return;
    }
    const QString url = run->path + QStringLiteral("?page=") + QString::number(run->page)
                      + QStringLiteral("&limit=") + QString::number(kPageLimit);
    QNetworkReply* r = nam_->get(req(url, true));
    connect(r, &QNetworkReply::finished, this, [this, r, run, done] {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QMap<QString, QString> headers = lowercasedHeaders(r);
        const QByteArray body = r->readAll();
        // A transport error has no HTTP status to classify, so it is reported as status 0 — which
        // classifyPage reads as "no response at all" and treats as retryable.
        const int effective = (r->error() != QNetworkReply::NoError && status == 0) ? 0 : status;
        const trakt::PageVerdict v = trakt::classifyPage(effective, headers, body);

        switch (v.outcome)
        {
        case trakt::PageOutcome::Ok:
            break;
        case trakt::PageOutcome::Retryable:
            if (trakt::shouldRetryAttempt(run->attempt))
            {
                const int wait = trakt::backoffSecFor(run->attempt, v.retryAfterSec);
                ++run->attempt;
                // The status and the retry, never the body: a 429's body and a captive portal's are
                // both places a token could be echoed back at us, and this line is a log.
                emit log(QStringLiteral("trakt: page %1 of %2 deferred (HTTP %3), retrying in %4s")
                             .arg(run->page).arg(run->path).arg(effective).arg(wait));
                QTimer::singleShot(wait * 1000, this, [this, run, done] { fetchPage(run, done); });
                return;
            }
            run->stopReason = tr("Trakt is rate-limiting or unavailable (HTTP %1).").arg(effective);
            if (done) done(run);
            return;
        case trakt::PageOutcome::AuthFailed:
            // The token gate's business, not the retry loop's. Hammering an expired or revoked token
            // cannot help, and on Trakt it is how an app gets its client id throttled.
            run->stopReason = tr("Trakt refused the request — the account may need re-linking.");
            if (done) done(run);
            return;
        case trakt::PageOutcome::Malformed:
            // HTTP 200 carrying something that is not the payload: a captive portal, a proxy's error
            // page. The transport SUCCEEDED, so a retry returns the same page; stop and say so.
            run->stopReason = tr("The reply was not a Trakt list (HTTP %1).").arg(effective);
            if (done) done(run);
            return;
        case trakt::PageOutcome::Fatal:
            run->stopReason = tr("Trakt rejected the request (HTTP %1).").arg(effective);
            if (done) done(run);
            return;
        }

        run->bodies.push_back(body);
        ++run->pagesFetched;
        const trakt::PageInfo info = trakt::parsePageInfo(headers);
        if (info.pageCount > 0) run->pagesExpected = info.pageCount;
        // nextPageAfter is handed the page WE ASKED FOR, never the one the server echoed — see
        // TraktSync.h. 0 means the run is done.
        const int next = trakt::nextPageAfter(info, run->page);
        if (next <= 0) { run->complete = true; if (done) done(run); return; }
        run->page = next;
        run->attempt = 1;             // a fresh page gets its own attempt budget
        fetchPage(run, done);
    });
}

void TraktClient::fetchAllPages(const QString& path, std::function<void(std::shared_ptr<PagedRun>)> done)
{
    auto run = std::make_shared<PagedRun>();
    run->path = path;
    if (!calendarAvailable())
    {
        run->stopReason = tr("Trakt is not connected.");
        if (done) done(run);
        return;
    }
    // Every read goes through the SAME token gate the scrobbler and the calendar use, for the reason
    // spelled out on ensureValidToken: Trakt rotates its refresh token, and a second place that
    // refreshed would eventually break the link rather than fail one call.
    ensureValidToken([this, run, done](bool ok) {
        if (!ok)
        {
            run->stopReason = tr("Could not refresh the Trakt token.");
            if (done) done(run);
            return;
        }
        fetchPage(run, done);
    });
}

void TraktClient::fetchListInto(const QString& path, const char* cacheKey,
                                std::function<void(bool, QVector<TraktListEntry>)> cb)
{
    fetchAllPages(path, [this, path, cacheKey, cb](std::shared_ptr<PagedRun> run) {
        QVector<TraktListEntry> all;
        for (const QByteArray& body : run->bodies) all += trakt::parseListPayload(body);

        // The cache is written ONLY by a complete run. A watchlist missing its second page, cached and
        // then drawn as if it were the whole list, is worse than a stale one: every later launch shows
        // the truncated version and nothing about it looks wrong. A failed run therefore leaves
        // whatever was there — the same rule the calendar fetch follows on a non-calendar reply.
        if (!run->complete)
        {
            emit log(QStringLiteral("trakt: %1 incomplete after %2 page(s) — cache kept (%3)")
                         .arg(path).arg(run->pagesFetched).arg(run->stopReason));
            if (cb) cb(false, {});
            return;
        }
        store().setValue(QLatin1String(cacheKey), trakt::serializeList(all));
        store().setValue(QLatin1String(kListsAtKey), QDateTime::currentSecsSinceEpoch());
        store().sync();
        if (cb) cb(true, all);
    });
}

void TraktClient::fetchWatchlist(std::function<void(bool, QVector<TraktListEntry>)> cb)
{
    // /sync/watchlist without a type returns every kind at once — movies, shows, seasons, episodes —
    // and parseListPayload keeps the two this app has a tile for. Asking for all of them in one
    // request rather than two costs the user nothing and halves the rate-limit budget this spends.
    fetchListInto(QStringLiteral("/sync/watchlist"), kWatchlistKey, cb);
}

void TraktClient::fetchCollection(std::function<void(bool, QVector<TraktListEntry>)> cb)
{
    // /sync/collection, by contrast, REQUIRES a type, so the movies and the shows are two runs. They
    // are chained rather than issued together: two concurrent runs would race on the one cache key,
    // and the second to land would overwrite the first with only its own half.
    fetchAllPages(QStringLiteral("/sync/collection/movies"),
                  [this, cb](std::shared_ptr<PagedRun> movies) {
        if (!movies->complete)
        {
            emit log(QStringLiteral("trakt: collection (movies) incomplete — cache kept (%1)")
                         .arg(movies->stopReason));
            if (cb) cb(false, {});
            return;
        }
        QVector<TraktListEntry> all;
        for (const QByteArray& b : movies->bodies) all += trakt::parseListPayload(b);
        fetchAllPages(QStringLiteral("/sync/collection/shows"),
                      [this, cb, all](std::shared_ptr<PagedRun> shows) mutable {
            if (!shows->complete)
            {
                // HALF a collection is not a collection. The movies really did arrive, but caching
                // them alone would silently drop every show the user owns, and the folder would look
                // complete. Keep the previous cache and report the failure.
                emit log(QStringLiteral("trakt: collection (shows) incomplete — cache kept (%1)")
                             .arg(shows->stopReason));
                if (cb) cb(false, {});
                return;
            }
            for (const QByteArray& b : shows->bodies) all += trakt::parseListPayload(b);
            store().setValue(QLatin1String(kCollectionKey), trakt::serializeList(all));
            store().setValue(QLatin1String(kListsAtKey), QDateTime::currentSecsSinceEpoch());
            store().sync();
            if (cb) cb(true, all);
        });
    });
}

void TraktClient::runWatchedBackfill(std::function<trakt::LocalState(const QString&)> localState,
                                     std::function<void(const QString&)> markWatched,
                                     std::function<void(BackfillReport)> cb)
{
    // Both halves of the history, chained for the same reason the collection's are: the plan has to be
    // computed over the WHOLE observed set, because the watermark it produces is the maximum over that
    // set. Planning the movies alone and then the shows would advance the watermark twice, and the
    // second advance would be over a set that had already been filtered by the first.
    fetchAllPages(QStringLiteral("/sync/watched/movies"),
                  [this, localState, markWatched, cb](std::shared_ptr<PagedRun> movies) {
        fetchAllPages(QStringLiteral("/sync/watched/shows"),
                      [this, localState, markWatched, cb, movies](std::shared_ptr<PagedRun> shows) {
            BackfillReport rep;
            trakt::WatchedParse w;
            for (const QByteArray& b : movies->bodies)
            {
                const trakt::WatchedParse p = trakt::parseWatchedPayload(b);
                w.marks += p.marks;
                w.droppedNoKey += p.droppedNoKey;
                w.droppedNoTimestamp += p.droppedNoTimestamp;
            }
            for (const QByteArray& b : shows->bodies)
            {
                const trakt::WatchedParse p = trakt::parseWatchedPayload(b);
                w.marks += p.marks;
                w.droppedNoKey += p.droppedNoKey;
                w.droppedNoTimestamp += p.droppedNoTimestamp;
            }

            const qint64 watermark = store().value(QLatin1String(kBackfillThroughKey), 0).toLongLong();
            trakt::BackfillPlan plan = trakt::planWatchedBackfill(w.marks, watermark, localState);
            // The CALLER'S verdict, which the planner cannot reach: both halves had to be read in full.
            plan.complete = movies->complete && shows->complete;

            // Applied whether or not the run was complete. Every write is Unmarked -> Watched and
            // therefore idempotent, so there is nothing to roll back and no reason to throw away work
            // that is already correct — what a partial run must NOT do is advance the watermark.
            if (markWatched) for (const QString& id : plan.toMark) markWatched(id);

            if (plan.complete && plan.newWatermark > watermark)
            {
                store().setValue(QLatin1String(kBackfillThroughKey), plan.newWatermark);
                store().setValue(QLatin1String(kBackfillDoneKey), true);
                store().sync();
            }
            else if (plan.complete)
            {
                // A complete run that observed nothing NEWER still completed. Recording that matters:
                // it is what stops the surface offering "import your history" for ever.
                store().setValue(QLatin1String(kBackfillDoneKey), true);
                store().sync();
            }

            rep.complete           = plan.complete;
            rep.marked             = int(plan.toMark.size());
            rep.alreadyWatched     = plan.alreadyWatched;
            rep.keptLocal          = plan.keptLocal;
            rep.skippedByWatermark = plan.skippedByWatermark;
            rep.droppedNoKey       = w.droppedNoKey;
            rep.droppedNoTimestamp = w.droppedNoTimestamp;
            if (!plan.complete)
                rep.stopReason = movies->complete ? shows->stopReason : movies->stopReason;
            if (cb) cb(rep);
        });
    });
}

void TraktClient::scrobbleStart(const QString& id, double pct) { scrobble(QStringLiteral("start"), id, pct); }
void TraktClient::scrobblePause(const QString& id, double pct) { scrobble(QStringLiteral("pause"), id, pct); }
void TraktClient::scrobbleStop(const QString& id, double pct)  { scrobble(QStringLiteral("stop"),  id, pct); }
