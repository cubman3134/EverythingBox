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
#include "Settings.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRandomGenerator>
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
    // A private channel name, unique to THIS RUN. The probe must not answer (or be answered by) anything else
    // holding a uitest channel on this machine: a real EverythingBox on the default name — routine now that
    // increments are driven live under EB_UITEST=1 — a concurrent suite run, or a socket file left in /tmp by
    // an earlier run whose pid the OS has since handed out again, which is the reason the pid alone is not a
    // unique name. None of that is behaviour this probe is testing: a collision is a listen that fails for
    // reasons unrelated to the code under test, and the suite then reports the wreckage in THIS probe's name
    // (issue #180, where the one attributed sighting is probe_uitest rc=139 on a named-pipe listen). Removing
    // the contention beats detecting it.
    //
    // Nothing outside has to be told what this run chose: UiTestServer::serverName() and native/tools/uitest.py
    // both read EB_UITEST_PIPE, so they agree by construction.
    const QByteArray channel = QByteArrayLiteral("EverythingBox-uitest-probe-")
                               + QByteArray::number(QCoreApplication::applicationPid()) + '-'
                               + QByteArray::number(QRandomGenerator::global()->generate(), 16);
    qputenv("EB_UITEST_PIPE", channel);
    // Announce it. If this process ever dies somewhere it cannot report from, the channel it was using is then
    // in the suite's transcript instead of dying with it — which is the whole complaint on #180.
    std::printf("uitest: private control channel '%s'\n", channel.constData());
    std::fflush(stdout);

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
    // uitest.py can put two lines in one packet, and after the client dies the loop must not come back
    // round to read the second one out of a freed QIODevice. This section pins that abandonment.
    //
    // WHICH HALF of the guard delivers it, measured rather than assumed: the early `return` after the
    // failed re-check — NOT the `alive &&` in the `while` condition. Dropping `alive &&` from that
    // condition (and reading through the raw `sock` again) leaves this entire probe GREEN, because the
    // return preempts the condition: the only paths back to it are the first iteration, an empty-line
    // `continue`, and a completed write, and on none of them can the socket have been freed. The clause
    // is therefore unreachable AS A GUARD and no mutation here can kill it.
    //
    // Labelled rather than reported as coverage, on the house rule for an assertion no mutation kills.
    // It stays because it is defence in depth for the day the `return` becomes a `continue` or moves
    // below the write — and the ABANDONMENT it backs up is genuinely pinned, by the two checks below.
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

    // ---- 6. a listen that FAILS is LOUD (issue #172) ----------------------------------------------
    // The old ctor returned silently when listen() failed, so the app came up looking entirely normal with
    // no channel on it — and the only symptom at the client's end was a failed connect, which is exactly
    // what you get when the app was never launched with EB_UITEST at all. A test channel that is silently
    // absent is indistinguishable from a test that passed, so the failure has to announce itself.
    //
    // The forced failure is a 300-character channel name, and the choice is load-bearing for portability:
    // Windows caps a pipe name at 256 characters and a Unix socket path at ~108 (sun_path), so this fails
    // on BOTH. The obvious alternative — stand a second server on an occupied name — is a failure only on
    // Windows, because QLocalServer::removeServer() UNLINKS the socket file on Unix and the second listen
    // then succeeds, which would make this section a no-op wherever CI runs Linux.
    //
    // (The `complain()` line also reaches the real stderr here. That is the point of it, and the suite reads
    // this probe's result from its exit code + the UITEST-OK sentinel, not from a clean stderr.)
    const QByteArray realName = qgetenv("EB_UITEST_PIPE");
    {
        qputenv("EB_UITEST_PIPE", QByteArray(300, 'x'));
        g_log.clear();
        UiTestServer dead;                                       // default Hooks — nothing bound, on purpose
        CHECK(!dead.isListening());
        CHECK(warningsMentioning("FAILED to listen") == 1);       // said once, not swallowed
        CHECK(warningsMentioning("EB_UITEST_PIPE") == 1);         // ... and it names the remedy
    }

    // ---- 6b. an OCCUPIED channel name is refused, loudly ------------------------------------------
    // Measured during #172, not assumed: two EverythingBox instances launched on one channel name BOTH ended
    // up listening (Qt's Windows backend adds a second named-pipe instance to an existing name), and the
    // clients were then routed to one or the other arbitrarily. Every command a harness sent was a coin flip
    // between two apps, and its "pass" was worth nothing. The channel now connects before it listens and
    // refuses the name if anyone answers.
    {
        qputenv("EB_UITEST_PIPE", realName + "-taken");
        QLocalServer occupier;                                   // stand in for the other instance
        QLocalServer::removeServer(UiTestServer::serverName());
        CHECK(occupier.listen(UiTestServer::serverName()));
        g_log.clear();
        UiTestServer second;
        CHECK(!second.isListening());                            // did NOT quietly join the name
        CHECK(warningsMentioning("ALREADY SERVED") == 1);
        CHECK(warningsMentioning("driving the other one") == 1); // says what the harness is actually driving
        occupier.close();
    }

    // ---- 7. the channel LISTENS before the window exists, and says so ------------------------------
    // The other half of #172. The server used to be built ~400 lines into the MainWindow ctor, so a startup
    // that stalled anywhere before that produced no pipe and no message. Now main() starts the channel first
    // and the window binds hooks later — which is only useful if a hookless channel actually answers, and
    // answers something a human can act on rather than a generic error.
    {
        qputenv("EB_UITEST_PIPE", realName + "-early");
        g_log.clear();
        UiTestServer early;                                      // exactly what main() creates: no hooks yet
        CHECK(early.isListening());
        CHECK(warningsMentioning("FAILED to listen") == 0);       // the success path stays quiet (§6 is not
                                                                 // a fixed point: it fails when it should)
        QLocalSocket* client = connectClient();                   // a client CAN connect with no window up
        CHECK(client != nullptr);
        if (client)
        {
            send(client, "status\n");
            CHECK(waitUntil([&] { return client->canReadLine(); }));
            CHECK(QString::fromUtf8(client->readLine()).trimmed() == QStringLiteral("ok starting"));

            send(client, "state\n");
            CHECK(waitUntil([&] { return client->canReadLine(); }));
            const QString reply = QString::fromUtf8(client->readLine()).trimmed();
            CHECK(reply.startsWith(QStringLiteral("err not-ready")));   // NOT "ok", and NOT a bare "err"
            CHECK(reply.contains(QStringLiteral("still starting")));
            CHECK(reply.contains(QStringLiteral("main window")));

            // A key with no hook is "not ready", not "unknown key": the two failures have nothing to do with
            // each other, and reporting a stalled startup as a client-side typo is how #172 stayed invisible.
            send(client, "key down\n");
            CHECK(waitUntil([&] { return client->canReadLine(); }));
            CHECK(QString::fromUtf8(client->readLine()).trimmed().startsWith(QStringLiteral("err not-ready")));
            send(client, "key nosuchkey\n");
            CHECK(waitUntil([&] { return client->canReadLine(); }));
            CHECK(QString::fromUtf8(client->readLine()).trimmed()
                  == QStringLiteral("err unknown key 'nosuchkey'"));

            // ... and the moment the window binds its hooks, the SAME channel serves the real thing.
            UiTestServer::Hooks late;
            late.state = [] { return QStringLiteral("{\"late\":1}"); };
            early.setHooks(late);
            send(client, "status\n");
            CHECK(waitUntil([&] { return client->canReadLine(); }));
            CHECK(QString::fromUtf8(client->readLine()).trimmed() == QStringLiteral("ok ready"));
            send(client, "state\n");
            CHECK(waitUntil([&] { return client->canReadLine(); }));
            CHECK(QString::fromUtf8(client->readLine()).trimmed()
                  == QStringLiteral("ok {\"late\":1}"));

            client->abort();
            delete client;
        }
    }

    // ---- 8. one channel per PROCESS, and the window adopts it --------------------------------------
    // ensureListening() is called twice on every launch (main() before the startup work, MainWindow when it
    // binds hooks) and must be the same object both times — two servers would race the same pipe name, and
    // the second would lose it to the first in exactly the silent way §6 exists to prevent.
    {
        qunsetenv("EB_UITEST");
        CHECK(!UiTestServer::wantedFromEnvOrArgs());
        CHECK(UiTestServer::ensureListening(UiTestServer::IniPhase::Settled) == nullptr); // not wanted => no channel
        CHECK(UiTestServer::instance() == nullptr);

        qputenv("EB_UITEST", "1");
        CHECK(UiTestServer::wantedFromEnvOrArgs());
        qputenv("EB_UITEST_PIPE", realName + "-ensure");
        UiTestServer* first = UiTestServer::ensureListening(UiTestServer::IniPhase::NotSettled);
        CHECK(first != nullptr);
        if (first)
        {
            CHECK(first->isListening());
            CHECK(UiTestServer::instance() == first);
            // Idempotent, not a second server — and idempotent ACROSS phases, because that is exactly how the
            // product calls it: main() opens the channel at NotSettled and the window adopts it at Settled.
            CHECK(UiTestServer::ensureListening(UiTestServer::IniPhase::NotSettled) == first);
            {
                QObject owner;
                CHECK(UiTestServer::ensureListening(UiTestServer::IniPhase::Settled, &owner) == first);
                CHECK(first->parent() == &owner);                 // the window takes ownership of main's channel
            }
            // owner is gone, and with it the channel: the process-wide pointer must go too, or the next
            // ensureListening() hands out a dangling one.
            CHECK(UiTestServer::instance() == nullptr);
        }
        qunsetenv("EB_UITEST");
    }

    // ---- 9. the PRE-MIGRATION call must not read Settings (issue #177) ------------------------------------
    // main() opens the channel BEFORE the brand migration copies the ini into place. Settings::store() holds a
    // function-local static QSettings that snapshots the file on its FIRST read, so a single read at that
    // point pins the whole session to a pre-migration (usually absent) file — every setting the user has,
    // silently wrong, for as long as the process lives. That is not a uitest bug; it is a data bug that
    // merely launching with EB_UITEST would cause.
    //
    // The old spelling was `wantedFromEnvOrArgs() || Settings::uiTestChannel()` behind a call-site `if`, and
    // it was safe only because the left operand short-circuited the right one away — an operand order in one
    // file standing in for a startup invariant of another, which nothing here could see. wanted() now takes
    // the phase, so what this section asserts is the OBSERVABLE consequence of not reading: with the toggle ON
    // and no env/args, NotSettled must answer NO where Settled answers YES. Any edit that puts the settings
    // read in front of the phase check — including collapsing it back into an `||` — makes those two answers
    // agree, and this section goes red.
    {
        qunsetenv("EB_UITEST");
        qputenv("EB_UITEST_PIPE", realName + "-phase");
        CHECK(!UiTestServer::wantedFromEnvOrArgs());           // the env/args half is OFF ...
        Settings::setUiTestChannel(true);
        // ... and the settings half is genuinely ON. Asserted, not assumed: a fixture that quietly says
        // nothing is a fixed point of wanted() and would pass whatever wanted() did.
        CHECK(Settings::uiTestChannel());

        CHECK(UiTestServer::wanted(UiTestServer::IniPhase::Settled));      // settled: the toggle counts
        CHECK(!UiTestServer::wanted(UiTestServer::IniPhase::NotSettled));  // not settled: it must not be read

        // The guard lives INSIDE ensureListening, so main()'s early call cannot get it wrong by omission.
        CHECK(UiTestServer::ensureListening(UiTestServer::IniPhase::NotSettled) == nullptr);
        CHECK(UiTestServer::instance() == nullptr);

        // The env/args half must still work at NotSettled — that is the whole point of the early call, and a
        // guard that swallowed it would break every EB_UITEST launch instead of the ini.
        qputenv("EB_UITEST", "1");
        CHECK(UiTestServer::wanted(UiTestServer::IniPhase::NotSettled));
        CHECK(UiTestServer::wanted(UiTestServer::IniPhase::Settled));
        qunsetenv("EB_UITEST");

        Settings::setUiTestChannel(false);
        CHECK(!UiTestServer::wanted(UiTestServer::IniPhase::Settled));     // fixture restored, not assumed
    }
    qputenv("EB_UITEST_PIPE", realName);

    g_server = nullptr;
    qInstallMessageHandler(nullptr);

    if (failures == 0) { std::puts("UITEST-OK"); return 0; }
    std::fprintf(stderr, "UITEST: %d check(s) failed\n", failures);
    for (const QString& m : g_log) std::fprintf(stderr, "  log: %s\n", qPrintable(m));
    return 1;
}
