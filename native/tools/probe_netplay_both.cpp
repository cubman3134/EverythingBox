// Headless check of the "Both" online orchestration (hostOnline / joinOnline), isolating each path:
//   mode "direct": relay points at a dead port, so ONLY a direct connection can pair the two sessions.
//                  Verifies host Path A (direct server wins) + joinOnline's direct-success branch.
//   mode "relay":  the joiner is given a dead direct endpoint, so the direct attempt fails and it MUST fall
//                  back to the relay. Verifies host Path B (relay wins) + joinOnline's relay fallback.
//   usage: probe_netplay_both <direct|relay> [relayPort]   (relay mode needs netplay-relay.py on relayPort)
//
// Nothing here is on a stopwatch (issue #164). Two probe processes on one machine used to share a hard-coded
// direct port and the room code "TESTROOM", so a concurrent run's joiner could connect to THIS run's host and
// win its direct race, and a concurrent run's host could take the room code out from under it. Both of those
// surfaced as an unexplained red gate. So: the direct port comes from the OS, the room code is unique to this
// process, and every wait is on a condition (the relay says the room is open; both sides say they started)
// with only a far-away ceiling as the failure deadline. A trace of the rendezvous is printed either way — a
// failure here should be readable without a rebuild.
#include "NetplaySession.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QString>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    g_clock.start();
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("direct");
    const quint16 relayPort = argc > 2 ? quint16(atoi(argv[2])) : 55677;
    const bool directMode = (mode == QStringLiteral("direct"));

    const QString relayHost = QStringLiteral("127.0.0.1");
    const quint16 usedRelayPort = directMode ? quint16(1) : relayPort;   // dead relay in direct mode
    // Unique to this process: two probes running at once must not be able to see each other's room.
    const QString room = QStringLiteral("EBPROBE-%1").arg(QCoreApplication::applicationPid());
    const int kCeilingMs = 20000;   // a real failure still has to end; this is not a pass/fail boundary

    NetplaySession host, join;
    host.gameId = join.gameId = QStringLiteral("game|123");
    host.coreName = join.coreName = QStringLiteral("testcore");
    const QByteArray stateSent = "SAVESTATE-BYTES";
    QByteArray stateGot;
    host.serializeState = [&] { return stateSent; };
    join.applyState = [&](const QByteArray& b) { stateGot = b; };

    bool hostStarted = false, joinStarted = false;
    QObject::connect(&host, &NetplaySession::started, [&] { hostStarted = true; trace("[host]", QStringLiteral("STARTED")); });
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
    auto startJoin = [&] {
        join.joinOnline(relayHost, usedRelayPort, room, QStringLiteral("127.0.0.1"), joinDirectPort);
    };
    if (directMode)
    {
        startJoin();   // the direct server is listening the moment hostOnline() returns — nothing to wait for
    }
    else
    {
        // Wait for the relay to confirm the room exists. A timer here is exactly the race this probe kept losing:
        // a JOIN that beats its own host's registration gets NOHOST.
        QObject::connect(&host, &NetplaySession::roomOpen, [&] { trace("[host]", QStringLiteral("ROOM OPEN")); startJoin(); });
    }

    // Two impostors are pointed at the host's direct port. Anything can reach that port — in the app it is
    // deliberately UPnP-forwarded to the internet — so "someone connected" must not be read as "player 2
    // arrived". If an impostor can win the host's direct race, the host closes its listener AND drops its relay
    // socket, the room vanishes, and the real joiner's relay fallback gets NOHOST: precisely the
    // hostStarted=1 joinStarted=0 shape this probe used to fail the gate with (issue #164). One impostor says
    // nothing at all, the other announces the wrong room; neither may be adopted, and the wrong-room one must be
    // hung up on rather than merely ignored.
    QTcpSocket silentStranger, wrongRoomStranger;
    bool wrongRoomDropped = false;
    auto strangerDropped = [&] { wrongRoomDropped = true; trace("[stranger]", QStringLiteral("dropped by the host")); };
    QObject::connect(&wrongRoomStranger, &QTcpSocket::connected, [&] {
        wrongRoomStranger.write("EBNP1 NOT-YOUR-ROOM\n"); });
    QObject::connect(&wrongRoomStranger, &QTcpSocket::disconnected, strangerDropped);
    QObject::connect(&wrongRoomStranger, &QAbstractSocket::errorOccurred, [&](QAbstractSocket::SocketError) {
        if (wrongRoomStranger.state() != QAbstractSocket::ConnectedState) strangerDropped(); });
    silentStranger.connectToHost(QStringLiteral("127.0.0.1"), hostDirectPort);
    wrongRoomStranger.connectToHost(QStringLiteral("127.0.0.1"), hostDirectPort);

    // Direct mode only: an ADOPTED direct peer must still be connected after the host's greeting deadline has
    // come and gone. The deadline exists to hang up on candidates that never identify themselves, and it is armed
    // on a socket that may since have become the session — so if it is not disarmed on adoption it hangs up on
    // player 2 mid-game instead. The first version of that guard did exactly this; only holding the session past
    // the deadline catches it, since the rest of the probe is finished within a tenth of a second.
    const qint64 holdUntilMs = NetplaySession::kDirectGreetingTimeoutMs + 750;
    bool inputSent = false, lateInputSent = false;
    QTimer poll;
    poll.setInterval(25);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        const bool paired = hostStarted && joinStarted && stateGot == stateSent && wrongRoomDropped;
        const bool overdue = g_clock.elapsed() >= kCeilingMs;
        if (!inputSent)
        {
            if (paired) { inputSent = true; host.sendLocalInput(0, 0x1234); return; }
            if (!overdue) return;
        }
        quint16 rb = 0;
        const bool gotInput = join.remoteInput(0, rb);
        if (!gotInput && !overdue) return;

        quint16 lateRb = 0;
        bool gotLate = true;
        if (directMode && gotInput)
        {
            if (!lateInputSent)
            {
                if (g_clock.elapsed() < holdUntilMs && !overdue) return;   // outlive the greeting deadline
                lateInputSent = true;
                host.sendLocalInput(7, 0x0ab1);
                return;
            }
            gotLate = join.remoteInput(7, lateRb) && lateRb == 0x0ab1;
            if (!gotLate && !overdue) return;
        }

        poll.stop();
        printf("mode=%s hostStarted=%d joinStarted=%d stateSynced=%d input=%d(0x%04x) strangerRejected=%d survivedGreetingDeadline=%d\n",
               qUtf8Printable(mode), hostStarted, joinStarted, int(stateGot == stateSent), int(gotInput), rb,
               int(wrongRoomDropped), int(gotLate));
        const bool ok = hostStarted && joinStarted && stateGot == stateSent && gotInput && rb == 0x1234
                     && wrongRoomDropped && gotLate;
        printf("%s\n", ok ? "NETPLAY-BOTH-OK" : "FAIL");
        app.exit(ok ? 0 : 1);
    });
    poll.start();
    return app.exec();
}
