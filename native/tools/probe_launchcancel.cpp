// Headless pure-logic probe for LaunchCancel::decide — the decision behind
// EmulatorManager::cancelPendingLaunch, i.e. whether an in-app frontend starting a game supersedes a pending
// external-emulator launch, and which of the two cancels is correct for the phase it is in.
//
// The input space is four booleans, so this probe pins ALL SIXTEEN of them rather than sampling: the bug this
// unit exists to prevent is a launch that boots on top of another frontend minutes later, and the way that
// comes back is one arm of the decision quietly widening. `covered` below asserts the sixteen really were all
// asserted on, so a case dropped from this file fails the probe instead of silently shrinking it.
//
// Nothing here needs a display, a network, a process, or even Qt — the unit is a pure function over four bools.
#include "core/LaunchCancel.h"

#include <cstdio>

using LaunchCancel::Action;
using LaunchCancel::decide;

static int g_fail = 0;
static bool g_covered[16] = { false };

static const char* name(Action a)
{
    switch (a)
    {
    case Action::None:            return "None";
    case Action::CancelInstall: return "CancelInstall";
    case Action::CancelNow:       return "CancelNow";
    }
    return "?";
}

// Assert one point of the truth table and record that it was covered.
static void expect(bool busy, bool gameRunning, bool launchAfterInstall, bool installing, Action want, int line)
{
    const int idx = (busy ? 8 : 0) | (gameRunning ? 4 : 0) | (launchAfterInstall ? 2 : 0) | (installing ? 1 : 0);
    g_covered[idx] = true;
    const Action got = decide(busy, gameRunning, launchAfterInstall, installing);
    if (got != want)
    {
        std::fprintf(stderr,
                     "decide(busy=%d, gameRunning=%d, launchAfterInstall=%d, installing=%d) = %s, expected %s "
                     "(line %d)\n",
                     busy, gameRunning, launchAfterInstall, installing, name(got), name(want), line);
        ++g_fail;
    }
}
#define EXPECT(b, g, l, i, want) expect((b), (g), (l), (i), (want), __LINE__)

// Nothing is pending when the manager is idle, so there is never anything to supersede — whatever the other
// three say. They are stale leftovers of the previous run at this point, and must not be read as a live launch.
static void testNotBusy()
{
    for (int bits = 0; bits < 8; ++bits)
        EXPECT(false, (bits & 4) != 0, (bits & 2) != 0, (bits & 1) != 0, Action::None);
}

// A game process already exists: ending it is closeGame()/terminateGame() territory — destructive and
// save-affecting — not this primitive's. An in-app launch while an external game runs keeps today's behaviour.
static void testGameRunningIsNeverCancelled()
{
    for (int bits = 0; bits < 4; ++bits)
        EXPECT(true, true, (bits & 2) != 0, (bits & 1) != 0, Action::None);
}

// An install-only run (Settings ▸ Emulators) boots nothing when it finishes, so there is no launch to
// supersede — and the user wants that download to complete. True in both phases of the install.
static void testInstallOnlyIsNeverCancelled()
{
    EXPECT(true, false, false, true,  Action::None);
    EXPECT(true, false, false, false, Action::None);
}

// The two live cancels. `installing` is the whole difference: inside the install chain the continuations hang
// off the manager itself, so the cancel disarms the boot and aborts the download and the chain's own finished
// handler clears busy_; past launch() every continuation is launchCtx_-bound, so retiring the context drops
// them and this arm must clear busy_ and emit the terminal signal itself. Getting these two the wrong way
// round wedges the manager busy forever (cancel-install applied in the launch phase, where no handler is left
// to free it) or races a running download (cancel-now applied mid-install, freeing busy_ while a `this`-bound
// continuation can still touch em_/rom_/archivePath_).
static void testPendingLaunchCancels()
{
    EXPECT(true, false, true, true,  Action::CancelInstall);
    EXPECT(true, false, true, false, Action::CancelNow);
}

int main()
{
    testNotBusy();
    testGameRunningIsNeverCancelled();
    testInstallOnlyIsNeverCancelled();
    testPendingLaunchCancels();

    // Deliberate absence-of-behaviour tripwire (CONTRIBUTING's carve-out): no LaunchCancel.h mutant can redden
    // this loop — it guards THIS file against silently losing a case, not the header against changing. The
    // probe-file mutant "truth-table-hole-sweep-fires" in mutate-launchcancel.json is what proves it fires.
    for (int i = 0; i < 16; ++i)
        if (!g_covered[i])
        {
            std::fprintf(stderr, "truth-table hole: busy=%d gameRunning=%d launchAfterInstall=%d installing=%d "
                                 "is never asserted\n",
                         (i >> 3) & 1, (i >> 2) & 1, (i >> 1) & 1, i & 1);
            ++g_fail;
        }

    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("LAUNCHCANCEL-OK\n");
    return 0;
}
