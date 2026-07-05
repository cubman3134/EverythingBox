// Headless check of the "Both" online orchestration (hostOnline / joinOnline), isolating each path:
//   mode "direct": relay points at a dead port, so ONLY a direct connection can pair the two sessions.
//                  Verifies host Path A (direct server wins) + joinOnline's direct-success branch.
//   mode "relay":  the joiner is given a dead direct endpoint, so the direct attempt fails and it MUST fall
//                  back to the relay. Verifies host Path B (relay wins) + joinOnline's relay fallback.
//   usage: probe_netplay_both <direct|relay> [relayPort]   (relay mode needs netplay-relay.py on relayPort)
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

    const quint16 gamePort = 55490;                 // host's direct-listen port
    const QString relayHost = QStringLiteral("127.0.0.1");
    const quint16 usedRelayPort = directMode ? quint16(1) : relayPort;   // dead relay in direct mode
    const quint16 joinDirectPort = directMode ? gamePort : quint16(9);   // dead direct endpoint in relay mode

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

    host.hostOnline(gamePort, relayHost, usedRelayPort, QStringLiteral("TESTROOM"));
    QTimer::singleShot(700, [&] {
        join.joinOnline(relayHost, usedRelayPort, QStringLiteral("TESTROOM"), QStringLiteral("127.0.0.1"), joinDirectPort);
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
