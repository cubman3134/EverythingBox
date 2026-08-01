// Headless check of the "Both" online orchestration (hostOnline / joinOnline), isolating each path:
//   mode "direct": relay points at a dead port, so ONLY a direct connection can pair the two sessions.
//                  Verifies host Path A (direct server wins) + joinOnline's direct-success branch, and every
//                  guard that stands between an inbound stranger and adoption.
//   mode "relay":  the joiner is given a dead direct endpoint, so the direct attempt fails and it MUST fall
//                  back to the relay. Verifies host Path B (relay wins) + joinOnline's relay fallback.
//   mode "slowconnect": the joiner reaches a host whose CONNECT and whose ANSWER each take most of the
//                  give-up budget but neither takes all of it. Verifies that the give-up deadline measures
//                  the handshake rather than the connect plus the handshake, and that stop() disarms it.
//   usage: probe_netplay_both <direct|relay|slowconnect> [relayPort]   (relay mode needs netplay-relay.py)
//
// Nothing here is on a stopwatch (issue #164). Two probe processes on one machine used to share a hard-coded
// direct port and the room code "TESTROOM", so a concurrent run's joiner could connect to THIS run's host and
// win its direct race, and a concurrent run's host could take the room code out from under it. Both of those
// surfaced as an unexplained red gate. So: the direct port comes from the OS, the room code is unique to this
// process, and every wait is on a condition (the relay says the room is open; the host hung up on the
// impostor; both sides say they started) with only a far-away ceiling as the failure deadline. The two waits
// that ARE durations — "an unproven peer is dropped by the greeting deadline" and "an adopted peer is still
// alive past it" — are the assertions themselves, and each is bounded on both sides. A trace of the
// rendezvous is printed either way: a failure here should be readable without a rebuild.
#include "NetplaySession.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QSharedPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QString>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// A real failure still has to end; this is a ceiling, never a pass/fail boundary. Direct mode deliberately
// spends ~11s of it (5s watching an unproven peer time out, then 5.75s watching an adopted one NOT time out).
static const int kCeilingMs = 30000;

// A loopback port nothing is listening on, so a connect to it is REFUSED rather than swallowed. Reserving one
// from the ephemeral range and releasing it beats naming a low port: on Windows a connect to 127.0.0.1:1 or :9
// is dropped, not refused, so "the dead endpoint" cost four seconds of SYN timeout on every relay-mode run.
// If the port is re-taken in the gap the joiner just falls back on its timer instead, which is still correct.
static quint16 reserveClosedPort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) return 1;
    const quint16 p = probe.serverPort();
    probe.close();
    return p;
}

static QElapsedTimer g_clock;
static void trace(const char* who, const QString& what)
{
    printf("[%6lld] %s %s\n", static_cast<long long>(g_clock.elapsed()), who, qUtf8Printable(what));
    fflush(stdout);
}

// The session wire format, built by hand so the fake host in slowconnect mode can speak it without a
// NetplaySession of its own: [u32 length][u8 type][payload], length covers type+payload.
static QByteArray wireFrame(quint8 type, const QByteArray& payload)
{
    const quint32 v = quint32(1 + payload.size());
    QByteArray f;
    f.append(char(v >> 24)); f.append(char(v >> 16)); f.append(char(v >> 8)); f.append(char(v));
    f.append(char(type));
    f += payload;
    return f;
}

// ---------------------------------------------------------------------------------------------------------
// slowconnect: the give-up deadline must measure the HANDSHAKE, not the connect plus the handshake.
//
// `resolved` means "the host answered" — completing a TCP connect never meant the host had accepted us as its
// player 2, and treating it that way is the defect this whole branch exists to fix. But that made the 4s
// give-up deadline cover BOTH phases: on a link where the connect costs most of the budget, the deadline can
// expire while the greeting is still in flight. The joiner then aborts the socket and tears the relay room
// down on its way out — and the host, whose adoption is irreversible (it closes its listener and drops its
// relay socket), commits to that same socket a moment later. Both sides strand, each with a message that
// describes the other's problem. On loopback the connect is free and the window is invisible; it is widest on
// exactly the slow, distant links the relay path exists for, which is the case netplay has never been run on.
//
// Making the connect slow ON PURPOSE is the only way to separate the two phases, and it cannot be done with a
// plain loopback socket: the kernel completes that handshake before Qt gets a turn. So the joiner is pointed
// at an HTTP CONNECT proxy, which puts an application-level exchange INSIDE the connect — QTcpSocket does not
// emit connected() until the proxy answers 200. The proxy answers late, and then plays the host itself,
// answering the greeting late again. Neither phase alone exceeds the budget; together they do.
static int runSlowConnect()
{
    // Expressed as fractions of the budget, so the arithmetic keeps holding if the budget is retuned.
    const int connectDelay = NetplaySession::kDirectGiveUpMs * 5 / 8;   // 62.5% of one budget
    const int answerDelay  = NetplaySession::kDirectGiveUpMs / 2;       // 50% of one budget
    // 112.5% of ONE budget: past a single deadline armed at joinOnline, inside either of the two real ones.

    QTcpServer fake;   // the delaying CONNECT proxy AND, once tunnelled, the host on the far side of it
    if (!fake.listen(QHostAddress::LocalHost, 0)) { printf("FAIL (the fake proxy did not bind)\n"); return 1; }

    const QString room = QStringLiteral("EBPROBE-%1").arg(QCoreApplication::applicationPid());
    const QByteArray wantGreeting = QByteArray("EBNP1 ") + room.toUtf8();
    const QByteArray stateSent = "SAVESTATE-BYTES";
    QByteArray greetingSeen, stateGot;
    bool tunnelUp = false, proxySawConnect = false;
    // The joiner's nominal direct endpoint. It is never dialled — the fake proxy answers CONNECT itself and
    // then plays the host — but it may NOT be a loopback address: QAbstractSocketPrivate::resolveProxy forces
    // NoProxy for localhost and 127.0.0.0/8, which silently takes the whole mechanism out and leaves the probe
    // measuring a connect to a dead port instead. TEST-NET-1 (RFC 5737) is reserved for exactly this and is
    // guaranteed not to route anywhere, so a bypass shows up as a hang rather than as traffic.
    const QString directIp = QStringLiteral("192.0.2.1");
    const quint16 directPort = 55420;

    NetplaySession join;
    join.gameId = QStringLiteral("game|123");
    join.coreName = QStringLiteral("testcore");
    join.applyState = [&](const QByteArray& b) { stateGot = b; };

    bool joinStarted = false, fellBack = false;
    qint64 joinStartedMs = -1;
    QObject::connect(&join, &NetplaySession::started, [&] {
        joinStarted = true; joinStartedMs = g_clock.elapsed(); trace("[join]", QStringLiteral("STARTED")); });
    QObject::connect(&join, &NetplaySession::status, [&](const QString& s) {
        // The fallback announces itself ("Direct connection failed — using the relay…", then "Connecting to
        // the netplay relay…"). No translator is installed in a probe, so this is the literal text.
        if (s.contains(QStringLiteral("relay"), Qt::CaseInsensitive)) fellBack = true;
        trace("[join]", s); });
    QObject::connect(&join, &NetplaySession::ended, [](const QString& r) { trace("[join]", QStringLiteral("ENDED: ") + r); });

    QObject::connect(&fake, &QTcpServer::newConnection, [&] {
        QTcpSocket* c = fake.nextPendingConnection();
        if (!c) return;
        auto buf = QSharedPointer<QByteArray>::create();
        QObject::connect(c, &QTcpSocket::readyRead, c, [&, c, buf] {
            *buf += c->readAll();
            if (!tunnelUp)
            {
                if (!buf->contains("\r\n\r\n")) return;               // wait for the whole CONNECT request
                buf->clear();
                proxySawConnect = true;
                trace("[proxy]", QStringLiteral("CONNECT received — stalling the tunnel for %1 ms").arg(connectDelay));
                QTimer::singleShot(connectDelay, c, [&, c] {
                    tunnelUp = true;
                    c->write("HTTP/1.1 200 Connection established\r\n\r\n");
                    trace("[proxy]", QStringLiteral("tunnel established (the joiner's connect completes now)"));
                });
                return;
            }
            const int nl = buf->indexOf('\n');
            if (nl < 0 || !greetingSeen.isEmpty()) return;
            greetingSeen = buf->left(nl).trimmed();
            trace("[host]", QStringLiteral("greeting = \"%1\" — answering in %2 ms")
                                .arg(QString::fromLatin1(greetingSeen)).arg(answerDelay));
            QTimer::singleShot(answerDelay, c, [&, c] {
                const QJsonObject hello{ { QStringLiteral("gameId"), QStringLiteral("game|123") },
                                         { QStringLiteral("core"),   QStringLiteral("testcore") } };
                c->write(wireFrame(2, QJsonDocument(hello).toJson(QJsonDocument::Compact)));
                c->write(wireFrame(3, stateSent));
                trace("[host]", QStringLiteral("HELLO + STATE sent"));
            });
        });
    });

    // A second session that is STOPPED inside the give-up window and must stay stopped (issue #164 review,
    // minor 3). The deadline is a singleShot on the SESSION, so destroying its socket does not disarm it:
    // before the generation check, "Leave" during those four seconds ran joinViaRelay afterwards and put the
    // player back into a session they had deliberately quit. Costs no wall time — it rides along.
    NetplaySession abandoned;
    int abandonedSignals = 0;
    bool abandonedStopped = false;
    auto countAfterStop = [&] { if (abandonedStopped) ++abandonedSignals; };
    QObject::connect(&abandoned, &NetplaySession::status, [&](const QString& s) {
        countAfterStop(); if (abandonedStopped) trace("[left]", QStringLiteral("late status: ") + s); });
    QObject::connect(&abandoned, &NetplaySession::ended, [&](const QString& r) {
        countAfterStop(); if (abandonedStopped) trace("[left]", QStringLiteral("late ended: ") + r); });

    const quint16 deadRelay = reserveClosedPort();
    const quint16 deadTarget = reserveClosedPort();
    // A host that ACCEPTS and then says nothing, so the abandoned attempt sits in exactly the state the window
    // is about — connected, greeting sent, waiting. A refused endpoint would resolve itself before stop() was
    // ever reached on the platforms that report a refused loopback connect synchronously, and the assertion
    // would then pass without the timer having been armed at all.
    QTcpServer sink;
    if (!sink.listen(QHostAddress::LocalHost, 0)) { printf("FAIL (the silent sink did not bind)\n"); return 1; }
    abandoned.joinOnline(QStringLiteral("127.0.0.1"), deadRelay, room, QStringLiteral("127.0.0.1"), sink.serverPort());
    abandoned.stop();
    abandonedStopped = true;
    trace("[left]", QStringLiteral("joinOnline then stop() — nothing may follow"));

    // The application proxy is the only way to reach the socket joinOnline creates. It is set for exactly the
    // duration of that call: the relay-fallback socket is created later, and must be direct.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), fake.serverPort()));
    trace("[join]", QStringLiteral("joinOnline via a proxy that stalls %1 ms, then a host that answers %2 ms later "
                                   "(give-up budget %3 ms per phase)")
                        .arg(connectDelay).arg(answerDelay).arg(NetplaySession::kDirectGiveUpMs));
    join.joinOnline(QStringLiteral("127.0.0.1"), deadRelay, room, directIp, directPort);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    QTimer poll;
    poll.setInterval(25);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        const bool overdue = g_clock.elapsed() >= kCeilingMs;
        const bool synced = joinStarted && stateGot == stateSent;
        // The abandoned session's deadline expires one budget after its (never-completed) connect. Hold until
        // then, so "it never re-entered" is a fact and not a race the probe happened to win.
        const bool abandonedSettled = g_clock.elapsed() >= NetplaySession::kDirectGiveUpMs + 750;
        if (!(synced && abandonedSettled) && !overdue) return;
        poll.stop();

        const bool greetingOk = greetingSeen == wantGreeting;
        const bool leftStayedLeft = !abandoned.active() && abandonedSignals == 0;
        printf("mode=slowconnect connectDelay=%d answerDelay=%d budget=%d proxyEngaged=%d joinStarted=%d(at %lldms) "
               "stateSynced=%d greetingOk=%d fellBack=%d leftStayedLeft=%d\n",
               connectDelay, answerDelay, NetplaySession::kDirectGiveUpMs, int(proxySawConnect), int(joinStarted),
               static_cast<long long>(joinStartedMs), int(stateGot == stateSent), int(greetingOk),
               int(fellBack), int(leftStayedLeft));
        const bool ok = proxySawConnect && joinStarted && stateGot == stateSent && greetingOk && !fellBack && leftStayedLeft;
        if (!proxySawConnect)
            printf("  the joiner never issued a CONNECT: the stalling proxy was bypassed, so the connect phase "
                   "cost nothing and this probe is asserting nothing about which phase the deadline covers\n");
        if (!ok && proxySawConnect && fellBack)
            printf("  the joiner gave up on the direct path while the handshake was still in flight — the "
                   "give-up deadline is covering the connect as well as the handshake\n");
        if (!ok && !leftStayedLeft)
            printf("  a session that was stop()ped inside the give-up window came back to life\n");
        printf("%s\n", ok ? "NETPLAY-BOTH-OK" : "FAIL");
        QCoreApplication::exit(ok ? 0 : 1);
    });
    poll.start();
    return QCoreApplication::exec();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    g_clock.start();
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("direct");
    const quint16 relayPort = argc > 2 ? quint16(atoi(argv[2])) : 55677;
    const bool directMode = (mode == QStringLiteral("direct"));
    if (mode == QStringLiteral("slowconnect")) return runSlowConnect();

    const QString relayHost = QStringLiteral("127.0.0.1");
    const quint16 usedRelayPort = directMode ? quint16(1) : relayPort;   // dead relay in direct mode
    // Unique to this process: two probes running at once must not be able to see each other's room.
    const QString room = QStringLiteral("EBPROBE-%1").arg(QCoreApplication::applicationPid());

    NetplaySession host, join;
    host.gameId = join.gameId = QStringLiteral("game|123");
    host.coreName = join.coreName = QStringLiteral("testcore");
    const QByteArray stateSent = "SAVESTATE-BYTES";
    QByteArray stateGot;
    host.serializeState = [&] { return stateSent; };
    join.applyState = [&](const QByteArray& b) { stateGot = b; };

    bool hostStarted = false, joinStarted = false;
    qint64 hostStartedMs = -1;
    QObject::connect(&host, &NetplaySession::started, [&] {
        hostStarted = true; hostStartedMs = g_clock.elapsed(); trace("[host]", QStringLiteral("STARTED")); });
    QObject::connect(&join, &NetplaySession::started, [&] { joinStarted = true; trace("[join]", QStringLiteral("STARTED")); });
    QObject::connect(&host, &NetplaySession::status, [](const QString& s) { trace("[host]", s); });
    QObject::connect(&join, &NetplaySession::status, [](const QString& s) { trace("[join]", s); });
    QObject::connect(&host, &NetplaySession::ended, [](const QString& r) { trace("[host]", QStringLiteral("ENDED: ") + r); });
    QObject::connect(&join, &NetplaySession::ended, [](const QString& r) { trace("[join]", QStringLiteral("ENDED: ") + r); });

    // Port 0: the OS hands out a free direct port, which the joiner then reads back. Nothing else on the machine
    // can be listening on it, and nothing else can dial it by guessing a constant.
    host.hostOnline(0, relayHost, usedRelayPort, room);
    const quint16 hostDirectPort = host.directPort();
    trace("[host]", QStringLiteral("direct port = %1, room = %2").arg(hostDirectPort).arg(room));
    if (hostDirectPort == 0) { printf("FAIL (host's direct server did not bind)\n"); return 1; }

    // The joiner's target: the host's real port in direct mode, a refused one in relay mode so the direct attempt
    // loses immediately and the relay fallback is what gets exercised.
    const quint16 joinDirectPort = directMode ? hostDirectPort : reserveClosedPort();
    bool joinRequested = false;
    auto startJoin = [&] {
        joinRequested = true;
        join.joinOnline(relayHost, usedRelayPort, room, QStringLiteral("127.0.0.1"), joinDirectPort);
    };

    // Impostors, pointed at the host's direct port. Anything can reach that port — in the app it is deliberately
    // UPnP-forwarded to the internet — so "someone connected" must not be read as "player 2 arrived". If an
    // impostor can win the host's direct race, the host closes its listener AND drops its relay socket, the room
    // vanishes, and the real joiner's relay fallback gets NOHOST: precisely the hostStarted=1 joinStarted=0 shape
    // this probe used to fail the gate with (issue #164).
    //
    // Being ignored is not enough for any of them. Each must be HUNG UP ON, and each by a different guard:
    //   * wrongRoom — announces a room that isn't ours: the code comparison drops it, at once.
    //   * flood     — sends 512+ bytes and never a newline: the cap drops it, well inside the greeting deadline.
    //                 Uncapped, a peer can pin the host's memory for the whole deadline just by talking.
    //   * silent    — says nothing at all: only the greeting deadline can end it, and it must, or every scan of
    //                 that port leaves an accepted socket parked on the host until the session does.
    // Both of those last two are DURATIONS, and both are asserted from both sides: dropped, and dropped when.
    QTcpSocket silentStranger, wrongRoomStranger, floodStranger;
    bool wrongRoomDropped = false, silentDropped = false, floodDropped = false;
    qint64 silentDropMs = -1, floodDropMs = -1, wrongRoomDropMs = -1;
    auto watchDrop = [&](QTcpSocket* s, const char* name, bool* flag, qint64* atMs) {
        auto note = [&, flag, atMs, name] {
            if (*flag) return;
            *flag = true; *atMs = g_clock.elapsed();
            trace("[stranger]", QStringLiteral("%1 dropped by the host").arg(QString::fromLatin1(name)));
        };
        QObject::connect(s, &QTcpSocket::disconnected, note);
        // abort() on the host's side arrives as a reset, not a clean close, so the error is the usual report.
        QObject::connect(s, &QAbstractSocket::errorOccurred, [s, note](QAbstractSocket::SocketError) {
            if (s->state() != QAbstractSocket::ConnectedState) note(); });
    };
    QObject::connect(&wrongRoomStranger, &QTcpSocket::connected, [&] {
        wrongRoomStranger.write("EBNP1 NOT-YOUR-ROOM\n"); });
    watchDrop(&wrongRoomStranger, "wrong-room", &wrongRoomDropped, &wrongRoomDropMs);
    silentStranger.connectToHost(QStringLiteral("127.0.0.1"), hostDirectPort);
    wrongRoomStranger.connectToHost(QStringLiteral("127.0.0.1"), hostDirectPort);

    if (directMode)
    {
        // The two timed guards are only observable while the host is still WAITING: adoption closes the
        // listener and takes every candidate still on it with it. So in direct mode player 2 is held back
        // until all three impostors have been hung up on — which also makes the pass mean "the host survived
        // the whole onslaught with both paths still armed", not merely "a good peer got in first".
        watchDrop(&silentStranger, "silent", &silentDropped, &silentDropMs);
        watchDrop(&floodStranger, "flood", &floodDropped, &floodDropMs);
        QObject::connect(&floodStranger, &QTcpSocket::connected, [&] {
            floodStranger.write(QByteArray(4096, 'A')); });   // not one newline in it: only the cap can end this
        floodStranger.connectToHost(QStringLiteral("127.0.0.1"), hostDirectPort);
    }
    else
    {
        // Wait for the relay to confirm the room exists. A timer here is exactly the race this probe kept losing:
        // a JOIN that beats its own host's registration gets NOHOST.
        QObject::connect(&host, &NetplaySession::roomOpen, [&] { trace("[host]", QStringLiteral("ROOM OPEN")); startJoin(); });
    }

    bool inputSent = false, lateInputSent = false;
    QTimer poll;
    poll.setInterval(25);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        const bool overdue = g_clock.elapsed() >= kCeilingMs;
        if (directMode && !joinRequested)
        {
            if (silentDropped && wrongRoomDropped && floodDropped) startJoin();
            else if (!overdue) return;
        }
        const bool strangersHandled = wrongRoomDropped && (!directMode || (silentDropped && floodDropped));
        const bool paired = hostStarted && joinStarted && stateGot == stateSent && strangersHandled;
        if (!inputSent)
        {
            if (paired) { inputSent = true; host.sendLocalInput(0, 0x1234); return; }
            if (!overdue) return;
        }
        quint16 rb = 0;
        const bool gotInput = join.remoteInput(0, rb);
        if (!gotInput && !overdue) return;

        // Direct mode only: an ADOPTED direct peer must still be connected after the host's greeting deadline has
        // come and gone. The deadline exists to hang up on candidates that never identify themselves, and it is
        // armed on a socket that may since have become the session — so if it is not disarmed on adoption it
        // hangs up on player 2 mid-game instead. The first version of that guard did exactly this; only holding
        // the session past the deadline catches it. Measured from ADOPTION, not from process start, so it stays
        // meaningful however long the impostor phase before it took.
        quint16 lateRb = 0;
        bool gotLate = true;
        if (directMode && gotInput)
        {
            if (!lateInputSent)
            {
                if (g_clock.elapsed() < hostStartedMs + NetplaySession::kDirectGreetingTimeoutMs + 750 && !overdue) return;
                lateInputSent = true;
                host.sendLocalInput(7, 0x0ab1);
                return;
            }
            gotLate = join.remoteInput(7, lateRb) && lateRb == 0x0ab1;
            if (!gotLate && !overdue) return;
        }

        poll.stop();
        // The silent peer must be dropped BY the greeting deadline and not appreciably before it: hanging up on
        // an unproven peer early is how a real player 2 on a slow link gets refused. The flood peer must be gone
        // well before that deadline, or the cap did nothing and the deadline is what dropped it.
        const qint64 kSilentFloorMs = NetplaySession::kDirectGreetingTimeoutMs / 2;
        const qint64 kSilentCeilMs = NetplaySession::kDirectGreetingTimeoutMs + 2000;
        const qint64 kCapCeilMs = NetplaySession::kDirectGreetingTimeoutMs / 2;
        const bool silentOk = !directMode || (silentDropped && silentDropMs >= kSilentFloorMs && silentDropMs <= kSilentCeilMs);
        const bool floodOk = !directMode || (floodDropped && floodDropMs <= kCapCeilMs);
        printf("mode=%s hostStarted=%d joinStarted=%d stateSynced=%d input=%d(0x%04x) strangerRejected=%d "
               "silentDropped=%d(at %lldms) floodDropped=%d(at %lldms) survivedGreetingDeadline=%d\n",
               qUtf8Printable(mode), hostStarted, joinStarted, int(stateGot == stateSent), int(gotInput), rb,
               int(wrongRoomDropped), int(silentOk), static_cast<long long>(silentDropMs),
               int(floodOk), static_cast<long long>(floodDropMs), int(gotLate));
        if (directMode && !silentOk)
            printf("  a peer that never announced itself was expected to be hung up on between %lldms and "
                   "%lldms after it connected (the greeting deadline is %dms)\n",
                   static_cast<long long>(kSilentFloorMs), static_cast<long long>(kSilentCeilMs),
                   NetplaySession::kDirectGreetingTimeoutMs);
        if (directMode && !floodOk)
            printf("  a peer that floods bytes with no newline was expected to hit the 512-byte cap within "
                   "%lldms — a later drop is the greeting deadline doing it, with the cap asserting nothing\n",
                   static_cast<long long>(kCapCeilMs));
        const bool ok = hostStarted && joinStarted && stateGot == stateSent && gotInput && rb == 0x1234
                     && wrongRoomDropped && silentOk && floodOk && gotLate;
        printf("%s\n", ok ? "NETPLAY-BOTH-OK" : "FAIL");
        app.exit(ok ? 0 : 1);
    });
    poll.start();
    return app.exec();
}
