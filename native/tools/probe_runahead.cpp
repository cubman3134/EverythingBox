// Headless check of RUNAHEAD (issue #100): the pure single-instance schedule in src/emu/Runahead.h, driven
// against a FAKE serializable core, plus the eligibility gate and the Settings store that holds N.
//
// A real libretro core cannot run in CI (no core, no ROM, no window) — but the thing runahead can get wrong is
// not the emulation, it is the ORDERING. So the ordering is the part that was extracted into a pure header,
// and this probe drives THE SAME code the app runs (RetroView supplies a different Ops adapter over the same
// runahead::runPlan) against a counter core whose state is two integers.
//
// WHAT IT PINS
//   * The schedule shape for N = 0, 1, 2, 3, hand-authored below from RetroArch's runahead.c: N+1 runs, the
//     save after run 0, the load after the last run, only the last run shown. N = 0 is the IDENTITY — one
//     plain shown run, no save, no load, no hidden run — which is the safety property the whole feature rests
//     on, because the frame loop is load-bearing for every game in the app.
//   * One frame of real time per displayed frame: after K displayed frames the core has advanced K frames,
//     for every N. Runahead is not fast-forward.
//   * The frame SHOWN is N ahead of the timeline — the reason the feature exists.
//   * THE STRONGEST ASSERTION AVAILABLE: 100 displayed frames with runahead leave the core in byte-identical
//     state to 100 plain runs with the same input sequence. If the rollback were wrong in any way — a missing
//     load, a save taken at the wrong point, one extra run escaping the sequence — the two diverge.
//   * The same input drives every run in a sequence.
//   * The rewind ring is fed exactly once per displayed frame, BEFORE any run, so it never holds a speculative
//     state.
//   * The eligibility gate REFUSES (never degrades to slow motion): on a slow core/device, on a zero
//     serialize size, on each disqualifying serialization quirk individually, and on the netplay / split-pane
//     / threaded exclusions. Its allowed cases are asserted too, so a constant-refuse implementation fails.
//   * Settings: the default N is 0 on an empty ini (runahead is opt-in), values round-trip, out-of-range
//     values are clamped, and a per-game N lives in its own keyspace — it does not move the global default,
//     and clearing it REMOVES the key rather than storing a 0 that would look deliberate.
//
// FIXTURES ARE HAND-AUTHORED: every expected step table, frame number and refusal below is written here from
// the issue and from RetroArch's loop, never produced by calling the code under test a second time.
//
// Prints RUNAHEAD-OK on success; any failure prints RUNAHEAD-FAIL <cond> (line) and exits non-zero.
#include "Runahead.h"
#include "Settings.h"

#include <QCoreApplication>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "RUNAHEAD-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---------------------------------------------------------------------------------------------------------
// The fake core: a frame counter and an accumulator folded from whatever input the frame ran with. Two
// integers, so its "save state" is exact and its equality is exact — which is what makes the 100-frame
// equivalence assertion below meaningful rather than approximate.
// ---------------------------------------------------------------------------------------------------------
struct FakeCore
{
    std::uint64_t frame = 0;      // frames of emulated time elapsed
    std::uint64_t acc   = 0;      // folded input history — diverges the instant a run happens out of order

    // instrumentation (not part of the state; never saved/restored)
    int runs = 0, saves = 0, loads = 0;
    std::vector<std::uint16_t> inputSeen;   // the input every run observed, in order
    std::vector<std::uint64_t> shownFrames; // the emulated frame number of each SHOWN run
    std::vector<std::uint64_t> hiddenFrames;

    bool saveWorks = true, loadWorks = true;
    std::vector<std::uint64_t> saved;       // the one runahead buffer

    void step(std::uint16_t input, bool shown)
    {
        ++frame;
        acc = acc * 1315423911ull + input + 0x9E3779B97F4A7C15ull;
        ++runs;
        inputSeen.push_back(input);
        if (shown) shownFrames.push_back(frame); else hiddenFrames.push_back(frame);
    }
    bool save() { ++saves; if (!saveWorks) return false; saved = { frame, acc }; return true; }
    bool load()
    {
        ++loads;
        if (!loadWorks || saved.size() != 2) return false;
        frame = saved[0]; acc = saved[1]; return true;
    }
    bool sameStateAs(const FakeCore& o) const { return frame == o.frame && acc == o.acc; }
};

// The Ops adapter the probe hands to runPlan — the twin of RetroView's, over the fake core.
struct FakeOps
{
    FakeCore* core = nullptr;
    std::uint16_t input = 0;
    int rewindCaptures = 0;
    std::vector<int> rewindAfterRuns;   // how many runs had happened when each capture was taken

    void captureRewind() { ++rewindCaptures; rewindAfterRuns.push_back(core->runs); }
    bool run(bool shown) { core->step(input, shown); return true; }
    bool save() { return core->save(); }
    bool load() { return core->load(); }
};

// ---------------------------------------------------------------------------------------------------------
static void testScheduleShape()
{
    using runahead::Step;

    // N = 0 — THE IDENTITY. One plain run, shown, with no state operation at all. Every other assertion in
    // this file is about runahead working; this one is about runahead being ABSENT when it was not asked for.
    const std::vector<Step> s0 = runahead::schedule(0);
    CHECK(s0.size() == 1);
    if (s0.size() == 1)
    {
        CHECK(s0[0].shown     == true);
        CHECK(s0[0].saveAfter == false);
        CHECK(s0[0].loadAfter == false);
    }

    // N = 1 — run (hidden) + save, then run (shown) + load. Hand-copied from RetroArch's loop: the save is
    // after frame_number == 0, the load after last_frame, and suspended_frame = !last_frame.
    const std::vector<Step> s1 = runahead::schedule(1);
    CHECK(s1.size() == 2);
    if (s1.size() == 2)
    {
        CHECK(s1[0].shown == false); CHECK(s1[0].saveAfter == true);  CHECK(s1[0].loadAfter == false);
        CHECK(s1[1].shown == true);  CHECK(s1[1].saveAfter == false); CHECK(s1[1].loadAfter == true);
    }

    // N = 2 — the middle run is pure speculation: hidden, and neither saves nor loads.
    const std::vector<Step> s2 = runahead::schedule(2);
    CHECK(s2.size() == 3);
    if (s2.size() == 3)
    {
        CHECK(s2[0].shown == false); CHECK(s2[0].saveAfter == true);  CHECK(s2[0].loadAfter == false);
        CHECK(s2[1].shown == false); CHECK(s2[1].saveAfter == false); CHECK(s2[1].loadAfter == false);
        CHECK(s2[2].shown == true);  CHECK(s2[2].saveAfter == false); CHECK(s2[2].loadAfter == true);
    }

    // N = 3 — the cap.
    const std::vector<Step> s3 = runahead::schedule(3);
    CHECK(s3.size() == 4);
    if (s3.size() == 4)
    {
        CHECK(s3[0].shown == false); CHECK(s3[0].saveAfter == true);  CHECK(s3[0].loadAfter == false);
        CHECK(s3[1].shown == false); CHECK(s3[1].saveAfter == false); CHECK(s3[1].loadAfter == false);
        CHECK(s3[2].shown == false); CHECK(s3[2].saveAfter == false); CHECK(s3[2].loadAfter == false);
        CHECK(s3[3].shown == true);  CHECK(s3[3].saveAfter == false); CHECK(s3[3].loadAfter == true);
    }

    // Exactly ONE save and ONE load per displayed frame, and exactly ONE shown run, for every supported N.
    for (int n = 0; n <= runahead::kMaxFrames; ++n)
    {
        const std::vector<Step> s = runahead::schedule(n);
        int shown = 0, saves = 0, loads = 0;
        for (const Step& st : s) { shown += st.shown; saves += st.saveAfter; loads += st.loadAfter; }
        CHECK(int(s.size()) == n + 1);
        CHECK(shown == 1);
        CHECK(saves == (n > 0 ? 1 : 0));
        CHECK(loads == (n > 0 ? 1 : 0));
        CHECK(s.back().shown == true);   // the LAST run is the one the player sees
    }

    // Out-of-range N is clamped, not obeyed: a hand-edited ini cannot ask the frame loop for 40 runs.
    CHECK(runahead::clampFrames(-7) == 0);
    CHECK(runahead::clampFrames(0)  == 0);
    CHECK(runahead::clampFrames(3)  == 3);
    CHECK(runahead::clampFrames(4)  == runahead::kMaxFrames);
    CHECK(runahead::schedule(99).size() == std::size_t(runahead::kMaxFrames) + 1);
}

// A deterministic, non-constant input stream — non-constant matters, because a constant one would make a
// sequence that ran the WRONG number of times indistinguishable from a correct one.
static std::uint16_t inputForFrame(int i) { return std::uint16_t((i * 7919 + 13) & 0xFFFF); }

// ---------------------------------------------------------------------------------------------------------
// The strongest assertion available: 100 displayed frames with runahead land on exactly the state 100 plain
// runs land on, for every N. Also pins the real-time rate, the N-ahead display, the per-sequence input and
// the rewind feed — all from the same 100-frame drive, because they are properties of the same run.
// ---------------------------------------------------------------------------------------------------------
static void testHundredFrames(int n)
{
    const int kFrames = 100;

    // Reference: no runahead at all, the plain loop.
    FakeCore plain;
    for (int i = 0; i < kFrames; ++i) plain.step(inputForFrame(i), true);

    FakeCore core;
    FakeOps ops; ops.core = &core;
    for (int i = 0; i < kFrames; ++i)
    {
        ops.input = inputForFrame(i);
        const int runsBefore = core.runs;
        const std::size_t inputsBefore = core.inputSeen.size();

        const runahead::Plan plan = runahead::planFrame(n, /*captureRewind*/ true);
        const runahead::Outcome out = runahead::runPlan(plan, ops);
        CHECK(out == runahead::Outcome::Completed);

        // The rewind snapshot for THIS displayed frame was taken before any of its runs — never between them,
        // and never after a speculative run. A ring holding speculative states rewinds into a timeline that
        // never happened.
        CHECK(ops.rewindCaptures == i + 1);
        CHECK(ops.rewindAfterRuns.back() == runsBefore);

        // N+1 runs happened, and every one of them saw the SAME input.
        CHECK(core.runs - runsBefore == n + 1);
        for (std::size_t k = inputsBefore; k < core.inputSeen.size(); ++k)
            CHECK(core.inputSeen[k] == ops.input);

        // One frame of REAL time per displayed frame — the emulated timeline is exactly i+1 frames on.
        CHECK(core.frame == std::uint64_t(i + 1));

        // …and what the player was SHOWN is the state N frames further ahead than that.
        CHECK(core.shownFrames.size() == std::size_t(i + 1));
        CHECK(core.shownFrames.back() == std::uint64_t(i + 1 + n));
    }

    // Exactly one save and one load per displayed frame (none at all when runahead is off), and exactly the
    // hidden runs the schedule called for.
    CHECK(core.saves == (n > 0 ? kFrames : 0));
    CHECK(core.loads == (n > 0 ? kFrames : 0));
    CHECK(core.runs  == kFrames * (n + 1));
    CHECK(core.hiddenFrames.size() == std::size_t(kFrames * n));
    CHECK(core.shownFrames.size()  == std::size_t(kFrames));

    // THE assertion: the timeline is unchanged. Speculation left no trace.
    CHECK(core.sameStateAs(plain));
    CHECK(core.frame == std::uint64_t(kFrames));
}

// N = 0 must not merely end in the same place — it must not perform a single extra operation.
static void testZeroIsUntouched()
{
    FakeCore core;
    FakeOps ops; ops.core = &core;
    for (int i = 0; i < 100; ++i)
    {
        ops.input = inputForFrame(i);
        CHECK(runahead::runPlan(runahead::planFrame(0, true), ops) == runahead::Outcome::Completed);
    }
    CHECK(core.runs  == 100);
    CHECK(core.saves == 0);        // no serialize
    CHECK(core.loads == 0);        // no restore
    CHECK(core.hiddenFrames.empty());   // nothing hidden from anything
    CHECK(core.shownFrames.size() == 100);
    CHECK(core.frame == 100);
}

// The rewind feed is the caller's own predicate (the same !splitPane_ gate rewind and fast-forward take): when
// it is off, the plan takes no snapshot at all, and the runs are otherwise identical.
static void testRewindGate()
{
    FakeCore core;
    FakeOps ops; ops.core = &core;
    for (int i = 0; i < 10; ++i)
    {
        ops.input = inputForFrame(i);
        runahead::runPlan(runahead::planFrame(2, /*captureRewind*/ false), ops);
    }
    CHECK(ops.rewindCaptures == 0);
    CHECK(core.runs == 30);
    CHECK(core.hiddenFrames.size() == 20);
}

// A core that fails a state operation mid-sequence must stop the sequence at that point and say which half
// failed, so the frontend can disengage instead of gambling every frame.
static void testStateOpFailure()
{
    {
        FakeCore core; core.saveWorks = false;
        FakeOps ops; ops.core = &core;
        CHECK(runahead::runPlan(runahead::planFrame(2, true), ops) == runahead::Outcome::SaveFailed);
        CHECK(core.runs == 1);      // stopped right after the run whose save failed
        CHECK(core.loads == 0);
    }
    {
        FakeCore core; core.loadWorks = false;
        FakeOps ops; ops.core = &core;
        CHECK(runahead::runPlan(runahead::planFrame(2, true), ops) == runahead::Outcome::LoadFailed);
        CHECK(core.runs == 3);      // all three runs happened; only the rollback failed
        CHECK(core.saves == 1);
    }
    // A crashed core aborts the sequence at the run that reported it, with no state operation attempted.
    {
        struct DyingOps
        {
            FakeCore* core; int allow;
            void captureRewind() {}
            bool run(bool shown) { if (allow-- <= 0) return false; core->step(0, shown); return true; }
            bool save() { return core->save(); }
            bool load() { return core->load(); }
        };
        FakeCore core;
        DyingOps ops{ &core, 0 };
        CHECK(runahead::runPlan(runahead::planFrame(3, true), ops) == runahead::Outcome::Aborted);
        CHECK(core.runs == 0);
        CHECK(core.saves == 0);
        CHECK(core.loads == 0);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Eligibility: runahead refuses rather than delivering slow motion.
// ---------------------------------------------------------------------------------------------------------
static runahead::Conditions healthyConditions(int n)
{
    // A comfortable 8/16-bit core on this kind of machine: ~0.5 ms a frame, sub-millisecond state ops, a
    // 60 Hz frame period. Hand-picked so that N = 3 fits (4*0.5 + 0.2 + 0.2 = 2.4 ms against a 13.3 ms budget).
    runahead::Conditions c;
    c.frames = n;
    c.stateSize = 64 * 1024;
    c.quirks = 0;
    c.stateOpsVerified = true;
    c.runMs = 0.5; c.saveMs = 0.2; c.loadMs = 0.2;
    c.frameMs = 16.6667;
    return c;
}

static void testEligibility()
{
    // The allowed cases first — without them a constant-refuse implementation would pass everything below.
    for (int n = 1; n <= runahead::kMaxFrames; ++n)
        CHECK(runahead::evaluate(healthyConditions(n)) == runahead::Refusal::None);

    // N = 0 is "off", not a failure: nothing to tell the user about.
    CHECK(runahead::evaluate(healthyConditions(0)) == runahead::Refusal::Off);
    CHECK(runahead::refusalMessage(runahead::Refusal::Off)[0]  == '\0');
    CHECK(runahead::refusalMessage(runahead::Refusal::None)[0] == '\0');

    // The exclusions — the same ones rewind and fast-forward already take, plus netplay's lockstep timeline.
    { auto c = healthyConditions(2); c.netplay   = true; CHECK(runahead::evaluate(c) == runahead::Refusal::Netplay); }
    { auto c = healthyConditions(2); c.splitPane = true; CHECK(runahead::evaluate(c) == runahead::Refusal::SplitPane); }
    { auto c = healthyConditions(2); c.threaded  = true; CHECK(runahead::evaluate(c) == runahead::Refusal::Threaded); }

    // A core with no state has nothing to roll back to.
    { auto c = healthyConditions(2); c.stateSize = 0; CHECK(runahead::evaluate(c) == runahead::Refusal::NoSerialization); }

    // Each disqualifying quirk, INDIVIDUALLY, so a mutant that drops one of the three is killed by its own line.
    { auto c = healthyConditions(2); c.quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
      CHECK(runahead::evaluate(c) == runahead::Refusal::UnstableQuirks); }
    { auto c = healthyConditions(2); c.quirks = RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE;
      CHECK(runahead::evaluate(c) == runahead::Refusal::UnstableQuirks); }
    { auto c = healthyConditions(2); c.quirks = RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
      CHECK(runahead::evaluate(c) == runahead::Refusal::UnstableQuirks); }

    // …and the quirks that are NOT disqualifying, because they only constrain a state written to disk and read
    // back somewhere else. Runahead saves and restores inside one session, one process, one machine.
    { auto c = healthyConditions(2); c.quirks = RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION;
      CHECK(runahead::evaluate(c) == runahead::Refusal::None); }
    { auto c = healthyConditions(2); c.quirks = RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT;
      CHECK(runahead::evaluate(c) == runahead::Refusal::None); }
    { auto c = healthyConditions(2); c.quirks = RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT;
      CHECK(runahead::evaluate(c) == runahead::Refusal::None); }
    { auto c = healthyConditions(2); c.quirks = RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
      CHECK(runahead::evaluate(c) == runahead::Refusal::None); }

    // A core that claims a state but cannot actually round-trip one.
    { auto c = healthyConditions(2); c.stateOpsVerified = false;
      CHECK(runahead::evaluate(c) == runahead::Refusal::StateOpsFailed); }

    // Too slow: a heavy core where three runs plus the state ops overflow the frame. Refuse — never halve the
    // frame rate in the name of latency.
    { auto c = healthyConditions(2); c.runMs = 6.0;   // 3*6 + 0.4 = 18.4 ms > 16.667*0.8
      CHECK(runahead::evaluate(c) == runahead::Refusal::TooSlow); }
    // …and the same core is fine at a smaller N, which is what makes this a budget and not a blanket ban.
    { auto c = healthyConditions(1); c.runMs = 4.0;   // 2*4 + 0.4 = 8.4 ms <= 13.3 ms
      CHECK(runahead::evaluate(c) == runahead::Refusal::None); }
    // An expensive SERIALIZE alone can blow the budget even with a cheap run — the save is per displayed frame.
    { auto c = healthyConditions(1); c.saveMs = 9.0; c.loadMs = 9.0;
      CHECK(runahead::evaluate(c) == runahead::Refusal::TooSlow); }
    // An unknown frame period is not a budget we can promise to hold.
    { auto c = healthyConditions(1); c.frameMs = 0.0;
      CHECK(runahead::evaluate(c) == runahead::Refusal::TooSlow); }

    // The cost arithmetic itself: N+1 runs, one save, one load. Hand-computed.
    CHECK(runahead::sequenceCostMs(0, 1.0, 0.25, 0.5) == 1.75);   // 1 run  + 0.75
    CHECK(runahead::sequenceCostMs(2, 1.0, 0.25, 0.5) == 3.75);   // 3 runs + 0.75
    CHECK(runahead::fitsBudget(2, 1.0, 0.25, 0.5, 16.6667) == true);
    CHECK(runahead::fitsBudget(2, 5.0, 0.25, 0.5, 16.6667) == false); // 15.75 > 13.33

    // Every refusal the user can actually hit carries a readable sentence — a setting that silently did
    // nothing is the failure mode this replaces.
    for (runahead::Refusal r : { runahead::Refusal::Netplay, runahead::Refusal::SplitPane,
                                 runahead::Refusal::Threaded, runahead::Refusal::NoSerialization,
                                 runahead::Refusal::UnstableQuirks, runahead::Refusal::StateOpsFailed,
                                 runahead::Refusal::TooSlow })
    {
        const char* m = runahead::refusalMessage(r);
        CHECK(m != nullptr && m[0] != '\0');
    }
}

// ---------------------------------------------------------------------------------------------------------
static void testSettings()
{
    // Shipped default on an empty ini: OFF. An install that has never heard of runahead runs today's frame
    // loop, which is the safety property the issue turns on. The key is a genuine absence — AppPaths::dataDir()
    // is this process's own scratch dir (issue #42), so the ini starts empty.
    CHECK(Settings::runaheadFrames() == 0);

    Settings::setRunaheadFrames(2);
    CHECK(Settings::runaheadFrames() == 2);
    // Clamped on the way in AND on the way out, so a hand-edited ini cannot ask for 40 runs a frame.
    Settings::setRunaheadFrames(99);
    CHECK(Settings::runaheadFrames() == runahead::kMaxFrames);
    Settings::setRunaheadFrames(-3);
    CHECK(Settings::runaheadFrames() == 0);

    // Per-game N: its own keyspace. Presence is the override; absence inherits.
    const QString tokA = Settings::gameToken(QStringLiteral("tt-fixture-game-a"));
    const QString tokB = Settings::gameToken(QStringLiteral("tt-fixture-game-b"));
    CHECK(!tokA.isEmpty());
    CHECK(tokA != tokB);
    CHECK(Settings::gameHasRunaheadFrames(tokA) == false);   // a fresh game has no opinion

    Settings::setRunaheadFrames(1);                          // a non-zero default, so "inherits" is visible
    Settings::setGameRunaheadFrames(tokA, 3);
    CHECK(Settings::gameHasRunaheadFrames(tokA) == true);
    CHECK(Settings::gameRunaheadFrames(tokA) == 3);
    // The no-leak rail: a per-game write moved neither the global default nor any other game.
    CHECK(Settings::runaheadFrames() == 1);
    CHECK(Settings::gameHasRunaheadFrames(tokB) == false);

    // A reset REMOVES the key rather than storing 0 — otherwise "back to the default" would be indistinguishable
    // from "deliberately off for this game", and the game could never follow the default again.
    Settings::clearGameRunaheadFrames(tokA);
    CHECK(Settings::gameHasRunaheadFrames(tokA) == false);
    CHECK(Settings::runaheadFrames() == 1);

    // An empty token (a game with no identity) can never write into the store.
    Settings::setGameRunaheadFrames(QString(), 3);
    CHECK(Settings::gameHasRunaheadFrames(QString()) == false);
    CHECK(Settings::gameRunaheadFrames(QString()) == 0);

    Settings::setRunaheadFrames(0);   // leave the scratch store as we found it
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testScheduleShape();
    testZeroIsUntouched();
    for (int n = 0; n <= runahead::kMaxFrames; ++n) testHundredFrames(n);
    testRewindGate();
    testStateOpFailure();
    testEligibility();
    testSettings();
    if (failures == 0) std::printf("RUNAHEAD-OK\n");
    return failures == 0 ? 0 : 1;
}
