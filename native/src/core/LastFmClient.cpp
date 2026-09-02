#include "LastFmClient.h"
#include "AppBrand.h"
#include "BuiltinSecretBlob.h"
#include "Settings.h"
#include "BuiltinSecrets.h" // generated into the BUILD TREE by cmake/GenerateSecrets.cmake

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

constexpr const char* kServiceRoot = "https://ws.audioscrobbler.com/2.0/";
constexpr const char* kAuthPage    = "https://www.last.fm/api/auth/";

// The authorisation poll. Five seconds is Last.fm's own suggested interval for the desktop flow; the total
// is bounded so an abandoned attempt (the user closed the browser tab and walked away) stops talking to the
// service on its own rather than for the rest of the session. The request token expires after an hour
// anyway, so a longer poll would only be asking a question with a known answer.
constexpr int kAuthPollSec      = 5;
constexpr int kAuthPollAttempts = 60;      // 60 x 5s == five minutes

// Where the client is pointed. Empty means the real service; see setApiRootForTests for why the only other
// thing it may ever hold is a loopback address.
QString g_testRoot;

// ==================================================================================================
// THE ONE PLACE A FAILURE BECOMES WORDS — the rule ListenBrainzClient.cpp states at length
// ==================================================================================================
// Everything the user is ever shown about a failed call comes from here. The inputs are Last.fm's own
// `message` field, the HTTP status, and Qt's transport error string. NEVER the request: a Last.fm request
// body carries api_key, api_sig AND the session key, so "the request that failed" is three credentials in a
// string that then travels into a status line, a screenshot and a pasted log.
QString failureMessage(QNetworkReply* reply, int httpStatus, const QJsonObject& body)
{
    const QString said = body.value(QStringLiteral("message")).toString().trimmed();
    if (!said.isEmpty()) return said;
    if (httpStatus > 0) return QObject::tr("Last.fm answered %1.").arg(httpStatus);
    return reply ? reply->errorString() : QObject::tr("Could not reach Last.fm.");
}

int statusOf(QNetworkReply* r)
{
    return r ? r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
}

// Last.fm's error code, or 0 when the body carried none. `error` is a NUMBER in the JSON responses and a
// string attribute in the XML ones; only JSON is ever requested here, but toVariant().toInt() reads both
// rather than depending on which the service felt like sending.
int lastFmErrorOf(const QJsonObject& body)
{
    const QJsonValue v = body.value(QStringLiteral("error"));
    return v.isUndefined() ? 0 : v.toVariant().toInt();
}

// The track parameters, with the suffix a batched call needs ("" for the single-track calls, "[3]" for the
// fourth entry of a scrobble batch). One builder, so the now-playing hint and a queued listen can never
// describe the same track two different ways.
void addTrackParams(QMap<QString, QString>& p, const Scrobble::Track& t, const QString& suffix)
{
    p.insert(QStringLiteral("artist") + suffix, t.artist.trimmed());
    p.insert(QStringLiteral("track") + suffix, t.title.trimmed());
    if (!t.album.isEmpty())       p.insert(QStringLiteral("album") + suffix, t.album);
    if (!t.albumArtist.isEmpty() && t.albumArtist != t.artist)
        p.insert(QStringLiteral("albumArtist") + suffix, t.albumArtist);
    if (t.trackNumber > 0)        p.insert(QStringLiteral("trackNumber") + suffix,
                                           QString::number(t.trackNumber));
    // SECONDS, not milliseconds — the opposite of ListenBrainz's additional_info.duration_ms, and the kind
    // of difference that is invisible until a history is full of four-hour songs.
    if (t.durationSec > 0)        p.insert(QStringLiteral("duration") + suffix,
                                           QString::number(t.durationSec));
}

} // namespace

// ==================================================================================================
// SIGNING
// ==================================================================================================
QString LastFm::signatureBase(const QMap<QString, QString>& params)
{
    // QMap iterates in ascending key order, and for these ASCII names that IS the spec's alphabetical order —
    // including the indices, where a plain string sort puts artist[10] between artist[1] and artist[2]. That
    // is what the service does; "fixing" it numerically produces a signature it rejects.
    QString base;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
    {
        // `format` and `api_sig` are never in this map by construction — see the request builder. Asserting
        // it here as well would be a second place to keep in step; the single insertion point is the rule.
        base += it.key();
        base += it.value();
    }
    return base;
}

QString LastFm::signature(const QMap<QString, QString>& params, const QString& secret)
{
    const QByteArray in = (signatureBase(params) + secret).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(in, QCryptographicHash::Md5).toHex());
}

ScrobbleResult::Outcome LastFm::outcomeFor(int httpStatus, int lastFmError)
{
    // THE ERROR CODE COMES FIRST, and this ordering is the whole point of the function. Last.fm answers most
    // failures with HTTP 200 and an error object in the body; a client that reads only the status treats a
    // refused session key as a success, drops the listens off the queue and loses them silently.
    switch (lastFmError)
    {
        case 0: break;
        case 4:     // authentication failed
        case 9:     // invalid session key — re-authenticate
        case 14:    // unauthorised token
        case 15:    // token has expired
            // KEEP the listens. The user can link the account again and they still land, backdated.
            return ScrobbleResult::Outcome::Auth;
        case 8:     // operation failed — a transient service-side problem
        case 11:    // service offline
        case 16:    // temporarily unavailable
        case 29:    // rate limit exceeded
            return ScrobbleResult::Outcome::Retryable;
        default:
            // 2, 3, 5, 6, 7, 10, 13, 26, 27 and anything new: the call is wrong and will stay wrong. DROP,
            // or the queue jams for ever behind one bad row and everything after it is lost too.
            return ScrobbleResult::Outcome::Rejected;
    }

    if (httpStatus <= 0)                       return ScrobbleResult::Outcome::Retryable; // no reply at all
    if (httpStatus >= 200 && httpStatus < 300) return ScrobbleResult::Outcome::Ok;
    if (httpStatus == 401 || httpStatus == 403) return ScrobbleResult::Outcome::Auth;
    if (httpStatus == 429 || httpStatus >= 500) return ScrobbleResult::Outcome::Retryable;
    return ScrobbleResult::Outcome::Rejected;
}

// ==================================================================================================
// THE APPLICATION IDENTITY
// ==================================================================================================
QString LastFmClient::appKey()
{
    return BuiltinSecret::join(eb_secrets::kLastFm_Key_A, eb_secrets::kLastFm_Key_ALen,
                               eb_secrets::kLastFm_Key_B, eb_secrets::kLastFm_Key_BLen);
}

QString LastFmClient::appSecret()
{
    return BuiltinSecret::join(eb_secrets::kLastFm_Secret_A, eb_secrets::kLastFm_Secret_ALen,
                               eb_secrets::kLastFm_Secret_B, eb_secrets::kLastFm_Secret_BLen);
}

bool LastFmClient::availableInThisBuild()
{
    // BOTH halves. A key with no secret cannot sign auth.getToken, so a half-filled slot would offer a
    // connect button that fails at the first request with "Invalid method signature" — which reads, to
    // everyone who sees it, as the app being broken rather than as the build having no credential.
    return !appKey().isEmpty() && !appSecret().isEmpty();
}

void LastFmClient::setApiRootForTests(const QString& root)
{
    if (root.isEmpty()) { g_testRoot = QString(); return; }
    const QUrl u(root);
    // LOOPBACK ONLY, enforced rather than documented. This setter decides where an application key and a
    // user's session key are sent; a test hook that could name any host would be a way to exfiltrate both
    // from a build that shipped with it. http rather than https because the fake is an in-process QTcpServer.
    if (u.scheme() != QLatin1String("http")) return;
    if (u.host() != QLatin1String("127.0.0.1") && u.host() != QLatin1String("localhost")) return;
    QString r = root;
    while (r.endsWith(QLatin1Char('/'))) r.chop(1);
    g_testRoot = r;
}

QString LastFmClient::apiRoot()
{
    if (g_testRoot.isEmpty()) return QString::fromLatin1(kServiceRoot);
    return g_testRoot + QStringLiteral("/2.0/");
}

QString LastFmClient::authPageRoot()
{
    if (g_testRoot.isEmpty()) return QString::fromLatin1(kAuthPage);
    return g_testRoot + QStringLiteral("/api/auth/");
}

// ==================================================================================================
// THE LINK
// ==================================================================================================
bool LastFmClient::connected() { return !Settings::lastFmSessionKey().trimmed().isEmpty(); }
QString LastFmClient::accountName() { return Settings::lastFmAccount(); }

bool LastFmClient::configured() const { return availableInThisBuild() && connected(); }

QString LastFmClient::statusFor(bool available, bool linked, const QString& user)
{
    // ONE sentence, and the order of the arms is the order of the things that can be wrong. "Not available in
    // this build" comes first because nothing the user can do changes it, and offering them a connect button
    // underneath a credential that does not exist is the failure this arm is for.
    if (!available) return tr("Last.fm is not available in this build.");
    if (!linked)    return tr("Not connected to Last.fm.");
    if (user.trimmed().isEmpty()) return tr("Connected to Last.fm.");
    return tr("Connected to Last.fm as %1.").arg(user.trimmed());
}

QString LastFmClient::connectActionLabel(bool available, bool linked)
{
    return (available && linked) ? tr("Disconnect from Last.fm") : tr("Connect to Last.fm");
}

QString LastFmClient::connectActionLabel()
{
    return connectActionLabel(availableInThisBuild(), connected());
}

QString LastFmClient::statusText()
{
    return statusFor(availableInThisBuild(), connected(), accountName());
}

LastFmClient::LastFmClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    authPoll_ = new QTimer(this);
    authPoll_->setSingleShot(true);
}

LastFmClient::~LastFmClient() = default;

// Build a signed request. The signature is computed over `params` EXACTLY as given; `format` is added
// afterwards and api_sig is added last, because neither is part of the signed set (see the header).
static QByteArray signedForm(const QMap<QString, QString>& params, const QString& secret)
{
    const QString sig = LastFm::signature(params, secret);
    QByteArray out;
    // Encoded by hand rather than through QUrlQuery: QUrlQuery stores what it is given and re-encodes on the
    // way out, so handing it already-encoded pairs double-encodes every `[` in a batch's parameter names and
    // every space in a title — and the signature, computed over the RAW values, then no longer matches what
    // was sent. One pass, at the last possible moment, is the only way those two can agree.
    const auto add = [&out](const QString& k, const QString& v) {
        if (!out.isEmpty()) out += '&';
        out += QUrl::toPercentEncoding(k);
        out += '=';
        out += QUrl::toPercentEncoding(v);
    };
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) add(it.key(), it.value());
    // AFTER the signature, and never inside it: api_sig is obviously not part of what it signs, and `format`
    // is a response-encoding hint that Last.fm excludes — signing it fails every call with "Invalid method
    // signature", which names nothing and sends you looking at the key. See the header.
    add(QStringLiteral("api_sig"), sig);
    add(QStringLiteral("format"), QStringLiteral("json"));
    return out;
}

static QNetworkRequest lfmRequest(const QUrl& url, bool form)
{
    QNetworkRequest r{ url };
    r.setRawHeader("User-Agent", QByteArray(AppBrand::kUserAgent));
    if (form) r.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
    return r;
}

// Read a reply into (status, error code, parsed body). The body is read ONCE — a QNetworkReply's buffer is
// consumed, and a second readAll() elsewhere would see an empty document and report "no error".
static QJsonObject readBody(QNetworkReply* r, int& status, int& lfmError)
{
    status = statusOf(r);
    const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
    lfmError = lastFmErrorOf(o);
    return o;
}

void LastFmClient::stopAuth()
{
    if (authPoll_) authPoll_->stop();
}

void LastFmClient::connectAccount()
{
    stopAuth();   // a second press cancels the first attempt rather than running two polls at once
    if (!availableInThisBuild())
    {
        emit connectError(tr("Last.fm is not available in this build."));
        return;
    }

    // STEP 1 — auth.getToken. Signed, even though no user is involved yet: the signature is what identifies
    // the application, and it is why this flow cannot be driven from a credential the user supplies.
    QMap<QString, QString> p;
    p.insert(QStringLiteral("method"), QStringLiteral("auth.getToken"));
    p.insert(QStringLiteral("api_key"), appKey());

    QUrl url(apiRoot());
    url.setQuery(QString::fromUtf8(signedForm(p, appSecret())));
    QNetworkReply* r = nam_->get(lfmRequest(url, false));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        int status = 0, err = 0;
        const QJsonObject body = readBody(r, status, err);
        const QString token = body.value(QStringLiteral("token")).toString();
        const QString why = failureMessage(r, status, body);
        r->deleteLater();

        if (token.isEmpty()) { emit connectError(why); return; }

        // STEP 2 — the user approves it in a browser. The request token is a one-use handle, not a
        // credential, and it is deliberately NOT stored: it is spent by step 3 and useless afterwards.
        QUrl page(authPageRoot());
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("api_key"), appKey());
        q.addQueryItem(QStringLiteral("token"), token);
        page.setQuery(q);
        emit authUrl(page.toString());

        // STEP 3 — poll until they have. There is no callback in this flow; Last.fm answers error 14 until
        // the approval lands. Same shape as TraktClient's device poll, and bounded for the same reason.
        pollSession(token, kAuthPollAttempts);
    });
}

void LastFmClient::pollSession(const QString& requestToken, int attemptsLeft)
{
    if (attemptsLeft <= 0)
    {
        emit connectError(tr("Last.fm was not authorised in time. Try connecting again."));
        return;
    }

    QMap<QString, QString> p;
    p.insert(QStringLiteral("method"), QStringLiteral("auth.getSession"));
    p.insert(QStringLiteral("api_key"), appKey());
    p.insert(QStringLiteral("token"), requestToken);

    QUrl url(apiRoot());
    url.setQuery(QString::fromUtf8(signedForm(p, appSecret())));
    QNetworkReply* r = nam_->get(lfmRequest(url, false));
    connect(r, &QNetworkReply::finished, this, [this, r, requestToken, attemptsLeft] {
        int status = 0, err = 0;
        const QJsonObject body = readBody(r, status, err);
        const QJsonObject session = body.value(QStringLiteral("session")).toObject();
        const QString key  = session.value(QStringLiteral("key")).toString();
        const QString user = session.value(QStringLiteral("name")).toString();
        const QString why  = failureMessage(r, status, body);
        r->deleteLater();

        if (!key.isEmpty())
        {
            // THE ONLY THING STORED. Not the request token (spent), not a password (there is none in this
            // flow at all), and the username beside it purely so the row can say who is linked.
            Settings::setLastFmSessionKey(key);
            Settings::setLastFmAccount(user);
            emit connectedChanged(true);
            return;
        }

        // 14 == "unauthorised token": the user simply has not pressed Yes yet. Keep waiting. Anything else
        // is a real answer and the attempt ends — retrying an expired token (15) forever would never come
        // right, and the user would be watching a spinner that means nothing.
        if (err == 14)
        {
            // Through the member timer, not QTimer::singleShot, so stopAuth() can actually CANCEL it. A
            // singleShot fires whatever happens, and a second Connect press would then leave two polls
            // racing each other against the same account.
            authPoll_->disconnect();
            connect(authPoll_, &QTimer::timeout, this,
                    [this, requestToken, attemptsLeft] { pollSession(requestToken, attemptsLeft - 1); });
            authPoll_->start(kAuthPollSec * 1000);
            return;
        }
        emit connectError(why);
    });
}

void LastFmClient::disconnectAccount()
{
    stopAuth();
    Settings::setLastFmSessionKey(QString());
    Settings::setLastFmAccount(QString());
    // The QUEUE IS LEFT ALONE on purpose. Listens waiting for Last.fm keep waiting under their own provider
    // id, with their own timestamps; linking the account again delivers them, backdated. Throwing them away
    // because somebody pressed Disconnect to fix something else is not recoverable.
    emit connectedChanged(false);
}

// ==================================================================================================
// THE THREE VERBS
// ==================================================================================================
void LastFmClient::nowPlaying(const Scrobble::Track& track)
{
    if (!configured()) return;
    // The 30-second rule applies here too: announcing a track Last.fm will refuse to scrobble is a request
    // whose only possible outcome is an error nobody reads (this call does not read its reply).
    if (!LastFm::longEnough(track.durationSec)) return;

    QMap<QString, QString> p;
    p.insert(QStringLiteral("method"), QStringLiteral("track.updateNowPlaying"));
    p.insert(QStringLiteral("api_key"), appKey());
    p.insert(QStringLiteral("sk"), Settings::lastFmSessionKey().trimmed());
    addTrackParams(p, track, QString());

    QNetworkReply* r = nam_->post(lfmRequest(QUrl(apiRoot()), true), signedForm(p, appSecret()));
    // Fire and forget, and deleteLater is the ONLY connection: nothing inspects the status, nothing records
    // an error, nothing retries. See ScrobbleProvider.h — a retry queue for this is a bug, not robustness.
    connect(r, &QNetworkReply::finished, r, &QNetworkReply::deleteLater);
}

void LastFmClient::submit(const QVector<Scrobble::Play>& plays,
                          std::function<void(ScrobbleResult)> cb)
{
    if (plays.isEmpty()) { if (cb) cb(ScrobbleResult::ok()); return; }
    if (!availableInThisBuild())
    {
        // Not the user's fault and not fixable by them, so this is Auth (hold, stop pumping), never Rejected.
        // A build that gains a key later delivers everything that was waiting.
        if (cb) cb(ScrobbleResult::auth(tr("Last.fm is not available in this build.")));
        return;
    }
    if (!connected())
    {
        if (cb) cb(ScrobbleResult::auth(tr("Connect your Last.fm account to start scrobbling there.")));
        return;
    }

    // LAST.FM'S OWN ACCEPT RULE, applied HERE and nowhere else. Scrobble.h says in as many words why it is
    // not in the shared rules: ListenBrainz has no such rule, and a 25-second track this app refused to send
    // ListenBrainz would be a listen lost to a restriction that service never had.
    QVector<Scrobble::Play> sending;
    for (const Scrobble::Play& p : plays)
        if (LastFm::longEnough(p.track.durationSec)) sending.push_back(p);

    if (sending.isEmpty())
    {
        // REJECTED, so the orchestrator drops them. Keeping a batch Last.fm will never accept at the head of
        // a FIFO silences every listen behind it for ever, which is strictly worse than losing the batch —
        // and the message says which rule it was, rather than leaving a queue that never moves.
        if (cb) cb(ScrobbleResult::rejected(tr("Last.fm does not accept tracks of 30 seconds or less.")));
        return;
    }

    QMap<QString, QString> p;
    p.insert(QStringLiteral("method"), QStringLiteral("track.scrobble"));
    p.insert(QStringLiteral("api_key"), appKey());
    p.insert(QStringLiteral("sk"), Settings::lastFmSessionKey().trimmed());
    const int n = qMin(int(sending.size()), LastFm::kMaxBatch);
    for (int i = 0; i < n; ++i)
    {
        const QString sfx = QStringLiteral("[") + QString::number(i) + QStringLiteral("]");
        addTrackParams(p, sending[i].track, sfx);
        // THE BACKDATE. The moment the track STARTED, captured then and carried through the offline queue
        // untouched — the whole reason a flight's worth of listening lands in the right order at the right
        // times instead of as one burst when the network came back.
        p.insert(QStringLiteral("timestamp") + sfx, QString::number(sending[i].listenedAt));
    }

    QNetworkReply* r = nam_->post(lfmRequest(QUrl(apiRoot()), true), signedForm(p, appSecret()));
    connect(r, &QNetworkReply::finished, this, [r, cb] {
        int status = 0, err = 0;
        const QJsonObject body = readBody(r, status, err);
        ScrobbleResult res;
        res.outcome = LastFm::outcomeFor(status, err);
        if (res.outcome != ScrobbleResult::Outcome::Ok) res.message = failureMessage(r, status, body);
        r->deleteLater();
        if (cb) cb(res);
    });
}

void LastFmClient::love(const Scrobble::Track& track, bool loved,
                        std::function<void(ScrobbleResult)> cb)
{
    if (!configured())
    {
        if (cb) cb(ScrobbleResult::auth(availableInThisBuild()
                                            ? tr("Connect your Last.fm account first.")
                                            : tr("Last.fm is not available in this build.")));
        return;
    }
    if (track.artist.trimmed().isEmpty() || track.title.trimmed().isEmpty())
    {
        if (cb) cb(ScrobbleResult::rejected(tr("That track has no artist and title to send.")));
        return;
    }

    // ONE CALL, unlike ListenBrainz's two: Last.fm's love is keyed on the artist/title pair the listener
    // actually has, with no MusicBrainz recording to resolve first. That difference is exactly why
    // ScrobbleProvider::supportsLove exists per provider rather than as one shared code path.
    QMap<QString, QString> p;
    p.insert(QStringLiteral("method"), loved ? QStringLiteral("track.love") : QStringLiteral("track.unlove"));
    p.insert(QStringLiteral("api_key"), appKey());
    p.insert(QStringLiteral("sk"), Settings::lastFmSessionKey().trimmed());
    p.insert(QStringLiteral("artist"), track.artist.trimmed());
    p.insert(QStringLiteral("track"), track.title.trimmed());

    QNetworkReply* r = nam_->post(lfmRequest(QUrl(apiRoot()), true), signedForm(p, appSecret()));
    connect(r, &QNetworkReply::finished, this, [r, cb] {
        int status = 0, err = 0;
        const QJsonObject body = readBody(r, status, err);
        ScrobbleResult res;
        res.outcome = LastFm::outcomeFor(status, err);
        if (res.outcome != ScrobbleResult::Outcome::Ok) res.message = failureMessage(r, status, body);
        r->deleteLater();
        if (cb) cb(res);
    });
}
