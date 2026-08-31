// Headless check of the pure save-state slot logic (src/emu/StateSlots.h) — issue #93.
//
// The correctness RAIL this pins: the reserved save-on-exit slot is NEVER one of the numbered user slots and
// the auto-increment "next free slot" search NEVER selects it, so no manual save is ever clobbered by a
// save-on-exit. Plus the ROM-mtime invalidation that stops a resume restoring a state that belongs to a
// different dump, and the grid pagination arithmetic.
//
// StateSlots.h is header-only, QtCore-free and does no I/O, so this probe needs no QCoreApplication and no
// scratch dir. Every expected value below is hand-computed (an INDEPENDENT oracle), never produced by calling
// the function under test — a fixture derived from the code proves nothing.
//
// Prints STATESLOTS-OK on success; any failure prints STATESLOTS-FAIL <cond> (line) and exits non-zero.
#include "StateSlots.h"

#include <cstdio>
#include <cstdint>
#include <set>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "STATESLOTS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Occupancy predicate over an explicit set of occupied user slots — the shape RetroView passes
// (QFile::exists(statePath(s))), reduced to a fixture the probe fully controls.
struct Occ {
    std::set<int> full;
    bool operator()(int s) const { return full.count(s) != 0; }
};

int main()
{
    using namespace StateSlots;

    // ---- 1. Reserved-slot isolation: the rail. Over a wide range of slot counts, the reserved sentinel is
    //         a reserved slot and is NEVER classified as a user slot. This is the collision the whole feature
    //         exists to prevent. ---------------------------------------------------------------------------
    CHECK(isReservedSlot(kReservedAutoSlot));
    for (int n = 1; n <= 200; ++n)
    {
        CHECK(!isUserSlot(kReservedAutoSlot, n));  // reserved is never a user slot, for any ceiling
        CHECK(isUserSlot(1, n));                   // slot 1 always is
        CHECK(isUserSlot(n, n));                   // the top slot always is
        CHECK(!isUserSlot(0, n));                  // 0 is neither user nor reserved
        CHECK(!isUserSlot(n + 1, n));              // one past the top is out
    }
    // A concrete user slot is not the reserved slot.
    for (int s = 1; s <= 50; ++s) CHECK(!isReservedSlot(s));

    // ---- 2. nextFreeUserSlot: lowest free slot, hand-computed oracle. ----------------------------------
    const int N = 6;
    // Empty -> slot 1.
    CHECK(nextFreeUserSlot(N, Occ{ {} }, /*current*/3) == 1);
    // {1,2,3} occupied -> 4.
    CHECK(nextFreeUserSlot(N, Occ{ {1,2,3} }, 3) == 4);
    // {1,3} occupied -> 2 (LOWEST free, not next-after-current).
    CHECK(nextFreeUserSlot(N, Occ{ {1,3} }, 3) == 2);
    // {2,3,4,5,6} occupied, only 1 free -> 1.
    CHECK(nextFreeUserSlot(N, Occ{ {2,3,4,5,6} }, 4) == 1);
    // {1,2,4,5,6} -> the single hole at 3.
    CHECK(nextFreeUserSlot(N, Occ{ {1,2,4,5,6} }, 6) == 3);
    // All full -> fall back to current (5), which is a valid user slot.
    CHECK(nextFreeUserSlot(N, Occ{ {1,2,3,4,5,6} }, 5) == 5);
    // All full with an out-of-range current -> clamped INTO range, never the reserved sentinel.
    CHECK(nextFreeUserSlot(N, Occ{ {1,2,3,4,5,6} }, kReservedAutoSlot) == 1);
    CHECK(nextFreeUserSlot(N, Occ{ {1,2,3,4,5,6} }, 999) == N);
    CHECK(nextFreeUserSlot(N, Occ{ {1,2,3,4,5,6} }, 0) == 1);

    // ---- 3. nextFreeUserSlot never returns the reserved slot and always returns a user slot, over EVERY
    //         occupancy subset of a small board (exhaustive) plus a range of currents. -------------------
    for (int mask = 0; mask < (1 << N); ++mask)
    {
        std::set<int> full;
        for (int b = 0; b < N; ++b) if (mask & (1 << b)) full.insert(b + 1);
        for (int cur = -3; cur <= N + 3; ++cur)
        {
            const int got = nextFreeUserSlot(N, Occ{ full }, cur);
            CHECK(!isReservedSlot(got));            // THE rail: the reserved slot is never chosen
            CHECK(isUserSlot(got, N));              // always a real, writable user slot
            if ((int)full.size() < N)               // some slot is free
            {
                CHECK(!full.count(got));            // and we picked a FREE one
                // it is the lowest free one
                int lowest = 0; for (int s = 1; s <= N; ++s) if (!full.count(s)) { lowest = s; break; }
                CHECK(got == lowest);
            }
        }
    }

    // ---- 4. autoStateValid: mtime + size gate (independent oracle: equal-and-positive). ----------------
    CHECK(autoStateValid(/*sM*/1000, /*sS*/2048, /*cM*/1000, /*cS*/2048));  // exact match resumes
    CHECK(!autoStateValid(1000, 2048, 1001, 2048));                        // ROM re-touched (mtime moved)
    CHECK(!autoStateValid(1000, 2048, 1000, 4096));                        // different dump (size changed)
    CHECK(!autoStateValid(1000, 2048, 1001, 4096));                        // both changed
    CHECK(!autoStateValid(0, 0, 0, 0));                                    // no sidecar (size 0) never validates
    CHECK(!autoStateValid(1000, 0, 1000, 0));                              // zero size, matching mtime -> refuse
    CHECK(autoStateValid(0, 512, 0, 512));                                 // mtime 0 is fine if size matches

    // ---- 5. Pagination arithmetic (hand-computed). 50 slots, 10 per page -> 5 pages. -------------------
    CHECK(pageCount(50, 10) == 5);
    CHECK(pageCount(6, 10) == 1);      // fewer slots than a page
    CHECK(pageCount(11, 10) == 2);     // one over -> a second page
    CHECK(pageCount(20, 10) == 2);
    CHECK(pageCount(21, 10) == 3);
    CHECK(pageCount(1, 10) == 1);
    CHECK(pageCount(0, 10) == 1);      // degenerate, never zero pages
    CHECK(pageCount(50, 0) == 1);      // degenerate perPage

    CHECK(firstSlotOnPage(0, 10) == 1);
    CHECK(firstSlotOnPage(1, 10) == 11);
    CHECK(firstSlotOnPage(4, 10) == 41);
    CHECK(lastSlotOnPage(0, 10, 50) == 10);
    CHECK(lastSlotOnPage(4, 10, 50) == 50);
    CHECK(lastSlotOnPage(1, 10, 15) == 15);   // partial last page clamps to the ceiling
    CHECK(lastSlotOnPage(0, 10, 6) == 6);

    // clampPage keeps the visible page in range as the ceiling changes.
    CHECK(clampPage(-1, 50, 10) == 0);
    CHECK(clampPage(99, 50, 10) == 4);        // past the end -> last page
    CHECK(clampPage(2, 50, 10) == 2);
    CHECK(clampPage(3, 20, 10) == 1);         // was on page 3, board shrank to 2 pages

    // Every page's [first..last] range stays within the user-slot band and never names the reserved slot.
    for (int perPage : { 5, 10 })
        for (int total : { 6, 20, 21, 50 })
            for (int p = 0; p < pageCount(total, perPage); ++p)
            {
                const int f = firstSlotOnPage(p, perPage);
                const int l = lastSlotOnPage(p, perPage, total);
                CHECK(isUserSlot(f, total));
                CHECK(isUserSlot(l, total));
                CHECK(f <= l);
                CHECK(!isReservedSlot(f) && !isReservedSlot(l));
            }

    // ---- Save-on-exit overwrite policy. A teardown must not replace a real resume point with the boot
    // screen of a session that never went anywhere: that is how "Resume where you left off" came to land
    // the player at the beginning (a launch whose prompt was declined, exited seconds later, wrote a
    // power-on state over a mid-game one, and the next launch still offered to resume it).
    // kMin below is 30 s at 60 fps; every expected value is hand-reasoned from the rule, never produced
    // by calling the function under test.
    const int64_t kMin = 1800;

    // Nothing on disk: always write. There is no resume point to lose, however short the session.
    CHECK(shouldWriteAutoState(false, false, 0, kMin));
    CHECK(shouldWriteAutoState(false, false, 1, kMin));
    CHECK(shouldWriteAutoState(false, true, 0, kMin));

    // Resumed from the auto-state: always write. Wherever the player got to is a successor of the point
    // being replaced, so even a few seconds of play is progress and must be kept.
    CHECK(shouldWriteAutoState(true, true, 0, kMin));
    CHECK(shouldWriteAutoState(true, true, 5, kMin));

    // Started from boot over an existing resume point: write only once the session has actually run.
    CHECK(!shouldWriteAutoState(true, false, 0, kMin));       // launched and backed straight out
    CHECK(!shouldWriteAutoState(true, false, 180, kMin));     // ~3 s: the reproduced data-loss case
    CHECK(!shouldWriteAutoState(true, false, kMin - 1, kMin));
    CHECK(shouldWriteAutoState(true, false, kMin, kMin));     // at the threshold this run is the new point
    CHECK(shouldWriteAutoState(true, false, kMin + 1, kMin));
    CHECK(shouldWriteAutoState(true, false, 100000, kMin));   // a long fresh run always supersedes

    // A nonsensical threshold can never turn the rule into "never write".
    CHECK(shouldWriteAutoState(true, false, 0, 0));
    CHECK(shouldWriteAutoState(true, false, 0, -5));

    if (failures == 0) { std::printf("STATESLOTS-OK\n"); return 0; }
    std::fprintf(stderr, "STATESLOTS: %d check(s) failed\n", failures);
    return 1;
}
