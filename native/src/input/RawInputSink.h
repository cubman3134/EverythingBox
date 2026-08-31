#pragma once
// ----------------------------------------------------------------------------------------------------------
// Raw-input SINK removal — the pure half, so it can be pinned by a probe on any platform.
//
// SDL's Windows joystick backend asks the OS for hot-plug notifications by registering a raw-input device
// (usage page 1 / usage 5, "Gamepad") against a message-only window it creates on whichever thread called
// SDL_Init — for us, the input thread in Gamepad.cpp. It registers that with RIDEV_DEVNOTIFY *and*
// RIDEV_INPUTSINK. Only the first of those two is what it wants: DEVNOTIFY is what delivers
// WM_INPUT_DEVICE_CHANGE when a pad arrives or leaves. INPUTSINK additionally subscribes the window to the
// FULL raw-input REPORT STREAM of every gamepad on the machine, in the background, forever.
//
// Each of those reports is queued as a WM_INPUT message, and each queued WM_INPUT holds a USER object (a
// win32k "HIDDATA") until the message is retrieved and processed. The per-process ceiling on USER objects is
// a hard 10,000. So any window that carries the sink is a live grenade: as long as its messages are drained
// the count sits near zero, and the moment they are not, the process burns one handle per device report until
// it is out of them — and NOTHING fails at that moment. It fails whenever some later code asks for a USER
// object, which for Qt means QEventDispatcherWin32 failing to SetTimer or to create its internal window, so
// the app wedges somewhere else entirely (tearing an emulator down and rebuilding the UI behind it is a
// favourite) and Windows files it as AppHangB1: a hang, no crash dump, no faulting module.
//
// The app has no use for a single one of those messages — nothing here reads raw input, and SDL's RawInput
// joystick backend (the one that would) is off. So the fix is to take the sink away and keep the
// notification: re-register the same usage, against the same window, with RIDEV_INPUTSINK cleared. Hot-plug
// still works; the report stream stops at the kernel and can never queue again, whatever else stalls.
//
// This half decides WHICH registrations to rewrite and to WHAT. It refuses to touch two things: a
// registration whose target window belongs to another thread (that is somebody else's subscription — mpv also
// links raw input — and re-registering it would silently steal their delivery), and anything that would turn
// a rewrite into a de-registration. Clearing INPUTSINK never clears DEVNOTIFY, so the hot-plug half of what
// SDL asked for always survives.
//
// That last sentence is load-bearing and cost a round trip to find out. The flags handed to this planner come
// from GetRegisteredRawInputDevices(), and that call DOES NOT REPORT RIDEV_DEVNOTIFY: measured live against
// SDL 2.30.11, SDL registers usage 1/5 with 0x2100 and the query reads it back as 0x0100. So a planner that
// faithfully "preserves the other flags" writes 0x0000 and silently unsubscribes SDL from pad hot-plug — the
// first build of this fix did exactly that. DEVNOTIFY is therefore asserted, not preserved: every rewrite
// carries it, whether or not the query admitted to it. That is safe because a rewrite only ever happens for a
// window on our own thread, where the notification is the entire reason the registration exists.
// ----------------------------------------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RawInputSink
{
    // The RIDEV_* bits this planner reasons about. Spelled out here rather than taken from <winuser.h> so the
    // logic compiles — and is tested — on the Linux CI runner too; they are ABI constants, not our choice.
    constexpr std::uint32_t kRemove    = 0x00000001u; // RIDEV_REMOVE
    constexpr std::uint32_t kNoLegacy  = 0x00000030u; // RIDEV_NOLEGACY
    constexpr std::uint32_t kInputSink = 0x00000100u; // RIDEV_INPUTSINK
    constexpr std::uint32_t kDevNotify = 0x00002000u; // RIDEV_DEVNOTIFY

    // One entry as GetRegisteredRawInputDevices() reports it, plus the one fact the planner cannot work out
    // for itself: whether the target window belongs to the thread doing the stripping.
    struct Registration
    {
        std::uint16_t usagePage = 0;
        std::uint16_t usage     = 0;
        std::uint32_t flags     = 0;
        bool          ownedHere = false;
    };

    // "Re-register entry `index` with these flags." Indices are into the vector handed to stripPlan(), so the
    // caller re-uses the original RAWINPUTDEVICE (same usage, same hwndTarget) and changes only dwFlags.
    struct Rewrite
    {
        std::size_t   index = 0;
        std::uint32_t flags = 0;
    };

    inline std::vector<Rewrite> stripPlan(const std::vector<Registration>& regs)
    {
        std::vector<Rewrite> out;
        for (std::size_t i = 0; i < regs.size(); ++i)
        {
            const Registration& r = regs[i];
            if ((r.flags & kInputSink) == 0u) continue; // no sink -> nothing to do (so re-running is a no-op)
            if (!r.ownedHere)                 continue; // another thread's subscription: not ours to rewrite

            // Sink off, hot-plug ASSERTED (see the header comment: the query hides DEVNOTIFY), everything
            // else the query did report left as it was.
            const std::uint32_t flags = (r.flags & ~kInputSink) | kDevNotify;
            if ((flags & kRemove) != 0u) continue;      // a rewrite must never become a de-registration
            out.push_back(Rewrite{ i, flags });
        }
        return out;
    }
}
