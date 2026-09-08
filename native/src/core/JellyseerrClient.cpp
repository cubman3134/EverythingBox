#include "JellyseerrClient.h"

#include "Jellyfin.h"           // the qualified-id minter, for the "In your library" deep link
#include "JellyfinServerStore.h"
#include "Jellyseerr.h"
#include "JellyseerrStore.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

// Transport failures, classified from the ENUM. Never from errorString(), which embeds the url — and every
// url in this file was reached with the API key in its headers. JellyseerrClient.h has the rule.
requests::Failure transportFailure(QNetworkReply::NetworkError e)
{
    switch (e)
    {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::SslHandshakeFailedError:
        return requests::Failure::Unreachable;
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return requests::Failure::TimedOut;
    case QNetworkReply::AuthenticationRequiredError:
        return requests::Failure::Unauthorized;
    case QNetworkReply::ContentAccessDenied:
        return requests::Failure::Forbidden;
    case QNetworkReply::ContentNotFoundError:
        return requests::Failure::NotFound;
    default:
        return requests::Failure::Unreachable;
    }
}

// THE ONE PLACE THE KEY IS SPELLED INTO A REQUEST in this file. It goes straight into the header and is
// never returned, logged or put into a message.
void applyKey(QNetworkRequest& req, const QString& apiKey)
{
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader(jellyseerr::apiKeyHeader(), apiKey.toUtf8());
    // A redirect to another host would carry the key to a service the user never configured.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::SameOriginRedirectPolicy));
}

// Arm a deadline on one reply. abort() makes the reply finish with OperationCanceledError, so there is
// exactly one completion path however it ends — which is what lets every callback below be called once.
QTimer* armDeadline(QNetworkReply* reply, int budgetMs)
{
    auto* t = new QTimer(reply);
    t->setSingleShot(true);
    QObject::connect(t, &QTimer::timeout, reply, [reply] { reply->abort(); });
    t->start(budgetMs > 0 ? budgetMs : 15000);
    return t;
}

int httpStatus(QNetworkReply* r)
{
    return r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

RequestLookup lookupFailure(requests::Failure f)
{
    RequestLookup out;
    out.ok = false;
    out.availability = requests::Availability::Unknown;   // NEVER a default "pending" — see Requests.h
    out.failure = f;
    out.message = requests::failureSentence(f);
    return out;
}

RequestAck ackFailure(requests::Failure f)
{
    RequestAck out;
    out.ok = false;
    out.availability = requests::Availability::Unknown;
    out.failure = f;
    out.message = requests::failureSentence(f);
    return out;
}

} // namespace

JellyseerrClient::JellyseerrClient(QObject* parent) : QObject(parent) {}

JellyseerrClient& JellyseerrClient::instance()
{
    static JellyseerrClient inst;
    return inst;
}

QNetworkAccessManager* JellyseerrClient::nam()
{
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    return nam_;
}

bool JellyseerrClient::configured() const { return JellyseerrStore::isConfigured(); }

void JellyseerrClient::verify(const QString& url, const QString& apiKey, bool allowPlainHttp, int budgetMs,
                              VerifyDone done)
{
    const QString root = jellyseerr::normalizeRoot(url, allowPlainHttp);
    if (root.isEmpty() || apiKey.isEmpty())
    {
        if (done) done(false, requests::failureSentence(requests::Failure::Malformed));
        return;
    }
    QNetworkRequest req{ QUrl(root + jellyseerr::statusPath()) };
    applyKey(req, apiKey);
    QNetworkReply* reply = nam()->get(req);
    QTimer* deadline = armDeadline(reply, budgetMs);
    QPointer<JellyseerrClient> self(this);
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, deadline, done, self] {
        deadline->stop();
        reply->deleteLater();
        if (!self) return;
        const int status = httpStatus(reply);
        if (reply->error() != QNetworkReply::NoError && status == 0)
        {
            if (done) done(false, requests::failureSentence(transportFailure(reply->error())));
            return;
        }
        const QByteArray body = reply->readAll();
        if (status >= 400)
        {
            if (done) done(false, requests::failureSentence(jellyseerr::failureForHttp(status, body)));
            return;
        }
        // A 200 that is not JSON is something else answering on that address — a router's login page, a
        // reverse proxy's placeholder. Reported as "not a request service" rather than accepted.
        if (!body.trimmed().startsWith('{'))
        {
            if (done) done(false, requests::failureSentence(requests::Failure::Malformed));
            return;
        }
        if (done) done(true, QString());
    });
}

void JellyseerrClient::lookup(const requests::MediaRef& ref, int budgetMs,
                              std::function<void(const RequestLookup&)> cb)
{
    if (!cb) return;
    const JellyseerrConfig cfg = JellyseerrStore::get();
    if (!cfg.configured()) { cb(lookupFailure(requests::Failure::NotConfigured)); return; }
    const QString root = jellyseerr::normalizeRoot(cfg.url, cfg.allowPlainHttp);
    if (root.isEmpty() || !ref.ok()) { cb(lookupFailure(requests::Failure::Malformed)); return; }

    // A TMDB reference needs no bridge — the title's own endpoint is addressed directly.
    if (ref.kind == requests::IdKind::Tmdb)
    {
        fetchMediaStatus(root, cfg.apiKey, ref.mediaType, ref.tmdb, budgetMs, cb);
        return;
    }

    // An IMDB reference goes through /search, which is where this service resolves an external id. Half the
    // budget each, so a slow first leg cannot leave the second with nothing.
    const int leg = (budgetMs > 0 ? budgetMs : 15000) / 2;
    QUrl u(root + jellyseerr::searchPath());
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("query"), ref.imdb);
    u.setQuery(q);
    QNetworkRequest req{ u };
    applyKey(req, cfg.apiKey);
    QNetworkReply* reply = nam()->get(req);
    QTimer* deadline = armDeadline(reply, leg);
    QPointer<JellyseerrClient> self(this);
    const QString apiKey = cfg.apiKey;
    const QString mediaType = ref.mediaType;
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, deadline, cb, self, root, apiKey, mediaType, leg] {
        deadline->stop();
        reply->deleteLater();
        if (!self) return;
        const int status = httpStatus(reply);
        if (reply->error() != QNetworkReply::NoError && status == 0)
        { cb(lookupFailure(transportFailure(reply->error()))); return; }
        const QByteArray body = reply->readAll();
        if (status >= 400) { cb(lookupFailure(jellyseerr::failureForHttp(status, body))); return; }
        const jellyseerr::SearchHit hit = jellyseerr::readSearchHit(body, mediaType);
        // NO MATCH IS NOT A MALFORMED ANSWER. The service read the id and has nothing under it, which is a
        // fact the user can act on ("it does not know this title"), unlike "something odd came back".
        if (!hit.ok) { cb(lookupFailure(requests::Failure::NotFound)); return; }
        fetchMediaStatus(root, apiKey, hit.mediaType, hit.tmdbId, leg, cb);
    });
}

void JellyseerrClient::fetchMediaStatus(const QString& root, const QString& apiKey,
                                        const QString& mediaType, const QString& tmdbId, int budgetMs,
                                        std::function<void(const RequestLookup&)> cb)
{
    const QString path = jellyseerr::mediaPath(mediaType, tmdbId);
    if (path.isEmpty()) { cb(lookupFailure(requests::Failure::Malformed)); return; }
    QNetworkRequest req{ QUrl(root + path) };
    applyKey(req, apiKey);
    QNetworkReply* reply = nam()->get(req);
    QTimer* deadline = armDeadline(reply, budgetMs);
    QPointer<JellyseerrClient> self(this);
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [reply, deadline, cb, self, mediaType, tmdbId] {
        deadline->stop();
        reply->deleteLater();
        if (!self) return;
        const int status = httpStatus(reply);
        if (reply->error() != QNetworkReply::NoError && status == 0)
        { cb(lookupFailure(transportFailure(reply->error()))); return; }
        const QByteArray body = reply->readAll();
        if (status >= 400) { cb(lookupFailure(jellyseerr::failureForHttp(status, body))); return; }
        const jellyseerr::MediaStatus ms = jellyseerr::readMediaStatus(body, mediaType);
        if (!ms.ok) { cb(lookupFailure(requests::Failure::Malformed)); return; }

        RequestLookup out;
        out.ok = true;
        out.availability = ms.availability;
        out.resolvedId = ms.tmdbId.isEmpty() ? tmdbId : ms.tmdbId;
        out.availableSeasons = ms.availableSeasons;
        out.seasonCount = ms.seasonCount;
        // The deep link behind "In your library". requests::libraryRefFor refuses to guess which of several
        // Jellyfin servers a bare item id belongs to — an unqualified id is the corruption #160 exists to
        // prevent, and that rule does not weaken because a link would be convenient.
        out.libraryRef = requests::libraryRefFor(ms.serverItemId, JellyfinServerStore::ids());
        cb(out);
    });
}

void JellyseerrClient::submit(const RequestSubmission& sub, int budgetMs,
                              std::function<void(const RequestAck&)> cb)
{
    if (!cb) return;
    const JellyseerrConfig cfg = JellyseerrStore::get();
    if (!cfg.configured()) { cb(ackFailure(requests::Failure::NotConfigured)); return; }
    const QString root = jellyseerr::normalizeRoot(cfg.url, cfg.allowPlainHttp);
    if (root.isEmpty()) { cb(ackFailure(requests::Failure::Malformed)); return; }

    // THE TMDB ID MUST ALREADY BE IN HAND. This verb does not resolve, does not search and does not fall
    // back: a submission is the one call that costs somebody bandwidth, and a resolve step inside it would
    // be a second way for a press to end up asking for a title nobody chose. The lookup that armed the
    // button carried the id here.
    const QString tmdbId = !sub.resolvedId.isEmpty() ? sub.resolvedId : sub.ref.tmdb;
    const QByteArray body = jellyseerr::requestBody(sub.ref.mediaType, tmdbId, sub.seasons);
    if (body.isEmpty()) { cb(ackFailure(requests::Failure::Malformed)); return; }

    QNetworkRequest req{ QUrl(root + jellyseerr::requestPath()) };
    applyKey(req, cfg.apiKey);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = nam()->post(req, body);
    QTimer* deadline = armDeadline(reply, budgetMs);
    QPointer<JellyseerrClient> self(this);
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, deadline, cb, self] {
        deadline->stop();
        reply->deleteLater();
        if (!self) return;
        const int status = httpStatus(reply);
        if (reply->error() != QNetworkReply::NoError && status == 0)
        {
            // NOTHING IS RETRIED HERE. A timed-out POST may or may not have reached the service, and a
            // silent retry is how one press becomes two fetches on somebody else's disk. The user is told
            // and decides.
            cb(ackFailure(transportFailure(reply->error())));
            return;
        }
        const QByteArray rbody = reply->readAll();
        if (status >= 400) { cb(ackFailure(jellyseerr::failureForHttp(status, rbody))); return; }
        const jellyseerr::Ack ack = jellyseerr::readRequestAck(rbody);
        // A 2xx WE CANNOT READ IS NOT A SUCCESS. Reporting it as one would leave a row on the shelf that
        // refreshes to "unknown" for ever, for a request that may not exist.
        if (!ack.ok) { cb(ackFailure(requests::Failure::Malformed)); return; }
        RequestAck out;
        out.ok = true;
        out.availability = ack.availability;
        cb(out);
    });
}

// ---- The chooser -----------------------------------------------------------------------------------------
// The ONE place that knows which implementations exist. See RequestBackend.h: the UI asks for "the backend"
// and never names a service, which is what lets EverythingBoxServer#16 be added here and nowhere else.

namespace {
#ifdef EB_REQUESTS_TEST_SEAM
RequestBackend* g_testBackend = nullptr;
#endif
}

RequestBackend* requests::configuredBackend()
{
#ifdef EB_REQUESTS_TEST_SEAM
    if (g_testBackend) return g_testBackend;
#endif
    JellyseerrClient& c = JellyseerrClient::instance();
    return c.configured() ? &c : nullptr;
}

#ifdef EB_REQUESTS_TEST_SEAM
void requests::setBackendForTesting(RequestBackend* backend) { g_testBackend = backend; }
#endif
