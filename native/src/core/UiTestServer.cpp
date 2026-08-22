#include "UiTestServer.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>

#include <cstdio>

bool UiTestServer::wantedFromEnvOrArgs()
{
    return qEnvironmentVariableIntValue("EB_UITEST") == 1
           || QCoreApplication::arguments().contains(QStringLiteral("--uitest"));
}

// NOT `wantedFromEnvOrArgs() || Settings::uiTestChannel()`, and the difference is the whole of issue #177.
// That form gave the right answer on the pre-migration call only because the left operand short-circuited the
// right one away — so the ORDER of an `||` here was a correctness invariant of main()'s startup sequence,
// pinned by nothing, and a reorder (the kind of edit that reads as a tidy-up) would have quietly run every
// EB_UITEST session off a pre-migration ini snapshot. There is no operand order here now: the settings read
// is behind an explicit early return on a phase the caller has to state.
//
// Pinned by probe_uitest §9 — reordering these three lines, or dropping the middle one, turns it red.
bool UiTestServer::wanted(IniPhase phase)
{
    if (wantedFromEnvOrArgs()) return true;             // EB_UITEST / --uitest: reads no settings, always safe
    if (phase == IniPhase::NotSettled) return false;    // the ini is not in place yet — do NOT read it
    return Settings::uiTestChannel();                   // the Settings ▸ Debug toggle
}

// The process-wide channel (see the header). A plain pointer rather than a QPointer so this unit stays
// dependency-free; the destructor clears it, which covers both the parented and the stack case.
static UiTestServer* g_channel = nullptr;

UiTestServer* UiTestServer::instance() { return g_channel; }

// Say it TWICE, on purpose. stderr is where a harness that launched the app with redirected output will see
// it (the app is a GUI-subsystem binary, so there is no console of its own); qCritical is where the app's own
// message handler will see it and write it into stream_debug.log, which is the only record a launch WITHOUT
// redirection leaves behind. A test channel that fails to come up must not be discoverable only by noticing
// that nothing happened.
static void complain(const QString& msg)
{
    std::fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    std::fflush(stderr);
    qCritical("%s", msg.toLocal8Bit().constData());
}

UiTestServer* UiTestServer::ensureListening(IniPhase phase, QObject* parent)
{
    // The enablement guard lives HERE, not at the call site. main()'s early call used to have to wrap itself
    // in `if (wantedFromEnvOrArgs())` to stay off the ini, which is a rule about a function held in a
    // different file; now it just says which phase it is in and this decides (issue #177).
    if (!wanted(phase)) return nullptr;
    if (g_channel)
    {
        // Adopt: the window that supplies the hooks also takes ownership, so the channel dies with it exactly
        // as it did when the ctor was the only entry point.
        if (parent && g_channel->parent() != parent) g_channel->setParent(parent);
        return g_channel;
    }
    g_channel = new UiTestServer(Hooks{}, parent ? parent : static_cast<QObject*>(QCoreApplication::instance()));
    return g_channel;
}

UiTestServer::UiTestServer(const Hooks& hooks, QObject* parent)
    : QObject(parent), hooks_(hooks)
{
    auto* server = new QLocalServer(this);
    server_ = server;

    // REFUSE TO SHARE THE NAME, and this is not a hypothetical: two EverythingBox instances were run against
    // one channel name during #172 and BOTH listened — Qt's Windows backend happily stands a second named-pipe
    // instance on an existing name, and the OS then hands each connecting client whichever one it likes. The
    // harness cannot tell, so every command lands on a coin flip and a green result means nothing. A cheap
    // connect-first probe turns that into a refusal with a reason. It costs effectively nothing when the name
    // is free (connecting to a pipe/socket that does not exist fails immediately, not after the timeout), and
    // it cannot be fooled by a stale endpoint: a dead process's pipe is gone on Windows, and on Unix a stale
    // socket file has nobody accepting on it, so the connect fails there too and we fall through to listen().
    {
        QLocalSocket probe;
        probe.connectToServer(serverName());
        if (probe.waitForConnected(300))
        {
            probe.abort();
            complain(QStringLiteral(
                "uitest: the control channel '%1' is ALREADY SERVED by another process, so this instance is "
                "running WITHOUT it. Do NOT assume a harness on that name is driving this app - it is driving "
                "the other one. Close the other instance, or give this one its own channel with "
                "EB_UITEST_PIPE=<name> (uitest.py honours the same variable).").arg(serverName()));
            return;
        }
    }

    QLocalServer::removeServer(serverName()); // clear a stale socket from a crashed previous run
    if (!server->listen(serverName()))
    {
        // The old code just returned here, and that silence is issue #172: the app came up looking completely
        // normal with no channel on it, and the only symptom at the other end was a connect that failed — the
        // same thing you get when the app was never launched with EB_UITEST at all. An occupied name is
        // handled above; what reaches here is the name itself being unusable (over 256 characters on Windows,
        // a socket path that cannot be created on Unix, a permissions problem). Name it, and say so.
        complain(QStringLiteral(
            // Deliberately ASCII-only: this string is written with toLocal8Bit() to a console whose code page
            // is nobody's business to predict, and a diagnostic that arrives as mojibake is a diagnostic
            // half-read.
            "uitest: FAILED to listen on the control channel '%1' (%2). This app is running WITHOUT the "
            "UI-test channel - nothing can drive it, and a client pointed at that name gets a connect "
            "failure indistinguishable from 'the app was never launched with EB_UITEST'. Check the channel "
            "name (EB_UITEST_PIPE overrides it; uitest.py honours the same variable).")
                .arg(serverName(), server->errorString()));
        return;
    }
    listening_ = true;
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

UiTestServer::~UiTestServer()
{
    if (g_channel == this) g_channel = nullptr;
}

// The reply when the channel is up but the app has not bound its hooks yet — i.e. the window is not built.
// This is the OTHER half of the #172 fix and the reason the channel now listens early: a startup that stalls
// used to be invisible (no pipe, no message, nothing), and is now a specific answer naming what is missing.
static QString notReady(const QString& cmd)
{
    return QStringLiteral("err not-ready: the app is still starting — '%1' needs the main window, which has "
                          "not been built yet (the channel listens from launch, on purpose: if this persists, "
                          "startup is stuck BEFORE the window, not after)").arg(cmd);
}

QString UiTestServer::handle(const QString& line)
{
    const QString cmd = line.section(QLatin1Char(' '), 0, 0).toLower();
    const QString arg = line.section(QLatin1Char(' '), 1).trimmed();

    // Answerable with no window at all, and the reason it exists: a harness can ask whether the app has got
    // as far as building its UI instead of inferring it from a command that fails for six other reasons.
    if (cmd == QStringLiteral("status"))
        return hooks_.state ? QStringLiteral("ok ready") : QStringLiteral("ok starting");

    if (cmd == QStringLiteral("key"))
    {
        static const QHash<QString, int> keys = {
            { QStringLiteral("up"), Qt::Key_Up },       { QStringLiteral("down"), Qt::Key_Down },
            { QStringLiteral("left"), Qt::Key_Left },   { QStringLiteral("right"), Qt::Key_Right },
            { QStringLiteral("enter"), Qt::Key_Return },{ QStringLiteral("back"), Qt::Key_Backspace },
            { QStringLiteral("escape"), Qt::Key_Escape },
            // "hwback" = Android's hardware/gesture/remote Back (Qt::Key_Back), distinct from the app's logical
            // "back" (Backspace). Lets a UI-test drive the exact key the OS delivers on Android from desktop.
            { QStringLiteral("hwback"), Qt::Key_Back },
            // Space: the nav ring passes it through (NavRing::handleKey default), so it reaches the focused
            // widget natively — e.g. toggles a QListWidget checkbox (the Library's per-source enable box).
            { QStringLiteral("space"), Qt::Key_Space },
            // Themed-surface shortcuts: "I"/Info opens the detail view, "P" adds to a playlist, "/" searches,
            // "F" opens the transient browse Filter menu (All/Favorites/status/tag).
            { QStringLiteral("info"), Qt::Key_I },      { QStringLiteral("i"), Qt::Key_I },
            { QStringLiteral("playlist"), Qt::Key_P },  { QStringLiteral("p"), Qt::Key_P },
            { QStringLiteral("search"), Qt::Key_Slash },{ QStringLiteral("slash"), Qt::Key_Slash },
            { QStringLiteral("filter"), Qt::Key_F },    { QStringLiteral("f"), Qt::Key_F },
        };
        int k = keys.value(arg.toLower(), 0);
        if (!k) k = arg.toInt();                       // raw Qt::Key value for anything exotic
        if (!k) return QStringLiteral("err unknown key '%1'").arg(arg);
        // A missing hook is NOT an unknown key, and saying so was the difference between "your test typo'd a
        // key name" and "the app never finished starting". Distinct answers for distinct failures.
        if (!hooks_.sendKey) return notReady(line);
        hooks_.sendKey(k);
        return QStringLiteral("ok");
    }
    if (cmd == QStringLiteral("state"))
        return hooks_.state ? QStringLiteral("ok ") + hooks_.state() : notReady(cmd);
    if (cmd == QStringLiteral("shot"))
    {
        if (arg.isEmpty()) return QStringLiteral("err usage: shot <path.png>");
        if (!hooks_.screenshot) return notReady(cmd);
        return hooks_.screenshot(arg) ? QStringLiteral("ok ") + arg
                                      : QStringLiteral("err couldn't save %1").arg(arg);
    }
    if (cmd == QStringLiteral("open"))
    {
        if (arg.isEmpty()) return QStringLiteral("err usage: open <path>");
        if (!hooks_.openDoc) return notReady(cmd);
        return hooks_.openDoc(arg) ? QStringLiteral("ok ") + arg
                                   : QStringLiteral("err couldn't open %1").arg(arg);
    }
    if (cmd == QStringLiteral("click"))
    {
        if (arg.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() < 2)
            return QStringLiteral("err usage: click X Y");
        if (!hooks_.click) return notReady(cmd);
        return hooks_.click(arg) ? QStringLiteral("ok") : QStringLiteral("err bad args");
    }
    if (cmd == QStringLiteral("touch"))
    {
        // arg is the sub-line ("tap X Y" / "flick X1 Y1 X2 Y2 [MS]" / "pinch CX CY SCALE [MS]"). The gesture
        // validates its own argument count app-side (MainWindow); here we only route the raw line. The hook
        // starts a QTimer state machine and returns immediately (no blocking of the pipe handler).
        const QString sub = arg.section(QLatin1Char(' '), 0, 0).toLower();
        if (sub != QStringLiteral("tap") && sub != QStringLiteral("flick") && sub != QStringLiteral("pinch"))
            return QStringLiteral("err usage: touch tap X Y | flick X1 Y1 X2 Y2 [MS] | pinch CX CY SCALE [MS]");
        if (!hooks_.touch) return notReady(cmd);
        // false = a sequence is already in flight; reject so overlapping gestures can't corrupt Qt touch state.
        return hooks_.touch(arg) ? QStringLiteral("ok") : QStringLiteral("err busy");
    }
    return QStringLiteral("err unknown command '%1' (status/key/state/shot/open/touch/click)").arg(cmd);
}
