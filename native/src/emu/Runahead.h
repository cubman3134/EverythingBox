#pragma once
// Runahead (issue #100) — the single-instance latency-removal schedule, expressed as pure data + a template
// so the ORDERING can be pinned by a probe against a fake core. No Qt, no widgets, no libretro entry points:
// only the libretro *constants* for the serialization quirks a core may declare.
//
// WHY THE FEATURE EXISTS
// ----------------------
// Many classic games have 1-3 frames of INTERNAL input lag: the game reads the pad on frame t and only draws
// the response on frame t+N. Runahead removes exactly that: it keeps the real timeline where it is, but shows
// the picture the game will draw N frames from now, computed with the input the player is holding RIGHT NOW.
// It is not fast-forward — real time still advances exactly one frame per displayed frame.
//
// THE SCHEDULE (RetroArch's `runahead_run()`, single-instance branch)
// ------------------------------------------------------------------
// Checked against RetroArch master `runahead.c`, the `for (frame_number = 0; frame_number <= runahead_count;
// frame_number++)` loop, on 2026-09-06. For N >= 1 one displayed frame is:
//
//   run 0        (hidden)  -> advances the REAL timeline by one frame with this tick's input
//   save state             -> the canonical timeline position, taken right after run 0
//   runs 1..N-1  (hidden)  -> speculative, same input again
//   run N        (SHOWN)   -> its video + audio are the ones the player sees and hears
//   load state             -> back to the post-run-0 state, so the timeline is exactly one frame further on
//
// Consequences, and the invariants a probe can assert:
//   * N+1 runs happen but only ONE of them advances real time  -> game speed is unchanged;
//   * the displayed run is N frames ahead of the timeline       -> the response appears the frame it was asked for;
//   * the state saved during the sequence is restored before the next displayed frame begins -> after K
//     displayed frames the core is in exactly the state K plain runs would have left it in;
//   * input is polled ONCE per displayed frame and the same input drives every run in the sequence
//     (RetroArch calls core_run() for run 0 and runahead_core_run_use_last_input() for the rest; here the whole
//     sequence executes inside a single frame tick with no poll and no event delivery between runs, so the
//     runs cannot see different input — and this header never asks its Ops for input, which is what makes the
//     property structural rather than incidental).
//
// N = 0 IS NOT THIS SCHEDULE. It is one plain run: no save, no load, no extra run, nothing hidden. RetroArch
// short-circuits the same way (`runahead_count <= 0` skips the loop). The frame loop is load-bearing for every
// game in the app, so schedule(0) is deliberately the identity and every probe asserts it.
//
// WHAT THE HIDDEN RUNS MUST NOT TOUCH
// -----------------------------------
// A hidden run is emulation the player never sees. If anything downstream counts it, that thing double-counts:
// audio would play N+1 times per frame, the rewind ring would fill with timelines that never happened,
// achievement logic would fire on speculative memory, play statistics would inflate. The `shown` flag on each
// Step is the single signal the frontend gates all of that on.

#include <cstddef>
#include <cstdint>
#include <vector>
#include "libretro.h"   // RETRO_SERIALIZATION_QUIRK_* only

namespace runahead {

// The user-facing ceiling. Beyond ~3 frames the speculation is longer than any real game's internal lag and
// the cost (N+1 runs a frame) stops fitting a frame budget on the hardware this runs on.
inline constexpr int kMaxFrames = 3;

// Fraction of the frame period the whole sequence is allowed to occupy. The remainder is the frontend's own
// per-frame work — the blit, the audio resample, the input poll, the paint. Runahead that eats the entire
// budget delivers slow motion, and a feature that halves the frame rate to reduce latency has made the game
// worse; refusing is the correct answer (issue #100), so this margin is deliberately generous.
inline constexpr double kBudgetShare = 0.80;

// Clamp a stored/typed N into the supported range. Negative or absent reads as off.
inline int clampFrames(int n) { return n < 0 ? 0 : (n > kMaxFrames ? kMaxFrames : n); }

// One core run inside a displayed frame, plus the state operation (if any) that follows it.
struct Step
{
    bool shown     = false;  // this run's video, audio and per-frame bookkeeping reach the player
    bool saveAfter = false;  // serialize the core state immediately after this run
    bool loadAfter = false;  // restore the serialized state immediately after this run
};

// The whole plan for ONE displayed frame: the rewind snapshot (taken before any run, from the real-timeline
// state — never from a speculative one) and then the run/save/load sequence.
struct Plan
{
    bool captureRewindFirst = false;
    std::vector<Step> steps;
};

// The run/save/load sequence for N. N = 0 is a single plain shown run.
inline std::vector<Step> schedule(int n)
{
    n = clampFrames(n);
    std::vector<Step> steps;
    if (n <= 0)
    {
        Step s; s.shown = true;          // exactly today's loop body: no save, no load, no hidden run
        steps.push_back(s);
        return steps;
    }
    steps.reserve(std::size_t(n) + 1);
    for (int i = 0; i <= n; ++i)
    {
        Step s;
        s.shown     = (i == n);          // only the last run is seen and heard
        s.saveAfter = (i == 0);          // the canonical timeline position, one frame on from where we started
        s.loadAfter = (i == n);          // …restored once the player has been shown the speculative frame
        steps.push_back(s);
    }
    return steps;
}

// `captureRewind` is the caller's rewind-enabled predicate (the SAME `!splitPane_` gate rewind and
// fast-forward already take). The snapshot is taken once, before any run, so the rewind ring only ever holds
// real-timeline states — a buffer holding speculative frames would rewind the player into a timeline that
// never happened.
inline Plan planFrame(int n, bool captureRewind)
{
    Plan p;
    p.captureRewindFirst = captureRewind;
    p.steps = schedule(n);
    return p;
}

enum class Outcome
{
    Completed,   // the whole plan ran
    Aborted,     // a run reported the core stopped (a crash) — nothing further was attempted
    SaveFailed,  // the core refused to serialize mid-sequence
    LoadFailed   // the core refused to restore mid-sequence (the timeline is now N frames ahead)
};

// Execute a plan. `Ops` supplies the four side effects, and NOTHING else — no input hook, which is what makes
// "the same input drives every run" structural:
//     void captureRewind();      // snapshot the pre-frame state into the rewind ring
//     bool run(bool shown);      // advance one frame; false = the core crashed and was stopped
//     bool save();               // serialize the current state into the sequence buffer
//     bool load();               // restore the state save() wrote
template <class Ops>
inline Outcome runPlan(const Plan& plan, Ops& ops)
{
    if (plan.captureRewindFirst) ops.captureRewind();
    for (const Step& s : plan.steps)
    {
        if (!ops.run(s.shown))            return Outcome::Aborted;
        if (s.saveAfter && !ops.save())   return Outcome::SaveFailed;
        if (s.loadAfter && !ops.load())   return Outcome::LoadFailed;
    }
    return Outcome::Completed;
}

// ---- eligibility: runahead REFUSES rather than degrading -------------------------------------------------
enum class Refusal
{
    None,            // engaged
    Off,             // N = 0 — the user hasn't asked for it (not an error; say nothing)
    Netplay,         // lockstep owns the timeline
    SplitPane,       // the same exclusion rewind and fast-forward already take
    Threaded,        // the core lives on a worker thread; the sequence is a GUI-tick construct
    NoSerialization, // retro_serialize_size() == 0 — there is no state to roll back to
    UnstableQuirks,  // the core declared serialization quirks that make state ops unreliable
    StateOpsFailed,  // save or load actually failed on this core
    TooSlow          // N+1 runs plus a save and a load do not fit the frame budget on this device
};

// The quirks that disqualify a core.
//   INCOMPLETE          — libretro.h says outright that such a state "should not be relied upon for
//                         frame-sensitive frontend features"; runahead is exactly that.
//   MUST_INITIALIZE     — serialize/unserialize "will initially fail"; we cannot tell an initialising core
//                         from a broken one, and a silent no-op rollback corrupts the timeline.
//   CORE_VARIABLE_SIZE  — the state size can change between the save and the load inside one sequence, which
//                         invalidates both the buffer and the cost we measured.
// Deliberately NOT disqualifying: SINGLE_SESSION, ENDIAN_DEPENDENT and PLATFORM_DEPENDENT. Those constrain
// states written to DISK and reloaded elsewhere; runahead saves and restores within one session, one process
// and one machine, so they are irrelevant here. (RetroArch reaches the same conclusion by a different route:
// it reads `savestate_features` out of the core's info file, which we do not ship.)
inline constexpr std::uint64_t kDisqualifyingQuirks =
      std::uint64_t(RETRO_SERIALIZATION_QUIRK_INCOMPLETE)
    | std::uint64_t(RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE)
    | std::uint64_t(RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE);

// What one displayed frame costs with runahead engaged: N+1 core runs, one serialize, one restore.
inline double sequenceCostMs(int n, double runMs, double saveMs, double loadMs)
{
    return double(clampFrames(n) + 1) * runMs + saveMs + loadMs;
}

// Does that fit the frame at FULL SPEED? A non-positive frame period means we don't know the game's timing
// yet, and an unknown budget is not a budget we can promise to hold — so it does not fit.
inline bool fitsBudget(int n, double runMs, double saveMs, double loadMs, double frameMs)
{
    if (!(frameMs > 0.0)) return false;
    return sequenceCostMs(n, runMs, saveMs, loadMs) <= frameMs * kBudgetShare;
}

// Everything the decision reads, gathered by the frontend at session start.
struct Conditions
{
    int  frames        = 0;      // the N the user asked for (clamped by the caller or by evaluate)
    bool netplay       = false;
    bool splitPane     = false;
    bool threaded      = false;
    std::size_t   stateSize = 0; // retro_serialize_size()
    std::uint64_t quirks    = 0; // RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, 0 if the core declared none
    bool stateOpsVerified = false; // a real save+load round trip succeeded on this core
    double runMs   = 0.0;        // measured cost of one retro_run
    double saveMs  = 0.0;        // measured cost of one serialize
    double loadMs  = 0.0;        // measured cost of one restore
    double frameMs = 0.0;        // this game's true frame period (1000 / fps)
};

// The one decision. Ordered so the reported reason is the most specific true one: what the session IS beats
// what the core CAN do, which beats how fast the device is.
inline Refusal evaluate(const Conditions& c)
{
    if (clampFrames(c.frames) <= 0) return Refusal::Off;
    if (c.netplay)                  return Refusal::Netplay;
    if (c.splitPane)                return Refusal::SplitPane;
    if (c.threaded)                 return Refusal::Threaded;
    if (c.stateSize == 0)           return Refusal::NoSerialization;
    if (c.quirks & kDisqualifyingQuirks) return Refusal::UnstableQuirks;
    if (!c.stateOpsVerified)        return Refusal::StateOpsFailed;
    if (!fitsBudget(c.frames, c.runMs, c.saveMs, c.loadMs, c.frameMs)) return Refusal::TooSlow;
    return Refusal::None;
}

// A readable sentence for each refusal — the user is told WHY, never left with a setting that silently did
// nothing. English source text; the UI layer is free to wrap it. Refusal::None and Refusal::Off return an
// empty string: neither is a failure worth interrupting anyone about.
inline const char* refusalMessage(Refusal r)
{
    switch (r)
    {
    case Refusal::None:            return "";
    case Refusal::Off:             return "";
    case Refusal::Netplay:         return "Runahead is off during netplay: both players must run the same timeline.";
    case Refusal::SplitPane:       return "Runahead is off in split screen, like rewind and fast-forward.";
    case Refusal::Threaded:        return "Runahead is off while the core runs on its own thread.";
    case Refusal::NoSerialization: return "This core can't save states, so runahead has nothing to roll back to.";
    case Refusal::UnstableQuirks:  return "This core's save states aren't reliable enough for runahead.";
    case Refusal::StateOpsFailed:  return "This core's save states failed, so runahead has been turned off for this session.";
    case Refusal::TooSlow:         return "Runahead is off: this device can't run this game that many times per frame without slowing it down.";
    }
    return "";
}

} // namespace runahead
