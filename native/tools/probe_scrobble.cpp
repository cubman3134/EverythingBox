// Headless test for MUSIC SCROBBLING (issue #192, increment 1). Prints SCROBBLE-OK.
//
// WHAT IS ACTUALLY BEING PINNED, and why each section exists rather than what it calls.
//
// §1 THE THRESHOLD. The one number in this feature that fails silently in both directions: too low and the
//    app scrobbles tracks the listener skipped, permanently, into a history built up over a decade; too high
//    and long tracks never scrobble and it looks like the network. Nothing in the app notices either. Every
//    case in the issue's own list is here, including the two that are only reachable by arithmetic (a
//    one-second track's half is zero; an unknown length has no half at all).
//
// §2 WHAT COUNTS. The untagged clause is the irreversible one — a submitted "Unknown Artist" cannot be
//    cleaned up from this app — and the spoken clause is what keeps a twelve-hour audiobook out of a listening
//    history. Both are asserted as VERDICTS rather than as booleans, so "off" cannot masquerade as "untagged".
//
// §3 THE ACCUMULATOR, which is where a naive implementation is wrong in four different ways at once. A seek
//    past the threshold is not listening; a pause is not listening; a skip one second short is not a listen;
//    and a GAPLESS boundary arrives with no reload, no play sink and a position that jumps backwards to zero.
//    §3g plays a whole three-track album through the real boundary sequence and asserts what a listening
//    history would end up containing.
//
// §4 THE KEY FAMILIES. Which half of the feature a settings Discard may revert, and — because the token is a
//    user's own secret — that neither half can ride a sync bundle. Asserted through SettingsTxn's real
//    predicate, not through a restatement of it.
//
// §5 THE OFFLINE QUEUE: FIFO order, timestamp preservation across a save/load, the cap dropping from the
//    right end, and a partial acceptance removing exactly the prefix that was accepted.
//
// §6 END TO END AGAINST A FAKE ENDPOINT. A real ListenBrainzClient pointed at an in-process QTcpServer on
//    127.0.0.1 that REFUSES first and then accepts — the offline-then-reconnect story, played for real. It
//    asserts the wire payload the service would receive: `listen_type`, the backdated `listened_at` values in
//    the order they happened, and that a "now playing" carries no timestamp. NO ACCOUNT AND NO REAL TOKEN IS
//    INVOLVED: the token in this file is the literal string "probe-not-a-real-token", it never leaves the
//    loopback socket, and §6e asserts that it appears in no message the app would ever show or log.
//
// §7 LAST.FM (increment 2), the second implementation of the same seam, and the first test of whether that
//    seam was the right shape. Four things here exist nowhere else in the feature and each is a way to be
//    wrong silently: the SIGNATURE (pinned against the spec's own worked example string, and re-verified on
//    the wire by a fake that recomputes it the way the service does); the fact that HTTP 200 IS NOT SUCCESS
//    (Last.fm answers most refusals with a 200 and an error object, so a status-only client loses the
//    listens it thinks it delivered); the BUILD-TIME APPLICATION KEY, whose de-obfuscation mirrors a CMake
//    script that no compiler checks against it; and the 30-SECOND rule, which is Last.fm's alone and must
//    not reach ListenBrainz. Then §7m runs BOTH providers at once, which is what the per-provider queue was
//    built for. Again no account and no real credential: the app key and secret come from
//    tools/fixtures/lastfm/BuiltinSecrets.h and are literally "probe-not-a-real-key" and
//    "probe-not-a-real-secret", the client's test hook refuses any endpoint that is not http on loopback,
//    and §7l asserts that none of the three credentials appears in anything the app would show or log.
#include "Scrobble.h"
#include "ScrobbleQueue.h"
#include "Scrobbler.h"
#include "ListenBrainzClient.h"
#include "LastFmClient.h"
#include "BuiltinSecretBlob.h"
#include "ScrobbleProvider.h"
#include "ScrobbleRemoval.h"            // what a removal TELLS the user (#337) - pure, so it is assertable
#include "Subsonic.h"                   // the protocol the third provider speaks (#193 increment 6)
#include "SubsonicScrobbleProvider.h"
#include "SubsonicServerStore.h"
#include "Settings.h"
#include "SettingsTxn.h"
#include "ProfileStore.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QDateTime>
#include <QUrl>
#include <QUrlQuery>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

using Scrobble::Kind;
using Scrobble::Origin;
using Scrobble::Play;
using Scrobble::Policy;
using Scrobble::Track;
using Scrobble::Verdict;
using Scrobble::Watch;

// A token that is not a token. It exists so the probe can drive the CONFIGURED path; it is meaningless to any
// real service and is only ever sent to a socket this process opened on 127.0.0.1.
static const char* kFakeToken = "probe-not-a-real-token";

static Track musicTrack(const QString& artist, const QString& title, int durationSec)
{
    Track t;
    t.artist = artist; t.title = title; t.album = QStringLiteral("A Record");
    t.durationSec = durationSec;
    return t;
}

// A spoken-word item, which BOTH services would accept as a track and neither should be given one. Long
// enough that the four-minute cap is the arm under test rather than the half arm.
static Track audiobookTrack()
{
    Track t;
    t.artist = QStringLiteral("A Narrator");
    t.title  = QStringLiteral("Chapter One");
    t.durationSec = 40000;
    t.kind = Kind::Spoken;
    return t;
}

// =========================================================================================================
// THE FAKE LAST.FM. It answers the five methods this app calls, records what it was given, and — the part
// that matters — RECOMPUTES api_sig over every signed call the way the real service does. A fake that only
// echoed 200 would let a wrong signature through, and a wrong signature is the single most likely way this
// provider is broken: it fails with "Invalid method signature", which names nothing and reads as a bad key.
//
// It also refuses the way Last.fm refuses: HTTP 200 with {"error":N,"message":"…"} in the body. That is not
// a detail. A client that reads only the status code treats every one of those as an accepted batch.
// =========================================================================================================
class FakeLastFm : public QObject
{
public:
    QTcpServer server;
    int refuseWith         = 0;   // a Last.fm ERROR CODE, answered with HTTP 200 as the service does
    int refuseSessionTimes = 0;   // answer auth.getSession with error 14 this many times before letting it through
    int sessionCalls       = 0;
    int badSignatures      = 0;   // signed calls whose api_sig did not recompute. MUST stay 0.
    QVector<QMap<QString, QString>> scrobbles, nowPlaying, loves;

    FakeLastFm() { connect(&server, &QTcpServer::newConnection, this, &FakeLastFm::onConn); }
    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    QString root() const
    { return QStringLiteral("http://127.0.0.1:") + QString::number(server.serverPort()); }

private:
    static QMap<QString, QString> parseParams(const QString& encoded)
    {
        QMap<QString, QString> out;
        QUrlQuery q;
        q.setQuery(encoded);
        const auto items = q.queryItems(QUrl::FullyDecoded);
        for (const auto& kv : items) out.insert(kv.first, kv.second);
        return out;
    }

    void onConn()
    {
        while (QTcpSocket* c = server.nextPendingConnection())
            connect(c, &QTcpSocket::readyRead, this, [this, c] { onData(c); });
    }

    void onData(QTcpSocket* c)
    {
        QByteArray req = c->readAll();
        const int hdrEnd = req.indexOf("\r\n\r\n");
        if (hdrEnd < 0) return;
        int wantLen = 0;
        for (const QByteArray& line : req.left(hdrEnd).split('\n'))
            if (line.toLower().startsWith("content-length:")) wantLen = line.mid(15).trimmed().toInt();
        while (req.size() - (hdrEnd + 4) < wantLen && c->waitForReadyRead(2000)) req += c->readAll();

        const QByteArray head = req.left(hdrEnd);
        const QByteArray body = req.mid(hdrEnd + 4);
        const QList<QByteArray> reqLine = head.split('\n').value(0).trimmed().split(' ');
        const QString path = QString::fromUtf8(reqLine.value(1));
        // A GET carries its parameters in the query; a POST in the body. Both are signed the same way.
        const int qm = path.indexOf(QLatin1Char('?'));
        const QString encoded = body.isEmpty() ? (qm >= 0 ? path.mid(qm + 1) : QString())
                                               : QString::fromUtf8(body);
        const QMap<QString, QString> p = parseParams(encoded);

        // VERIFY THE SIGNATURE, exactly as Last.fm does: over every parameter except `format` and `api_sig`,
        // sorted by name, with the shared secret appended. This is the assertion the whole provider rests on.
        QMap<QString, QString> signedSet = p;
        const QString sig = signedSet.take(QStringLiteral("api_sig"));
        signedSet.remove(QStringLiteral("format"));
        if (sig.isEmpty() || sig != LastFm::signature(signedSet, LastFmClient::appSecret()))
            ++badSignatures;

        const QString method = p.value(QStringLiteral("method"));

        if (method == QLatin1String("auth.getToken"))
        { reply(c, 200, "{\"token\":\"probe-request-token\"}"); return; }

        if (method == QLatin1String("auth.getSession"))
        {
            ++sessionCalls;
            if (sessionCalls <= refuseSessionTimes)
            {   // "This token has not been authorized" — the user has not pressed Yes yet.
                reply(c, 200, "{\"error\":14,\"message\":\"This token has not been authorized\"}");
                return;
            }
            reply(c, 200,
                  "{\"session\":{\"name\":\"probe-listener\",\"key\":\"probe-session-key\",\"subscriber\":0}}");
            return;
        }

        if (refuseWith != 0)
        {   // The way the real service refuses: a 200 with an error object. Nothing is recorded.
            reply(c, 200, "{\"error\":" + QByteArray::number(refuseWith)
                          + ",\"message\":\"the fake service is refusing\"}");
            return;
        }

        if (method == QLatin1String("track.scrobble"))
        { scrobbles.push_back(p); reply(c, 200, "{\"scrobbles\":{\"@attr\":{\"accepted\":1,\"ignored\":0}}}"); return; }
        if (method == QLatin1String("track.updateNowPlaying"))
        { nowPlaying.push_back(p); reply(c, 200, "{\"nowplaying\":{}}"); return; }
        if (method == QLatin1String("track.love") || method == QLatin1String("track.unlove"))
        { loves.push_back(p); reply(c, 200, "{}"); return; }

        reply(c, 200, "{\"error\":3,\"message\":\"Invalid Method\"}");
    }

    static void reply(QTcpSocket* c, int status, const QByteArray& body)
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                         "Connection: close\r\n\r\n" + body;
        c->write(out);
        c->flush();
        c->disconnectFromHost();
    }
};

// ---------------------------------------------------------------------------------------------------------
// The fake ListenBrainz. Small on purpose: it answers exactly the two endpoints the client posts to, records
// what it was sent, and can be told to refuse.
// ---------------------------------------------------------------------------------------------------------
class FakeService : public QObject
{
public:
    QTcpServer server;
    int    refuseWith = 0;              // 0 == accept; otherwise the HTTP status to answer with
    QVector<QJsonObject> submissions;   // every body posted to /1/submit-listens, in arrival order
    QStringList          paths;         // every request path handled

    FakeService() { connect(&server, &QTcpServer::newConnection, this, &FakeService::onConn); }
    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    QString root() const
    { return QStringLiteral("http://127.0.0.1:") + QString::number(server.serverPort()); }

    // Every completed listen the service has been given, flattened, in arrival order.
    QVector<QJsonObject> acceptedListens() const
    {
        QVector<QJsonObject> out;
        for (const QJsonObject& b : submissions)
        {
            if (b.value(QStringLiteral("listen_type")).toString() == QLatin1String("playing_now")) continue;
            for (const QJsonValue& v : b.value(QStringLiteral("payload")).toArray())
                out.push_back(v.toObject());
        }
        return out;
    }

private:
    void onConn()
    {
        while (QTcpSocket* c = server.nextPendingConnection())
            connect(c, &QTcpSocket::readyRead, this, [this, c] { onData(c); });
    }
    void onData(QTcpSocket* c)
    {
        QByteArray req = c->readAll();
        // Read the rest of the body if the headers promised more than arrived in the first packet.
        const int hdrEnd = req.indexOf("\r\n\r\n");
        if (hdrEnd < 0) return;
        int wantLen = 0;
        for (const QByteArray& line : req.left(hdrEnd).split('\n'))
            if (line.toLower().startsWith("content-length:")) wantLen = line.mid(15).trimmed().toInt();
        while (req.size() - (hdrEnd + 4) < wantLen && c->waitForReadyRead(2000)) req += c->readAll();

        const QByteArray head = req.left(hdrEnd);
        const QByteArray body = req.mid(hdrEnd + 4);
        const QList<QByteArray> reqLine = head.split('\n').value(0).trimmed().split(' ');
        const QString path = QString::fromUtf8(reqLine.value(1));
        paths << path;

        if (path.startsWith(QStringLiteral("/1/submit-listens")))
        {
            const QJsonObject o = QJsonDocument::fromJson(body).object();
            if (refuseWith == 0) submissions.push_back(o);
            reply(c, refuseWith ? refuseWith : 200,
                  refuseWith ? QByteArray("{\"error\":\"the fake service is refusing\"}") : QByteArray("{}"));
            return;
        }
        reply(c, 404, "{\"error\":\"no such endpoint\"}");
    }
    static void reply(QTcpSocket* c, int status, const QByteArray& body)
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                         "Connection: close\r\n\r\n" + body;
        c->write(out);
        c->flush();
        c->disconnectFromHost();
    }
};

// ==================================================================================================
// A FAKE SUBSONIC SERVER (issue #193, increment 6)
// ==================================================================================================
// Everything a Subsonic server answers arrives as HTTP 200 with an envelope inside, INCLUDING every error -
// which is the trap the whole protocol sets and the reason this fake answers 200 to everything. A fake that
// used HTTP statuses would let a status-only client pass, which is exactly the bug being defended against.
//
// NO REAL SERVER AND NO REAL ACCOUNT. It listens on 127.0.0.1 on an ephemeral port, the password below is
// the literal string "probe-not-a-real-password", and 8g asserts that neither it nor the token derived from
// it appears in anything the app would show, log or persist.
class FakeSubsonic : public QObject
{
public:
    QTcpServer server;
    QByteArray answerWith;              // the envelope to reply with; empty == a plain ok
    // ...and a SCRIPT, consumed one entry per request before answerWith is consulted (issue #337). It exists
    // for the one outcome a single fixed answer cannot express: a submission that PARTLY succeeded, which is
    // the batch that landed followed by the batch that did not. An empty entry means "a plain ok".
    QVector<QByteArray> scripted;
    QStringList methods;                // "scrobble", "star", "unstar" ... in arrival order
    QVector<QUrlQuery> queries;         // every request's query, in arrival order

    FakeSubsonic() { connect(&server, &QTcpServer::newConnection, this, &FakeSubsonic::onConn); }
    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    QString root() const
    { return QStringLiteral("http://127.0.0.1:") + QString::number(server.serverPort()); }
    void forget() { methods.clear(); queries.clear(); }

    // Every value a repeated parameter carried on the LAST request, in order.
    QStringList valuesOf(const QString& key) const
    {
        QStringList out;
        if (queries.isEmpty()) return out;
        for (const auto& kv : queries.last().queryItems())
            if (kv.first == key) out << kv.second;
        return out;
    }
    QString lastValue(const QString& key) const
    { return queries.isEmpty() ? QString() : queries.last().queryItemValue(key); }

private:
    void onConn()
    {
        while (QTcpSocket* c = server.nextPendingConnection())
            connect(c, &QTcpSocket::readyRead, this, [this, c] { onData(c); });
    }
    void onData(QTcpSocket* c)
    {
        const QByteArray req = c->readAll();
        const int hdrEnd = req.indexOf("\r\n\r\n");
        if (hdrEnd < 0) return;
        const QList<QByteArray> reqLine = req.left(hdrEnd).split('\n').value(0).trimmed().split(' ');
        const QUrl u(QString::fromUtf8(reqLine.value(1)));
        QString method = u.path();
        method = method.mid(method.lastIndexOf(QLatin1Char('/')) + 1);
        if (method.endsWith(QStringLiteral(".view"))) method.chop(5);
        methods << method;
        queries << QUrlQuery(u.query());

        QByteArray chosen = answerWith;
        if (!scripted.isEmpty()) chosen = scripted.takeFirst();
        const QByteArray body = chosen.isEmpty()
            ? QByteArray("<subsonic-response status=\"ok\" version=\"1.16.1\"/>")
            : chosen;
        const QByteArray out = "HTTP/1.1 200 OK\r\n"                  // ALWAYS 200 - see the note above
                               "Content-Type: text/xml\r\n"
                               "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + body;
        c->write(out);
        c->flush();
        c->disconnectFromHost();
    }
};

static Track subsonicTrack(const QString& sourceId, const QString& title, int durationSec)
{
    Track t = musicTrack(QStringLiteral("Amber"), title, durationSec);
    t.origin   = Scrobble::Origin::Server;
    t.sourceId = sourceId;
    return t;
}

// Play a track past its threshold through a Scrobbler, the way the host reports it.
static void playThrough(Scrobbler& s, const Track& t, int seconds)
{
    s.trackStarted(t);
    for (int i = 0; i <= seconds; ++i) s.positionTick(double(i));
    s.playbackStopped();
}

// Spin the event loop until `pred` holds or the deadline passes. Returns whether it held.
static bool spinUntil(std::function<bool()> pred, int msec = 5000)
{
    QDeadlineTimer dl(msec);
    while (!pred() && !dl.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return pred();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // =====================================================================================================
    // §1 THE THRESHOLD — half the track, or four minutes, whichever comes first
    // =====================================================================================================
    {
        // The issue's own worked cases. The oracle beside each is what a listening history would contain if
        // the number were wrong, not a restatement of the arithmetic.
        CHECK(Scrobble::thresholdSec(30) == 15,
              "threshold: a 30-second track needs 15 seconds");
        CHECK(Scrobble::thresholdSec(180) == 90,
              "threshold: a 3-minute track needs 90 seconds");
        CHECK(Scrobble::thresholdSec(600) == 240,
              "threshold: a 10-minute track is CAPPED at 4 minutes, not 5 — half would be 300");
        CHECK(Scrobble::thresholdSec(2400) == 240,
              "threshold: a 40-minute side needs 4 minutes, not 20 — without the cap, long-form music would "
              "never scrobble at all");
        CHECK(Scrobble::thresholdSec(0) == 240,
              "threshold: an UNKNOWN length falls back to the 4-minute cap. The alternative (treat unknown as "
              "zero) would scrobble every stream the instant it started");
        CHECK(Scrobble::thresholdSec(-5) == 240, "threshold: a nonsense duration reads as unknown");

        // THE EXACT BOUNDARY, both sides. An off-by-one here is invisible: it scrobbles tracks the listener
        // skipped, and nothing anywhere reports it.
        CHECK(Scrobble::thresholdSec(479) == 239, "threshold: one second under the cap's crossover is half");
        CHECK(Scrobble::thresholdSec(480) == 240, "threshold: at the crossover, half and the cap agree");
        CHECK(Scrobble::thresholdSec(481) == 240, "threshold: one second over, the cap takes it");

        // THE FLOOR. A one-second track's half is zero, and a zero threshold is passed before a single sample
        // has played — the same off-by-one arriving from the bottom instead of the top.
        CHECK(Scrobble::thresholdSec(1) == 1,
              "threshold: a 1-second track still needs a second of PLAY, never zero");
        CHECK(Scrobble::thresholdSec(2) == 1, "threshold: and a 2-second track needs its honest one second");
        CHECK(Scrobble::kCapSec == 240, "threshold: the cap is four minutes");
    }

    // =====================================================================================================
    // §2 WHAT COUNTS
    // =====================================================================================================
    {
        Policy on;  on.enabled = true;
        Policy off; off.enabled = false;

        const Track good = musicTrack(QStringLiteral("Nina Simone"), QStringLiteral("Sinnerman"), 600);
        CHECK(Scrobble::verdictFor(good, on) == Verdict::Submit, "counts: a tagged music track submits");
        CHECK(Scrobble::verdictFor(good, off) == Verdict::SkipOff,
              "counts: with scrobbling off, nothing is sent — and the reason says OFF, not 'untagged'");

        Track noArtist = good; noArtist.artist.clear();
        CHECK(Scrobble::verdictFor(noArtist, on) == Verdict::SkipUntagged,
              "counts: a file with no artist is SKIPPED. Submitting it as 'Unknown Artist' cannot be undone "
              "from this app");
        Track noTitle = good; noTitle.title.clear();
        CHECK(Scrobble::verdictFor(noTitle, on) == Verdict::SkipUntagged, "counts: nor with no title");
        Track blankArtist = good; blankArtist.artist = QStringLiteral("   ");
        CHECK(Scrobble::verdictFor(blankArtist, on) == Verdict::SkipUntagged,
              "counts: whitespace is not an artist name");

        Track book = good; book.kind = Kind::Spoken;
        CHECK(Scrobble::verdictFor(book, on) == Verdict::SkipSpoken,
              "counts: an audiobook is excluded by default — a twelve-hour 'track' is noise in a history");
        Policy spoken = on; spoken.includeSpoken = true;
        CHECK(Scrobble::verdictFor(book, spoken) == Verdict::Submit,
              "counts: ...and the per-source toggle is what covers anyone who disagrees");
        // ORDER: an untagged audiobook with scrobbling off reports OFF. The most specific TRUE statement, not
        // the first test that happens to fire.
        Track worst = book; worst.artist.clear();
        CHECK(Scrobble::verdictFor(worst, off) == Verdict::SkipOff,
              "counts: with scrobbling off, that is the answer whatever else is wrong with the track");

        // THE DOUBLE-COUNT ARM. Unused in anger today (the server records locally and forwards nothing), and
        // the whole point of it existing now is that turning it on later is one flag rather than a redesign.
        Track fromServer = good; fromServer.origin = Origin::Server;
        CHECK(Scrobble::verdictFor(fromServer, on) == Verdict::Submit,
              "double-count: today the server forwards nothing, so the client is the only one counting");
        Policy forwarding = on; forwarding.serverForwards = true;
        CHECK(Scrobble::verdictFor(fromServer, forwarding) == Verdict::SkipServerForwards,
              "double-count: the day the server forwards its own plays, ONE flag stops the client counting "
              "them a second time");
        CHECK(Scrobble::verdictFor(good, forwarding) == Verdict::Submit,
              "double-count: ...and a LOCAL play is unaffected — the server never saw it");
    }

    // =====================================================================================================
    // §3 THE ACCUMULATOR
    // =====================================================================================================
    Policy on; on.enabled = true;
    {
        // §3a played straight through: crosses at the threshold, exactly once.
        Watch w;
        Scrobble::begin(w, musicTrack(QStringLiteral("A"), QStringLiteral("T"), 180), 1000, on);
        int crossings = 0;
        for (int sec = 0; sec <= 120; ++sec) if (Scrobble::advance(w, double(sec))) ++crossings;
        CHECK(crossings == 1, "play: a track crossing its threshold reports EXACTLY once — without the latch, "
                              "every later tick would submit the same track again");
        CHECK(w.playedSec >= 90.0 && w.playedSec <= 121.0, "play: the heard time is the time actually played");
        CHECK(!Scrobble::finish(w), "play: and the boundary owes nothing more for a track already sent");

        // §3b the same track SKIPPED one second short. This is the case the whole threshold exists for.
        Watch s;
        Scrobble::begin(s, musicTrack(QStringLiteral("A"), QStringLiteral("T"), 180), 1000, on);
        bool sent = false;
        for (int sec = 0; sec <= 89; ++sec) if (Scrobble::advance(s, double(sec))) sent = true;
        CHECK(!sent, "skip: 89 seconds of a 3-minute track is not a listen");
        CHECK(!Scrobble::finish(s), "skip: and the boundary does not quietly submit it either");

        // §3c SEEKED past the threshold rather than played to it. Dragging the seek bar across an album must
        // not scrobble the album.
        Watch k;
        Scrobble::begin(k, musicTrack(QStringLiteral("A"), QStringLiteral("T"), 180), 1000, on);
        Scrobble::advance(k, 1.0);
        const bool seekFired = Scrobble::advance(k, 175.0);   // one enormous step
        CHECK(!seekFired, "seek: a jump to 2:55 credits NOTHING — the position says the threshold is passed "
                          "and nothing has been heard");
        CHECK(k.playedSec < 2.0, "seek: ...and no heard time was banked by the jump");
        CHECK(!Scrobble::finish(k), "seek: so the boundary owes nothing");

        // §3d PAUSED and resumed. A paused track reports the same position repeatedly and must credit nothing,
        // with no pause flag plumbed anywhere.
        Watch p;
        Scrobble::begin(p, musicTrack(QStringLiteral("A"), QStringLiteral("T"), 180), 1000, on);
        for (int sec = 0; sec <= 50; ++sec) Scrobble::advance(p, double(sec));
        const double atPause = p.playedSec;
        for (int i = 0; i < 200; ++i) Scrobble::advance(p, 50.0);   // paused: the same number, 200 times
        CHECK(qFuzzyCompare(p.playedSec + 1.0, atPause + 1.0),
              "pause: a paused track banks nothing, however long it is paused for");
        bool resumedFired = false;
        for (int sec = 51; sec <= 95; ++sec) if (Scrobble::advance(p, double(sec))) resumedFired = true;
        CHECK(resumedFired, "pause: and resuming carries the SAME accumulated total across the threshold");

        // §3e a track of UNKNOWN length. Four minutes of it, and not a second sooner.
        Watch u;
        Scrobble::begin(u, musicTrack(QStringLiteral("A"), QStringLiteral("Stream"), 0), 1000, on);
        bool early = false;
        for (int sec = 0; sec <= 239; ++sec) if (Scrobble::advance(u, double(sec))) early = true;
        CHECK(!early, "unknown: 239 seconds of a track of unknown length is not yet a listen");
        bool late = false;
        for (int sec = 240; sec <= 245; ++sec) if (Scrobble::advance(u, double(sec))) late = true;
        CHECK(late, "unknown: four minutes of it is");

        // §3f AN INELIGIBLE TRACK accumulates nothing that can ever be submitted, however long it plays.
        Watch bad;
        Track untagged = musicTrack(QString(), QStringLiteral("01"), 180);
        Scrobble::begin(bad, untagged, 1000, on);
        bool badFired = false;
        for (int sec = 0; sec <= 200; ++sec) if (Scrobble::advance(bad, double(sec))) badFired = true;
        CHECK(!badFired && !Scrobble::finish(bad),
              "untagged: a file with no artist plays to the end and submits nothing");

        // §3g THE GAPLESS ALBUM. Three tracks, played the way mpv actually plays them under gapless: the
        // position runs up inside a track and then RESETS TO ZERO at a boundary that involves no reload, no
        // file open and no trip through the play sink. The middle track is skipped early. What a listening
        // history should end up holding is tracks 1 and 3 and not track 2.
        //
        // This is the case a per-track hook wired to the play sink gets wrong in the worst way: it never fires
        // at all, so an album played end to end scrobbles once (or not at all) instead of ten times.
        QVector<Play> landed;
        auto boundary = [&landed](Watch& w) { if (Scrobble::finish(w)) landed.push_back({ w.track, w.startedAt }); };

        Watch g;
        const Track t1 = musicTrack(QStringLiteral("Artist"), QStringLiteral("One"), 200);
        const Track t2 = musicTrack(QStringLiteral("Artist"), QStringLiteral("Two"), 200);
        const Track t3 = musicTrack(QStringLiteral("Artist"), QStringLiteral("Three"), 200);

        Scrobble::begin(g, t1, 5000, on);
        for (int sec = 0; sec <= 199; ++sec)
            if (Scrobble::advance(g, double(sec))) landed.push_back({ g.track, g.startedAt });
        // THE BOUNDARY: no reload. The host reports the new track, and the position's next report is 0.
        boundary(g);
        Scrobble::begin(g, t2, 5210, on);
        for (int sec = 0; sec <= 40; ++sec) Scrobble::advance(g, double(sec));   // skipped at 0:40
        boundary(g);
        Scrobble::begin(g, t3, 5260, on);
        for (int sec = 0; sec <= 199; ++sec)
            if (Scrobble::advance(g, double(sec))) landed.push_back({ g.track, g.startedAt });
        boundary(g);

        CHECK(landed.size() == 2, "gapless: an album whose middle track was skipped lands TWO listens");
        CHECK(landed.size() == 2 && landed[0].track.title == QLatin1String("One")
              && landed[1].track.title == QLatin1String("Three"),
              "gapless: ...and they are the two that were played, in the order they were played");
        CHECK(landed.size() == 2 && landed[0].listenedAt == 5000 && landed[1].listenedAt == 5260,
              "gapless: each carries the time ITS OWN track started, not the time the boundary was crossed");
        // The position resetting to zero across the boundary must credit the new track NOTHING for the jump.
        CHECK(g.playedSec <= 201.0, "gapless: the 200 -> 0 reset is never credited as play time");
    }

    // =====================================================================================================
    // §4 THE KEY FAMILIES
    // =====================================================================================================
    {
        const QString tokenKey = Scrobble::settingsKeyPrefix() + QStringLiteral("default/lb/token");
        const QString enabled  = Scrobble::settingsKeyPrefix() + QStringLiteral("default/enabled");
        const QString queueK   = ScrobbleQueue::queueKey(QStringLiteral("default"), QStringLiteral("listenbrainz"));
        const QString countK   = ScrobbleQueue::counterKey(QStringLiteral("default"), QStringLiteral("listenbrainz"));

        // BOTH families are device-local: a token must never ride a sync bundle to another machine, and the
        // counter/queue are this device's own accumulators.
        CHECK(Scrobble::isDeviceLocalKey(tokenKey), "keys: the token never syncs");
        CHECK(Scrobble::isDeviceLocalKey(enabled), "keys: nor does the on/off that is bound to it");
        CHECK(Scrobble::isDeviceLocalKey(queueK), "keys: nor this device's undelivered listens");
        CHECK(Scrobble::isDeviceLocalKey(countK), "keys: nor its delivered counter");
        CHECK(!Scrobble::isDeviceLocalKey(QStringLiteral("subs/osApiKey")),
              "keys: and the prefix has not been widened into a sweep of the whole tree");

        // THE SPLIT A DISCARD TURNS ON, asserted through SettingsTxn's REAL predicate. Both halves, because a
        // prefix that covered both would silently make the token undiscardable and nothing would say so.
        CHECK(SettingsTxn::inScope(tokenKey),
              "txn: the TOKEN is in scope — paste the wrong one, press Discard, get the old one back");
        CHECK(SettingsTxn::inScope(enabled), "txn: so is the on/off");
        CHECK(!SettingsTxn::inScope(queueK),
              "txn: the offline QUEUE is not — a Discard must never delete listens that already happened");
        CHECK(!SettingsTxn::inScope(countK),
              "txn: nor the counter, which playback moves while a settings panel is open");
        CHECK(Scrobble::isBackgroundStateKey(queueK) && !Scrobble::isBackgroundStateKey(tokenKey),
              "txn: the two families are told apart by their TOP-LEVEL prefix, so no profile id can collide "
              "with the discriminator");
    }

    // =====================================================================================================
    // §5 THE OFFLINE QUEUE
    // =====================================================================================================
    {
        const QString pid = QStringLiteral("probe");
        ScrobbleQueue::clear(pid);

        // Timestamps are the whole point. A listen delivered a day late must still say WHEN it happened.
        for (int i = 0; i < 5; ++i)
        {
            Play p;
            p.track = musicTrack(QStringLiteral("Artist"), QStringLiteral("Track %1").arg(i), 200);
            p.listenedAt = 1700000000 + i * 200;
            ScrobbleQueue::append(pid, p);
        }
        CHECK(ScrobbleQueue::count(pid) == 5, "queue: five listens are held");
        QVector<Play> head = ScrobbleQueue::head(pid, 3);
        CHECK(head.size() == 3 && head[0].listenedAt == 1700000000 && head[2].listenedAt == 1700000400,
              "queue: the oldest go first, in the order they happened — a service de-duplicating on "
              "(artist, title, listened_at) sees a monotone batch");
        CHECK(head.size() == 3 && head[1].track.title == QLatin1String("Track 1"),
              "queue: ...and the tags survive the round trip through disk");

        // A PARTIAL acceptance removes exactly the prefix that was accepted, and leaves the rest in order.
        ScrobbleQueue::dropFront(pid, 3);
        const QVector<Play> rest = ScrobbleQueue::head(pid, 10);
        CHECK(rest.size() == 2 && rest[0].listenedAt == 1700000600,
              "queue: dropping the accepted prefix leaves the rest, still oldest-first");

        // THE CAP drops from the FRONT. A cap that dropped the newest would make a full queue permanently
        // useless: the newest listening is the listening most likely to still be worth submitting.
        QVector<Play> big;
        for (int i = 0; i < ScrobbleQueue::kMaxQueued + 7; ++i)
        {
            Play p; p.track = musicTrack(QStringLiteral("A"), QStringLiteral("T"), 200);
            p.listenedAt = 1600000000 + i;
            big.push_back(p);
        }
        const int lost = ScrobbleQueue::applyCap(big);
        CHECK(lost == 7 && big.size() == ScrobbleQueue::kMaxQueued, "queue: the cap sheds exactly the excess");
        CHECK(big.first().listenedAt == 1600000007,
              "queue: ...from the OLDEST end, so the newest listening is what survives");

        // A row with no timestamp cannot be backdated and would land at 'now' — the one outcome the whole
        // queue exists to prevent. It is dropped on read rather than delivered as a lie.
        const QByteArray forged = QByteArray("[{\"a\":\"X\",\"t\":\"Y\"},{\"ts\":123,\"a\":\"X\",\"t\":\"Y\"}]");
        CHECK(ScrobbleQueue::decode(forged).size() == 1,
              "queue: an unstamped row is discarded, never submitted at the time it was finally read");

        // The delivered counter and the error line, which are the whole of the confidence indicator.
        ScrobbleQueue::noteDelivered(pid, 4);
        CHECK(ScrobbleQueue::delivered(pid) == 4, "queue: the counter is what 'scrobbled N tracks' reads");
        ScrobbleQueue::setLastError(pid, QStringLiteral("something went wrong"));
        CHECK(ScrobbleQueue::lastError(pid) == QLatin1String("something went wrong"),
              "queue: a failure is recorded, so the surface can say what it was");
        ScrobbleQueue::setLastError(pid, QString());
        CHECK(ScrobbleQueue::lastError(pid).isEmpty(),
              "queue: ...and a success clears it, so a working feature is not reported as broken for ever");
        ScrobbleQueue::clear(pid);
    }

    // =====================================================================================================
    // §6 END TO END AGAINST A FAKE ENDPOINT
    // =====================================================================================================
    {
        FakeService svc;
        if (!svc.listen()) { printf("FAIL e2e: the fake service could not listen\n"); ++fails; }

        // Point the real client at the fake, with a token that is not a token.
        Settings::setListenBrainzApiUrl(svc.root());
        Settings::setListenBrainzToken(QString::fromLatin1(kFakeToken));
        Settings::setScrobbleEnabled(true);
        Settings::setScrobbleSpokenAudio(false);

        CHECK(ListenBrainzClient::apiRoot() == svc.root(),
              "e2e: the custom API URL is what is used — this is the setting that covers Maloja and friends");
        Settings::setListenBrainzApiUrl(QStringLiteral("not a url"));
        CHECK(ListenBrainzClient::apiRoot() == ListenBrainzClient::defaultApiRoot(),
              "e2e: an unusable custom URL falls back to the public service rather than being posted as typed");
        Settings::setListenBrainzApiUrl(svc.root() + QStringLiteral("/"));
        CHECK(ListenBrainzClient::apiRoot() == svc.root(),
              "e2e: a trailing slash is trimmed once, here, so no request builder has to think about it");

        ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
        ScrobbleQueue::setLastError(QStringLiteral("listenbrainz"), QString());

        Scrobbler sc;
        sc.setProvider(new ListenBrainzClient(&sc));

        // ---- §6a THE SERVICE IS DOWN. Two tracks played through, and neither is lost. ----
        svc.refuseWith = 503;
        const qint64 t0 = QDateTime::currentSecsSinceEpoch();
        sc.trackStarted(musicTrack(QStringLiteral("Nina Simone"), QStringLiteral("Sinnerman"), 30));
        for (int s = 0; s <= 20; ++s) sc.positionTick(double(s));
        sc.trackStarted(musicTrack(QStringLiteral("Alice Coltrane"), QStringLiteral("Turiya"), 30));
        for (int s = 0; s <= 20; ++s) sc.positionTick(double(s));
        sc.playbackStopped();

        // Wait for the SUBMISSION's refusal to have been processed, not merely for some request to have been
        // made: the "now playing" announcement above reaches the socket first, and spinning on that would
        // check the queue before the thing under test had answered.
        spinUntil([&] { return !ScrobbleQueue::lastError(QStringLiteral("listenbrainz")).isEmpty(); }, 4000);
        CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 2,
              "e2e/offline: a refused submission KEEPS both listens — this is the flight's-worth-of-listening "
              "case, and losing them here is the failure the queue exists for");
        CHECK(!ScrobbleQueue::lastError(QStringLiteral("listenbrainz")).isEmpty(),
              "e2e/offline: ...and the surface can say why nothing has landed");

        // ---- §6b THE NETWORK COMES BACK. ----
        // Through retryNow(), which is what a settings change calls: a 503 armed the backoff ladder, and the
        // whole reason that second entry point exists is that a user who has just fixed something must not be
        // made to wait out a delay earned before they fixed it.
        svc.refuseWith = 0;
        sc.retryNow();
        spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 0; }, 5000);
        CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 0,
              "e2e/reconnect: the queue drains on the next attempt");
        CHECK(ScrobbleQueue::delivered(QStringLiteral("listenbrainz")) == 2,
              "e2e/reconnect: and the confidence counter says two tracks went");
        CHECK(ScrobbleQueue::lastError(QStringLiteral("listenbrainz")).isEmpty(),
              "e2e/reconnect: a success clears the error");

        // ---- §6c THE WIRE PAYLOAD, which is what the service actually receives. ----
        const QVector<QJsonObject> got = svc.acceptedListens();
        CHECK(got.size() == 2, "e2e/wire: both listens arrived");
        if (got.size() == 2)
        {
            const qint64 a = qint64(got[0].value(QStringLiteral("listened_at")).toDouble());
            const qint64 b = qint64(got[1].value(QStringLiteral("listened_at")).toDouble());
            CHECK(a >= t0 && a <= b,
                  "e2e/wire: each listen is BACKDATED to when its track started, oldest first — not stamped "
                  "with the moment the network came back");
            const QJsonObject m = got[0].value(QStringLiteral("track_metadata")).toObject();
            CHECK(m.value(QStringLiteral("artist_name")).toString() == QLatin1String("Nina Simone")
                  && m.value(QStringLiteral("track_name")).toString() == QLatin1String("Sinnerman"),
                  "e2e/wire: the artist and title are the ones that played");
            CHECK(m.value(QStringLiteral("additional_info")).toObject()
                   .value(QStringLiteral("duration_ms")).toInt() == 30000,
                  "e2e/wire: the duration goes in milliseconds, as the protocol wants");
        }
        bool sawImport = false;
        for (const QJsonObject& s : svc.submissions)
            if (s.value(QStringLiteral("listen_type")).toString() == QLatin1String("import")) sawImport = true;
        CHECK(sawImport, "e2e/wire: a batch goes as an `import` — `single` rejects more than one listen");

        // ---- §6d NOW PLAYING IS EPHEMERAL. It carries no timestamp and is never queued. ----
        // Announced while the service is ACCEPTING, so the fake records it. (§6a's announcements happened
        // while it was refusing, and a refusing service records nothing — which is itself the point of the
        // next assertion but one.)
        const int subsBefore = svc.submissions.size();
        sc.trackStarted(musicTrack(QStringLiteral("Sun Ra"), QStringLiteral("Space Is The Place"), 1200));
        spinUntil([&] { return svc.submissions.size() > subsBefore; }, 3000);
        bool sawPlayingNow = false, playingNowHadTimestamp = false;
        for (const QJsonObject& s : svc.submissions)
            if (s.value(QStringLiteral("listen_type")).toString() == QLatin1String("playing_now"))
            {
                sawPlayingNow = true;
                for (const QJsonValue& v : s.value(QStringLiteral("payload")).toArray())
                    if (v.toObject().contains(QStringLiteral("listened_at"))) playingNowHadTimestamp = true;
            }
        CHECK(sawPlayingNow, "e2e/nowplaying: starting a track announces it");
        CHECK(!playingNowHadTimestamp,
              "e2e/nowplaying: ...with NO listened_at — the field is forbidden for playing_now, and this call "
              "does not read its reply, so a 400 here would be invisible");

        // A now-playing that FAILS leaves nothing behind. Delivering one four minutes late would tell the
        // service the user is listening to something they finished.
        const int queuedBefore = ScrobbleQueue::count(QStringLiteral("listenbrainz"));
        svc.refuseWith = 500;
        sc.trackStarted(musicTrack(QStringLiteral("Someone"), QStringLiteral("A Song"), 600));
        spinUntil([&] { return false; }, 300);
        CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == queuedBefore,
              "e2e/nowplaying: a failed announcement queues NOTHING — a retry queue for it is a bug");
        svc.refuseWith = 0;
        sc.playbackStopped();

        // ---- §6e THE CREDENTIAL APPEARS IN NOTHING THE USER OR A LOG WOULD EVER SEE. ----
        // An error path that reports "the request that failed" reports the Authorization header inside it, and
        // that string then travels into a status line, a screenshot and a pasted log. This asserts the one
        // thing that makes that impossible: the message the app produces is built from the SERVICE's words.
        svc.refuseWith = 401;
        ScrobbleQueue::setLastError(QStringLiteral("listenbrainz"), QString());
        Play p; p.track = musicTrack(QStringLiteral("A"), QStringLiteral("T"), 200);
        p.listenedAt = QDateTime::currentSecsSinceEpoch();
        ScrobbleQueue::append(QStringLiteral("listenbrainz"), p);
        sc.retryNow();
        spinUntil([&] { return !ScrobbleQueue::lastError(QStringLiteral("listenbrainz")).isEmpty(); }, 4000);
        const QString err = ScrobbleQueue::lastError(QStringLiteral("listenbrainz"));
        CHECK(!err.isEmpty(), "e2e/secret: a refused credential is reported at all");
        CHECK(!err.contains(QString::fromLatin1(kFakeToken)),
              "e2e/secret: the reported failure contains NO part of the token");
        CHECK(!sc.statusLine().contains(QString::fromLatin1(kFakeToken)),
              "e2e/secret: nor does the status line the settings surfaces display");
        CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) >= 1,
              "e2e/secret: a refused credential KEEPS the listens — the user can fix the token and they still "
              "land, backdated");
        svc.refuseWith = 0;

        // ---- §6f A REJECTED batch is dropped rather than jamming everything behind it. ----
        ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
        ScrobbleQueue::append(QStringLiteral("listenbrainz"), p);
        svc.refuseWith = 400;
        sc.retryNow();
        spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 0; }, 4000);
        CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 0,
              "e2e/rejected: a batch the service will never accept is dropped — keeping it would silence every "
              "listen behind it for ever");
        svc.refuseWith = 0;

        // ---- §6g SWITCHED OFF, nothing is queued at all. ----
        Settings::setScrobbleEnabled(false);
        ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
        sc.trackStarted(musicTrack(QStringLiteral("Nobody"), QStringLiteral("Nothing"), 30));
        for (int s = 0; s <= 25; ++s) sc.positionTick(double(s));
        sc.playbackStopped();
        CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 0,
              "e2e/off: with the toggle off, a fully-played track queues nothing");
        CHECK(sc.statusLine().contains(QStringLiteral("off")),
              "e2e/off: ...and the status line says so, rather than showing a counter that will never move");

        ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
    }

    // =====================================================================================================
    // §7 LAST.FM (increment 2) — the second provider behind the same seam
    // =====================================================================================================
    // Everything in this section runs against an in-process QTcpServer that speaks Last.fm's 2.0 JSON. NO
    // ACCOUNT AND NO REAL CREDENTIAL IS INVOLVED: the application key and secret come from
    // tools/fixtures/lastfm/BuiltinSecrets.h and are the literal strings "probe-not-a-real-key" and
    // "probe-not-a-real-secret", the session key is whatever the fake hands back, and
    // LastFmClient::setApiRootForTests REFUSES anything that is not http on loopback — so none of it can
    // leave this machine even if the assertions below were wrong.
    {
        // ---- §7a THE EMBEDDED APPLICATION KEY (#81) ----
        // The one part of the slot mechanism no compiler can check: the runtime de-obfuscation mirrors
        // GenerateSecrets.cmake's obfuscation, in another language, in another file. If either side is
        // edited without the other, a real key would come out of the shipped binary as plausible mojibake and
        // be refused by Last.fm as "invalid key" — with nothing anywhere saying why. This is the assertion
        // that notices, and the fixture's bytes were produced by the CMake formula for exactly that reason.
        CHECK(LastFmClient::appKey() == QLatin1String("probe-not-a-real-key"),
              "lastfm/slot: the app key de-obfuscates back to its plaintext — the runtime XOR still mirrors "
              "GenerateSecrets.cmake's");
        CHECK(LastFmClient::appSecret() == QLatin1String("probe-not-a-real-secret"),
              "lastfm/slot: ...and so does the shared secret");
        CHECK(LastFmClient::availableInThisBuild(),
              "lastfm/slot: with both halves embedded, the provider is available in this build");
        CHECK(LastFmClient::statusFor(false, false, QString())
                  .contains(QStringLiteral("not available in this build")),
              "lastfm/slot: a build with NO key says so in one sentence — and offers nothing else. That is "
              "the ordinary case for anybody who clones this repository");
        CHECK(!LastFmClient::statusFor(false, false, QString()).contains(QStringLiteral("onnect")),
              "lastfm/slot: ...and it does not invite them to connect an account that cannot be connected");
        CHECK(LastFmClient::statusFor(true, false, QString()).contains(QStringLiteral("Not connected")),
              "lastfm/slot: with a key and no link, the answer is 'not connected', not 'not available'");
        CHECK(LastFmClient::statusFor(true, true, QStringLiteral("someone"))
                  .contains(QStringLiteral("someone")),
              "lastfm/slot: and a linked account is NAMED, so two profiles cannot silently share one link");
        // WHAT THE ACTION ROW SAYS, which is not simply "connected?" — the live drive caught this. A session
        // key stored by a build that HAD an application key is still in the ini when the same install is
        // rebuilt without one, so `connected()` is true while the provider is not installed at all: the row
        // offered to "Disconnect from Last.fm" underneath a line saying Last.fm was not available in this
        // build. Disabled, so nothing could come of pressing it, and wrong all the same.
        CHECK(LastFmClient::connectActionLabel(false, true).contains(QStringLiteral("Connect"))
                  && !LastFmClient::connectActionLabel(false, true).contains(QStringLiteral("Disconnect")),
              "lastfm/label: with no key in this build there is nothing to disconnect FROM, whatever a "
              "leftover session key in the ini says");
        CHECK(LastFmClient::connectActionLabel(true, true).contains(QStringLiteral("Disconnect")),
              "lastfm/label: a build that CAN reach Last.fm, with a link, offers to unlink");
        CHECK(!LastFmClient::connectActionLabel(true, false).contains(QStringLiteral("Disconnect")),
              "lastfm/label: ...and without one, offers to link");

        CHECK(BuiltinSecret::join(nullptr, 0, nullptr, 0).isEmpty(),
              "lastfm/slot: an EMPTY slot de-obfuscates to an empty string rather than to garbage — that is "
              "what every 'is this build carrying a key' test reads");

        // ---- §7b THE SIGNATURE, against the spec's own worked example ----
        // https://www.last.fm/api/authspec: "order all the parameters alphabetically by parameter name and
        // concatenate them into one string using a <name><value> scheme", then append the secret and md5 it.
        // The example the spec itself prints for auth.getSession is asserted VERBATIM — comparing one md5 to
        // another md5 would only prove the two sides of this repository agree with each other.
        {
            QMap<QString, QString> spec;
            spec.insert(QStringLiteral("api_key"), QStringLiteral("xxxxxxxx"));
            spec.insert(QStringLiteral("method"), QStringLiteral("auth.getSession"));
            spec.insert(QStringLiteral("token"), QStringLiteral("xxxxxxx"));
            CHECK(LastFm::signatureBase(spec)
                      == QLatin1String("api_keyxxxxxxxxmethodauth.getSessiontokenxxxxxxx"),
                  "lastfm/sig: the pre-hash string is the spec's worked example, byte for byte");
            // md5 of that string + "probe-not-a-real-secret", computed independently of this code.
            CHECK(LastFm::signature(spec, QStringLiteral("probe-not-a-real-secret"))
                      == QLatin1String("d59b076a3d49a4d45c21ad139716c930"),
                  "lastfm/sig: ...and the md5 over it matches a hash computed outside this program");

            // THE ORDER IS OVER THE NAMES AS STRINGS, INDICES AND ALL. A batch of eleven listens sends
            // artist[0]…artist[10], and a string sort puts artist[10] BEFORE artist[1] — ']' is 0x5D and '0'
            // is 0x30, so the longer name wins at the first differing character. That is counter-intuitive
            // enough that this assertion caught the author's own expectation being the other way round; an
            // implementation that "helpfully" sorted the indices numerically would sign a different string
            // and every batch of eleven or more would come back "Invalid method signature".
            QMap<QString, QString> idx;
            idx.insert(QStringLiteral("artist[1]"), QStringLiteral("B"));
            idx.insert(QStringLiteral("artist[10]"), QStringLiteral("C"));
            idx.insert(QStringLiteral("artist[2]"), QStringLiteral("D"));
            CHECK(LastFm::signatureBase(idx) == QLatin1String("artist[10]Cartist[1]Bartist[2]D"),
                  "lastfm/sig: parameter names sort as STRINGS — artist[10] comes before artist[1], not "
                  "between it and artist[2], and a batch of eleven listens depends on getting that right");

            // `format` changes the string, which is why it must never be in the signed map. The request
            // builder adds it afterwards; §7e proves that on the wire rather than here.
            QMap<QString, QString> withFormat = spec;
            withFormat.insert(QStringLiteral("format"), QStringLiteral("json"));
            CHECK(LastFm::signatureBase(withFormat) != LastFm::signatureBase(spec),
                  "lastfm/sig: including `format` WOULD change the signature — so signing it fails every "
                  "call, with a message that names nothing and sends you looking at the key");
        }

        // ---- §7c LAST.FM'S OWN ACCEPT RULE, which is deliberately not in Scrobble.h ----
        CHECK(!LastFm::longEnough(30),
              "lastfm/short: Last.fm ignores tracks of 30 seconds or less, so this app does not send them");
        CHECK(LastFm::longEnough(31), "lastfm/short: 31 seconds is long enough");
        CHECK(LastFm::longEnough(0),
              "lastfm/short: an UNKNOWN length is still sent — duration is optional to Last.fm, and refusing "
              "everything untimed would silently drop whole streaming sources");
        CHECK(Scrobble::thresholdSec(20) == 10,
              "lastfm/short: ...and the SHARED threshold is untouched by that rule. A 20-second track is "
              "still a listen for ListenBrainz, which has no such restriction (Scrobble.h says so)");

        // ---- §7d HTTP 200 IS NOT SUCCESS ----
        // Last.fm answers most failures with a 200 and an error object in the body. A client that reads only
        // the status code treats a refused session key as an accepted batch, drops the listens off the front
        // of the queue and loses them — silently, permanently, and only for the people whose key expired.
        CHECK(LastFm::outcomeFor(200, 9) == ScrobbleResult::Outcome::Auth,
              "lastfm/errors: HTTP 200 with error 9 (invalid session key) is an AUTH refusal, not a success. "
              "Reading the status alone here loses the listens for good");
        CHECK(LastFm::outcomeFor(200, 4) == ScrobbleResult::Outcome::Auth,
              "lastfm/errors: ...and so is error 4");
        CHECK(LastFm::outcomeFor(200, 29) == ScrobbleResult::Outcome::Retryable,
              "lastfm/errors: a rate limit is retryable — the listens are kept and go later");
        CHECK(LastFm::outcomeFor(200, 11) == ScrobbleResult::Outcome::Retryable,
              "lastfm/errors: so is 'service offline'");
        CHECK(LastFm::outcomeFor(200, 6) == ScrobbleResult::Outcome::Rejected,
              "lastfm/errors: an invalid parameter will stay invalid — DROP it, or the queue jams for ever "
              "behind one bad row and every listen after it is lost too");
        CHECK(LastFm::outcomeFor(200, 0) == ScrobbleResult::Outcome::Ok,
              "lastfm/errors: a 200 with no error object is the only success");
        CHECK(LastFm::outcomeFor(0, 0) == ScrobbleResult::Outcome::Retryable,
              "lastfm/errors: no reply at all is the network, and the listens are kept");

        // ---- the fake service ----
        FakeLastFm lfm;
        if (!lfm.listen()) { printf("FAIL lastfm: the fake service could not listen\n"); ++fails; }
        LastFmClient::setApiRootForTests(lfm.root());
        CHECK(LastFmClient::apiRoot().startsWith(lfm.root()),
              "lastfm/testhook: the client can be pointed at a loopback fake");
        LastFmClient::setApiRootForTests(QStringLiteral("https://evil.example.com"));
        CHECK(LastFmClient::apiRoot().startsWith(lfm.root()),
              "lastfm/testhook: ...and CANNOT be pointed anywhere else. The hook decides where an application "
              "key and a user's session key are sent, so it refuses everything but http on loopback");

        Settings::setLastFmSessionKey(QString());
        Settings::setLastFmAccount(QString());
        Settings::setScrobbleEnabled(true);
        Settings::setScrobbleSpokenAudio(false);
        ScrobbleQueue::clear(QStringLiteral("lastfm"));
        ScrobbleQueue::setLastError(QStringLiteral("lastfm"), QString());

        // ---- §7e THE DESKTOP AUTHORISATION ----
        {
            LastFmClient* auth = new LastFmClient(&app);
            QString shown;
            bool linked = false;
            QObject::connect(auth, &LastFmClient::authUrl, &app, [&shown](const QString& u) { shown = u; });
            QObject::connect(auth, &LastFmClient::connectedChanged, &app,
                             [&linked](bool on) { linked = on; });

            // The user does not approve immediately: the fake answers error 14 ("unauthorised token") twice
            // before letting the session through, which is the ordinary shape of this flow — the browser tab
            // is still open and nothing has been pressed yet.
            lfm.refuseSessionTimes = 1;
            auth->connectAccount();
            spinUntil([&] { return !shown.isEmpty(); }, 4000);
            CHECK(shown.contains(QStringLiteral("/api/auth/")),
                  "lastfm/auth: the user is sent to Last.fm's own authorisation page");
            CHECK(shown.contains(QStringLiteral("token=probe-request-token")),
                  "lastfm/auth: ...carrying the request token auth.getToken just minted");
            CHECK(shown.contains(QStringLiteral("api_key=")),
                  "lastfm/auth: ...and the application key, which is what the page authorises");

            // The poll. Two refusals then a session; the whole point is that nothing has to be pressed twice.
            spinUntil([&] { return linked; }, 20000);
            CHECK(linked, "lastfm/auth: the link completes on its own once the user has approved it");
            CHECK(lfm.sessionCalls >= 2,
                  "lastfm/auth: ...because error 14 is POLLED rather than reported — there is no callback in "
                  "this flow, and giving up on the first 14 would mean it never worked at all");
            CHECK(Settings::lastFmSessionKey() == QLatin1String("probe-session-key"),
                  "lastfm/auth: the SESSION KEY is what is stored");
            CHECK(Settings::lastFmAccount() == QLatin1String("probe-listener"),
                  "lastfm/auth: ...beside the account it belongs to, so the row can say who is linked");
            CHECK(Settings::lastFmSessionKey() != QLatin1String("probe-request-token"),
                  "lastfm/auth: the REQUEST token is spent by step 3 and is not what gets stored — storing "
                  "it instead would look identical here and fail at the first scrobble");
            CHECK(LastFmClient::connected(), "lastfm/auth: and the provider is now configured");

            // THE KEYS. Both carve-outs are written in terms of Scrobble's own prefixes, so the session key
            // inherits them rather than needing a second pair of literals that could drift from the writer.
            CHECK(Scrobble::isDeviceLocalKey(QStringLiteral("scrobble/default/lastfm/sk")),
                  "lastfm/keys: the session key is DEVICE-LOCAL — a synced settings bundle is a zip in "
                  "somebody's Drive folder, and a session key in it is a session key on a third party's disk");
            // ...and OUT of a settings transaction's scope, which is the opposite of the ListenBrainz token
            // beside it. THE LIVE DRIVE FOUND THIS. Connecting an account and pressing Back put up "Save
            // changes? 2 setting(s) changed" for a session key and a username the user never typed, and
            // Discard would have thrown away a link that took a browser round trip to make — which a typed
            // token does not, because you can just type it again. It is the "ra/user"/"ra/token" case
            // exactly (SettingsTxn.cpp says so in as many words: sign in, then Discard, and the stored
            // token reverts while the session stays live), and it is worse here because the write arrives
            // from a background POLL reply that can land in the middle of a settings visit the user is
            // making about something else entirely.
            CHECK(!SettingsTxn::inScope(QStringLiteral("scrobble/default/lastfm/sk")),
                  "lastfm/keys: a session key AUTHORISED in a browser is not a typed setting, so a Discard "
                  "must not silently unlink the account");
            CHECK(!SettingsTxn::inScope(QStringLiteral("scrobble/default/lastfm/user")),
                  "lastfm/keys: ...nor revert the username stored beside it, which would leave the row "
                  "naming nobody while the link is live");
            CHECK(SettingsTxn::inScope(QStringLiteral("scrobble/default/lb/token")),
                  "lastfm/keys: while the TYPED ListenBrainz token stays IN scope — pasting the wrong one "
                  "and pressing Discard still has to put the old one back. Two halves, one prefix");
            CHECK(SettingsTxn::inScope(QStringLiteral("scrobble/default/enabled")),
                  "lastfm/keys: ...and so does the on/off the user toggled");
            CHECK(Scrobble::isAuthorisedCredentialKey(QStringLiteral("scrobble/default/lastfm/sk"))
                      && !Scrobble::isAuthorisedCredentialKey(QStringLiteral("scrobble/default/lb/token")),
                  "lastfm/keys: the split is made by ONE predicate the writer's own prefix builds, so it "
                  "cannot drift from the keys Settings.cpp actually writes");

            // Disconnect FORGETS the credential and leaves the queue alone.
            Play waiting; waiting.track = musicTrack(QStringLiteral("Held"), QStringLiteral("Back"), 200);
            waiting.listenedAt = 1700000000;
            ScrobbleQueue::append(QStringLiteral("lastfm"), waiting);
            auth->disconnectAccount();
            CHECK(Settings::lastFmSessionKey().isEmpty() && Settings::lastFmAccount().isEmpty(),
                  "lastfm/auth: disconnecting forgets the session key AND the account name — a username left "
                  "behind claims a link that is no longer there");
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == 1,
                  "lastfm/auth: ...and does NOT throw away queued listens. They are backdated; linking again "
                  "delivers them");
            ScrobbleQueue::clear(QStringLiteral("lastfm"));
            delete auth;
        }

        // Re-link for the delivery sections (no second authorisation dance: the session key is what a linked
        // account IS, and storing it is the whole of what step 3 did above).
        Settings::setLastFmSessionKey(QStringLiteral("probe-session-key"));
        Settings::setLastFmAccount(QStringLiteral("probe-listener"));

        // ---- §7f OFFLINE, THEN RECONNECT, THROUGH THE SHARED ORCHESTRATOR ----
        {
            Scrobbler sc;
            LastFmClient* lf = new LastFmClient(nullptr);
            sc.setProvider(lf);                       // takes ownership

            lfm.refuseWith = 16;                      // "temporarily unavailable" — retryable
            const qint64 t0 = QDateTime::currentSecsSinceEpoch();
            sc.trackStarted(musicTrack(QStringLiteral("Nina Simone"), QStringLiteral("Sinnerman"), 60));
            for (int s = 0; s <= 35; ++s) sc.positionTick(double(s));
            sc.trackStarted(musicTrack(QStringLiteral("Alice Coltrane"), QStringLiteral("Turiya"), 60));
            for (int s = 0; s <= 35; ++s) sc.positionTick(double(s));
            sc.playbackStopped();

            spinUntil([&] { return !ScrobbleQueue::lastError(QStringLiteral("lastfm")).isEmpty(); }, 4000);
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == 2,
                  "lastfm/offline: a refused submission KEEPS both listens — the flight's-worth-of-listening "
                  "case, played against a service that answers its refusals with HTTP 200");

            lfm.refuseWith = 0;
            sc.retryNow();
            spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("lastfm")) == 0; }, 6000);
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == 0,
                  "lastfm/reconnect: the queue drains on the next attempt");
            CHECK(ScrobbleQueue::delivered(QStringLiteral("lastfm")) == 2,
                  "lastfm/reconnect: and the confidence counter says two tracks went to Last.fm");

            // ---- §7g THE WIRE, which is what Last.fm would actually have received ----
            CHECK(lfm.badSignatures == 0,
                  "lastfm/wire: EVERY signed call verified — the fake recomputes api_sig the way Last.fm "
                  "does, over the sorted parameters with `format` and `api_sig` excluded");
            CHECK(lfm.scrobbles.size() >= 1, "lastfm/wire: the batch arrived as one track.scrobble call");
            if (!lfm.scrobbles.isEmpty())
            {
                const QMap<QString, QString> b = lfm.scrobbles.last();
                CHECK(b.value(QStringLiteral("artist[0]")) == QLatin1String("Nina Simone")
                          && b.value(QStringLiteral("track[0]")) == QLatin1String("Sinnerman"),
                      "lastfm/wire: the artist and title are the ones that played");
                CHECK(b.value(QStringLiteral("artist[1]")) == QLatin1String("Alice Coltrane"),
                      "lastfm/wire: ...and BOTH listens went in ONE call, indexed — that is what the offline "
                      "queue exists to be able to do");
                const qint64 ts0 = b.value(QStringLiteral("timestamp[0]")).toLongLong();
                const qint64 ts1 = b.value(QStringLiteral("timestamp[1]")).toLongLong();
                CHECK(ts0 >= t0 && ts0 <= ts1,
                      "lastfm/wire: each listen is BACKDATED to when its track started, oldest first — not "
                      "stamped with the moment the network came back");
                CHECK(b.value(QStringLiteral("duration[0]")) == QLatin1String("60"),
                      "lastfm/wire: the duration goes in SECONDS — the opposite of ListenBrainz's "
                      "duration_ms, and invisible until a history is full of hour-long songs");
                CHECK(b.contains(QStringLiteral("api_sig")),
                      "lastfm/wire: the call is signed at all");
                CHECK(b.value(QStringLiteral("format")) == QLatin1String("json"),
                      "lastfm/wire: ...and asks for JSON, which is added AFTER the signature and never "
                      "inside it");
                CHECK(b.value(QStringLiteral("sk")) == QLatin1String("probe-session-key"),
                      "lastfm/wire: ...on behalf of the linked session, not of the application alone");
            }

            // ---- §7h NOW PLAYING IS EPHEMERAL ----
            const int npBefore = lfm.nowPlaying.size();
            sc.trackStarted(musicTrack(QStringLiteral("Sun Ra"), QStringLiteral("Space Is The Place"), 1200));
            spinUntil([&] { return lfm.nowPlaying.size() > npBefore; }, 3000);
            CHECK(lfm.nowPlaying.size() > npBefore, "lastfm/nowplaying: starting a track announces it");
            if (!lfm.nowPlaying.isEmpty())
            {
                const QMap<QString, QString> np = lfm.nowPlaying.last();
                CHECK(np.value(QStringLiteral("track")) == QLatin1String("Space Is The Place"),
                      "lastfm/nowplaying: ...as the track that is playing now");
                CHECK(!np.contains(QStringLiteral("timestamp")),
                      "lastfm/nowplaying: with NO timestamp. It is a hint that expires on its own, and a "
                      "queue for it would announce finished tracks as current ones");
            }
            const int queuedBefore = ScrobbleQueue::count(QStringLiteral("lastfm"));
            lfm.refuseWith = 16;
            sc.trackStarted(musicTrack(QStringLiteral("Someone"), QStringLiteral("A Song"), 600));
            spinUntil([&] { return false; }, 300);
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == queuedBefore,
                  "lastfm/nowplaying: a FAILED announcement queues nothing — a retry queue for it is a bug");
            lfm.refuseWith = 0;
            sc.playbackStopped();
            ScrobbleQueue::clear(QStringLiteral("lastfm"));

            // ---- §7i THE 30-SECOND RULE ON THE WIRE ----
            // A batch of nothing but short tracks is REJECTED rather than kept: Last.fm will never accept it,
            // and a permanently-refused batch at the head of a FIFO silences every listen behind it for ever.
            Play tiny; tiny.track = musicTrack(QStringLiteral("Brief"), QStringLiteral("Interlude"), 20);
            tiny.listenedAt = 1700000000;
            ScrobbleQueue::append(QStringLiteral("lastfm"), tiny);
            const int scrobbleCallsBefore = lfm.scrobbles.size();
            sc.retryNow();
            spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("lastfm")) == 0; }, 4000);
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == 0,
                  "lastfm/short: a batch Last.fm would never accept is dropped, not kept for ever");
            CHECK(lfm.scrobbles.size() == scrobbleCallsBefore,
                  "lastfm/short: ...and was never SENT. Asking a service to accept something it documents "
                  "that it ignores is a request whose only outcome is an error nobody reads");
            CHECK(ScrobbleQueue::lastError(QStringLiteral("lastfm")).contains(QStringLiteral("30 seconds")),
                  "lastfm/short: and the surface says which rule it was, rather than showing a queue that "
                  "quietly emptied itself");

            // ---- §7j LOVE AND UNLOVE ----
            ScrobbleQueue::setLastError(QStringLiteral("lastfm"), QString());
            const Track loved = musicTrack(QStringLiteral("Nina Simone"), QStringLiteral("Sinnerman"), 400);
            sc.noteFavorite(loved, true);
            spinUntil([&] { return !lfm.loves.isEmpty(); }, 4000);
            CHECK(!lfm.loves.isEmpty(), "lastfm/love: starring a track reaches the service");
            if (!lfm.loves.isEmpty())
            {
                CHECK(lfm.loves.last().value(QStringLiteral("method")) == QLatin1String("track.love")
                          && lfm.loves.last().value(QStringLiteral("artist")) == QLatin1String("Nina Simone"),
                      "lastfm/love: ...as track.love on the artist/title pair — ONE call, where "
                      "ListenBrainz needs a MusicBrainz recording resolved first");
            }
            const int lovesBefore = lfm.loves.size();
            sc.noteFavorite(loved, false);
            spinUntil([&] { return lfm.loves.size() > lovesBefore; }, 4000);
            CHECK(lfm.loves.size() > lovesBefore
                      && lfm.loves.last().value(QStringLiteral("method")) == QLatin1String("track.unlove"),
                  "lastfm/love: un-starring sends track.unlove, not a second love");
            const int lovesBefore2 = lfm.loves.size();
            Track untagged = loved; untagged.artist.clear();
            sc.noteFavorite(untagged, true);
            spinUntil([&] { return false; }, 300);
            CHECK(lfm.loves.size() == lovesBefore2,
                  "lastfm/love: an untagged file has nothing to love, and nothing is sent for it");

            // ---- §7k WHAT IS NOT SENT ----
            ScrobbleQueue::clear(QStringLiteral("lastfm"));
            sc.trackStarted(audiobookTrack());
            for (int s = 0; s <= 260; ++s) sc.positionTick(double(s));
            sc.playbackStopped();
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == 0,
                  "lastfm/counts: an audiobook is excluded by default — a twelve-hour 'track' is noise in a "
                  "listening history, and Last.fm would take it happily");
            sc.trackStarted(musicTrack(QString(), QStringLiteral("01"), 300));
            for (int s = 0; s <= 160; ++s) sc.positionTick(double(s));
            sc.playbackStopped();
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == 0,
                  "lastfm/counts: a file with no artist is skipped rather than scrobbled as 'Unknown Artist' "
                  "— submitting it cannot be undone from this app");

            // THE DOUBLE-COUNT COORDINATION, confirmed to cover this provider too. The rule lives in
            // Scrobble::verdictFor, above the seam, so it applies to whichever services are configured — a
            // server that forwards its own plays must not also be scrobbled from here, on EITHER of them.
            Track fromServer = musicTrack(QStringLiteral("Server"), QStringLiteral("Track"), 300);
            fromServer.origin = Origin::Server;
            Policy forwarding; forwarding.enabled = true; forwarding.serverForwards = true;
            CHECK(Scrobble::verdictFor(fromServer, forwarding) == Verdict::SkipServerForwards,
                  "lastfm/doublecount: a play a server is already forwarding is refused ABOVE the seam, so "
                  "adding a second provider cannot reopen the every-play-counted-twice hole");

            // ---- §7l THE CREDENTIALS APPEAR IN NOTHING A USER OR A LOG WOULD SEE ----
            // A Last.fm request body carries the api_key, the api_sig AND the session key. An error path that
            // reported "the request that failed" would put all three into a status line, a screenshot and a
            // pasted log, and there is no later stage that takes them back out.
            lfm.refuseWith = 9;                       // invalid session key: HTTP 200, error 9
            ScrobbleQueue::setLastError(QStringLiteral("lastfm"), QString());
            Play p2; p2.track = musicTrack(QStringLiteral("A"), QStringLiteral("T"), 300);
            p2.listenedAt = QDateTime::currentSecsSinceEpoch();
            ScrobbleQueue::append(QStringLiteral("lastfm"), p2);
            sc.retryNow();
            spinUntil([&] { return !ScrobbleQueue::lastError(QStringLiteral("lastfm")).isEmpty(); }, 4000);
            const QString err = ScrobbleQueue::lastError(QStringLiteral("lastfm"));
            const QString line = sc.statusLine();
            CHECK(!err.isEmpty(), "lastfm/secret: a refused session key is reported at all");
            CHECK(!err.contains(LastFmClient::appSecret()) && !line.contains(LastFmClient::appSecret()),
                  "lastfm/secret: the shared secret appears in NO message the app would show");
            CHECK(!err.contains(LastFmClient::appKey()) && !line.contains(LastFmClient::appKey()),
                  "lastfm/secret: nor does the application key");
            CHECK(!err.contains(QStringLiteral("probe-session-key"))
                      && !line.contains(QStringLiteral("probe-session-key")),
                  "lastfm/secret: nor the session key");
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) >= 1,
                  "lastfm/secret: a refused credential KEEPS the listens — re-linking the account still "
                  "delivers them, backdated");
            lfm.refuseWith = 0;
            ScrobbleQueue::clear(QStringLiteral("lastfm"));
        }

        // ---- §7m TWO SERVICES AT ONCE, which is what the per-provider queue was always for ----
        {
            ScrobbleQueue::clear(QStringLiteral("lastfm"));
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            Settings::setListenBrainzToken(QString::fromLatin1(kFakeToken));

            Scrobbler both;
            LastFmClient* lf = new LastFmClient(nullptr);
            both.setProvider(lf);
            both.addProvider(new ListenBrainzClient(nullptr));
            CHECK(both.providers().size() == 2,
                  "two-services: Last.fm is installed BESIDE ListenBrainz, not instead of it");

            // Nothing can be delivered: point ListenBrainz at a port nobody is listening on, and refuse
            // everything at the Last.fm fake. Both queues must fill, independently.
            lfm.refuseWith = 16;
            Settings::setListenBrainzApiUrl(QStringLiteral("http://127.0.0.1:1"));
            both.trackStarted(musicTrack(QStringLiteral("Both"), QStringLiteral("Ways"), 60));
            for (int s = 0; s <= 35; ++s) both.positionTick(double(s));
            both.playbackStopped();
            spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("lastfm")) == 1
                                && ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1; }, 5000);
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == 1
                      && ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1,
                  "two-services: ONE listen is filed once per service, with its own timestamp, so a listen "
                  "delivered to one is still owed to the other");
            CHECK(both.statusLine().contains(QStringLiteral("Last.fm"))
                      && both.statusLine().contains(QStringLiteral("ListenBrainz")),
                  "two-services: the status line says something about EACH — 'it is working' can be true of "
                  "one and false of the other, and an averaged number is how a half-broken feature looks fine");

            // ONE SERVICE COMING BACK DOES NOT WAIT FOR THE OTHER. ListenBrainz is still pointed at a dead
            // port and is climbing its backoff ladder; Last.fm must drain anyway.
            lfm.refuseWith = 0;
            both.retryNow();
            spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("lastfm")) == 0; }, 6000);
            CHECK(ScrobbleQueue::count(QStringLiteral("lastfm")) == 0,
                  "two-services: Last.fm drains while ListenBrainz is still down — the backoff is PER "
                  "PROVIDER, or one service's worst day would be the other's as well");
            CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1,
                  "two-services: ...and the listen still owed to ListenBrainz is still owed, not lost");

            ScrobbleQueue::clear(QStringLiteral("lastfm"));
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            Settings::setListenBrainzApiUrl(QString());
            Settings::setListenBrainzToken(QString());
        }

        LastFmClient::setApiRootForTests(QString());
        Settings::setLastFmSessionKey(QString());
        Settings::setLastFmAccount(QString());
        Settings::setScrobbleEnabled(false);
        ScrobbleQueue::clear(QStringLiteral("lastfm"));
        ScrobbleQueue::setLastError(QStringLiteral("lastfm"), QString());
    }

    // =====================================================================================================
    // 8. A MUSIC SERVER AS A SCROBBLE DESTINATION (issue #193, increment 6)
    // =====================================================================================================
    // The THIRD implementation of this seam, and the first that is not a public service. What is under test
    // is not the Subsonic protocol - probe_subsonic drives every pure part of that - but the seam itself:
    //
    //   * a destination that can only be told about SOME listens (a music server has no way to name a file
    //     on this disk), which is a queue-jamming bug if the orchestrator files the others with it anyway;
    //   * the DOUBLE-COUNT COORDINATION, driven both ways, including the arm that would count a listen zero
    //     times instead of once;
    //   * a supplier's own id surviving the offline queue, because `scrobble.view` takes an id and nothing
    //     else and a listen that spends a night on disk arrives without one otherwise;
    //   * a LOVE that is a library edit rather than a broadcast, and is therefore gated differently;
    //   * and, again, that no credential reaches anything the app shows, logs or persists.
    {
        FakeSubsonic fake;
        CHECK(fake.listen(), "subsonic: the fake music server is listening on loopback");

        const QString sidA = QStringLiteral("11111111-1111-4111-8111-111111111111");
        const QString sidB = QStringLiteral("22222222-2222-4222-8222-222222222222");
        SubsonicServer srv;
        srv.id = sidA;
        srv.name = QStringLiteral("Fake Navidrome");
        srv.url = fake.root();
        srv.username = QStringLiteral("probe");
        srv.password = QStringLiteral("probe-not-a-real-password");
        srv.allowPlainHttp = true;            // loopback http, explicitly opted into - never a downgrade
        SubsonicServerStore::add(srv);

        const QString trackA = Subsonic::qualify(sidA, Subsonic::Kind::Track, QStringLiteral("s-1"));
        const QString trackA2 = Subsonic::qualify(sidA, Subsonic::Kind::Track, QStringLiteral("s-2"));
        const QString trackB = Subsonic::qualify(sidB, Subsonic::Kind::Track, QStringLiteral("s-1"));
        const QString providerId = SubsonicScrobbleProvider::idFor(sidA);
        ScrobbleQueue::clear(providerId);
        ScrobbleQueue::setLastError(providerId, QString());

        // ---- 8a. who this destination is, and which listens it can be told about ----------------------
        {
            SubsonicScrobbleProvider p(sidA);
            CHECK(p.id() == QStringLiteral("subsonic:") + sidA,
                  "subsonic: the provider id carries the SERVER - one queue, one backoff and one counter "
                  "per server, so a batch is homogeneous and one asleep box does not hold up another");
            CHECK(p.displayName() == QStringLiteral("Fake Navidrome"),
                  "subsonic: ...and the status line names the server, which is what makes it an answer");
            CHECK(p.configured(), "subsonic: a saved server with a usable address is configured");

            CHECK(p.accepts(subsonicTrack(trackA, QStringLiteral("Mine"), 60)),
                  "subsonic: a play of THIS server's track can be reported to it");
            CHECK(!p.accepts(subsonicTrack(trackB, QStringLiteral("Theirs"), 60)),
                  "subsonic: ...and another server's cannot - its id means nothing here, and queueing it "
                  "would jam this queue behind a row that can never be delivered");
            CHECK(!p.accepts(musicTrack(QStringLiteral("Amber"), QStringLiteral("A local file"), 60)),
                  "subsonic: nor can a local file: scrobble.view takes an id and there is no id for it");
            CHECK(p.ownsSource(subsonicTrack(trackA, QStringLiteral("Mine"), 60)),
                  "subsonic: a play this server served is one it OWNS - reporting it back is the first "
                  "count of that play, not a second one");
            CHECK(p.supportsLove() && p.loveIsLibraryEdit(),
                  "subsonic: a star is a library edit rather than a broadcast");

            SubsonicScrobbleProvider gone(sidB);
            CHECK(!gone.configured(),
                  "subsonic: a server that is not in the store is not configured - which is how a REMOVED "
                  "server stops being a destination with nothing having to hunt its provider down");
            CHECK(!gone.ownsSource(subsonicTrack(trackA, QStringLiteral("Mine"), 60)),
                  "subsonic: ...and it owns nothing");
        }

        // ---- 8b. now playing: ephemeral, and never queued --------------------------------------------
        {
            Settings::setScrobbleEnabled(true);
            Settings::setScrobbleServerForwards(false);
            fake.forget();
            Scrobbler s;
            s.setProvider(new SubsonicScrobbleProvider(sidA));
            s.trackStarted(subsonicTrack(trackA, QStringLiteral("Announced"), 600));
            spinUntil([&] { return !fake.methods.isEmpty(); });
            CHECK(fake.methods.value(0) == QStringLiteral("scrobble"),
                  "subsonic: a track starting announces itself through scrobble.view");
            CHECK(fake.lastValue(QStringLiteral("submission")) == QStringLiteral("false"),
                  "subsonic: ...as submission=false, the ephemeral hint");
            CHECK(fake.lastValue(QStringLiteral("time")).isEmpty(),
                  "subsonic: ...carrying NO time, because a now-playing hint is about this moment and a "
                  "timestamp on it would be a stale claim the instant it arrived");
            CHECK(fake.lastValue(QStringLiteral("id")) == QStringLiteral("s-1"),
                  "subsonic: ...naming the server's OWN id for the track");
            CHECK(ScrobbleQueue::count(providerId) == 0,
                  "subsonic: a now-playing is NEVER queued - delivering one four minutes late would tell "
                  "the server about something the listener finished");
            // Only a few ticks: nowhere near the threshold, so nothing is owed at the boundary either.
            s.positionTick(0.0); s.positionTick(1.0);
            s.playbackStopped();
            CHECK(ScrobbleQueue::count(providerId) == 0,
                  "subsonic: ...and stopping short of the threshold still queues nothing");
        }

        // ---- 8c. a completed listen: submission=true, backdated in MILLISECONDS ------------------------
        {
            Settings::setScrobbleEnabled(true);
            Settings::setScrobbleServerForwards(false);
            ScrobbleQueue::clear(providerId);
            fake.forget();
            Scrobbler s;
            s.setProvider(new SubsonicScrobbleProvider(sidA));
            playThrough(s, subsonicTrack(trackA, QStringLiteral("Heard"), 60), 40);
            spinUntil([&] { return ScrobbleQueue::count(providerId) == 0
                                && fake.methods.count(QStringLiteral("scrobble")) >= 2; });
            CHECK(ScrobbleQueue::count(providerId) == 0,
                  "subsonic: a listened-to track is delivered and leaves the queue");
            CHECK(ScrobbleQueue::delivered(providerId) >= 1,
                  "subsonic: ...and the confidence counter says so");
            CHECK(fake.lastValue(QStringLiteral("submission")) == QStringLiteral("true"),
                  "subsonic: the durable form is submission=true");
            const QString ms = fake.lastValue(QStringLiteral("time"));
            CHECK(ms.size() >= 13,
                  "subsonic: the time is in MILLISECONDS - seconds here would file the play in 1970, where "
                  "nothing displays it and nothing complains");
            CHECK(ms.toLongLong() / 1000 >= QDateTime::currentSecsSinceEpoch() - 300,
                  "subsonic: ...and it is the moment the track STARTED, backdated rather than restamped");
            CHECK(ScrobbleQueue::lastError(providerId).isEmpty(),
                  "subsonic: a success clears the last error");
        }

        // ---- 8d. the offline queue carries the SERVER'S OWN ID across a restart ------------------------
        {
            Scrobble::Play p;
            p.track      = subsonicTrack(trackA, QStringLiteral("Kept overnight"), 300);
            p.listenedAt = 1700000000LL;
            const QVector<Scrobble::Play> back = ScrobbleQueue::decode(ScrobbleQueue::encode({ p }));
            CHECK(back.size() == 1 && back.at(0).track.sourceId == trackA,
                  "subsonic: the supplier's own id survives the queue - without it a listen that waited out "
                  "an outage arrives with no way to name the track, and can never be delivered at all");
            CHECK(back.size() == 1 && back.at(0).listenedAt == 1700000000LL,
                  "subsonic: ...with its original timestamp, as every queued listen keeps");
            CHECK(!QString::fromUtf8(ScrobbleQueue::encode({ p })).contains(QStringLiteral("http")),
                  "subsonic: and what is written is the ID, never the signed stream url - the url carries "
                  "the token, and this row goes on disk");
            // A LOCAL play still encodes to what it always did: no id, no extra key.
            Scrobble::Play local;
            local.track      = musicTrack(QStringLiteral("Amber"), QStringLiteral("A file"), 300);
            local.listenedAt = 1700000000LL;
            CHECK(!QString::fromUtf8(ScrobbleQueue::encode({ local })).contains(QStringLiteral("sid")),
                  "subsonic: ...and a local listen costs not one extra byte");
        }

        // ---- 8e. THE DOUBLE-COUNT COORDINATION, both ways ---------------------------------------------
        // Two destinations at once: the music server, and a ListenBrainz pointed at a port nobody is
        // listening on so its queue simply fills. What is being asserted is WHO IS TOLD, not who succeeded.
        {
            Settings::setListenBrainzToken(QString::fromLatin1(kFakeToken));
            Settings::setListenBrainzApiUrl(QStringLiteral("http://127.0.0.1:1"));
            Settings::setScrobbleEnabled(true);

            // --- OFF (the default): the server AND the upstream service are both told. ---
            Settings::setScrobbleServerForwards(false);
            ScrobbleQueue::clear(providerId);
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            fake.forget();
            {
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                s.addProvider(new ListenBrainzClient(nullptr));
                playThrough(s, subsonicTrack(trackA, QStringLiteral("Counted once"), 60), 40);
                // Spin on the SERVER's side of it as well as the queue's: a queue append is synchronous, so
                // waiting only on that returns before the event loop has run once and the fake has been
                // handed nothing at all.
                spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1
                                    && fake.methods.count(QStringLiteral("scrobble")) >= 2; });
                CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1,
                      "coordination OFF: a server-sourced play is reported to the listening service too - "
                      "which is right when the server forwards nothing, the ordinary setup");
                CHECK(fake.methods.contains(QStringLiteral("scrobble")),
                      "coordination OFF: ...and to the server itself");
            }

            // --- ON: the server ONLY. The upstream service is left to the server, which is the whole
            //     point; and the SERVER IS STILL TOLD, because it can only forward what it hears.
            Settings::setScrobbleServerForwards(true);
            ScrobbleQueue::clear(providerId);
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            fake.forget();
            {
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                s.addProvider(new ListenBrainzClient(nullptr));
                playThrough(s, subsonicTrack(trackA2, QStringLiteral("Counted once too"), 60), 40);
                spinUntil([&] { return fake.methods.count(QStringLiteral("scrobble")) >= 2
                                    && ScrobbleQueue::count(providerId) == 0; });
                CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 0,
                      "coordination ON: the listening service is NOT told - the server forwards it, and "
                      "telling both is the double count the setting exists to prevent");
                CHECK(fake.methods.count(QStringLiteral("scrobble")) >= 1,
                      "coordination ON: ...but the SERVER IS STILL TOLD. Suppressing this too would count "
                      "the listen zero times instead of once: scrobble.view is the only way a client play "
                      "reaches the server, so a server told nothing forwards nothing");
                CHECK(fake.lastValue(QStringLiteral("submission")) == QStringLiteral("true"),
                      "coordination ON: ...as a real, durable submission");
            }

            // --- ...and a LOCAL play is unaffected either way: there is no server in the middle of it. ---
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            ScrobbleQueue::clear(providerId);
            {
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                s.addProvider(new ListenBrainzClient(nullptr));
                playThrough(s, musicTrack(QStringLiteral("Amber"), QStringLiteral("On this disk"), 60), 40);
                spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1; });
                CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1,
                      "coordination ON: music on this device still scrobbles - the setting is about plays a "
                      "server served, and nothing else");
                CHECK(ScrobbleQueue::count(providerId) == 0,
                      "coordination: ...and the music server is not offered a listen it could never name, "
                      "which is what keeps its queue from jamming behind an undeliverable row");
            }
            Settings::setScrobbleServerForwards(false);
            Settings::setListenBrainzApiUrl(QString());
            Settings::setListenBrainzToken(QString());
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            ScrobbleQueue::setLastError(QStringLiteral("listenbrainz"), QString());
        }

        // ---- 8f. the failure envelopes, which arrive as 200 -------------------------------------------
        {
            Settings::setScrobbleEnabled(true);
            ScrobbleQueue::clear(providerId);
            ScrobbleQueue::setLastError(providerId, QString());

            // A REFUSED CREDENTIAL. The listens are KEPT: the user can fix the password and they still land.
            fake.answerWith = "<subsonic-response status=\"failed\" version=\"1.16.1\">"
                              "<error code=\"40\" message=\"Wrong username or password.\"/>"
                              "</subsonic-response>";
            {
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                playThrough(s, subsonicTrack(trackA, QStringLiteral("Refused"), 60), 40);
                spinUntil([&] { return !ScrobbleQueue::lastError(providerId).isEmpty(); });
                CHECK(ScrobbleQueue::count(providerId) == 1,
                      "subsonic: a 200 carrying a failure envelope is NOT a success - the listen is kept. "
                      "A client that read the HTTP status would have reported it delivered and lost it");
                CHECK(ScrobbleQueue::lastError(providerId)
                          == QStringLiteral("Wrong username or password."),
                      "subsonic: ...and the user is told the SERVER'S OWN words");
            }

            // PERMANENTLY GONE. Dropped, or the queue jams for ever behind one row and every listen after
            // it is lost too.
            fake.answerWith = "<subsonic-response status=\"failed\" version=\"1.16.1\">"
                              "<error code=\"70\" message=\"The requested data was not found.\"/>"
                              "</subsonic-response>";
            {
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                s.retryNow();
                spinUntil([&] { return ScrobbleQueue::count(providerId) == 0; });
                CHECK(ScrobbleQueue::count(providerId) == 0,
                      "subsonic: a listen the server says will never be accepted is DROPPED - keeping it "
                      "would stop everything behind it being delivered, which is a silent total failure");
            }
            fake.answerWith.clear();
            ScrobbleQueue::clear(providerId);
            ScrobbleQueue::setLastError(providerId, QString());
        }

        // ---- 8g. the favourite verb: star and unstar, and the gate it passes ---------------------------
        {
            fake.forget();
            Settings::setScrobbleEnabled(true);
            Scrobbler s;
            s.setProvider(new SubsonicScrobbleProvider(sidA));
            s.noteFavorite(subsonicTrack(trackA, QStringLiteral("Loved"), 300), true);
            spinUntil([&] { return !fake.methods.isEmpty(); });
            CHECK(fake.methods.value(0) == QStringLiteral("star"),
                  "subsonic: the favourite verb the app already has reaches the server as star.view");
            CHECK(fake.lastValue(QStringLiteral("id")) == QStringLiteral("s-1"),
                  "subsonic: ...naming the SONG id, not an album or artist id in the same namespace");

            fake.forget();
            s.noteFavorite(subsonicTrack(trackA, QStringLiteral("Loved"), 300), false);
            spinUntil([&] { return !fake.methods.isEmpty(); });
            CHECK(fake.methods.value(0) == QStringLiteral("unstar"),
                  "subsonic: and un-starring reaches it as unstar.view");

            // THE GATE A LOVE PASSES IS NOT THE GATE A LISTEN PASSES. Scrobbling off, and the star still
            // reaches the server: it is an edit to the user's OWN library, in the place the track already
            // lives, and gating it on the listening-history switch makes a pressed button do nothing.
            Settings::setScrobbleEnabled(false);
            fake.forget();
            s.noteFavorite(subsonicTrack(trackA, QStringLiteral("Loved"), 300), true);
            spinUntil([&] { return !fake.methods.isEmpty(); });
            CHECK(fake.methods.value(0) == QStringLiteral("star"),
                  "subsonic: a star still reaches the server with scrobbling switched off - a library edit "
                  "is not a broadcast, and two unrelated things behind one switch is a feature that "
                  "silently stops working");

            // ...and the coordination is about PLAYS, not stars. A server that forwards its plays upstream
            // does not forward its stars, and suppressing the star would be the same bug in another dress.
            Settings::setScrobbleEnabled(true);
            Settings::setScrobbleServerForwards(true);
            fake.forget();
            s.noteFavorite(subsonicTrack(trackA, QStringLiteral("Loved"), 300), true);
            spinUntil([&] { return !fake.methods.isEmpty(); });
            CHECK(fake.methods.value(0) == QStringLiteral("star"),
                  "subsonic: ...and with the double-count coordination on, because that is about plays");
            Settings::setScrobbleServerForwards(false);

            // A track from ANOTHER server is not this destination's to star, and nothing is sent at all.
            fake.forget();
            s.noteFavorite(subsonicTrack(trackB, QStringLiteral("Somebody else's"), 300), true);
            spinUntil([&] { return !fake.methods.isEmpty(); }, 400);
            CHECK(fake.methods.isEmpty(),
                  "subsonic: a track this server never held is not starred here - matching on artist and "
                  "title instead would star the wrong record on the wrong box");
        }

        // ---- 8h. THE CREDENTIAL SCAN ------------------------------------------------------------------
        // The password, and the token derived from it, appear in nothing the app shows, logs or persists.
        // An assertion rather than a claim: `QNetworkReply::errorString()` embeds the url, and for this
        // protocol the url IS the credential, so the one idiomatic diagnostic line is the leak.
        {
            ScrobbleQueue::clear(providerId);
            Settings::setScrobbleEnabled(true);
            // Point the server at a dead port so a TRANSPORT failure - the errorString() path - is what
            // produces the message. This is the arm the rule is actually about.
            SubsonicServer dead;
            SubsonicServerStore::get(sidA, dead);
            const QString liveUrl = dead.url;
            dead.url = QStringLiteral("http://127.0.0.1:1");
            SubsonicServerStore::update(dead);
            {
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                playThrough(s, subsonicTrack(trackA, QStringLiteral("Unreachable"), 60), 40);
                spinUntil([&] { return !ScrobbleQueue::lastError(providerId).isEmpty(); });
            }
            QByteArray scanned;
            scanned += ScrobbleQueue::lastError(providerId).toUtf8();
            scanned += ScrobbleQueue::encode(ScrobbleQueue::head(providerId, 50));
            {
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                scanned += s.statusLine().toUtf8();
            }
            CHECK(!scanned.contains(QByteArray("probe-not-a-real-password")),
                  "subsonic: the password appears in no message, no status line and no queue row");
            const QString salt = Subsonic::saltFrom(Q_UINT64_C(0x0123456789abcdef));
            CHECK(!scanned.contains(Subsonic::tokenFor(QStringLiteral("probe-not-a-real-password"),
                                                       salt).toUtf8()),
                  "subsonic: nor does a token derived from it");
            CHECK(!scanned.contains(QByteArray("&t=")) && !scanned.contains(QByteArray("&s=")),
                  "subsonic: nor anything shaped like a signed request - the whole family of 'somebody "
                  "logged the failing url', which for this protocol logs the credential");
            CHECK(!scanned.contains(QByteArray("/rest/")),
                  "subsonic: ...and no request url reaches a message at all, which is the rule rather than "
                  "the symptom");
            CHECK(!ScrobbleQueue::lastError(providerId).isEmpty(),
                  "subsonic: ...while the user is still TOLD something went wrong, in our own words");

            dead.url = liveUrl;
            SubsonicServerStore::update(dead);
            ScrobbleQueue::clear(providerId);
            ScrobbleQueue::setLastError(providerId, QString());
        }

        // ---- 8i. A REMOVED SERVER TAKES ITS PROVIDER WITH IT (issue #299) -----------------------------
        // The provider set used to be add-only, on the argument that a removed server's provider answers
        // configured() == false and so accepts nothing, queues nothing and posts nothing. All true, and it
        // was still installed - counted, walked by every pump, and there until the next launch.
        {
            const QString pA = SubsonicScrobbleProvider::idFor(sidA);
            const QString pB = SubsonicScrobbleProvider::idFor(sidB);

            // THE RULE, pure: which installed ids belong to a server that is gone.
            CHECK(SubsonicScrobbleProvider::staleIds({ pA, pB }, { sidA }) == QStringList{ pB },
                  "remove: the provider whose server went away is named, and only that one");
            CHECK(SubsonicScrobbleProvider::staleIds({ pA }, { sidA }).isEmpty(),
                  "remove: a server that is still configured is never named - this runs on EVERY store "
                  "change, including the one that ADDS a server");
            CHECK(SubsonicScrobbleProvider::staleIds({ pA, pB }, {}).size() == 2,
                  "remove: every server gone means every one of these providers goes");
            CHECK(SubsonicScrobbleProvider::staleIds(
                      { QStringLiteral("lastfm"), QStringLiteral("listenbrainz") }, {}).isEmpty(),
                  "remove: ...and NEVER another service's provider, whatever the server list says - that "
                  "is the whole reason the set is not rebuilt");

            // ...AND THE ORCHESTRATOR, with a live unrelated destination beside the two servers. Its state
            // is what a rebuild would have destroyed, so it is what this pins.
            SubsonicServer second = srv;
            second.id   = sidB;
            second.name = QStringLiteral("Second box");
            SubsonicServerStore::add(second);
            Settings::setScrobbleEnabled(true);
            Settings::setListenBrainzToken(QString::fromLatin1(kFakeToken));
            Settings::setListenBrainzApiUrl(QStringLiteral("http://127.0.0.1:1"));   // nobody is listening
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            ScrobbleQueue::setLastError(QStringLiteral("listenbrainz"), QString());

            Scrobbler s;
            ListenBrainzClient* lb = new ListenBrainzClient(nullptr);
            s.setProvider(lb);
            s.addProvider(new SubsonicScrobbleProvider(sidA));
            s.addProvider(new SubsonicScrobbleProvider(sidB));
            CHECK(s.providers().size() == 3, "remove: three destinations installed to begin with");

            // Give the unrelated destination REAL LIVE STATE: a listen it could not deliver, an error line
            // and a backoff climbing. This is the mid-flight state the header refuses to destroy.
            playThrough(s, musicTrack(QStringLiteral("Amber"), QStringLiteral("Still owed"), 60), 40);
            spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1
                                && !ScrobbleQueue::lastError(QStringLiteral("listenbrainz")).isEmpty(); });
            CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1,
                  "remove: the unrelated destination has a listen waiting before the removal");

            // THE REMOVAL. One server deleted in Settings; the sync reconciles by id.
            SubsonicServerStore::remove(sidB);
            QStringList installed;
            for (const ScrobbleProvider* p : s.providers()) installed.push_back(p->id());
            QStringList live;
            for (const SubsonicServer& srvRow : SubsonicServerStore::list()) live.push_back(srvRow.id);
            const QStringList stale = SubsonicScrobbleProvider::staleIds(installed, live);
            CHECK(stale == QStringList{ pB }, "remove: exactly one provider is stale");
            for (const QString& id : stale) CHECK(s.removeProvider(id), "remove: ...and it is removed");

            CHECK(s.providers().size() == 2,
                  "remove: the set SHRANK - a removed server no longer leaves an inert provider behind for "
                  "the session");
            bool stillThere = false, srvAStillThere = false, sameObject = false;
            for (const ScrobbleProvider* p : s.providers())
            {
                if (p->id() == pB) stillThere = true;
                if (p->id() == pA) srvAStillThere = true;
                if (p == static_cast<const ScrobbleProvider*>(lb)) sameObject = true;
            }
            CHECK(!stillThere, "remove: the removed server's provider is gone");
            CHECK(srvAStillThere, "remove: ...and the OTHER music server's provider is untouched");
            CHECK(sameObject,
                  "remove: the unrelated destination is the SAME OBJECT, not a recreated one - rebuilding "
                  "the set is what would abandon Last.fm's authorisation poll mid-flight");
            CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 1,
                  "remove: its queued listen is still queued");
            CHECK(!ScrobbleQueue::lastError(QStringLiteral("listenbrainz")).isEmpty(),
                  "remove: ...and its error line still says what happened");
            CHECK(!s.statusLine().contains(QStringLiteral("Second box")),
                  "remove: the status line stops naming a destination that no longer exists");

            // ...AND IT STILL WORKS. The surviving slot keeps its timer and its queue, so pointing the
            // service back at something that answers delivers the listen that was waiting through it.
            {
                FakeService svc;
                CHECK(svc.listen(), "remove: the fake listening service is up");
                Settings::setListenBrainzApiUrl(svc.root());
                s.retryNow();
                spinUntil([&] { return ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 0; }, 6000);
                CHECK(ScrobbleQueue::count(QStringLiteral("listenbrainz")) == 0,
                      "remove: the surviving destination delivers what it was owed - its slot, its timer "
                      "and its queue all came through the removal intact");
            }

            // A REMOVAL WHILE A SUBMISSION IS IN FLIGHT. The batch is on its way to a port nobody is
            // listening on when the destination is taken away; the reply is looked up by id when it lands,
            // finds nothing, and is dropped rather than written through a freed slot.
            {
                SubsonicServer third = srv;
                third.id = sidB;
                third.name = QStringLiteral("Third box");
                third.url = QStringLiteral("http://127.0.0.1:1");
                SubsonicServerStore::add(third);
                const QString trackC = Subsonic::qualify(sidB, Subsonic::Kind::Track,
                                                         QStringLiteral("s-9"));
                ScrobbleQueue::clear(pB);
                s.addProvider(new SubsonicScrobbleProvider(sidB));
                playThrough(s, subsonicTrack(trackC, QStringLiteral("In flight"), 60), 40);
                SubsonicServerStore::remove(sidB);
                CHECK(s.removeProvider(pB), "remove: the destination goes while its batch is on the wire");
                spinUntil([&] { return false; }, 1200);   // let the reply land on a slot that is not there
                CHECK(s.providers().size() == 2,
                      "remove: ...and the set is still the two that remain - a reply for a removed "
                      "destination records nothing rather than writing through a freed slot");
                CHECK(!s.statusLine().isEmpty(),
                      "remove: ...and the status line is still answerable afterwards");
                ScrobbleQueue::clear(pB);
                ScrobbleQueue::setLastError(pB, QString());
            }

            Settings::setListenBrainzApiUrl(QString());
            Settings::setListenBrainzToken(QString());
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            ScrobbleQueue::setLastError(QStringLiteral("listenbrainz"), QString());
        }

        // ---- 8j. A REMOVED SERVER'S UNSENT LISTENS ARE NOT ORPHANED (issue #337) ----------------------
        // #299 took the provider away and left the queue alone, deliberately — deleting somebody's unsent
        // history is not a decision a leak fix gets to make. What that left behind is this issue: the queue
        // is filed under an id built from the server's UUID, a re-added server mints a NEW UUID, and the
        // rows can therefore never be matched to a destination again. Undeliverable, undrainable, invisible.
        //
        // Three things are pinned here and they are three because they fail in three different ways:
        //
        //   * THE STORE, where "orphaned" is four keys and not one. clear() empties the queue and leaves the
        //     delivered counter, the dropped counter and the last error behind for ever.
        //   * THE FLUSH, whose whole reason to exist is the one moment before the sign-in is forgotten.
        //     All three endings, because a flush that half worked is the one a careless implementation
        //     reports as either "sent" or "failed" and is neither — and because a failure must not veto the
        //     removal the user asked for.
        //   * THE WORDS. The rule this issue turns on is that nothing is discarded without the user being
        //     told first, in the same interaction. That is a property of a SENTENCE, so the sentences are
        //     pure functions and the invariant is asserted over the whole matrix of outcomes.
        {
            const QString pA = SubsonicScrobbleProvider::idFor(sidA);
            const QString pB = SubsonicScrobbleProvider::idFor(sidB);
            const QString serverName = QStringLiteral("Fake Navidrome");
            Settings::setScrobbleEnabled(true);
            Settings::setScrobbleServerForwards(false);
            ScrobbleQueue::forget(pA);
            ScrobbleQueue::forget(pB);

            // Queue `n` listens for `pid` without a network in the middle: what is under test below is the
            // draining, and playing them through a Scrobbler would drain some of them on the way in.
            auto queueUp = [&](const QString& pid, int n) {
                for (int i = 0; i < n; ++i)
                {
                    Scrobble::Play p;
                    p.track      = subsonicTrack(trackA, QStringLiteral("Waiting"), 300);
                    p.listenedAt = 1700000000LL + i;
                    ScrobbleQueue::append(pid, p);
                }
            };

            // -- WHAT IS ON DISK, AND WHAT FORGETTING IT MEANS ------------------------------------------
            {
                queueUp(pA, 1);
                ScrobbleQueue::noteDelivered(pA, 7);
                ScrobbleQueue::setLastError(pA, QStringLiteral("something went wrong once"));
                CHECK(ScrobbleQueue::providerIdsOnDisk().contains(pA),
                      "#337: a queue on disk NAMES its destination - the only way to find one whose server "
                      "is gone, since nothing installs a provider for a server that is not configured, "
                      "which is exactly why these rows became invisible");
                ScrobbleQueue::clear(pA);
                CHECK(ScrobbleQueue::delivered(pA) == 7 && !ScrobbleQueue::lastError(pA).isEmpty(),
                      "#337: emptying the QUEUE leaves the counter and the error line behind - which is why "
                      "forgetting a destination is a different operation and not a synonym");
                ScrobbleQueue::forget(pA);
                CHECK(ScrobbleQueue::count(pA) == 0 && ScrobbleQueue::delivered(pA) == 0
                          && ScrobbleQueue::dropped(pA) == 0 && ScrobbleQueue::lastError(pA).isEmpty(),
                      "#337: forgetting a destination takes ALL FOUR keys with it");
                CHECK(!ScrobbleQueue::providerIdsOnDisk().contains(pA),
                      "#337: ...and nothing on disk names that destination any more");
            }

            // -- THE SWEEP'S RULE: which queues on disk belong to no configured server ------------------
            // The same pure predicate #299 already answers about INSTALLED providers, asked about what is on
            // disk. Deliberately the same function: it names our own ids only, so a Last.fm or ListenBrainz
            // queue - owed to a service that is still perfectly reachable - can never be swept up by it.
            {
                queueUp(pA, 2);                       // sidA is configured
                queueUp(pB, 3);                       // sidB was removed in 8i: the orphan
                ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
                Scrobble::Play lb;
                lb.track      = musicTrack(QStringLiteral("Amber"), QStringLiteral("Owed upstream"), 300);
                lb.listenedAt = 1700000000LL;
                ScrobbleQueue::append(QStringLiteral("listenbrainz"), lb);

                QStringList serverIds;
                for (const SubsonicServer& row : SubsonicServerStore::list()) serverIds.push_back(row.id);
                const QStringList onDisk  = ScrobbleQueue::providerIdsOnDisk();
                const QStringList orphans = SubsonicScrobbleProvider::staleIds(onDisk, serverIds);
                CHECK(onDisk.contains(pA) && onDisk.contains(pB)
                          && onDisk.contains(QStringLiteral("listenbrainz")),
                      "#337: every destination with state on disk is found, whoever is installed");
                CHECK(orphans.contains(pB),
                      "#337: a queue whose server is no longer configured is named - unreachable by "
                      "construction, and the whole of what this issue is about");
                CHECK(!orphans.contains(pA),
                      "#337: ...and a queue whose server is STILL configured is never touched");
                CHECK(!orphans.contains(QStringLiteral("listenbrainz")),
                      "#337: ...and never another service's queue, which is owed to something that is still "
                      "perfectly reachable");
                ScrobbleQueue::forget(pB);
                ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
            }

            // -- THE COUNT THE USER IS TOLD, at the moment they are told it -----------------------------
            {
                ScrobbleQueue::forget(pA);
                queueUp(pA, 3);
                queueUp(pB, 5);
                CHECK(ScrobbleQueue::count(pA) == 3,
                      "#337: the number in the confirmation is the number waiting for THAT server - a count "
                      "summed over destinations would offer to send another server's listens");
                ScrobbleQueue::forget(pB);
            }

            // -- THE FLUSH, ENDING 1: EVERYTHING LANDS --------------------------------------------------
            {
                fake.forget(); fake.answerWith.clear(); fake.scripted.clear();
                ScrobbleQueue::forget(pA);
                queueUp(pA, 3);
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                int calls = 0;
                ScrobbleFlush got;
                s.flushProvider(pA, [&](ScrobbleFlush f) { ++calls; got = f; });
                CHECK(calls == 0,
                      "#337: the answer never arrives inside the caller's own frame - the one call site "
                      "opens a nav-kit card in it, and doing that inside the press is the #28/#211 family");
                spinUntil([&] { return calls > 0; }, 8000);
                CHECK(calls == 1, "#337: ...and it arrives exactly once");
                CHECK(got.sent == 3 && got.left == 0,
                      "#337: a flush that succeeds reports everything sent and nothing left");
                CHECK(got.message.isEmpty(),
                      "#337: ...and says nothing went wrong, because nothing did");
                CHECK(ScrobbleQueue::count(pA) == 0, "#337: ...and the queue is empty");
            }

            // ...AND IT DRAINS PAST THE FIRST BATCH. Sixty listens is two batches (kBatchSize == 50), and a
            // goodbye that stopped at the end of the first would report "sent" while ten listens were about
            // to be deleted — the same silent loss this issue is about, one batch further along.
            {
                fake.forget(); fake.answerWith.clear(); fake.scripted.clear();
                ScrobbleQueue::forget(pA);
                queueUp(pA, 60);
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                int calls = 0;
                ScrobbleFlush got;
                s.flushProvider(pA, [&](ScrobbleFlush f) { ++calls; got = f; });
                spinUntil([&] { return calls > 0; }, 10000);
                CHECK(calls == 1 && got.sent == 60 && got.left == 0,
                      "#337: a flush keeps going until the QUEUE is empty, not until a batch is");
                CHECK(ScrobbleQueue::count(pA) == 0, "#337: ...leaving nothing behind to be discarded");
            }

            // -- THE FLUSH, ENDING 2: SOME LAND AND SOME DO NOT ----------------------------------------
            // Sixty listens is two batches (kBatchSize == 50). The first is accepted and the second is
            // refused, which is the ending a single fixed answer cannot produce and the one that gets
            // reported wrongly in both directions: "sent" loses ten listens silently all over again, and
            // "failed" tells the user nothing arrived when fifty did.
            {
                fake.forget(); fake.answerWith.clear();
                fake.scripted = { QByteArray(),                       // batch one: a plain ok
                                  QByteArray("<subsonic-response status=\"failed\" version=\"1.16.1\">"
                                             "<error code=\"40\" message=\"Wrong username or password.\"/>"
                                             "</subsonic-response>") };
                ScrobbleQueue::forget(pA);
                queueUp(pA, 60);
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                int calls = 0;
                ScrobbleFlush got;
                s.flushProvider(pA, [&](ScrobbleFlush f) { ++calls; got = f; });
                spinUntil([&] { return calls > 0; }, 10000);
                CHECK(calls == 1, "#337: a partly-successful flush answers once");
                CHECK(got.sent == ScrobbleQueue::kBatchSize,
                      "#337: ...reporting the batch that LANDED");
                CHECK(got.left == 60 - ScrobbleQueue::kBatchSize,
                      "#337: ...and the ones that did not, which are what the user is about to lose");
                CHECK(got.message == QStringLiteral("Wrong username or password."),
                      "#337: ...in the server's own words, which is the only account of it anybody gets");
                fake.scripted.clear();
            }

            // -- THE FLUSH, ENDING 3: NOTHING LANDS, AND THE REMOVAL IS NOT VETOED ----------------------
            // A box that is asleep. The listens stay on disk (the caller discards them, and only after
            // saying so) and - the point - the flush ANSWERS rather than climbing the retry ladder. There
            // is no later: the sign-in is about to be forgotten.
            {
                SubsonicServer live;
                SubsonicServerStore::get(sidA, live);
                const QString liveUrl = live.url;
                live.url = QStringLiteral("http://127.0.0.1:1");     // nobody is listening
                SubsonicServerStore::update(live);

                ScrobbleQueue::forget(pA);
                queueUp(pA, 3);
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                int calls = 0;
                ScrobbleFlush got;
                s.flushProvider(pA, [&](ScrobbleFlush f) { ++calls; got = f; });
                spinUntil([&] { return calls > 0; }, 10000);
                CHECK(calls == 1,
                      "#337: a flush that fails entirely still ANSWERS - a removal held open waiting for a "
                      "callback that never comes is a confirmation the user never sees");
                CHECK(got.sent == 0 && got.left == 3,
                      "#337: ...saying nothing landed and three are still waiting");
                CHECK(!got.message.isEmpty() && !got.message.contains(QStringLiteral("/rest/")),
                      "#337: ...with a reason, and never a url - for this protocol the url IS the "
                      "credential");
                CHECK(ScrobbleQueue::count(pA) == 3,
                      "#337: ...and nothing has been deleted yet: the discard belongs to the surface that "
                      "tells the user about it, not to the attempt");

                // THE DESTINATION GOING AWAY MID-FLUSH answers the caller too. In the ordinary order the
                // flush finishes first and the removal follows it, but a server can also be taken away
                // underneath one, and the waiting confirmation must not be stranded.
                {
                    ScrobbleQueue::forget(pA);
                    queueUp(pA, 2);
                    Scrobbler s2;
                    s2.setProvider(new SubsonicScrobbleProvider(sidA));
                    int calls2 = 0;
                    s2.flushProvider(pA, [&](ScrobbleFlush) { ++calls2; });
                    s2.removeProvider(pA);
                    spinUntil([&] { return calls2 > 0; }, 4000);
                    CHECK(calls2 == 1,
                          "#337: a destination removed mid-flush answers its flush exactly once");
                }

                live.url = liveUrl;
                SubsonicServerStore::update(live);
            }

            // -- THE FLUSH WITH NOTHING TO DO, AND WITH NOWHERE TO SEND --------------------------------
            {
                ScrobbleQueue::forget(pA);
                Scrobbler s;
                s.setProvider(new SubsonicScrobbleProvider(sidA));
                int calls = 0;
                ScrobbleFlush got;
                s.flushProvider(pA, [&](ScrobbleFlush f) { ++calls; got = f; });
                spinUntil([&] { return calls > 0; }, 4000);
                CHECK(calls == 1 && got.sent == 0 && got.left == 0 && got.message.isEmpty(),
                      "#337: a flush with nothing waiting answers immediately and claims nothing");

                // A queue for a destination this orchestrator does not hold at all - the shape the sweep
                // finds. It cannot be sent and the answer says so rather than hanging.
                queueUp(pB, 4);
                int calls2 = 0;
                ScrobbleFlush got2;
                s.flushProvider(pB, [&](ScrobbleFlush f) { ++calls2; got2 = f; });
                spinUntil([&] { return calls2 > 0; }, 4000);
                CHECK(calls2 == 1 && got2.sent == 0 && got2.left == 4 && !got2.message.isEmpty(),
                      "#337: a queue with no installed destination is answered, not hung - nothing sent, "
                      "four still waiting, and a reason");
                ScrobbleQueue::forget(pB);
            }

            // -- THE WORDS: NOTHING IS DISCARDED WITHOUT THE USER BEING TOLD ---------------------------
            {
                const QString offer = ScrobbleRemoval::offerMessage(serverName, 3);
                CHECK(offer.contains(QStringLiteral("3")) && offer.contains(serverName),
                      "#337: the offer names the server and how many listens are at stake");
                CHECK(offer.contains(QStringLiteral("deleted")),
                      "#337: ...and says, BEFORE the removal, that what is not sent is deleted - the whole "
                      "rule this issue turns on is that the loss is stated first, not discovered later");
                CHECK(ScrobbleRemoval::sendLabel(3).contains(QStringLiteral("3"))
                          && ScrobbleRemoval::discardLabel(3).contains(QStringLiteral("3")),
                      "#337: ...and so do the buttons, so somebody who read only those still knows the "
                      "cost of the one on the right");

                // THE INVARIANT, over every outcome a flush can have. A message that describes a discard
                // NAMES THE NUMBER discarded; a message that describes no discard never says one happened;
                // and no outcome is left with nothing to show at all.
                bool told = true, quiet = true, never = true, reason = true;
                const QString why = QStringLiteral("Wrong username or password.");
                for (int sent : { 0, 1, 7, 50 })
                    for (int left : { 0, 1, 7, 50 })
                        for (const QString& w : { QString(), why })
                        {
                            const QString m = ScrobbleRemoval::outcomeMessage(serverName, sent, left, w);
                            if (m.trimmed().isEmpty()) never = false;
                            if (left > 0 && !(m.contains(QString::number(left))
                                              && m.contains(QStringLiteral("discard")))) told = false;
                            if (left <= 0 && m.contains(QStringLiteral("discard"))) quiet = false;
                            if (left > 0 && !w.isEmpty() && !m.contains(w)) reason = false;
                        }
                CHECK(told,
                      "#337: EVERY outcome that discards listens says so and names how many - the one "
                      "thing this issue forbids is data going away with nobody being told");
                CHECK(quiet,
                      "#337: ...and an outcome that discarded nothing never claims it did, which is the "
                      "same lie in the other direction");
                CHECK(reason,
                      "#337: ...and when the service said why, the user is given its words");
                CHECK(never, "#337: ...and no outcome leaves the user with an empty card");

                const QString sweep = ScrobbleRemoval::sweepMessage(1, 9);
                CHECK(sweep.contains(QStringLiteral("9")),
                      "#337: the sweep of the orphans that ALREADY exist names what it found");
                CHECK(sweep.contains(QStringLiteral("no longer set up")),
                      "#337: ...and says why nothing can be sent, rather than leaving somebody hunting for "
                      "the button that would send them");
                CHECK(ScrobbleRemoval::sweepDiscardLabel(9).contains(QStringLiteral("9")),
                      "#337: ...and its delete button names the number too");
            }

            ScrobbleQueue::forget(pA);
            ScrobbleQueue::forget(pB);
            ScrobbleQueue::clear(QStringLiteral("listenbrainz"));
        }

        SubsonicServerStore::remove(sidA);
        Settings::setScrobbleEnabled(false);
        Settings::setScrobbleServerForwards(false);
    }

    if (fails) { printf("SCROBBLE-FAIL %d\n", fails); return 1; }
    printf("SCROBBLE-OK\n");
    return 0;
}
