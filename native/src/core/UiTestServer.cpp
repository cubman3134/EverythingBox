#include "UiTestServer.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>

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
            // handle() can re-enter a nested event loop (a key that opens a BLOCKING prompt — e.g. "/"
            // opens the OSK, whose Osk::getText spins a QEventLoop inside our synchronous sendKey hook).
            // If the client disconnects while this frame is suspended in there (uitest.py is one
            // connection per command; a timed-out/killed client closes the pipe), deleteLater() runs in
            // that nested loop and frees `sock` under our feet — resuming into write()/canReadLine() on
            // the freed socket was an 0xc0000005 in Qt6Core (the OSK search-submit crash). Guard every
            // touch after handle() behind a QPointer: a dead client just drops the reply.
            QPointer<QLocalSocket> alive(sock);
            while (alive && alive->canReadLine())
            {
                const QString line = QString::fromUtf8(alive->readLine()).trimmed();
                if (line.isEmpty()) continue;
                const QString reply = handle(line); // may nest an event loop; `sock` can die inside
                if (!alive)
                {
                    qWarning("uitest: client vanished during a blocking command; dropping reply for '%s'",
                             qPrintable(line));
                    return;
                }
                alive->write((reply + QLatin1Char('\n')).toUtf8());
                alive->flush();
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
            // Space: the nav ring passes it through (NavRing::handleKey default), so it reaches the focused
            // widget natively — e.g. toggles a QListWidget checkbox (the Library's per-source enable box).
            { QStringLiteral("space"), Qt::Key_Space },
            // Themed-surface shortcuts: "I"/Info opens the detail view, "P" adds to a playlist, "/" searches.
            { QStringLiteral("info"), Qt::Key_I },      { QStringLiteral("i"), Qt::Key_I },
            { QStringLiteral("playlist"), Qt::Key_P },  { QStringLiteral("p"), Qt::Key_P },
            { QStringLiteral("search"), Qt::Key_Slash },{ QStringLiteral("slash"), Qt::Key_Slash },
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
    if (cmd == QStringLiteral("open"))
    {
        if (arg.isEmpty() || !hooks_.openDoc) return QStringLiteral("err usage: open <path>");
        return hooks_.openDoc(arg) ? QStringLiteral("ok ") + arg
                                   : QStringLiteral("err couldn't open %1").arg(arg);
    }
    if (cmd == QStringLiteral("touch"))
    {
        // arg is the sub-line ("tap X Y" / "flick X1 Y1 X2 Y2 [MS]" / "pinch CX CY SCALE [MS]"). The gesture
        // validates its own argument count app-side (MainWindow); here we only route the raw line. The hook
        // starts a QTimer state machine and returns immediately (no blocking of the pipe handler).
        const QString sub = arg.section(QLatin1Char(' '), 0, 0).toLower();
        if (sub != QStringLiteral("tap") && sub != QStringLiteral("flick") && sub != QStringLiteral("pinch"))
            return QStringLiteral("err usage: touch tap X Y | flick X1 Y1 X2 Y2 [MS] | pinch CX CY SCALE [MS]");
        if (!hooks_.touch) return QStringLiteral("err no touch hook");
        // false = a sequence is already in flight; reject so overlapping gestures can't corrupt Qt touch state.
        return hooks_.touch(arg) ? QStringLiteral("ok") : QStringLiteral("err busy");
    }
    return QStringLiteral("err unknown command '%1' (key/state/shot/open/touch)").arg(cmd);
}
