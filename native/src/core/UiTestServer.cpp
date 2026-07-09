#include "UiTestServer.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>

bool UiTestServer::wanted()
{
    return qEnvironmentVariableIntValue("MMV_UITEST") == 1
           || QCoreApplication::arguments().contains(QStringLiteral("--uitest"))
           || Settings::uiTestChannel(); // the Settings ▸ Debug toggle
}

UiTestServer::UiTestServer(const Hooks& hooks, QObject* parent)
    : QObject(parent), hooks_(hooks)
{
    auto* server = new QLocalServer(this);
    QLocalServer::removeServer(serverName()); // clear a stale socket from a crashed previous run
    if (!server->listen(serverName()))
        return; // another instance already serves it; this one just doesn't
    connect(server, &QLocalServer::newConnection, this, [this, server] {
        QLocalSocket* sock = server->nextPendingConnection();
        if (!sock) return;
        connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QLocalSocket::readyRead, sock, [this, sock] {
            while (sock->canReadLine())
            {
                const QString line = QString::fromUtf8(sock->readLine()).trimmed();
                if (line.isEmpty()) continue;
                sock->write((handle(line) + QLatin1Char('\n')).toUtf8());
                sock->flush();
            }
        });
    });
}

QString UiTestServer::handle(const QString& line)
{
    const QString cmd = line.section(QLatin1Char(' '), 0, 0).toLower();
    const QString arg = line.section(QLatin1Char(' '), 1).trimmed();

    if (cmd == QStringLiteral("key"))
    {
        static const QHash<QString, int> keys = {
            { QStringLiteral("up"), Qt::Key_Up },       { QStringLiteral("down"), Qt::Key_Down },
            { QStringLiteral("left"), Qt::Key_Left },   { QStringLiteral("right"), Qt::Key_Right },
            { QStringLiteral("enter"), Qt::Key_Return },{ QStringLiteral("back"), Qt::Key_Backspace },
            { QStringLiteral("escape"), Qt::Key_Escape },
        };
        int k = keys.value(arg.toLower(), 0);
        if (!k) k = arg.toInt();                       // raw Qt::Key value for anything exotic
        if (!k || !hooks_.sendKey) return QStringLiteral("err unknown key '%1'").arg(arg);
        hooks_.sendKey(k);
        return QStringLiteral("ok");
    }
    if (cmd == QStringLiteral("state"))
        return hooks_.state ? QStringLiteral("ok ") + hooks_.state() : QStringLiteral("err no state hook");
    if (cmd == QStringLiteral("shot"))
    {
        if (arg.isEmpty() || !hooks_.screenshot) return QStringLiteral("err usage: shot <path.png>");
        return hooks_.screenshot(arg) ? QStringLiteral("ok ") + arg
                                      : QStringLiteral("err couldn't save %1").arg(arg);
    }
    return QStringLiteral("err unknown command '%1' (key/state/shot)").arg(cmd);
}
