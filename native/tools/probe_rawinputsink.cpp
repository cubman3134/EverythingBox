// Headless check of the raw-input SINK planner (src/input/RawInputSink.h) — the pure half of the fix for the
// second USER-handle exhaustion (issue #226).
//
// Background, because the assertions below are meaningless without it: SDL's Windows joystick backend asks
// for gamepad hot-plug by registering a raw-input device against a message-only window on the thread that
// called SDL_Init — our input thread — and it asks with RIDEV_DEVNOTIFY | RIDEV_INPUTSINK. DEVNOTIFY is the
// part it wants. INPUTSINK subscribes that window to the whole background report stream of every gamepad,
// and every queued WM_INPUT holds a USER object against a hard per-process ceiling of 10,000. Measured in
// isolation, a sink whose messages are not retrieved costs one USER object per device report — 128/s from a
// 128Hz device, i.e. the ceiling in about a minute — while the same registration with the sink bit cleared
// costs exactly none. So the fix re-registers the same usage, against the same window, minus the sink.
//
// This probe pins the decision, which is the part that has to be right on a machine none of us is holding:
//
//   * the real SDL registration (page 1 / usage 5 / ours) is rewritten to 0x2000 — the sink gone, the
//     hot-plug notification KEPT, because losing DEVNOTIFY would silently cost pad hot-plug. That is pinned
//     from BOTH spellings, because GetRegisteredRawInputDevices does not report DEVNOTIFY: SDL registers
//     0x2100 and the query reads it back as 0x0100, so hot-plug has to be asserted rather than preserved,
//     and the first build of this fix wrote 0x0000 and unsubscribed SDL from hot-plug;
//   * a registration WITHOUT the sink is left alone, which is what makes the whole thing idempotent: the
//     input thread re-asserts it every couple of seconds and must not re-register anything in steady state;
//   * a sink belonging to ANOTHER thread's window is never touched — mpv links raw input too, and
//     re-registering a usage steals delivery from whoever owned it;
//   * unrelated flags survive the rewrite (only the sink bit is cleared, never a wholesale mask assignment);
//   * no plan ever carries RIDEV_REMOVE, so a rewrite can never turn into a de-registration;
//   * indices point back at the caller's own array, so the caller re-uses the original usage + hwndTarget
//     rather than reconstructing one.
//
// Prints RAWINPUTSINK-OK on success; any failure prints RAWINPUTSINK-FAIL <cond> (line) and exits non-zero.
#include "RawInputSink.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("RAWINPUTSINK-FAIL %s (line %d)\n", #cond, __LINE__);         \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

using RawInputSink::Registration;
using RawInputSink::Rewrite;
using RawInputSink::stripPlan;

int main()
{
    // 1) The exact registration SDL 2.30.11 makes: generic desktop / gamepad, DEVNOTIFY|INPUTSINK, on our
    //    own thread's window — in both the spelling SDL passes (0x2100) and the spelling the query reads it
    //    back as (0x0100, DEVNOTIFY dropped by Windows). Either way: one rewrite, entry 0, sink gone,
    //    hot-plug present.
    for (const std::uint32_t reported : { 0x2100u, 0x0100u })
    {
        const std::vector<Registration> regs{ Registration{ 0x0001, 0x0005, reported, true } };
        const std::vector<Rewrite> plan = stripPlan(regs);
        CHECK(plan.size() == 1);
        if (plan.size() == 1)
        {
            CHECK(plan[0].index == 0);
            CHECK(plan[0].flags == 0x2000);
            CHECK((plan[0].flags & RawInputSink::kInputSink) == 0);
            CHECK((plan[0].flags & RawInputSink::kDevNotify) != 0); // hot-plug survives
        }
    }

    // 2) Idempotence: the already-stripped registration is not rewritten again. The input thread re-checks
    //    every ~2s, so a plan that kept "fixing" a fixed entry would be a re-registration storm.
    {
        const std::vector<Registration> regs{ Registration{ 0x0001, 0x0005, 0x2000, true } };
        CHECK(stripPlan(regs).empty());
    }

    // 3) Another thread's sink is untouchable — that is somebody else's subscription.
    {
        const std::vector<Registration> regs{ Registration{ 0x0001, 0x0002, 0x2100, false } };
        CHECK(stripPlan(regs).empty());
    }

    // 4) A mixed table: only the sinks we own are rewritten, and the indices are the caller's.
    {
        const std::vector<Registration> regs{
            Registration{ 0x0001, 0x0002, 0x2100, false }, // mouse, someone else's window
            Registration{ 0x0001, 0x0004, 0x0000, true  }, // joystick, no sink at all
            Registration{ 0x0001, 0x0005, 0x2100, true  }, // SDL's gamepad hot-plug window: ours
            Registration{ 0x000C, 0x0001, 0x0130, true  }, // consumer control, sink + NOLEGACY, ours
        };
        const std::vector<Rewrite> plan = stripPlan(regs);
        CHECK(plan.size() == 2);
        if (plan.size() == 2)
        {
            CHECK(plan[0].index == 2);
            CHECK(plan[0].flags == 0x2000);
            CHECK(plan[1].index == 3);
            CHECK(plan[1].flags == 0x2030); // NOLEGACY survives the rewrite, hot-plug is asserted onto it
        }
    }

    // 5) A rewrite is never a removal, and never lands on flags that mean "no subscription at all": the
    //    thinnest possible input still comes out as hot-plug only.
    {
        const std::vector<Registration> regs{ Registration{ 0x0001, 0x0005, RawInputSink::kInputSink, true } };
        const std::vector<Rewrite> plan = stripPlan(regs);
        CHECK(plan.size() == 1);
        if (plan.size() == 1)
        {
            CHECK(plan[0].flags == RawInputSink::kDevNotify);
            CHECK((plan[0].flags & RawInputSink::kRemove) == 0);
        }
    }

    // 6) Nothing in any plan ever asks for RIDEV_REMOVE, including from an entry that somehow carries it.
    {
        const std::vector<Registration> regs{
            Registration{ 0x0001, 0x0005, 0x2100, true },
            Registration{ 0x0001, 0x0006, RawInputSink::kRemove | RawInputSink::kInputSink, true },
        };
        for (const Rewrite& w : stripPlan(regs)) CHECK((w.flags & RawInputSink::kRemove) == 0);
        // ...and every rewrite the planner does emit carries hot-plug and no sink, whatever it was handed.
        for (const Rewrite& w : stripPlan(regs))
        {
            CHECK((w.flags & RawInputSink::kInputSink) == 0);
            CHECK((w.flags & RawInputSink::kDevNotify) != 0);
        }
    }

    // 7) Degenerate input: an empty table plans nothing.
    CHECK(stripPlan({}).empty());

    if (failures == 0) std::printf("RAWINPUTSINK-OK\n");
    return failures == 0 ? 0 : 1;
}
