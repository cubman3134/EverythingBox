// Headless check of the "Both" online orchestration (hostOnline / joinOnline), isolating each path:
//   mode "direct": relay points at a dead port, so ONLY a direct connection can pair the two sessions.
//                  Verifies host Path A (direct server wins) + joinOnline's direct-success branch.
//   mode "relay":  the joiner is given a dead direct endpoint, so the direct attempt fails and it MUST fall
//                  back to the relay. Verifies host Path B (relay wins) + joinOnline's relay fallback.
//   usage: probe_netplay_both <direct|relay> [relayPort]   (relay mode needs netplay-relay.py on relayPort)
//
// Nothing here may use a FIXED port or a fixed room code. This probe used to listen on a hardcoded 55490, which
// made it fail roughly one run in three on a machine where a second copy was running (a parallel suite, another
// worktree, a developer re-running it by hand): the second process lost the bind, its joiner then reached the
// FIRST process's host — same protocol, same gameId, same state bytes — and paired with it, so the probe
// reported a synced joiner beside a host that never started. The direct port is therefore OS-assigned and read
// back from the session, and the room code carries the pid.
#include "NetplaySession.h"
#include <QCoreApplication>
#include <QTimer>
#include <QString>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("direct");
    const quint16 relayPort = argc > 2 ? quint16(atoi(argv[2])) : 55677;
    const bool directMode = (mode == QStringLiteral("direct"));

    const QString relayHost = QStringLiteral("127.0.0.1");
    const quint16 usedRelayPort = directMode ? quint16(1) : relayPort;   // dead relay in direct mode
    // Unique per process, so two concurrent runs can't pair with each other through a shared relay.
    const QString room = QStringLiteral("TESTROOM%1").arg(QCoreApplication::applicationPid());

    NetplaySession host, join;
    host.gameId = join.gameId = QStringLiteral("game|123");
    host.coreName = join.coreName = QStringLiteral("testcore");
    const QByteArray stateSent = "SAVESTATE-BYTES";
    QByteArray stateGot;
    host.serializeState = [&] { return stateSent; };
    join.applyState = [&](const QByteArray& b) { stateGot = b; };

    bool hostStarted = false, joinStarted = false;
    QObject::connect(&host, &NetplaySession::started, [&] { hostStarted = true; });
    QObject::connect(&join, &NetplaySession::started, [&] { joinStarted = true; });
    QObject::connect(&host, &NetplaySession::ended, [](const QString& r) { printf("[host] ENDED: %s\n", qUtf8Printable(r)); });
    QObject::connect(&join, &NetplaySession::ended, [](const QString& r) { printf("[join] ENDED: %s\n", qUtf8Printable(r)); });

    // Port 0: the OS hands out a free one, so a concurrent run of this probe cannot steal it.
    host.hostOnline(0, relayHost, usedRelayPort, room);
    const quint16 gamePort = host.directPort();
    if (directMode && gamePort == 0)
    {
        // Direct mode tests the direct path specifically — without a listener there is nothing to test, and
        // carrying on would print a joiner that paired with somebody else's host.
        printf("mode=direct: the host's direct listener never came up — cannot test the direct path\nFAIL\n");
        return 1;
    }
    printf("mode=%s directPort=%u room=%s\n", qUtf8Printable(mode), unsigned(gamePort), qUtf8Printable(room));
    const quint16 joinDirectPort = directMode ? gamePort : quint16(9);   // dead direct endpoint in relay mode
    QTimer::singleShot(700, [&] {
        join.joinOnline(relayHost, usedRelayPort, room, QStringLiteral("127.0.0.1"), joinDirectPort);
    });

    // Give the relay fallback (4s direct timeout) room before checking.
    const int checkAt = directMode ? 2500 : 7000;
    QTimer::singleShot(checkAt, [&] {
        host.sendLocalInput(0, 0x1234);
        QTimer::singleShot(500, [&] {
            quint16 rb = 0;
            const bool gotInput = join.remoteInput(0, rb);
            printf("mode=%s hostStarted=%d joinStarted=%d stateSynced=%d input=%d(0x%04x)\n",
                   qUtf8Printable(mode), hostStarted, joinStarted, int(stateGot == stateSent), int(gotInput), rb);
            const bool ok = hostStarted && joinStarted && stateGot == stateSent && gotInput && rb == 0x1234;
            printf("%s\n", ok ? "NETPLAY-BOTH-OK" : "FAIL");
            app.exit(ok ? 0 : 1);
        });
    });
    return app.exec();
}
