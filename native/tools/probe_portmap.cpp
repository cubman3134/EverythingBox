// Exercise PortMapper against the real router on this network: try to forward a port via UPnP-IGD and read the
// public IP. Prints the result; used to confirm UPnP works here (many routers ship with it disabled).
//   usage: probe_portmap [port]
#include "PortMapper.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const quint16 port = argc > 1 ? quint16(atoi(argv[1])) : 55420;
    PortMapper pm;
    QObject::connect(&pm, &PortMapper::mapped, [&](const QString& ip, quint16 ext) {
        printf("MAPPED  public %s : %u  -> this machine : %u\n", qUtf8Printable(ip), ext, port);
        pm.unmap();
        QTimer::singleShot(600, [&] { app.exit(0); });
    });
    QObject::connect(&pm, &PortMapper::failed, [&](const QString& why) {
        printf("FAILED  %s\n", qUtf8Printable(why));
        app.exit(1);
    });
    pm.map(port, port);
    QTimer::singleShot(9000, [&] { printf("TIMEOUT\n"); app.exit(2); });
    return app.exec();
}
