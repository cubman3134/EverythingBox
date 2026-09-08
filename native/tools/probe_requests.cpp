// probe_requests — asking for something you do not have (issue #109), driven against RECORDED FIXTURE
// PAYLOADS and a FIXTURE HTTP STUB this file starts on a loopback port.
//
// NO REAL JELLYSEERR, NO REAL ACCOUNT, AND — the point of saying so — NO REAL CREDENTIAL. The key below is
// the literal string "probe-fixture-apikey-4d7b1e93", named so nobody can mistake it for one and
// distinctive enough that a byte scan over a whole directory tree cannot match it by accident. The last
// test in this file IS that byte scan, across everything the feature writes and everything it prints,
// which is what the credential rule reduces to: a key belongs in exactly one device-local ini row and in
// exactly one request header, and nowhere else.
//
// What is under test, and why each of these:
//
//   1. THE ID PATH. Both id kinds, an episode collapsing to its season, an id whose own word contradicts
//      the row's type, and — the arm that matters most — an item carrying NEITHER id yielding no action at
//      all rather than a broken one.
//   2. THE ANTI-DUPLICATE BRANCH. A title the service says is already there produces "In your library" and
//      is NOT requestable, on the pure rule and end-to-end against the stub.
//   3. THE SUBMISSIONS. A film, a whole series and a single season — the exact bodies, and the live POSTs.
//      Plus the one that is not a submission: nothing in this feature posts anything without being asked.
//   4. EVERY FAILURE SHAPE — unreachable, 401, 403, a malformed body, and a duplicate rejection in BOTH the
//      spellings a real service uses — each rendering a sentence of OUR OWN, and none of them containing
//      the url, the host, the key or a status code.
//   5. THE SHELF. Its grouping, its ordering, and its unknown-status state — which is never "pending".
//   6. THE STORE. De-dupe by title, a season union, and a refresh that cannot create a row.
//   7. THE SEAM. Exactly one implementation in this build, and the surface's own decision function taking
//      no backend identity at all — driven through a two-line fake installed at the chooser, which is how
//      "the UI never learns which backend answered" is asserted rather than described.
//   8. THE BYTE SCAN.
#include "Jellyfin.h"
#include "JellyfinServerStore.h"
#include "Jellyseerr.h"
#include "JellyseerrClient.h"
#include "JellyseerrStore.h"
#include "RequestBackend.h"
#include "RequestStore.h"
#include "Requests.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// The one string the credential rule is asserted over.
static const char* kApiKey = "probe-fixture-apikey-4d7b1e93";

using requests::Availability;
using requests::ActionKind;
using requests::Failure;
using requests::IdKind;
using requests::MediaRef;

// ==========================================================================================================
// THE FIXTURE SERVICE
// ==========================================================================================================
// A deliberately small HTTP/1.1 server: one request per connection, Content-Length bodies only, no chunking
// and no keep-alive. It is not pretending to be Jellyseerr — it answers the four endpoints this client
// speaks and records what it was asked, which is what the assertions are about.
class SeerrStub : public QTcpServer
{
public:
    struct Seen { QString method; QString path; QString key; QByteArray body; };
    QVector<Seen> seen;

    // Knobs, one per failure shape the surface has to render.
    int     forceStatus = 0;          // answer every call with this HTTP status
    bool    duplicateOn409 = false;   // POST /request answers 409
    bool    duplicateOn500 = false;   // ...or a 500 whose body says it already exists
    bool    malformedMedia = false;   // the title endpoint answers valid JSON that is not a title
    bool    postAcceptedButUnreadable = false;   // a 201 with a body carrying no request id

    explicit SeerrStub(QObject* parent = nullptr) : QTcpServer(parent) {}

    int countOf(const QString& method, const QString& pathPrefix) const
    {
        int n = 0;
        for (const Seen& s : seen) if (s.method == method && s.path.startsWith(pathPrefix)) ++n;
        return n;
    }
    const Seen* lastOf(const QString& method, const QString& pathPrefix) const
    {
        for (int i = int(seen.size()) - 1; i >= 0; --i)
            if (seen[i].method == method && seen[i].path.startsWith(pathPrefix)) return &seen[i];
        return nullptr;
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto* sock = new QTcpSocket(this);
        sock->setSocketDescriptor(handle);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            sock->setProperty("buf", sock->property("buf").toByteArray() + sock->readAll());
            QByteArray buf = sock->property("buf").toByteArray();
            const int headEnd = buf.indexOf("\r\n\r\n");
            if (headEnd < 0) return;
            const QByteArray head = buf.left(headEnd);
            const QList<QByteArray> lines = head.split('\n');
            const QList<QByteArray> reqLine = lines.value(0).trimmed().split(' ');
            QString key;
            int wantBody = 0;
            for (int i = 1; i < lines.size(); ++i)
            {
                const QByteArray l = lines.at(i).trimmed();
                // CASE-INSENSITIVELY: Qt 6 lower-cases the header names it puts on the wire, and a stub
                // matching "X-Api-Key:" would see none and report the key as ABSENT from every request —
                // a green light for exactly the mistake this file checks.
                if (l.toLower().startsWith("x-api-key:")) key = QString::fromUtf8(l.mid(10)).trimmed();
                if (l.toLower().startsWith("content-length:")) wantBody = l.mid(15).trimmed().toInt();
            }
            const QByteArray body = buf.mid(headEnd + 4);
            if (body.size() < wantBody) return;
            reply(sock, QString::fromUtf8(reqLine.value(0)), QString::fromUtf8(reqLine.value(1)), key,
                  body.left(wantBody));
        });
    }

private:
    void send(QTcpSocket* sock, int status, const QByteArray& body,
              const char* type = "application/json")
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + (status < 400 ? " OK" : " ERR")
                       + "\r\nContent-Type: " + type
                       + "\r\nContent-Length: " + QByteArray::number(body.size())
                       + "\r\nConnection: close\r\n\r\n" + body;
        sock->write(out);
        sock->flush();
        sock->disconnectFromHost();
    }

    void reply(QTcpSocket* sock, const QString& method, const QString& target, const QString& key,
               const QByteArray& body)
    {
        const QUrl u(target);
        const QString path = u.path();
        seen.push_back({ method, target, key, body });

        if (forceStatus != 0) { send(sock, forceStatus, "{\"message\":\"no\"}"); return; }

        if (path == QLatin1String("/api/v1/status"))
        { send(sock, 200, R"({"version":"2.1.0","commitTag":"local"})"); return; }

        if (path == QLatin1String("/api/v1/search"))
        {
            const QString q = QUrlQuery(u).queryItemValue(QStringLiteral("query"));
            if (q == QLatin1String("tt0111161"))
            {
                // A person result first, deliberately: the reader must skip it rather than request it.
                send(sock, 200, R"({"results":[{"id":99,"mediaType":"person","name":"Someone"},
                                               {"id":278,"mediaType":"movie","title":"The Film"}]})");
                return;
            }
            if (q == QLatin1String("tt0903747"))
            { send(sock, 200, R"({"results":[{"id":1396,"mediaType":"tv","name":"The Show"}]})"); return; }
            if (q == QLatin1String("tt9999999"))
            { send(sock, 200, R"({"results":[]})"); return; }
            send(sock, 200, R"({"results":[]})");
            return;
        }

        if (path.startsWith(QLatin1String("/api/v1/movie/")))
        {
            if (malformedMedia) { send(sock, 200, R"({"nothing":"useful"})"); return; }
            const QString id = path.mid(QStringLiteral("/api/v1/movie/").size());
            if (id == QLatin1String("278"))          // nobody has asked for it: no mediaInfo at all
            { send(sock, 200, R"({"id":278,"title":"The Film"})"); return; }
            if (id == QLatin1String("603"))          // already on the linked server
            {
                send(sock, 200, R"({"id":603,"title":"Have It",
                    "mediaInfo":{"status":5,"jellyfinMediaId":"aaaabbbbccccddddeeeeffff00001111"}})");
                return;
            }
            if (id == QLatin1String("604"))          // asked for, waiting on approval
            { send(sock, 200, R"({"id":604,"mediaInfo":{"status":2,"requests":[{"status":1}]}})"); return; }
            if (id == QLatin1String("605"))          // asked for, and somebody said no
            { send(sock, 200, R"({"id":605,"mediaInfo":{"status":2,"requests":[{"status":3}]}})"); return; }
            send(sock, 404, R"({"message":"Not found"})");
            return;
        }

        if (path.startsWith(QLatin1String("/api/v1/tv/")))
        {
            const QString id = path.mid(QStringLiteral("/api/v1/tv/").size());
            if (id == QLatin1String("1396"))
            {
                // Four real seasons plus specials; season 1 is already there, so the picker must offer 2-4.
                send(sock, 200, R"({"id":1396,"name":"The Show",
                    "seasons":[{"seasonNumber":0},{"seasonNumber":1},{"seasonNumber":2},
                               {"seasonNumber":3},{"seasonNumber":4}],
                    "mediaInfo":{"status":4,"jellyfinMediaId":"1111222233334444555566667777888",
                                 "seasons":[{"seasonNumber":1,"status":5}]}})");
                return;
            }
            if (id == QLatin1String("1397"))
            { send(sock, 200, R"({"id":1397,"name":"Fresh","seasons":[{"seasonNumber":1},{"seasonNumber":2}]})"); return; }
            send(sock, 404, R"({"message":"Not found"})");
            return;
        }

        if (path == QLatin1String("/api/v1/request") && method == QLatin1String("POST"))
        {
            if (duplicateOn409) { send(sock, 409, R"({"message":"Request already exists"})"); return; }
            if (duplicateOn500)
            { send(sock, 500, R"({"message":"Request for this media already exists."})"); return; }
            if (postAcceptedButUnreadable) { send(sock, 201, R"({"ok":true})"); return; }
            send(sock, 201, R"({"id":42,"status":1,"type":"movie"})");
            return;
        }
        send(sock, 404, R"({"message":"Not found"})");
    }
};

// Spin the event loop until `pred` or the deadline. Never a QThread::sleep: every wait here is for a socket.
template <typename Pred>
static bool waitFor(Pred pred, int ms = 8000)
{
    QDeadlineTimer dl(ms);
    while (!pred() && !dl.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return pred();
}

// ==========================================================================================================
// 7. THE FAKE BACKEND — the seam, substituted in two lines
// ==========================================================================================================
class FakeBackend : public RequestBackend
{
public:
    int lookups = 0;
    int submits = 0;
    Availability answer = Availability::NotRequested;

    QString id() const override          { return QStringLiteral("fake"); }
    QString displayName() const override { return QStringLiteral("A Test Service"); }
    bool    configured() const override  { return true; }

    void lookup(const MediaRef&, int, std::function<void(const RequestLookup&)> cb) override
    {
        ++lookups;
        RequestLookup out;
        out.ok = true;
        out.availability = answer;
        cb(out);
    }
    void submit(const RequestSubmission&, int, std::function<void(const RequestAck&)> cb) override
    {
        ++submits;
        RequestAck out;
        out.ok = true;
        out.availability = Availability::Pending;
        cb(out);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("probe_requests"));

    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "FAIL: no temp dir\n"); return 1; }
    const QString iniPath = tmp.path() + QStringLiteral("/probe_requests.ini");
    JellyseerrStore::setIniPathForTesting(iniPath);
    RequestStore::setIniPathForTesting(iniPath);
    JellyfinServerStore::setIniPathForTesting(iniPath);

    // ======================================================================================================
    // 1. THE ID PATH
    // ======================================================================================================
    {
        // TMDB, all three shapes the bundled catalogue mints.
        const MediaRef m = requests::refFor(QStringLiteral("tmdb:movie:603"), QString(),
                                            QStringLiteral("movie"));
        CHECK(m.kind == IdKind::Tmdb);
        CHECK(m.tmdb == QStringLiteral("603"));
        CHECK(m.mediaType == requests::kMovie());
        CHECK(m.season == 0);
        CHECK(m.key() == QStringLiteral("tmdb:movie:603"));

        const MediaRef s = requests::refFor(QStringLiteral("tmdb:tv:1396"), QString(),
                                            QStringLiteral("series"));
        CHECK(s.kind == IdKind::Tmdb && s.mediaType == requests::kTv() && s.season == 0);
        CHECK(s.key() == QStringLiteral("tmdb:tv:1396"));

        // An EPISODE collapses to its series plus a season, and its key names the TITLE, not the episode:
        // two episodes of one show are one thing being acquired.
        const MediaRef e = requests::refFor(QStringLiteral("tmdb:episode:1396:2:7"), QString(),
                                            QStringLiteral("episode"));
        CHECK(e.kind == IdKind::Tmdb && e.tmdb == QStringLiteral("1396"));
        CHECK(e.mediaType == requests::kTv());
        CHECK(e.season == 2);
        CHECK(e.key() == QStringLiteral("tmdb:tv:1396"));

        // Specials (season 0) collapse to "the whole series" rather than to season zero.
        CHECK(requests::refFor(QStringLiteral("tmdb:episode:1396:0:1"), QString(),
                               QStringLiteral("episode")).season == 0);

        // THE ID'S OWN WORD BEATS THE ROW'S TYPE. A loosely typed row must not turn a film into a series.
        CHECK(requests::refFor(QStringLiteral("tmdb:movie:603"), QString(),
                               QStringLiteral("video")).mediaType == requests::kMovie());
        CHECK(requests::refFor(QStringLiteral("tmdb:tv:1396"), QString(),
                               QStringLiteral("video")).mediaType == requests::kTv());
    }
    {
        // IMDB, as the item's own id and as the bridged stream id.
        const MediaRef m = requests::refFor(QStringLiteral("tt0111161"), QString(),
                                            QStringLiteral("movie"));
        CHECK(m.kind == IdKind::Imdb && m.imdb == QStringLiteral("tt0111161"));
        CHECK(m.mediaType == requests::kMovie());
        CHECK(m.key() == QStringLiteral("imdb:tt0111161"));

        const MediaRef bridged = requests::refFor(QStringLiteral("someaddon:xyz"),
                                                  QStringLiteral("tt0111161"), QStringLiteral("movie"));
        CHECK(bridged.kind == IdKind::Imdb && bridged.imdb == QStringLiteral("tt0111161"));

        // The Stremio episode stream-id shape: series + season, whatever the row calls itself.
        const MediaRef ep = requests::refFor(QString(), QStringLiteral("tt0903747:4:9"),
                                             QStringLiteral("video"));
        CHECK(ep.kind == IdKind::Imdb && ep.imdb == QStringLiteral("tt0903747"));
        CHECK(ep.mediaType == requests::kTv() && ep.season == 4);
        CHECK(ep.key() == QStringLiteral("imdb:tt0903747"));
    }
    {
        // AN ITEM WITH NEITHER ID. The arm the issue turns on: no action at all, never a broken one.
        CHECK(!requests::refFor(QStringLiteral("local:/movies/thing.mkv"), QString(),
                                QStringLiteral("movie")).ok());
        CHECK(!requests::refFor(QStringLiteral("jf:aaaabbbbccccddddeeeeffff00001111:xyz"), QString(),
                                QStringLiteral("movie")).ok());
        CHECK(!requests::refFor(QString(), QString(), QStringLiteral("movie")).ok());
        // ...and things that are not films or series at all, however well identified.
        CHECK(!requests::refFor(QStringLiteral("tt0111161"), QString(), QStringLiteral("game")).ok());
        CHECK(!requests::refFor(QStringLiteral("tt0111161"), QString(), QStringLiteral("album")).ok());
        CHECK(!requests::refFor(QStringLiteral("tt0111161"), QString(), QStringLiteral("ebook")).ok());
        CHECK(!requests::refFor(QStringLiteral("tt0111161"), QString(), QStringLiteral("_downloads")).ok());
        // An IMDB id with no type to classify it is not guessed at.
        CHECK(!requests::refFor(QStringLiteral("tt0111161"), QString(), QString()).ok());
        // Near misses that must not read as ids.
        CHECK(!requests::refFor(QStringLiteral("ttsomething"), QString(), QStringLiteral("movie")).ok());
        CHECK(!requests::refFor(QStringLiteral("tt12"), QString(), QStringLiteral("movie")).ok());
        CHECK(!requests::refFor(QStringLiteral("tmdb:person:5"), QString(), QStringLiteral("movie")).ok());
        CHECK(!requests::refFor(QStringLiteral("prefix-tmdb:movie:603"), QString(),
                                QStringLiteral("movie")).ok());
        CHECK(requests::isRequestableType(QStringLiteral("series")));
        CHECK(!requests::isRequestableType(QStringLiteral("game")));
    }

    // ======================================================================================================
    // 2. THE ANTI-DUPLICATE RULE, on the pure decision
    // ======================================================================================================
    {
        const MediaRef ref = requests::refFor(QStringLiteral("tmdb:movie:603"), QString(),
                                              QStringLiteral("movie"));
        CHECK(requests::actionFor(ref, true, true, Availability::Available) == ActionKind::InLibrary);
        CHECK(!requests::isRequestable(Availability::Available));
        CHECK(requests::actionLabel(ActionKind::InLibrary, Availability::Available)
              == QStringLiteral("In your library"));
        // Everything in flight is a badge, not a button.
        for (Availability a : { Availability::Pending, Availability::Approved, Availability::Processing })
        {
            CHECK(requests::actionFor(ref, true, true, a) == ActionKind::Waiting);
            CHECK(!requests::isRequestable(a));
        }
        CHECK(requests::actionFor(ref, true, true, Availability::NotRequested) == ActionKind::Request);
        // Partly there IS requestable, and says what it would do.
        CHECK(requests::actionFor(ref, true, true, Availability::PartiallyAvailable) == ActionKind::Request);
        CHECK(requests::actionLabel(ActionKind::Request, Availability::PartiallyAvailable)
              == QStringLiteral("Request what is missing"));
        // A LOOKUP THAT DID NOT ANSWER IS NEVER "PENDING". It is unknown, and it says so.
        CHECK(requests::actionFor(ref, true, false, Availability::Unknown) == ActionKind::Unknown);
        CHECK(requests::actionLabel(ActionKind::Unknown, Availability::Unknown)
              == QStringLiteral("Request (status unknown)"));
        // No backend, or nothing to key on: no action at all.
        CHECK(requests::actionFor(ref, false, true, Availability::NotRequested) == ActionKind::None);
        CHECK(requests::actionFor(MediaRef{}, true, true, Availability::NotRequested) == ActionKind::None);
        CHECK(requests::actionLabel(ActionKind::None, Availability::NotRequested).isEmpty());
    }

    // ======================================================================================================
    // 3. THE PAYLOAD READERS, over recorded fixture bodies
    // ======================================================================================================
    {
        // No mediaInfo = nobody has asked. An ANSWER, not a gap.
        const jellyseerr::MediaStatus none =
            jellyseerr::readMediaStatus(QByteArray(R"({"id":278,"title":"The Film"})"),
                                        requests::kMovie());
        CHECK(none.ok && none.availability == Availability::NotRequested);
        CHECK(none.tmdbId == QStringLiteral("278"));
        CHECK(none.serverItemId.isEmpty());

        const jellyseerr::MediaStatus have = jellyseerr::readMediaStatus(
            QByteArray(R"({"id":603,"mediaInfo":{"status":5,"jellyfinMediaId":"abc"}})"),
            requests::kMovie());
        CHECK(have.ok && have.availability == Availability::Available);
        CHECK(have.serverItemId == QStringLiteral("abc"));

        // A DECLINE LIVES ON THE REQUEST and outranks the media status, which still says "pending".
        const jellyseerr::MediaStatus dec = jellyseerr::readMediaStatus(
            QByteArray(R"({"id":605,"mediaInfo":{"status":2,"requests":[{"status":3}]}})"),
            requests::kMovie());
        CHECK(dec.ok && dec.availability == Availability::Declined);

        // A series: how many seasons there are (specials excluded) and which are already there.
        const jellyseerr::MediaStatus tv = jellyseerr::readMediaStatus(
            QByteArray(R"({"id":1396,"seasons":[{"seasonNumber":0},{"seasonNumber":1},
                          {"seasonNumber":2},{"seasonNumber":3},{"seasonNumber":4}],
                          "mediaInfo":{"status":4,"seasons":[{"seasonNumber":1,"status":5}]}})"),
            requests::kTv());
        CHECK(tv.ok && tv.availability == Availability::PartiallyAvailable);
        CHECK(tv.seasonCount == 4);
        CHECK(tv.availableSeasons == QVector<int>{ 1 });

        // NOT AN ENVELOPE AT ALL. Valid JSON, no title in it: malformed, and specifically NOT
        // "nobody has asked for it".
        CHECK(!jellyseerr::readMediaStatus(QByteArray(R"({"error":"nope"})"), requests::kMovie()).ok);
        CHECK(!jellyseerr::readMediaStatus(QByteArray("<html>login</html>"), requests::kMovie()).ok);
        CHECK(!jellyseerr::readMediaStatus(QByteArray(), requests::kMovie()).ok);
    }
    {
        // The IMDB bridge. A person result is skipped; a type mismatch is refused rather than requested.
        const jellyseerr::SearchHit hit = jellyseerr::readSearchHit(
            QByteArray(R"({"results":[{"id":99,"mediaType":"person"},{"id":278,"mediaType":"movie"}]})"),
            requests::kMovie());
        CHECK(hit.ok && hit.tmdbId == QStringLiteral("278") && hit.mediaType == requests::kMovie());
        CHECK(!jellyseerr::readSearchHit(
            QByteArray(R"({"results":[{"id":1396,"mediaType":"tv"}]})"), requests::kMovie()).ok);
        CHECK(!jellyseerr::readSearchHit(QByteArray(R"({"results":[]})"), requests::kMovie()).ok);
    }
    {
        // The exact request bodies. THREE SHAPES, and the one that must never be sent.
        const QByteArray film = jellyseerr::requestBody(requests::kMovie(), QStringLiteral("603"), {});
        const QJsonObject fo = QJsonDocument::fromJson(film).object();
        CHECK(fo.value(QStringLiteral("mediaType")).toString() == QStringLiteral("movie"));
        CHECK(qint64(fo.value(QStringLiteral("mediaId")).toDouble()) == 603);
        CHECK(!fo.contains(QStringLiteral("seasons")));
        // NO QUALITY PROFILE, NO ROOT FOLDER, NO SERVER — v1 leaves all three to the server's default, and
        // sending any of them would silently override somebody's own configuration.
        CHECK(!fo.contains(QStringLiteral("profileId")));
        CHECK(!fo.contains(QStringLiteral("rootFolder")));
        CHECK(!fo.contains(QStringLiteral("serverId")));

        const QJsonObject whole = QJsonDocument::fromJson(
            jellyseerr::requestBody(requests::kTv(), QStringLiteral("1396"), {})).object();
        CHECK(whole.value(QStringLiteral("seasons")).toString() == QStringLiteral("all"));

        const QJsonObject part = QJsonDocument::fromJson(
            jellyseerr::requestBody(requests::kTv(), QStringLiteral("1396"), { 3, 2, 2 })).object();
        const QJsonArray arr = part.value(QStringLiteral("seasons")).toArray();
        CHECK(arr.size() == 2 && arr.at(0).toInt() == 2 && arr.at(1).toInt() == 3);

        // Nothing sendable => an EMPTY body, which the client refuses to post.
        CHECK(jellyseerr::requestBody(requests::kMovie(), QString(), {}).isEmpty());
        CHECK(jellyseerr::requestBody(requests::kMovie(), QStringLiteral("tt0111161"), {}).isEmpty());
        CHECK(jellyseerr::requestBody(requests::kTv(), QStringLiteral("1396"), { 0 }).isEmpty());
    }
    {
        // The acknowledgement, and the 2xx that is NOT one.
        CHECK(jellyseerr::readRequestAck(QByteArray(R"({"id":42,"status":1})")).ok);
        CHECK(jellyseerr::readRequestAck(QByteArray(R"({"id":42,"status":2})")).availability
              == Availability::Approved);
        CHECK(!jellyseerr::readRequestAck(QByteArray(R"({"ok":true})")).ok);
        CHECK(!jellyseerr::readRequestAck(QByteArray("not json")).ok);
    }
    {
        // Transport safety: the key rides a header on every call, so plain HTTP is refused unless allowed.
        CHECK(jellyseerr::checkUrl(QStringLiteral("https://r.example.com"), false)
              == jellyseerr::UrlVerdict::Ok);
        CHECK(jellyseerr::checkUrl(QStringLiteral("http://10.0.0.4:5055"), false)
              == jellyseerr::UrlVerdict::InsecureRefused);
        CHECK(jellyseerr::checkUrl(QStringLiteral("http://10.0.0.4:5055"), true)
              == jellyseerr::UrlVerdict::Ok);
        CHECK(jellyseerr::checkUrl(QStringLiteral("ftp://x/"), true) == jellyseerr::UrlVerdict::NotHttp);
        CHECK(jellyseerr::checkUrl(QStringLiteral("not a url"), true) == jellyseerr::UrlVerdict::Malformed);
        CHECK(jellyseerr::normalizeRoot(QStringLiteral("https://r.example.com///"), false)
              == QStringLiteral("https://r.example.com"));
        CHECK(jellyseerr::normalizeRoot(QStringLiteral("http://x/"), false).isEmpty());
    }

    // ======================================================================================================
    // 4. FAILURE CLASSIFICATION, AND THE SENTENCES
    // ======================================================================================================
    {
        CHECK(jellyseerr::failureForHttp(401, {}) == Failure::Unauthorized);
        CHECK(jellyseerr::failureForHttp(403, {}) == Failure::Forbidden);
        CHECK(jellyseerr::failureForHttp(404, {}) == Failure::NotFound);
        CHECK(jellyseerr::failureForHttp(409, {}) == Failure::Duplicate);
        CHECK(jellyseerr::failureForHttp(429, {}) == Failure::RateLimited);
        CHECK(jellyseerr::failureForHttp(500, {}) == Failure::ServerError);
        // BOTH spellings of a duplicate rejection, because real services use both.
        CHECK(jellyseerr::failureForHttp(500, QByteArray(R"({"message":"Request for this media already exists."})"))
              == Failure::Duplicate);
        CHECK(jellyseerr::failureForHttp(422, {}) == Failure::Malformed);
        CHECK(jellyseerr::failureForHttp(200, {}) == Failure::None);

        // EVERY ARM RENDERS A SENTENCE OF OUR OWN, and none of them is empty.
        for (Failure f : { Failure::NotConfigured, Failure::Unreachable, Failure::TimedOut,
                           Failure::Unauthorized, Failure::Forbidden, Failure::NotFound,
                           Failure::Duplicate, Failure::Malformed, Failure::RateLimited,
                           Failure::ServerError })
        {
            const QString s = requests::failureSentence(f);
            CHECK(!s.isEmpty());
            // NOTHING BUILT OUT OF A REQUEST. No url, no host, no scheme, no status code, no key.
            CHECK(!s.contains(QLatin1String("http")));
            CHECK(!s.contains(QLatin1String("://")));
            CHECK(!s.contains(QLatin1String("127.0.0.1")));
            CHECK(!s.contains(QLatin1String("api/v1")));
            CHECK(!s.contains(QString::fromLatin1(kApiKey)));
            CHECK(!s.contains(QLatin1String("401")) && !s.contains(QLatin1String("403")));
        }
        CHECK(requests::failureSentence(Failure::None).isEmpty());
    }

    // ======================================================================================================
    // 5. THE SHELF
    // ======================================================================================================
    {
        auto row = [](const char* key, const char* title, Availability a, qint64 at) {
            requests::StoredRequest r;
            r.key = QString::fromLatin1(key);
            r.title = QString::fromLatin1(title);
            r.backendId = QStringLiteral("jellyseerr");
            r.mediaType = requests::kMovie();
            r.requestedAt = at;
            r.status = requests::statusToken(a);
            return r;
        };
        QVector<requests::StoredRequest> rows{
            row("a", "Waiting one",  Availability::Pending,     100),
            row("b", "Arrived",      Availability::Available,   200),
            row("c", "Refused",      Availability::Declined,    150),
            row("d", "No idea",      Availability::Unknown,     300),
            row("e", "Waiting two",  Availability::Processing,  400),
        };
        const QVector<requests::ShelfGroup> g = requests::shelfGroups(rows);
        CHECK(g.size() == 4);
        CHECK(g.at(0).header == QStringLiteral("Ready to watch") && g.at(0).items.size() == 1);
        CHECK(g.at(1).header == QStringLiteral("On the way") && g.at(1).items.size() == 2);
        // Newest first inside a group.
        CHECK(g.at(1).items.at(0).title == QStringLiteral("Waiting two"));
        CHECK(g.at(2).header == QStringLiteral("Needs attention"));
        // UNKNOWN IS ITS OWN GROUP. Folding it into "On the way" would be a claim about somebody's server.
        CHECK(g.at(3).header == QStringLiteral("Status unknown"));
        CHECK(g.at(3).items.at(0).title == QStringLiteral("No idea"));
        // Empty groups are omitted entirely; an empty shelf is no groups at all.
        CHECK(requests::shelfGroups({}).isEmpty());
        CHECK(requests::shelfGroups({ row("z", "Only one", Availability::Pending, 1) }).size() == 1);

        // The token round trip, and the arm that matters: an unrecognised token is UNKNOWN, never pending.
        for (Availability a : { Availability::Unknown, Availability::NotRequested, Availability::Pending,
                                Availability::Approved, Availability::Processing,
                                Availability::PartiallyAvailable, Availability::Available,
                                Availability::Declined, Availability::Failed })
            CHECK(requests::availabilityFromToken(requests::statusToken(a)) == a);
        CHECK(requests::availabilityFromToken(QString()) == Availability::Unknown);
        CHECK(requests::availabilityFromToken(QStringLiteral("something-new")) == Availability::Unknown);
    }

    // ======================================================================================================
    // 6. THE STORE
    // ======================================================================================================
    {
        requests::StoredRequest r;
        r.key = QStringLiteral("tmdb:tv:1396");
        r.backendId = QStringLiteral("jellyseerr");
        r.title = QStringLiteral("The Show");
        r.mediaType = requests::kTv();
        r.tmdb = QStringLiteral("1396");
        r.seasons = { 2 };
        r.requestedAt = 1000;
        r.status = requests::statusToken(Availability::Pending);
        RequestStore::add(r);
        CHECK(RequestStore::count() == 1);

        // The same title again: ONE row, with the season sets unioned.
        r.seasons = { 3 };
        r.requestedAt = 2000;
        RequestStore::add(r);
        CHECK(RequestStore::count() == 1);
        requests::StoredRequest got;
        CHECK(RequestStore::get(QStringLiteral("tmdb:tv:1396"), got));
        CHECK(got.seasons == (QVector<int>{ 2, 3 }));
        CHECK(got.requestedAt == 2000);

        // ...and a whole-series press subsumes the subset.
        r.seasons = {};
        RequestStore::add(r);
        CHECK(RequestStore::get(QStringLiteral("tmdb:tv:1396"), got) && got.seasons.isEmpty());

        RequestStore::setStatus(QStringLiteral("tmdb:tv:1396"),
                                requests::statusToken(Availability::Available));
        CHECK(RequestStore::get(QStringLiteral("tmdb:tv:1396"), got));
        CHECK(got.status == QStringLiteral("available"));

        // A REFRESH NEVER CREATES A ROW. A row means "this profile asked for this"; a status fetch is not
        // an ask, and a shelf that grew a row every time somebody opened a detail page would be a diary of
        // what they browsed.
        RequestStore::setStatus(QStringLiteral("tmdb:movie:999"),
                                requests::statusToken(Availability::Available));
        CHECK(RequestStore::count() == 1);
        CHECK(!RequestStore::get(QStringLiteral("tmdb:movie:999"), got));

        RequestStore::remove(QStringLiteral("tmdb:tv:1396"));
        CHECK(RequestStore::count() == 0);
    }

    // ======================================================================================================
    // 7. THE SEAM — one implementation, and a surface that cannot tell which answered
    // ======================================================================================================
    {
        // Nothing configured: the chooser answers nobody, and the surface shows no action.
        JellyseerrStore::clear();
        CHECK(requests::configuredBackend() == nullptr);

        FakeBackend fake;
        requests::setBackendForTesting(&fake);
        RequestBackend* b = requests::configuredBackend();
        CHECK(b == &fake);
        CHECK(b->configured());

        // The decision the surface makes is driven entirely off what came BACK — no backend id anywhere in
        // its inputs. This same call answers identically for the fake and for the real client, which is
        // what "the UI never learns which backend answered" means.
        const MediaRef ref = requests::refFor(QStringLiteral("tmdb:movie:603"), QString(),
                                              QStringLiteral("movie"));
        RequestLookup seen;
        b->lookup(ref, 1000, [&seen](const RequestLookup& l) { seen = l; });
        CHECK(fake.lookups == 1);
        CHECK(requests::actionFor(ref, b->configured(), seen.ok, seen.availability) == ActionKind::Request);

        fake.answer = Availability::Available;
        b->lookup(ref, 1000, [&seen](const RequestLookup& l) { seen = l; });
        CHECK(requests::actionFor(ref, b->configured(), seen.ok, seen.availability) == ActionKind::InLibrary);

        // A LOOKUP SUBMITS NOTHING. Viewing an item never asks anybody for anything.
        CHECK(fake.submits == 0);
        requests::setBackendForTesting(nullptr);
        CHECK(requests::configuredBackend() == nullptr);
    }

    // ======================================================================================================
    // 8. LIVE, AGAINST THE FIXTURE SERVICE
    // ======================================================================================================
    SeerrStub stub;
    if (!stub.listen(QHostAddress::LocalHost, 0))
    { std::fprintf(stderr, "FAIL: stub could not listen\n"); return 1; }
    const QString root = QStringLiteral("http://127.0.0.1:") + QString::number(stub.serverPort());

    JellyseerrClient& client = JellyseerrClient::instance();
    {
        // The setup check refuses a plain-HTTP address unless it has been allowed, before any key is stored.
        bool done = false, ok = false;
        QString err;
        client.verify(root, QString::fromLatin1(kApiKey), /*allowPlainHttp*/ false, 4000,
                      [&](bool o, const QString& e) { done = true; ok = o; err = e; });
        CHECK(waitFor([&] { return done; }));
        CHECK(!ok && !err.isEmpty());
        CHECK(!err.contains(QLatin1String("127.0.0.1")));

        done = false;
        client.verify(root, QString::fromLatin1(kApiKey), true, 4000,
                      [&](bool o, const QString& e) { done = true; ok = o; err = e; });
        CHECK(waitFor([&] { return done; }));
        CHECK(ok && err.isEmpty());
        // ...and the key really did ride the header, or every assertion below about it would be vacuous.
        const SeerrStub::Seen* st = stub.lastOf(QStringLiteral("GET"), QStringLiteral("/api/v1/status"));
        CHECK(st && st->key == QString::fromLatin1(kApiKey));
    }

    JellyseerrConfig cfg;
    cfg.url = root;
    cfg.apiKey = QString::fromLatin1(kApiKey);
    cfg.allowPlainHttp = true;
    JellyseerrStore::save(cfg);
    CHECK(JellyseerrStore::isConfigured());
    CHECK(requests::configuredBackend() != nullptr);
    // The settings line names nothing.
    CHECK(!JellyseerrStore::statusLine().contains(QString::fromLatin1(kApiKey)));
    CHECK(!JellyseerrStore::statusLine().contains(QLatin1String("127.0.0.1")));

    RequestBackend* backend = requests::configuredBackend();

    {
        // A TMDB film nobody has asked for: ONE round trip, no search leg.
        const int before = stub.countOf(QStringLiteral("GET"), QStringLiteral("/api/v1/search"));
        RequestLookup l;
        bool done = false;
        backend->lookup(requests::refFor(QStringLiteral("tmdb:movie:278"), QString(),
                                         QStringLiteral("movie")), 6000,
                        [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(l.ok && l.availability == Availability::NotRequested);
        CHECK(l.resolvedId == QStringLiteral("278"));
        CHECK(stub.countOf(QStringLiteral("GET"), QStringLiteral("/api/v1/search")) == before);
    }
    {
        // AN IMDB-ONLY ITEM. Two legs: the bridge, then the title. This is what makes the action work on an
        // addon-catalogue title you cannot stream, which is the whole point of the feature.
        RequestLookup l;
        bool done = false;
        backend->lookup(requests::refFor(QStringLiteral("tt0111161"), QString(),
                                         QStringLiteral("movie")), 6000,
                        [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(l.ok && l.availability == Availability::NotRequested);
        CHECK(l.resolvedId == QStringLiteral("278"));
        const SeerrStub::Seen* s = stub.lastOf(QStringLiteral("GET"), QStringLiteral("/api/v1/search"));
        CHECK(s && s->path.contains(QStringLiteral("tt0111161")));
    }
    {
        // AN IMDB ID THE SERVICE HAS NEVER HEARD OF: "it does not know this title", not a malformed answer,
        // and definitely not a request for nothing.
        RequestLookup l;
        bool done = false;
        backend->lookup(requests::refFor(QStringLiteral("tt9999999"), QString(),
                                         QStringLiteral("movie")), 6000,
                        [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(!l.ok && l.failure == Failure::NotFound);
        CHECK(l.message == requests::failureSentence(Failure::NotFound));
        CHECK(l.availability == Availability::Unknown);   // never a default "pending"
    }
    {
        // THE ALREADY-AVAILABLE BRANCH, END TO END. With exactly one Jellyfin server configured the badge
        // deep-links; the id it deep-links to is QUALIFIED.
        JellyfinServer jf;
        jf.id = QStringLiteral("aaaabbbbccccddddeeeeffff00001111");
        jf.name = QStringLiteral("Attic");
        jf.url = QStringLiteral("https://jf.example.com");
        jf.userId = QStringLiteral("u1");
        jf.token = QStringLiteral("probe-not-a-real-jellyfin-token");
        CHECK(JellyfinServerStore::add(jf));

        RequestLookup l;
        bool done = false;
        backend->lookup(requests::refFor(QStringLiteral("tmdb:movie:603"), QString(),
                                         QStringLiteral("movie")), 6000,
                        [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(l.ok && l.availability == Availability::Available);
        CHECK(requests::actionFor(requests::refFor(QStringLiteral("tmdb:movie:603"), QString(),
                                                   QStringLiteral("movie")),
                                  true, l.ok, l.availability) == ActionKind::InLibrary);
        CHECK(l.libraryRef == QStringLiteral("jf:aaaabbbbccccddddeeeeffff00001111:aaaabbbbccccddddeeeeffff00001111"));
        CHECK(Jellyfin::isQualified(l.libraryRef));

        // ...and with a SECOND server configured the app cannot say which copy this is, so it does not
        // guess: the badge stays, the deep link goes. An unqualified id is the corruption #160 exists to
        // prevent and that rule does not weaken because a link would be convenient.
        JellyfinServer other = jf;
        other.id = QStringLiteral("99998888777766665555444433332222");
        other.name = QStringLiteral("Basement");
        CHECK(JellyfinServerStore::add(other));
        done = false;
        backend->lookup(requests::refFor(QStringLiteral("tmdb:movie:603"), QString(),
                                         QStringLiteral("movie")), 6000,
                        [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(l.ok && l.availability == Availability::Available);
        CHECK(l.libraryRef.isEmpty());
        JellyfinServerStore::remove(other.id);
    }
    {
        // A SERIES: partly there, so the picker knows which seasons would change something.
        RequestLookup l;
        bool done = false;
        backend->lookup(requests::refFor(QStringLiteral("tmdb:tv:1396"), QString(),
                                         QStringLiteral("series")), 6000,
                        [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(l.ok && l.availability == Availability::PartiallyAvailable);
        CHECK(l.seasonCount == 4);
        CHECK(l.availableSeasons == QVector<int>{ 1 });
    }
    {
        // THE SUBMISSIONS. A film…
        const int before = stub.countOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request"));
        RequestSubmission sub;
        sub.ref = requests::refFor(QStringLiteral("tmdb:movie:278"), QString(), QStringLiteral("movie"));
        sub.resolvedId = QStringLiteral("278");
        RequestAck ack;
        bool done = false;
        backend->submit(sub, 6000, [&](const RequestAck& a) { ack = a; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(ack.ok && ack.availability == Availability::Pending);
        CHECK(stub.countOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request")) == before + 1);
        const SeerrStub::Seen* p = stub.lastOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request"));
        CHECK(p && p->key == QString::fromLatin1(kApiKey));
        CHECK(p && QJsonDocument::fromJson(p->body).object()
                        .value(QStringLiteral("mediaType")).toString() == QStringLiteral("movie"));

        // …a WHOLE SERIES…
        sub.ref = requests::refFor(QStringLiteral("tmdb:tv:1397"), QString(), QStringLiteral("series"));
        sub.resolvedId = QStringLiteral("1397");
        sub.seasons = {};
        done = false;
        backend->submit(sub, 6000, [&](const RequestAck& a) { ack = a; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(ack.ok);
        p = stub.lastOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request"));
        CHECK(p && QJsonDocument::fromJson(p->body).object()
                        .value(QStringLiteral("seasons")).toString() == QStringLiteral("all"));

        // …and ONE SEASON.
        sub.seasons = { 2 };
        done = false;
        backend->submit(sub, 6000, [&](const RequestAck& a) { ack = a; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(ack.ok);
        p = stub.lastOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request"));
        const QJsonArray seasons = QJsonDocument::fromJson(p->body).object()
                                       .value(QStringLiteral("seasons")).toArray();
        CHECK(seasons.size() == 1 && seasons.at(0).toInt() == 2);
    }
    {
        // A SUBMISSION WITH NOTHING SENDABLE POSTS NOTHING AT ALL. The client refuses rather than sending
        // something the service would half-understand.
        const int before = stub.countOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request"));
        RequestSubmission sub;
        sub.ref = requests::refFor(QStringLiteral("tt0111161"), QString(), QStringLiteral("movie"));
        RequestAck ack;
        bool done = false;
        backend->submit(sub, 6000, [&](const RequestAck& a) { ack = a; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(!ack.ok && ack.failure == Failure::Malformed);
        CHECK(stub.countOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request")) == before);
    }
    {
        // THE DUPLICATE REJECTION, in both spellings, and ONE POST each — never a retry.
        RequestSubmission sub;
        sub.ref = requests::refFor(QStringLiteral("tmdb:movie:278"), QString(), QStringLiteral("movie"));
        sub.resolvedId = QStringLiteral("278");
        for (int spelling = 0; spelling < 2; ++spelling)
        {
            stub.duplicateOn409 = (spelling == 0);
            stub.duplicateOn500 = (spelling == 1);
            const int before = stub.countOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request"));
            RequestAck ack;
            bool done = false;
            backend->submit(sub, 6000, [&](const RequestAck& a) { ack = a; done = true; });
            CHECK(waitFor([&] { return done; }));
            CHECK(!ack.ok && ack.failure == Failure::Duplicate);
            CHECK(ack.message == requests::failureSentence(Failure::Duplicate));
            CHECK(stub.countOf(QStringLiteral("POST"), QStringLiteral("/api/v1/request")) == before + 1);
        }
        stub.duplicateOn409 = stub.duplicateOn500 = false;
    }
    {
        // A 2xx WE CANNOT READ IS NOT A SUCCESS.
        stub.postAcceptedButUnreadable = true;
        RequestSubmission sub;
        sub.ref = requests::refFor(QStringLiteral("tmdb:movie:278"), QString(), QStringLiteral("movie"));
        sub.resolvedId = QStringLiteral("278");
        RequestAck ack;
        bool done = false;
        backend->submit(sub, 6000, [&](const RequestAck& a) { ack = a; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(!ack.ok && ack.failure == Failure::Malformed);
        stub.postAcceptedButUnreadable = false;
    }
    {
        // 401 AND 403 — different problems, different sentences, neither built out of the request.
        for (int status : { 401, 403 })
        {
            stub.forceStatus = status;
            RequestLookup l;
            bool done = false;
            backend->lookup(requests::refFor(QStringLiteral("tmdb:movie:278"), QString(),
                                             QStringLiteral("movie")), 6000,
                            [&](const RequestLookup& r) { l = r; done = true; });
            CHECK(waitFor([&] { return done; }));
            CHECK(!l.ok);
            CHECK(l.failure == (status == 401 ? Failure::Unauthorized : Failure::Forbidden));
            CHECK(l.message == requests::failureSentence(l.failure));
            CHECK(!l.message.contains(QLatin1String("127.0.0.1")));
            CHECK(!l.message.contains(QString::fromLatin1(kApiKey)));
            CHECK(l.availability == Availability::Unknown);
        }
        stub.forceStatus = 0;
    }
    {
        // A MALFORMED BODY: something answered, but it is not a request service.
        stub.malformedMedia = true;
        RequestLookup l;
        bool done = false;
        backend->lookup(requests::refFor(QStringLiteral("tmdb:movie:278"), QString(),
                                         QStringLiteral("movie")), 6000,
                        [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(!l.ok && l.failure == Failure::Malformed);
        CHECK(l.availability == Availability::Unknown);
        stub.malformedMedia = false;
    }
    {
        // UNREACHABLE. A port nothing is listening on — the shape of a service that is switched off.
        JellyseerrConfig dead = cfg;
        dead.url = QStringLiteral("http://127.0.0.1:1");
        JellyseerrStore::save(dead);
        RequestLookup l;
        bool done = false;
        requests::configuredBackend()->lookup(
            requests::refFor(QStringLiteral("tmdb:movie:278"), QString(), QStringLiteral("movie")), 4000,
            [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }, 10000));
        CHECK(!l.ok);
        CHECK(l.failure == Failure::Unreachable || l.failure == Failure::TimedOut);
        CHECK(!l.message.isEmpty());
        CHECK(!l.message.contains(QLatin1String("127.0.0.1")));
        CHECK(!l.message.contains(QLatin1String("http")));
        CHECK(l.availability == Availability::Unknown);
        JellyseerrStore::save(cfg);
    }
    {
        // NOT CONFIGURED AT ALL: an answer, not a hang, and nothing is posted.
        JellyseerrStore::clear();
        CHECK(requests::configuredBackend() == nullptr);
        RequestLookup l;
        bool done = false;
        JellyseerrClient::instance().lookup(
            requests::refFor(QStringLiteral("tmdb:movie:278"), QString(), QStringLiteral("movie")), 2000,
            [&](const RequestLookup& r) { l = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        CHECK(!l.ok && l.failure == Failure::NotConfigured);
        JellyseerrStore::save(cfg);
    }

    // ======================================================================================================
    // 8b. A HOVER ASKS NOBODY (issue #315)
    // ======================================================================================================
    // #109 fetched the status "on view", and the themed surface builds a title's detail data for two quite
    // different reasons: a detail view being OPENED, and a row being HOVERED (the metadata path re-derives
    // the action row when a stream id bridges in). Both read as "view", so scrolling a shelf of requestable
    // titles issued one GET per row — bounded and read-only, and still somebody's server answering to cursor
    // movement.
    //
    // Driven here through the SAME two gates the surface applies, over the real backend and the fixture
    // service: requests::fetchesStatus(trigger), then the once-per-title-per-session set. Counted at the
    // stub, so what is asserted is requests that were or were not MADE, not a predicate agreeing with
    // itself.
    {
        // The rule, first and alone.
        CHECK(requests::fetchesStatus(requests::StatusTrigger::Hover) == false);
        CHECK(requests::fetchesStatus(requests::StatusTrigger::DetailOpened) == true);

        QSet<QString> asked;   // MainWindow::requestAsked_ — the second gate, unchanged by this issue
        auto onDetailBuild = [&](const QString& id, requests::StatusTrigger trigger) {
            // requestStateFor's cache-only half runs whatever the trigger is: the pill is always drawn.
            const MediaRef ref = requests::refFor(id, QString(), QStringLiteral("movie"));
            if (!ref.ok()) return;
            if (!requests::fetchesStatus(trigger)) return;      // a hover stops HERE
            if (asked.contains(ref.key())) return;
            asked.insert(ref.key());
            bool done = false;
            requests::configuredBackend()->lookup(ref, 6000, [&](const RequestLookup&) { done = true; });
            CHECK(waitFor([&] { return done; }));
        };

        const int before = stub.seen.size();
        // A SHELF SCROLLED PAST: twelve requestable rows under the cursor, one after another, and one of
        // them hovered repeatedly the way a cursor that wanders back does.
        QStringList shelf;
        for (int i = 0; i < 12; ++i) shelf << QStringLiteral("tmdb:movie:%1").arg(600 + i);
        for (const QString& id : shelf) onDetailBuild(id, requests::StatusTrigger::Hover);
        for (int again = 0; again < 3; ++again)
            onDetailBuild(shelf.value(4), requests::StatusTrigger::Hover);
        CHECK(stub.seen.size() == before);          // THE ASSERTION: not one request left the box
        CHECK(asked.isEmpty());                     // ...and nothing was recorded as asked, either

        // ...and then one of them is actually OPENED. Now, and only now, the service is asked — once.
        const int beforeOpen = stub.countOf(QStringLiteral("GET"), QStringLiteral("/api/v1/movie"));
        onDetailBuild(shelf.value(4), requests::StatusTrigger::DetailOpened);
        CHECK(stub.countOf(QStringLiteral("GET"), QStringLiteral("/api/v1/movie")) == beforeOpen + 1);
        // The card is re-pushed several times for one open page (a correction, a late answer); the
        // once-per-session gate is what keeps that free, and it still holds.
        onDetailBuild(shelf.value(4), requests::StatusTrigger::DetailOpened);
        onDetailBuild(shelf.value(4), requests::StatusTrigger::DetailOpened);
        CHECK(stub.countOf(QStringLiteral("GET"), QStringLiteral("/api/v1/movie")) == beforeOpen + 1);
        // Backing out to the shelf and hovering the OPENED title again is still not a reason to ask.
        const int afterOpen = stub.seen.size();
        onDetailBuild(shelf.value(4), requests::StatusTrigger::Hover);
        onDetailBuild(shelf.value(7), requests::StatusTrigger::Hover);
        CHECK(stub.seen.size() == afterOpen);
    }

    // ======================================================================================================
    // 9. THE BYTE SCAN — the assertion the whole credential rule reduces to
    // ======================================================================================================
    {
        // Record a shelf row, exactly as a real press would, so the store this scans is a populated one.
        requests::StoredRequest r;
        r.key = QStringLiteral("tmdb:movie:278");
        r.backendId = requests::configuredBackend()->id();
        r.title = QStringLiteral("The Film");
        r.mediaType = requests::kMovie();
        r.tmdb = QStringLiteral("278");
        r.requestedAt = 1234;
        r.status = requests::statusToken(Availability::Pending);
        RequestStore::add(r);
        CHECK(RequestStore::count() == 1);

        QSettings probe(iniPath, QSettings::IniFormat);
        // THE KEY IS IN EXACTLY ONE INI ROW — the device-local one this store owns — and in no other.
        int hits = 0;
        QString hitKey;
        for (const QString& k : probe.allKeys())
            if (probe.value(k).toString().contains(QString::fromLatin1(kApiKey))) { ++hits; hitKey = k; }
        CHECK(hits == 1);
        CHECK(hitKey.startsWith(QStringLiteral("jellyseerr/")));
        CHECK(hitKey.endsWith(QStringLiteral("/apiKey")));
        // ...and specifically NOT in anything the request store wrote.
        for (const QString& k : probe.allKeys())
            if (k.startsWith(QStringLiteral("requests/")))
                CHECK(!probe.value(k).toString().contains(QString::fromLatin1(kApiKey)));

        // AND THE PREFIX IS THE ONE CloudSync CARVES OUT. Asserted as a string here (CloudSync is not
        // linked into this probe — probe_cloudmerge owns the carve-out itself), so a store that quietly
        // moved to another prefix fails here rather than starting to sync a credential.
        CHECK(hitKey.startsWith(QStringLiteral("jellyseerr/")));

        // Nothing anywhere else under the temp tree holds it either — no cache, no sidecar, no log.
        QDirIterator it(tmp.path(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString path = it.next();
            if (QFileInfo(path).absoluteFilePath() == QFileInfo(iniPath).absoluteFilePath()) continue;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) continue;
            CHECK(!f.readAll().contains(kApiKey));
        }
    }

    if (g_fail == 0) std::printf("REQUESTS-OK\n");
    else             std::printf("probe_requests: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
