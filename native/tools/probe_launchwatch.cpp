// Headless checks for issue #61: play time for games a STORE launches (steam:// / com.epicgames.launcher://).
// Pure — no registry, no process list, no clock. Every sample and every second is injected:
//   * an aborted launch (the game never runs; the store updated or asked for a login) times out, records nothing;
//   * normal play records first-running-sample to last-running-sample;
//   * ONE missed sample does not end a session (two consecutive ones do);
//   * a launcher wrapper that restarts the game is one session, recorded once;
//   * a session under the minimum (a crash at start) records nothing;
//   * the install-dir prefix rule: case-insensitive, either separator, and C:\Games\Foo never claims C:\Games\FooBar;
//   * Steam's RunningAppID rule and its install-dir fallback;
//   * steam://install/<id> gets no watch; a run / an Epic launch does;
//   * the watcher: one timer, running only while a watch is pending or running; one watch per game; quitting
//     drops every watch and records nothing.
#include "LaunchWatch.h"

#include <QCoreApplication>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "LAUNCHWATCH-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using LaunchWatch::Machine;
using LaunchWatch::State;

// Feed `running` for every sample time in [from, to] at the watcher's cadence.
static void run(Machine& m, qint64 from, qint64 to, bool running)
{
    for (qint64 t = from; t <= to; t += LaunchWatch::kSampleIntervalMs / 1000) m.sample(t, running);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- the constants the issue decided --------------------------------------------------------------------
    CHECK(LaunchWatch::kPendingTimeoutSecs == 180);
    CHECK(LaunchWatch::kExitMissedSamples == 2);
    CHECK(LaunchWatch::kMinSessionSecs == 30);
    CHECK(LaunchWatch::kSampleIntervalMs == 5000);

    // ---- 1. an aborted launch: never running -> times out at 3 minutes, records nothing ---------------------
    {
        Machine m(1000);
        run(m, 1005, 1175, false);
        CHECK(m.state() == State::Pending);          // still waiting at 2m55s
        CHECK(m.active());
        m.sample(1180, false);
        CHECK(m.state() == State::TimedOut);         // 3 minutes, no running sample
        CHECK(!m.active());
        CHECK(m.secondsToRecord() == 0);
        m.sample(1185, true);                        // a late start after the timeout does not revive it
        CHECK(m.state() == State::TimedOut);
        CHECK(m.secondsToRecord() == 0);
    }
    // A game that comes up just inside the window is NOT timed out.
    {
        Machine m(0);
        run(m, 5, 175, false);
        m.sample(178, true);
        CHECK(m.state() == State::Running);
    }

    // ---- 2. normal play: first running sample to last running sample -----------------------------------------
    {
        Machine m(0);
        run(m, 5, 15, false);                        // the store is starting the game
        CHECK(m.state() == State::Pending);
        run(m, 20, 620, true);                       // ten minutes of play
        CHECK(m.state() == State::Running);
        CHECK(m.secondsToRecord() == 0);             // nothing is recorded while it is still running
        m.sample(625, false);
        CHECK(m.state() == State::Running);          // one miss is not an exit
        m.sample(630, false);
        CHECK(m.state() == State::Exited);
        CHECK(m.sessionSeconds() == 600);
        CHECK(m.secondsToRecord() == 600);
    }

    // ---- 3. a single missed sample does not end the session -------------------------------------------------
    {
        Machine m(0);
        run(m, 20, 300, true);
        m.sample(305, false);                        // one sample misses (a hitch, an alt-tab to a crash dialog…)
        CHECK(m.state() == State::Running);
        run(m, 310, 600, true);
        m.sample(605, false);
        m.sample(610, false);
        CHECK(m.state() == State::Exited);
        CHECK(m.secondsToRecord() == 580);           // 20 -> 600, the gap included, as ONE session
    }
    // …while two consecutive misses DO end it, and a later running sample starts nothing new.
    {
        Machine m(0);
        run(m, 20, 300, true);
        m.sample(305, false);
        m.sample(310, false);
        CHECK(m.state() == State::Exited);
        CHECK(m.secondsToRecord() == 280);
        m.sample(315, true);
        CHECK(m.state() == State::Exited);
        CHECK(m.secondsToRecord() == 280);
    }

    // ---- 5. under the minimum: a crash at start records nothing ----------------------------------------------
    {
        Machine m(0);
        run(m, 10, 35, true);                        // 25 s
        m.sample(40, false); m.sample(45, false);
        CHECK(m.state() == State::Exited);
        CHECK(m.sessionSeconds() == 25);
        CHECK(m.secondsToRecord() == 0);
    }
    {
        Machine m(0);                                // exactly the minimum IS recorded
        run(m, 10, 40, true);
        m.sample(45, false); m.sample(50, false);
        CHECK(m.state() == State::Exited);
        CHECK(m.secondsToRecord() == 30);
    }

    // ---- 6. the install-dir prefix rule ---------------------------------------------------------------------
    {
        using LaunchWatch::imageUnderDir;
        CHECK(imageUnderDir(QStringLiteral("C:\\Games\\Foo\\game.exe"), QStringLiteral("C:/Games/Foo")));
        CHECK(imageUnderDir(QStringLiteral("C:\\Games\\Foo\\Bin\\x64\\game.exe"), QStringLiteral("C:\\Games\\Foo")));
        CHECK(imageUnderDir(QStringLiteral("c:\\games\\foo\\GAME.EXE"), QStringLiteral("C:\\Games\\Foo\\")));   // case, trailing sep
        CHECK(imageUnderDir(QStringLiteral("C:/Games/Foo/game.exe"), QStringLiteral("C:/Games/Foo/")));
        // Foo vs FooBar: the sibling whose name merely STARTS with the install dir's name is not the game.
        CHECK(!imageUnderDir(QStringLiteral("C:\\Games\\FooBar\\game.exe"), QStringLiteral("C:\\Games\\Foo")));
        CHECK(!imageUnderDir(QStringLiteral("C:\\Games\\FooBar\\game.exe"), QStringLiteral("C:/Games/Foo/")));
        CHECK(!imageUnderDir(QStringLiteral("C:\\Games\\Foo.exe"), QStringLiteral("C:\\Games\\Foo")));
        CHECK(!imageUnderDir(QStringLiteral("C:\\Games\\Foo"), QStringLiteral("C:\\Games\\Foo")));  // the dir itself is no process
        // Nothing claims "every process on the drive".
        CHECK(!imageUnderDir(QStringLiteral("C:\\Games\\Foo\\game.exe"), QString()));
        CHECK(!imageUnderDir(QStringLiteral("C:\\Games\\Foo\\game.exe"), QStringLiteral("C:\\")));
        CHECK(!imageUnderDir(QStringLiteral("C:\\Games\\Foo\\game.exe"), QStringLiteral("C:")));
        CHECK(!imageUnderDir(QStringLiteral("/usr/bin/game"), QStringLiteral("/")));
        CHECK(!imageUnderDir(QString(), QStringLiteral("C:\\Games\\Foo")));
        // A dotted segment in the install path does not defeat the match.
        CHECK(imageUnderDir(QStringLiteral("D:\\Epic\\Foo\\game.exe"), QStringLiteral("D:/Epic/Bar/../Foo")));

        const QStringList procs = { QStringLiteral("C:\\Windows\\explorer.exe"),
                                    QStringLiteral("C:\\Program Files (x86)\\Epic Games\\Launcher\\Portal\\Binaries\\Win64\\EpicGamesLauncher.exe"),
                                    QStringLiteral("D:\\Epic\\FooBar\\FooBar.exe") };
        CHECK(!LaunchWatch::anyImageUnderDir(procs, QStringLiteral("D:/Epic/Foo")));
        CHECK(LaunchWatch::anyImageUnderDir(procs + QStringList{ QStringLiteral("D:\\Epic\\Foo\\Foo.exe") },
                                            QStringLiteral("D:/Epic/Foo")));
    }

    // ---- Steam: RunningAppID wins when present; the install-dir check is only the fallback ------------------
    {
        int fallbackCalls = 0;
        const std::function<bool()> yes = [&fallbackCalls] { ++fallbackCalls; return true; };
        CHECK(LaunchWatch::steamRunning(QVariant(620), QStringLiteral("620"), yes));
        CHECK(!LaunchWatch::steamRunning(QVariant(0), QStringLiteral("620"), yes));      // Steam says nothing runs
        CHECK(!LaunchWatch::steamRunning(QVariant(440), QStringLiteral("620"), yes));    // a different game
        CHECK(fallbackCalls == 0);                                                       // never consulted
        CHECK(LaunchWatch::steamRunning(QVariant(), QStringLiteral("620"), yes));        // value absent -> fallback
        CHECK(fallbackCalls == 1);
        CHECK(!LaunchWatch::steamRunning(QVariant(), QStringLiteral("620"), {}));        // absent, no fallback
    }

    // ---- 8. which launch URLs get a watch: steam://install gets none ----------------------------------------
    {
        using K = LaunchWatch::Target::Kind;
        const auto run1 = LaunchWatch::targetForLaunchUrl(QStringLiteral("steam://rungameid/620"));
        CHECK(run1.kind == K::Steam && run1.storeId == QStringLiteral("620"));
        const auto run2 = LaunchWatch::targetForLaunchUrl(QStringLiteral("steam://run/620"));
        CHECK(run2.kind == K::Steam && run2.storeId == QStringLiteral("620"));
        CHECK(LaunchWatch::targetForLaunchUrl(QStringLiteral("steam://install/620")).kind == K::None);
        CHECK(LaunchWatch::targetForLaunchUrl(QStringLiteral("steam://open/games")).kind == K::None);
        CHECK(LaunchWatch::targetForLaunchUrl(QStringLiteral("steam://rungameid/")).kind == K::None);
        const auto epic = LaunchWatch::targetForLaunchUrl(
            QStringLiteral("com.epicgames.launcher://apps/Fortnite?action=launch&silent=true"));
        CHECK(epic.kind == K::Epic && epic.storeId == QStringLiteral("Fortnite"));
        CHECK(LaunchWatch::targetForLaunchUrl(
                  QStringLiteral("com.epicgames.launcher://apps/Fortnite?action=install")).kind == K::None);
        CHECK(LaunchWatch::targetForLaunchUrl(QStringLiteral("com.epicgames.launcher://store/")).kind == K::None);
        CHECK(LaunchWatch::targetForLaunchUrl(QStringLiteral("battlenet://wow")).kind == K::None);
        CHECK(LaunchWatch::targetForLaunchUrl(QStringLiteral("C:/Games/Foo/game.exe")).kind == K::None);
    }

    // ---- the watcher: one timer, alive only while a watch is live; one watch per game; quit records nothing -
    {
        qint64 clock = 0;
        struct End { QString key, playId; State state; qint64 secs; };
        QVector<End> ends;
        LaunchWatcher w([&ends](const QString& k, const QString& id, State s, qint64 secs) { ends.push_back({ k, id, s, secs }); },
                        [&clock] { return clock; });
        CHECK(!w.timerActive());                                         // nothing watched -> no timer
        QStringList started;
        w.setOnRunning([&started](const QString& k) { started << k; });

        // 4. A launcher wrapper that restarts the game: the wrapper's exe, a one-sample gap, then the real game.
        QStringList procs;
        const QString dir = QStringLiteral("D:/Epic/Foo");
        CHECK(!w.watch(QStringLiteral("epic:Foo"), QStringLiteral("epic:Foo"), {}));  // no sampler -> no watch
        CHECK(!w.timerActive());
        CHECK(w.watch(QStringLiteral("epic:Foo"), QStringLiteral("epic:Foo"),
                      [&procs, dir] { return LaunchWatch::anyImageUnderDir(procs, dir); }));
        CHECK(w.timerActive());
        CHECK(w.count() == 1);
        // A second launch of the same game while it is watched is ignored.
        CHECK(!w.watch(QStringLiteral("epic:Foo"), QStringLiteral("epic:Foo"), [] { return true; }));
        CHECK(w.count() == 1);

        auto step = [&](qint64 t) { clock = t; w.tick(); };
        step(5); step(10);                                               // store starting
        procs = { QStringLiteral("D:\\Epic\\Foo\\FooLauncher.exe") };
        step(15); step(20);                                              // the wrapper
        procs = {};
        step(25);                                                        // it restarts the game: one gap
        procs = { QStringLiteral("D:\\Epic\\Foo\\Binaries\\Win64\\Foo-Win64-Shipping.exe") };
        for (qint64 t = 30; t <= 400; t += 5) step(t);
        CHECK(ends.isEmpty());                                           // still one live session
        CHECK(started == QStringList{ QStringLiteral("epic:Foo") });     // seen starting ONCE, not per process
        CHECK(w.timerActive());
        procs = {};
        step(405);
        CHECK(ends.isEmpty());
        step(410);
        CHECK(ends.size() == 1);                                         // ONE session, reported once
        CHECK(ends.size() == 1 && ends[0].state == State::Exited && ends[0].secs == 385   // 15 -> 400
              && ends[0].key == QStringLiteral("epic:Foo") && ends[0].playId == QStringLiteral("epic:Foo"));
        CHECK(w.count() == 0);
        CHECK(!w.timerActive());                                         // last watch gone -> timer stopped
        step(415);
        CHECK(ends.size() == 1);                                         // a stray tick records nothing more

        // The same game can be watched again once its previous watch has ended.
        CHECK(w.watch(QStringLiteral("epic:Foo"), QStringLiteral("epic:Foo"), [] { return false; }));
        CHECK(w.timerActive());
        // An aborted launch through the watcher: reported as timed out, 0 seconds.
        const qint64 t0 = clock;
        for (qint64 t = t0 + 5; t <= t0 + LaunchWatch::kPendingTimeoutSecs; t += 5) step(t);
        CHECK(ends.size() == 2 && ends[1].state == State::TimedOut && ends[1].secs == 0);
        CHECK(!w.timerActive());

        // Two games at once; the timer lives until the LAST one ends.
        bool aRun = true, bRun = true;
        clock = 1000;
        CHECK(w.watch(QStringLiteral("steam:1"), QStringLiteral("steam:1"), [&aRun] { return aRun; }));
        CHECK(w.watch(QStringLiteral("steam:2"), QStringLiteral("steam:2"), [&bRun] { return bRun; }));
        CHECK(w.count() == 2);
        for (qint64 t = 1005; t <= 1100; t += 5) step(t);
        aRun = false; step(1105); step(1110);
        CHECK(ends.size() == 3 && ends[2].key == QStringLiteral("steam:1") && ends[2].secs == 95);
        CHECK(w.count() == 1);
        CHECK(w.timerActive());                                          // steam:2 still running
        CHECK(w.isWatching(QStringLiteral("steam:2")) && !w.isWatching(QStringLiteral("steam:1")));

        // Quit: a running session is dropped, NOT recorded as a partial one; the timer stops.
        w.stopAll();
        CHECK(w.count() == 0);
        CHECK(!w.timerActive());
        CHECK(ends.size() == 3);
        step(2000);
        CHECK(ends.size() == 3);
    }

    if (failures == 0) { std::puts("LAUNCHWATCH-OK"); return 0; }
    std::fprintf(stderr, "LAUNCHWATCH: %d check(s) failed\n", failures);
    return 1;
}
