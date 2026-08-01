// Headless check of the relay netplay path: spin up two real NetplaySession objects, connect one via
// hostViaRelay and the other via joinViaRelay to a locally-running relay, and verify the handshake
// (started() on both), the state sync (host's serialized state reaches the joiner), and an input exchange.
//   usage: probe_netplay [relayPort]   (needs netplay-relay.py running on that port)
//
// The room code is unique to this process and the joiner waits for the relay's HOSTED acknowledgement rather
// than a timer — see the header of probe_netplay_both.cpp and issue #164 for why both of those matter.
#include "NetplaySession.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

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
    const quint16 relayPort = argc > 1 ? quint16(atoi(argv[1])) : 55677;
    const QString room = QStringLiteral("EBPROBE-%1").arg(QCoreApplication::applicationPid());
    const int kCeilingMs = 20000;

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

    // Join only once the relay says the room is registered — a JOIN that beats its own HOST gets NOHOST.
    QObject::connect(&host, &NetplaySession::roomOpen, [&] {
        trace("[host]", QStringLiteral("ROOM OPEN"));
        join.joinViaRelay(QStringLiteral("127.0.0.1"), relayPort, room);
    });
    host.hostViaRelay(QStringLiteral("127.0.0.1"), relayPort, room);

    // Second act: a room whose host has gone away must be answered with NOHOST. The relay parks a lone host in a
    // one-second poll, so for up to a second after it dies its room is still in the table — and pairing someone
    // into it hands them a PAIRED on a pipe with nobody at the other end, which surfaces as an emulator that sits
    // on "syncing game state" forever rather than as an error. `ghost` registers a room and is then killed; the
    // orphan joining it has to be turned away.
    NetplaySession ghost, orphan;
    const QString deadRoom = room + QStringLiteral("-DEAD");
    bool orphanTurnedAway = false, orphanStarted = false;
    QObject::connect(&orphan, &NetplaySession::started, [&] { orphanStarted = true; });
    QObject::connect(&orphan, &NetplaySession::ended, [&](const QString& r) {
        trace("[orphan]", QStringLiteral("ENDED: ") + r);
        orphanTurnedAway = r.contains(QStringLiteral("No one is hosting"));
    });
    QObject::connect(&ghost, &NetplaySession::roomOpen, [&] {
        ghost.stop();                                   // the host vanishes without unregistering
        orphan.joinViaRelay(QStringLiteral("127.0.0.1"), relayPort, deadRoom);
    });

    bool inputSent = false, deadRoomTried = false;
    QTimer poll;
    poll.setInterval(25);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        const bool overdue = g_clock.elapsed() >= kCeilingMs;
        const bool paired = hostStarted && joinStarted && stateGot == stateSent;
        if (!inputSent)
        {
            if (paired) { inputSent = true; host.sendLocalInput(0, 0x1234); return; }  // host -> joiner, over the pipe
            if (!overdue) return;
        }
        quint16 rb = 0;
        const bool gotInput = join.remoteInput(0, rb);
        if (!gotInput && !overdue) return;
        if (!deadRoomTried) { deadRoomTried = true; ghost.hostViaRelay(QStringLiteral("127.0.0.1"), relayPort, deadRoom); return; }
        if (!orphanTurnedAway && !orphanStarted && !overdue) return;
        poll.stop();
        printf("hostStarted=%d joinStarted=%d stateSynced=%d input=%d(0x%04x) deadRoomRefused=%d\n",
               hostStarted, joinStarted, int(stateGot == stateSent), int(gotInput), rb, int(orphanTurnedAway));
        const bool ok = hostStarted && joinStarted && stateGot == stateSent && gotInput && rb == 0x1234
                     && orphanTurnedAway && !orphanStarted;
        printf("%s\n", ok ? "NETPLAY-RELAY-OK" : "FAIL");
        app.exit(ok ? 0 : 1);
    });
    poll.start();
    return app.exec();
}
