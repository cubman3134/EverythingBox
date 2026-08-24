#include "ListenBrainzClient.h"
#include "AppBrand.h"
#include "Settings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

namespace {

constexpr const char* kPublicRoot = "https://api.listenbrainz.org";

// ==================================================================================================
// THE ONE PLACE A FAILURE BECOMES WORDS
// ==================================================================================================
// Everything the user is ever shown about a failed submission comes from here, and nothing else in this file
// builds a message. That is the whole guarantee that the token cannot leak into one: the inputs are the HTTP
// status, Qt's own transport error string, and the SERVICE's `error` field — never the request, never a URL
// with a query on it, never a header.
//
// The reason that matters is specific rather than general. The obvious diagnostic to write when a scrobble
// fails is "POST <url> with <headers> failed" — and that string contains `Authorization: Token <the user's
// secret>`. It then goes into a status line, a notification, a screenshot in a bug report, and a log file that
// gets pasted into an issue. There is no later stage that can take it back out.
QString failureMessage(QNetworkReply* reply, int httpStatus, const QByteArray& body)
{
    // The service's own explanation, when it gave one. ListenBrainz answers errors as {"code":…,"error":"…"}.
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QString said = o.value(QStringLiteral("error")).toString().trimmed();
    if (!said.isEmpty()) return said;
    if (httpStatus > 0) return QObject::tr("The service answered %1.").arg(httpStatus);
    // A transport failure: Qt's message describes the SOCKET (host not found, timed out), not the request.
    return reply ? reply->errorString() : QObject::tr("Could not reach the service.");
}

ScrobbleResult::Outcome outcomeFor(int httpStatus, QNetworkReply::NetworkError err)
{
    if (httpStatus <= 0)                    return ScrobbleResult::Outcome::Retryable;  // no reply at all
    if (httpStatus >= 200 && httpStatus < 300) return ScrobbleResult::Outcome::Ok;
    if (httpStatus == 401 || httpStatus == 403) return ScrobbleResult::Outcome::Auth;
    if (httpStatus == 429 || httpStatus >= 500) return ScrobbleResult::Outcome::Retryable;
    (void)err;
    // Everything else in the 4xx band is the service saying this payload is wrong and will stay wrong. The
    // listens are DROPPED rather than kept: a queue that keeps a permanently-rejected batch at its head never
    // delivers anything behind it again, which turns one malformed row into total silence.
    return ScrobbleResult::Outcome::Rejected;
}

int statusOf(QNetworkReply* r)
{
    return r ? r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
}

// track_metadata, as both the playing_now hint and a completed listen carry it.
QJsonObject metadataFor(const Scrobble::Track& t)
{
    QJsonObject m;
    m.insert(QStringLiteral("artist_name"), t.artist);
    m.insert(QStringLiteral("track_name"), t.title);
    if (!t.album.isEmpty()) m.insert(QStringLiteral("release_name"), t.album);

    QJsonObject extra;
    // Named so a user reading their own listen history can see where it came from — and so a duplicate caused
    // by a server ALSO forwarding the same play is attributable rather than mysterious. See Scrobble::Origin.
    extra.insert(QStringLiteral("media_player"), QString::fromLatin1(AppBrand::kDisplayName));
    extra.insert(QStringLiteral("submission_client"), QString::fromLatin1(AppBrand::kDisplayName));
    if (t.trackNumber > 0) extra.insert(QStringLiteral("tracknumber"), t.trackNumber);
    if (t.durationSec > 0) extra.insert(QStringLiteral("duration_ms"), t.durationSec * 1000);
    if (!t.albumArtist.isEmpty() && t.albumArtist != t.artist)
        extra.insert(QStringLiteral("release_artist_name"), t.albumArtist);
    m.insert(QStringLiteral("additional_info"), extra);
    return m;
}

} // namespace

ListenBrainzClient::ListenBrainzClient(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

QString ListenBrainzClient::defaultApiRoot() { return QString::fromLatin1(kPublicRoot); }

QString ListenBrainzClient::apiRoot()
{
    QString custom = Settings::listenBrainzApiUrl().trimmed();
    while (custom.endsWith(QLatin1Char('/'))) custom.chop(1);
    if (custom.isEmpty()) return defaultApiRoot();
    const QUrl u(custom);
    // A root that is not a usable http(s) URL falls back to the DEFAULT rather than being sent as typed. It
    // does NOT silently disable the feature: a half-typed URL is a mistake to correct, and the settings
    // surface shows which root is in force.
    if (!u.isValid() || u.host().isEmpty()
        || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https")))
        return defaultApiRoot();
    return custom;
}

bool ListenBrainzClient::configured() const
{
    return !Settings::listenBrainzToken().trimmed().isEmpty();
}

// Build a request against the configured root. The token is read HERE, at the moment of use, and goes nowhere
// else — not into a member, not into a returned string, not into any diagnostic.
static QNetworkRequest lbRequest(const QString& url, bool json)
{
    QNetworkRequest r{ QUrl(url) };
    r.setRawHeader("User-Agent", QByteArray(AppBrand::kUserAgent));
    if (json) r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QString token = Settings::listenBrainzToken().trimmed();
    if (!token.isEmpty()) r.setRawHeader("Authorization", QByteArray("Token ") + token.toUtf8());
    return r;
}

void ListenBrainzClient::nowPlaying(const Scrobble::Track& track)
{
    if (!configured()) return;

    QJsonObject listen;
    // NO listened_at. The field is forbidden for playing_now and its presence is a 400 — which would be
    // invisible here, because this call deliberately does not read its reply.
    listen.insert(QStringLiteral("track_metadata"), metadataFor(track));
    QJsonObject body;
    body.insert(QStringLiteral("listen_type"), QStringLiteral("playing_now"));
    body.insert(QStringLiteral("payload"), QJsonArray{ listen });

    QNetworkReply* r = nam_->post(lbRequest(apiRoot() + QStringLiteral("/1/submit-listens"), true),
                                  QJsonDocument(body).toJson(QJsonDocument::Compact));
    // Fire and forget, and the deleteLater is the ONLY thing connected: nothing inspects the status, nothing
    // records an error, nothing retries. A "now playing" that failed is worth exactly nothing a moment later,
    // and a queue for it would announce finished tracks as current ones.
    connect(r, &QNetworkReply::finished, r, &QNetworkReply::deleteLater);
}

void ListenBrainzClient::submit(const QVector<Scrobble::Play>& plays,
                                std::function<void(ScrobbleResult)> cb)
{
    if (plays.isEmpty()) { if (cb) cb(ScrobbleResult::ok()); return; }
    if (!configured())
    {
        // KEEP them. A user who has not pasted a token yet may paste one tomorrow, and both services accept
        // backdated listens — so this is Auth (hold, stop pumping), never Rejected (throw away).
        if (cb) cb(ScrobbleResult::auth(tr("No ListenBrainz token has been entered.")));
        return;
    }

    QJsonArray payload;
    for (const Scrobble::Play& p : plays)
    {
        QJsonObject listen;
        // THE BACKDATE. This is the timestamp captured when the track STARTED, carried through the offline
        // queue untouched. It is the entire reason a flight's worth of listening lands in the right order at
        // the right times instead of as a single burst at the moment the network came back.
        listen.insert(QStringLiteral("listened_at"), double(p.listenedAt));
        listen.insert(QStringLiteral("track_metadata"), metadataFor(p.track));
        payload.append(listen);
    }
    QJsonObject body;
    // `import`, not `single`: this is a batch, and `single` rejects a payload of more than one listen. A batch
    // of one is a perfectly ordinary import, so there is no second shape to keep in step.
    body.insert(QStringLiteral("listen_type"), QStringLiteral("import"));
    body.insert(QStringLiteral("payload"), payload);

    QNetworkReply* r = nam_->post(lbRequest(apiRoot() + QStringLiteral("/1/submit-listens"), true),
                                  QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [r, cb] {
        const int status = statusOf(r);
        const QByteArray bodyBytes = r->readAll();
        const ScrobbleResult::Outcome o = outcomeFor(status, r->error());
        ScrobbleResult res;
        res.outcome = o;
        if (o != ScrobbleResult::Outcome::Ok) res.message = failureMessage(r, status, bodyBytes);
        r->deleteLater();
        if (cb) cb(res);
    });
}

void ListenBrainzClient::love(const Scrobble::Track& track, bool loved,
                              std::function<void(ScrobbleResult)> cb)
{
    if (!configured())
    {
        if (cb) cb(ScrobbleResult::auth(tr("No ListenBrainz token has been entered.")));
        return;
    }
    if (track.artist.trimmed().isEmpty() || track.title.trimmed().isEmpty())
    {
        if (cb) cb(ScrobbleResult::rejected(tr("That track has no artist and title to look up.")));
        return;
    }

    // STEP 1 — turn an artist/title pair into a MusicBrainz recording. ListenBrainz files feedback against a
    // recording, not against a name, and nothing in this app's tag reader produces an MBID; this endpoint is
    // the service's own answer to exactly that. It needs no auth, but the request is built the same way so
    // there is one request builder and no second place a header could be got wrong.
    QUrl lookup(apiRoot() + QStringLiteral("/1/metadata/lookup/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("artist_name"), track.artist.trimmed());
    q.addQueryItem(QStringLiteral("recording_name"), track.title.trimmed());
    lookup.setQuery(q);

    QNetworkReply* r = nam_->get(lbRequest(lookup.toString(), false));
    connect(r, &QNetworkReply::finished, this, [this, r, loved, cb] {
        const int status = statusOf(r);
        const QByteArray body = r->readAll();
        const QString mbid = QJsonDocument::fromJson(body).object()
                                 .value(QStringLiteral("recording_mbid")).toString();
        const QString why = failureMessage(r, status, body);
        r->deleteLater();

        if (status <= 0 || status >= 500 || status == 429)
        { if (cb) cb(ScrobbleResult::retryable(why)); return; }
        if (mbid.isEmpty())
        {
            // SAID, not swallowed. The service simply does not know this recording — which happens for
            // bootlegs, live rips and anything mistagged — and a favourite that silently did not travel is
            // the failure mode this whole feature is supposed to stop having.
            if (cb) cb(ScrobbleResult::rejected(tr("ListenBrainz does not know that recording, so it cannot "
                                                    "be loved there.")));
            return;
        }

        // STEP 2 — the feedback itself. score 1 == loved, 0 == no opinion (which is what UN-loving means here;
        // -1 is "hated" and is emphatically NOT what removing a favourite says).
        QJsonObject fb;
        fb.insert(QStringLiteral("recording_mbid"), mbid);
        fb.insert(QStringLiteral("score"), loved ? 1 : 0);
        QNetworkReply* f = nam_->post(
            lbRequest(apiRoot() + QStringLiteral("/1/feedback/recording-feedback"), true),
            QJsonDocument(fb).toJson(QJsonDocument::Compact));
        connect(f, &QNetworkReply::finished, f, [f, cb] {
            const int st = statusOf(f);
            const QByteArray b = f->readAll();
            ScrobbleResult res;
            res.outcome = outcomeFor(st, f->error());
            if (res.outcome != ScrobbleResult::Outcome::Ok) res.message = failureMessage(f, st, b);
            f->deleteLater();
            if (cb) cb(res);
        });
    });
}
