// Headless check of the relay netplay path: spin up two real NetplaySession objects, connect one via
// hostViaRelay and the other via joinViaRelay to a locally-running relay, and verify the handshake
// (started() on both), the state sync (host's serialized state reaches the joiner), and an input exchange.
//   usage: probe_netplay [relayPort]   (needs netplay-relay.py running on that port)
#include "NetplaySession.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const quint16 relayPort = argc > 1 ? quint16(atoi(argv[1])) : 55677;

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

    host.hostViaRelay(QStringLiteral("127.0.0.1"), relayPort, QStringLiteral("TESTROOM"));
    QTimer::singleShot(500, [&] { join.joinViaRelay(QStringLiteral("127.0.0.1"), relayPort, QStringLiteral("TESTROOM")); });

    QTimer::singleShot(3000, [&] {
        host.sendLocalInput(0, 0x1234);  // host -> joiner input packet, over the relay pipe
        QTimer::singleShot(400, [&] {
            quint16 rb = 0;
            const bool gotInput = join.remoteInput(0, rb);
            printf("hostStarted=%d joinStarted=%d stateSynced=%d input=%d(0x%04x)\n",
                   hostStarted, joinStarted, int(stateGot == stateSent), int(gotInput), rb);
            const bool ok = hostStarted && joinStarted && stateGot == stateSent && gotInput && rb == 0x1234;
            printf("%s\n", ok ? "NETPLAY-RELAY-OK" : "FAIL");
            app.exit(ok ? 0 : 1);
        });
    });
    return app.exec();
}
