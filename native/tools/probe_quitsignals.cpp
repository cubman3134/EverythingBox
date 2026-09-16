// Headless check of core/QuitSignals (issue #409): SIGTERM, SIGINT and SIGHUP reach the app's quit path on its
// event loop, once, and a second signal keeps its default action.
//
// What this probe pins, on desktop Unix (the CI Linux runner):
//   1. NOTHING RUNS IN THE HANDLER. Right after raise(SIGTERM) returns -- the handler has run synchronously on
//      this thread by then -- the quit callback has NOT been called. It is called only once the event loop
//      turns, from the QSocketNotifier, on the context's thread, while exec() is running.
//   2. EXACTLY ONCE. Further turns of the loop do not call it again.
//   3. THE SECOND SIGNAL IS THE DEFAULT. After the first, all three dispositions read back as SIG_DFL, and in a
//      forked child a second raise(SIGTERM) terminates the process by SIGTERM.
//   4. SIGINT and SIGHUP take the same path, each in its own forked child (install() is once per process).
//   5. AN INHERITED SIG_IGN IS KEPT. The parent starts with SIGHUP ignored, as `nohup` would leave it: install()
//      must not take it over, and the first signal must not flip it to SIG_DFL either.
// What it cannot see: that main.cpp installs it, that the callback closes MainWindow, or that SDL has been told
// to keep its own handlers off (SDL_HINT_NO_SIGNAL_HANDLERS). The release workflow's AppImage smoke is the
// end-to-end evidence for those: the app must exit inside timeout's grace after SIGTERM (rc 124, never 137).
//
// On other platforms the unit is a no-op by design, and the probe asserts exactly that.
//
// Prints QUITSIGNALS-OK on success; any failure prints QUITSIGNALS-FAIL <cond> (line) and exits non-zero.
#include "QuitSignals.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <cstdio>

#if defined(Q_OS_UNIX) && !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#define PROBE_UNIX 1
#endif

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "QUITSIGNALS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

#ifdef PROBE_UNIX
static bool isHandler(int sig, void (*h)(int))
{
    struct sigaction cur;
    return ::sigaction(sig, nullptr, &cur) == 0 && !(cur.sa_flags & SA_SIGINFO) && cur.sa_handler == h;
}
static bool isDefault(int sig) { return isHandler(sig, SIG_DFL); }
static bool isIgnored(int sig) { return isHandler(sig, SIG_IGN); }

// Child: install, raise `sig`, and report through the exit status whether the callback ran for it on the loop.
// With `second` set, raise SIGTERM again from inside the callback: the process must die of it.
static int childRun(int argc, char** argv, int sig, bool second)
{
    QCoreApplication app(argc, argv);
    int calls = 0, got = 0;
    if (!QuitSignals::install(&app, [&](int s) {
            ++calls; got = s;
            if (second) { ::raise(SIGTERM); std::_Exit(40); } // reaching this line means SIGTERM was caught again
            app.quit();
        }))
        return 41;
    ::raise(sig);
    if (calls != 0) return 42; // the handler dispatched the callback itself
    QTimer::singleShot(3000, &app, [&app] { app.exit(43); });
    const int rc = app.exec();
    if (rc != 0) return rc;
    return (calls == 1 && got == sig) ? 0 : 44;
}

struct ChildResult { bool signaled = false; int code = -1; };
static ChildResult forkChild(int argc, char** argv, int sig, bool second)
{
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) std::_Exit(childRun(argc, argv, sig, second));
    ChildResult r;
    int st = 0;
    if (pid < 0 || ::waitpid(pid, &st, 0) != pid) return r;
    if (WIFSIGNALED(st)) { r.signaled = true; r.code = WTERMSIG(st); }
    else if (WIFEXITED(st)) r.code = WEXITSTATUS(st);
    return r;
}
#endif

int main(int argc, char** argv)
{
#ifdef PROBE_UNIX
    // Children first, while this process has no QCoreApplication and no threads to fork from under.
    const ChildResult intChild = forkChild(argc, argv, SIGINT, false);
    const ChildResult hupChild = forkChild(argc, argv, SIGHUP, false);
    const ChildResult twice = forkChild(argc, argv, SIGTERM, true);
    std::printf("child SIGINT: %s %d; child SIGHUP: %s %d; child second SIGTERM: %s %d\n",
                intChild.signaled ? "signal" : "exit", intChild.code, hupChild.signaled ? "signal" : "exit",
                hupChild.code, twice.signaled ? "signal" : "exit", twice.code);
    CHECK(!intChild.signaled && intChild.code == 0);
    CHECK(!hupChild.signaled && hupChild.code == 0);
    CHECK(twice.signaled && twice.code == SIGTERM);

    QCoreApplication app(argc, argv);
    CHECK(QuitSignals::supported());
    CHECK(isDefault(SIGTERM));
    ::signal(SIGHUP, SIG_IGN); // as `nohup` starts a process

    int calls = 0, got = 0;
    bool onGuiThread = false, loopRunning = false, calledInLoop = false;
    QTimer::singleShot(0, &app, [&] { loopRunning = true; });
    CHECK(QuitSignals::install(&app, [&](int s) {
        ++calls; got = s;
        onGuiThread = QThread::currentThread() == app.thread();
        calledInLoop = loopRunning;
    }));
    CHECK(!QuitSignals::install(&app, [](int) {})); // once per process
    CHECK(!isDefault(SIGTERM) && !isDefault(SIGINT));
    CHECK(isIgnored(SIGHUP)); // an inherited ignore is not taken over

    ::raise(SIGTERM); // the handler has run by the time this returns
    CHECK(calls == 0);
    CHECK(isDefault(SIGTERM));
    CHECK(isDefault(SIGINT));
    CHECK(isIgnored(SIGHUP)); // ...and not turned into a fatal default by the first signal either

    QTimer::singleShot(500, &app, [&app] { app.quit(); }); // several loop turns after the notifier fires
    app.exec();
    std::printf("parent: calls=%d signal=%d guiThread=%d inLoop=%d\n", calls, got, int(onGuiThread),
                int(calledInLoop));
    CHECK(calls == 1);
    CHECK(got == SIGTERM);
    CHECK(onGuiThread);
    CHECK(calledInLoop);
#else
    QCoreApplication app(argc, argv);
    bool called = false;
    CHECK(!QuitSignals::supported());
    CHECK(!QuitSignals::install(&app, [&](int) { called = true; }));
    QTimer::singleShot(0, &app, [&app] { app.quit(); });
    app.exec();
    CHECK(!called);
    std::printf("not desktop Unix: QuitSignals is a no-op here, as designed\n");
#endif
    if (failures) return 1;
    std::printf("QUITSIGNALS-OK\n");
    return 0;
}
