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

    if (fails) { printf("SCROBBLE-FAIL %d\n", fails); return 1; }
    printf("SCROBBLE-OK\n");
    return 0;
}
