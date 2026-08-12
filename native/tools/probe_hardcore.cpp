// Headless check of RetroAchievements HARDCORE MODE (issue #94): the pure hardcore-affordance policy
// (src/core/Hardcore.h, header-only, QtCore-free) plus the per-install opt-in Setting that drives it.
// QtCore-only — the policy is a header-only pure predicate and Settings is a QSettings wrapper over the
// isolated everythingbox.ini — so it runs under the offscreen QPA in CI with no window and no emulator.
//
// WHAT IT PINS (the ONE place the rule lives, so every gate in RetroView + Achievements reads the same thing):
//   * forbidsInHardcore() returns TRUE for every affordance a hardcore session disables — save state, load
//     state, rewind, fast-forward, the cheat editor and cheat search — and FALSE for a genuinely-allowed one
//     (a screenshot). The FALSE case is what makes the predicate non-trivial: without it a mutant that always
//     returns true would be indistinguishable, so the rule would be inert.
//   * The Setting default: an absent "ra/hardcore" reads back FALSE — softcore stays the shipped default, the
//     crux of the issue — and the toggle round-trips true/false through the store.
//
// FIXTURES ARE HAND-AUTHORED, INDEPENDENT OF THE CODE UNDER TEST: each expected true/false below is written
// here by a human from the issue's list of disabled affordances, NOT produced by calling forbidsInHardcore a
// second time. The default is a genuine absence — AppPaths::dataDir() is this process's own scratch dir
// (issue #42), so the ini the Settings half reads starts empty and is removed at exit.
//
// Prints HARDCORE-OK on success; any failure prints HARDCORE-FAIL <cond> (line) and exits non-zero.
#include "Hardcore.h"
#include "Settings.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "HARDCORE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static void testPolicy()
{
    using hardcore::Feature;
    using hardcore::forbidsInHardcore;

    // The six affordances the issue disables in hardcore. Each is asserted true INDIVIDUALLY so a mutant that
    // flips exactly one case (e.g. drops FastForward through to the allowed default) is killed by that line.
    CHECK(forbidsInHardcore(Feature::SaveState)   == true);
    CHECK(forbidsInHardcore(Feature::LoadState)   == true);
    CHECK(forbidsInHardcore(Feature::Rewind)      == true);
    CHECK(forbidsInHardcore(Feature::FastForward) == true);
    CHECK(forbidsInHardcore(Feature::Cheats)      == true);
    CHECK(forbidsInHardcore(Feature::CheatSearch) == true);

    // The allowed exemplar — a screenshot never voids a hardcore run. This is the tripwire that keeps the
    // predicate honest: a constant-true implementation fails HERE.
    CHECK(forbidsInHardcore(Feature::Screenshot)  == false);
}

static void testSetting()
{
    // Shipped default on an empty ini: hardcore is OFF, so softcore is the default. An absent key must read
    // false, not true — the crux of the "opt-in, softcore stays first-class" requirement. The key is a genuine
    // absence (isolated scratch ini), so this is not a fixed point of the reader.
    CHECK(Settings::hardcoreAchievements() == false);   // "ra/hardcore" absent -> OFF

    // The toggle round-trips, and ON is distinguishable from the default.
    Settings::setHardcoreAchievements(true);
    CHECK(Settings::hardcoreAchievements() == true);
    Settings::setHardcoreAchievements(false);
    CHECK(Settings::hardcoreAchievements() == false);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testPolicy();
    testSetting();
    if (failures == 0) std::printf("HARDCORE-OK\n");
    return failures == 0 ? 0 : 1;
}
