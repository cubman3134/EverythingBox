// Pure, testable slot-selection + auto-state-validity logic for RetroView's save states (issue #93).
//
// This header carries the load-bearing CORRECTNESS RAIL of the feature and nothing else: the rule that the
// reserved save-on-exit slot is never one of the numbered user slots, and the auto-increment "next free
// slot" search never selects it. Keeping it here — header-only, QtCore-free, with no I/O — lets a headless
// probe (probe_stateslots) assert the rail over fixtures that are independent of RetroView, which owns the
// filesystem side (paths, QSaveFile, thumbnails) that a probe cannot reach.
#pragma once
#include <cstdint>

namespace StateSlots {

// The reserved auto-slot (save-on-exit / resume). Deliberately a value OUTSIDE the user-slot range
// [1..userSlotCount], so it can never be offered as a save/load target in the user grid and the
// auto-increment search below can never select it. It is a logical sentinel, not a filesystem slot number:
// the reserved state lives at its own path (RetroView::autoStatePath(), "<base>.state.auto"), never at a
// numbered "<base>.stateN". RetroArch's exact discipline — the auto-state and the user slots never collide.
constexpr int kReservedAutoSlot = -1;

inline bool isReservedSlot(int slot) { return slot == kReservedAutoSlot; }

// A user slot is a numbered grid slot 1..userSlotCount. The reserved slot is never one of them (it is < 1),
// which is the property the probe pins for every slot count.
inline bool isUserSlot(int slot, int userSlotCount)
{
    return slot >= 1 && slot <= userSlotCount;
}

// Auto-increment quick-save target: the LOWEST FREE user slot (1..userSlotCount) per the `occupied`
// predicate, turning quick-saves into a history instead of overwriting one slot. If every user slot is
// occupied, fall back to `current` (overwrite the current slot rather than silently drop the save). The
// result is ALWAYS a user slot in [1..userSlotCount] and NEVER the reserved slot: the reserved slot is not
// in the search range, and `current` is clamped into range so a stale/uninitialised current can't leak it.
template <typename OccupiedPred>
inline int nextFreeUserSlot(int userSlotCount, OccupiedPred occupied, int current)
{
    for (int s = 1; s <= userSlotCount; ++s)
        if (!occupied(s)) return s;
    if (current < 1) return 1;                       // clamp: never return the reserved sentinel
    if (current > userSlotCount) return userSlotCount;
    return current;
}

// The auto-state (save-on-exit) belongs to a specific ROM DUMP. It is valid to resume only when the ROM
// file on disk is still the one it was written from — same rule HashVerify uses for its stamp: mtime + size.
// A swapped-in different dump, or an edited ROM, changes one of them and the stale auto-state is refused, so
// a resume can never restore a state that belongs to a different game. `stored*` come from the sidecar
// written at save-on-exit; `current*` are read from the ROM on relaunch. A zero/absent size (no sidecar)
// never validates.
inline bool autoStateValid(int64_t storedMtime, int64_t storedSize,
                           int64_t currentMtime, int64_t currentSize)
{
    return storedSize > 0
        && storedMtime == currentMtime
        && storedSize  == currentSize;
}

// ---- Grid pagination. Slots 1..userSlotCount are shown `perPage` at a time. ----

inline int pageCount(int userSlotCount, int perPage)
{
    if (perPage < 1) return 1;
    if (userSlotCount < 1) return 1;
    return (userSlotCount + perPage - 1) / perPage;  // ceil-divide
}

// Clamp a (0-based) page index into [0, pageCount-1].
inline int clampPage(int page, int userSlotCount, int perPage)
{
    const int last = pageCount(userSlotCount, perPage) - 1;
    if (page < 0) return 0;
    if (page > last) return last;
    return page;
}

// First user slot (1-based, inclusive) shown on page `page` (0-based).
inline int firstSlotOnPage(int page, int perPage) { return page * perPage + 1; }

// Last user slot (1-based, inclusive) shown on page `page` (0-based), clamped to userSlotCount.
inline int lastSlotOnPage(int page, int perPage, int userSlotCount)
{
    const int last = (page + 1) * perPage;
    return last < userSlotCount ? last : userSlotCount;
}

} // namespace StateSlots
