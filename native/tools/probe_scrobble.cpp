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
#include "Scrobble.h"
#include "ScrobbleQueue.h"
#include "Scrobbler.h"
#include "ListenBrainzClient.h"
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
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
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

    if (fails) { printf("SCROBBLE-FAIL %d\n", fails); return 1; }
    printf("SCROBBLE-OK\n");
    return 0;
}
