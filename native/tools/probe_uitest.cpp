// Headless regression test for the uitest channel's blocking-command use-after-free (commit 8da19a3).
//
// The bug: UiTestServer's readyRead handler calls handle() SYNCHRONOUSLY, and handle() can re-enter a
// nested event loop — the "/" key opens the on-screen keyboard, and Osk::getText spins a QEventLoop
// inside our sendKey hook. uitest.py is one connection per command, so a client that times out or is
// killed while that nested loop runs closes the pipe; the server socket's `disconnected` -> deleteLater
// then frees the QLocalSocket inside the nested loop, under the suspended readyRead frame. When the loop
// unwound, the frame resumed straight into write()/flush()/canReadLine() on freed memory — an 0xc0000005
// in Qt6Core, and it was the OSK search-submit crash.
//
// The fix is four lines: hold the socket in a QPointer across the handle() call and re-check it before
// touching the socket again, dropping the reply with a warning if the client is gone. Four lines with no
// test are four lines a future edit deletes without noticing — eight commits touched UiTestServer.cpp in
// the weeks after the fix, and the guard is invisible to every one of the other probes. So the guard's
// behaviour is pinned here, on the real class, over a real socket.
//
// What §3 does about determinism, since it matters for reading the result: a QObject deleteLater()d
// inside a nested event loop is collected when Qt decides to, which is not a property a test may depend
// on. The probe therefore drains the deferred deletes explicitly inside the nested loop and ASSERTS the
// server-side socket is actually gone before letting the loop unwind. That assertion is the precondition
// — it is what makes "handle() returned with the socket already destroyed" a fact rather than a hope, so
// that everything the probe checks afterwards is checking the guard and not the timing.
//
// Prints UITEST-OK on success; any failure prints UITEST-FAIL <cond> and exits non-zero.
#include "UiTestServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <functional>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "UITEST-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---------------------------------------------------------------------------------------------------
// Captured qWarning output. The guard's only externally visible act, when the client is already gone, is
// the warning it logs instead of writing — a dropped reply is by definition unobservable from the
// (dead) client's end. So the message is the probe's window onto which branch ran.
// ---------------------------------------------------------------------------------------------------
static QStringList g_log;

static void captureMessages(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    g_log << msg;
}

static int warningsMentioning(const char* needle)
{
    int n = 0;
    for (const QString& m : g_log)
        if (m.contains(QLatin1String(needle))) ++n;
    return n;
}

// ---------------------------------------------------------------------------------------------------
// Event-loop plumbing.
//
// pump() delivers pending events AND the deferred deletes posted at this level. QLocalSocket's own
// `disconnected -> deleteLater` is the mechanism the bug rides on, so a probe that never collects those
// would be testing a socket graph that only ever grows.
// ---------------------------------------------------------------------------------------------------
static void pump(int ms = 5)
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, ms);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static bool waitUntil(const std::function<bool()>& done, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) pump();
    return done();
}

// The UiTestServer under test, and the QLocalServer it owns (it parents one to itself in its ctor).
// Every accepted connection is parented to that QLocalServer, which is how the probe can see the
// SERVER-side socket — the object the crash was about — without UiTestServer exposing it.
static UiTestServer* g_server = nullptr;

static QLocalSocket* serverSideSocket()
{
    QLocalServer* ls = g_server ? g_server->findChild<QLocalServer*>() : nullptr;
    return ls ? ls->findChild<QLocalSocket*>() : nullptr;
}

// serverSideSocket() takes the FIRST accepted socket, which is only the one being handled while exactly
// one exists. Each case waits for the previous connection to be collected, and the hooks assert this —
// a stale sibling would mean §3 watched the wrong object die and its precondition proved nothing.
static int serverSideSocketCount()
{
    QLocalServer* ls = g_server ? g_server->findChild<QLocalServer*>() : nullptr;
    return ls ? int(ls->findChildren<QLocalSocket*>().size()) : 0;
}

// ---------------------------------------------------------------------------------------------------
// The hook under the command. UiTestServer copies its Hooks by value at construction, so the per-case
// behaviour is routed through this indirection rather than by rebuilding the server each time (the
// server owns the listening pipe name; rebuilding it per case would re-race the same name).
// ---------------------------------------------------------------------------------------------------
static std::function<void(int)> g_onKey;
static int g_keyCalls = 0;
static QList<int> g_keysSeen;

// A blocking prompt, in the shape that matters: a real nested QEventLoop entered from inside the
// synchronous sendKey hook, exactly as Osk::getText does. `during` runs inside that loop; the loop ends
// when `until` is satisfied (or the deadline expires, which the caller asserts against).
static void spinNestedLoopWhile(const std::function<void()>& during,
                                const std::function<bool()>& until,
                                int ms = 5000)
{
    QEventLoop nested;
    QElapsedTimer t;
    t.start();

    QTimer tick;
    tick.setInterval(1);
    QObject::connect(&tick, &QTimer::timeout, &nested, [&] {
        // Collect what the nested loop's own guests posted — the disconnected -> deleteLater that frees
        // the server socket is the whole point of the exercise.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (until() || t.elapsed() > ms) nested.quit();
    });
    tick.start();

    if (during) during();
    nested.exec();
}

static QLocalSocket* connectClient()
{
    auto* c = new QLocalSocket;
    c->connectToServer(UiTestServer::serverName());
    if (!c->waitForConnected(3000)) { delete c; return nullptr; }
    return c;
}

static void send(QLocalSocket* c, const QByteArray& line)
{
    c->write(line);
    c->flush();
}

int main(int argc, char** argv)
{
    // A private channel name: the probe must not answer (or be answered by) a real EverythingBox that is
    // already serving the default pipe on this machine. UiTestServer::serverName() reads this.
    qputenv("EB_UITEST_PIPE",
            QByteArrayLiteral("EverythingBox-uitest-probe-")
                + QByteArray::number(QCoreApplication::applicationPid()));

    QCoreApplication app(argc, argv);
    qInstallMessageHandler(captureMessages);

    // ---- 1. the channel name is the private one, not the shared default ---------------------------
    // Everything below asserts against a socket; if the probe were talking to a live app's pipe the
    // results would be about that app's UI, not about this code.
    CHECK(UiTestServer::serverName().startsWith(QStringLiteral("EverythingBox-uitest-probe-")));
    CHECK(UiTestServer::serverName() != QStringLiteral("EverythingBox-uitest"));

    UiTestServer::Hooks hooks;
    hooks.sendKey = [](int k) { ++g_keyCalls; g_keysSeen << k; if (g_onKey) g_onKey(k); };
    hooks.state   = [] { return QStringLiteral("{\"probe\":1}"); };

    UiTestServer server(hooks);
    g_server = &server;

    // The server has to be listening, or every case below "passes" by never connecting.
    QLocalServer* listener = server.findChild<QLocalServer*>();
    CHECK(listener != nullptr);
    if (!listener || !listener->isListening())
    {
        std::fprintf(stderr, "UITEST-FAIL server is not listening on %s\n",
                     qPrintable(UiTestServer::serverName()));
        return 1;
    }

    // ---- 2. control: a blocking command with the client still there DELIVERS its reply ------------
    // Without this, "drop the reply whenever handle() nested a loop" would pass every other case here —
    // and it would break the OSK search the fix was made for, since a successful "/" must still answer
    // `ok`. The guard's contract is narrow: drop the reply IF AND ONLY IF the client is gone.
    {
        g_keyCalls = 0;
        g_keysSeen.clear();
        QLocalSocket* client = connectClient();
        CHECK(client != nullptr);
        if (client)
        {
            bool nested = false;
            g_onKey = [&](int) {
                // Same nested loop as the crash case; nobody dies in it.
                nested = true;
                spinNestedLoopWhile(nullptr, [] { return true; }, 500);
            };
            send(client, "key slash\n");

            const bool got = waitUntil([&] { return client->canReadLine(); });
            CHECK(got);
            CHECK(nested);
            CHECK(g_keyCalls == 1);
            CHECK(g_keysSeen.value(0) == Qt::Key_Slash);
            if (got)
                CHECK(QString::fromUtf8(client->readLine()).trimmed() == QStringLiteral("ok"));
            CHECK(warningsMentioning("vanished") == 0);

            client->abort();
            delete client;
        }
        CHECK(waitUntil([] { return serverSideSocket() == nullptr; }));
    }

    // ---- 3. THE REGRESSION: the client dies inside the nested loop --------------------------------
    // The crash, reproduced: "key slash" -> sendKey -> a nested QEventLoop -> the client goes away ->
    // the server socket's disconnected/deleteLater frees it INSIDE that loop -> the loop unwinds and the
    // readyRead frame resumes holding a pointer to freed memory.
    //
    // Delete the QPointer guard from UiTestServer.cpp and this section goes red: the resumed frame
    // writes through the dangling socket, which either faults outright (the original 0xc0000005) or
    // silently writes into freed memory and never logs the drop. Either way the checks below fail.
    {
        g_log.clear();
        g_keyCalls = 0;
        QLocalSocket* client = connectClient();
        CHECK(client != nullptr);
        if (client)
        {
            bool sawDeath = false;
            g_onKey = [&](int) {
                CHECK(serverSideSocketCount() == 1);      // ... so the one below IS the one being handled
                QPointer<QLocalSocket> srv(serverSideSocket());
                CHECK(!srv.isNull());                     // the connection we are being called for
                spinNestedLoopWhile([&] { client->abort(); delete client; client = nullptr; },
                                    [&] { return srv.isNull(); });
                // THE PRECONDITION. If this fails, everything after it is vacuous: the socket outlived
                // the nested loop, so the frame below never resumed onto a freed object and the guard
                // was never the thing being exercised.
                sawDeath = srv.isNull();
                CHECK(sawDeath);
            };
            send(client, "key slash\n");

            // The guard's branch: no write, one warning naming the command whose reply was dropped.
            CHECK(waitUntil([] { return warningsMentioning("vanished") > 0; }));
            CHECK(sawDeath);
            CHECK(g_keyCalls == 1);
            CHECK(warningsMentioning("vanished") == 1);
            CHECK(warningsMentioning("key slash") == 1);   // it says WHICH reply was dropped
            delete client;                                 // no-op unless the hook never ran
        }
        CHECK(waitUntil([] { return serverSideSocket() == nullptr; }));
    }

    // ---- 4. a command QUEUED behind the fatal one is abandoned, not run off the dead socket -------
    // The guard is on the `while (alive && alive->canReadLine())` condition as much as on the write:
    // uitest.py can put two lines in one packet, and after the client dies the loop must not come back
    // round to read the second one out of a freed QIODevice.
    {
        g_log.clear();
        g_keyCalls = 0;
        g_keysSeen.clear();
        QLocalSocket* client = connectClient();
        CHECK(client != nullptr);
        if (client)
        {
            g_onKey = [&](int) {
                CHECK(serverSideSocketCount() == 1);
                QPointer<QLocalSocket> srv(serverSideSocket());
                spinNestedLoopWhile([&] { client->abort(); delete client; client = nullptr; },
                                    [&] { return srv.isNull(); });
                CHECK(srv.isNull());                       // same precondition as §3
            };
            send(client, "key slash\nkey up\n");           // two commands, one write

            CHECK(waitUntil([] { return warningsMentioning("vanished") > 0; }));
            CHECK(g_keyCalls == 1);                        // the second line never reached handle()
            CHECK(g_keysSeen == QList<int>{ Qt::Key_Slash });
            delete client;
        }
        CHECK(waitUntil([] { return serverSideSocket() == nullptr; }));
    }

    // ---- 5. the channel survives the death, and still serves the next client ----------------------
    // The point of dropping the reply rather than crashing: one timed-out uitest.py invocation must not
    // take the harness down with it. This is also the assertion that does not depend on the wording of
    // any log line — if the guard is gone and the process survived its write to freed memory, whatever
    // state that left behind has to answer a fresh command correctly here.
    {
        g_log.clear();
        g_keyCalls = 0;
        QLocalSocket* client = connectClient();
        CHECK(client != nullptr);
        if (client)
        {
            g_onKey = nullptr;
            send(client, "state\n");
            const bool got = waitUntil([&] { return client->canReadLine(); });
            CHECK(got);
            if (got)
                CHECK(QString::fromUtf8(client->readLine()).trimmed()
                      == QStringLiteral("ok {\"probe\":1}"));

            // ... and a second command down the SAME connection, which is the loop condition again on a
            // socket that is very much alive.
            send(client, "key up\n");
            const bool got2 = waitUntil([&] { return client->canReadLine(); });
            CHECK(got2);
            if (got2)
                CHECK(QString::fromUtf8(client->readLine()).trimmed() == QStringLiteral("ok"));
            CHECK(g_keyCalls == 1);

            client->abort();
            delete client;
        }
        CHECK(warningsMentioning("vanished") == 0);
    }

    g_server = nullptr;
    qInstallMessageHandler(nullptr);

    if (failures == 0) { std::puts("UITEST-OK"); return 0; }
    std::fprintf(stderr, "UITEST: %d check(s) failed\n", failures);
    for (const QString& m : g_log) std::fprintf(stderr, "  log: %s\n", qPrintable(m));
    return 1;
}
