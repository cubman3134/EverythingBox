// Headless check of the pure cheat-search engine (src/emu/CheatSearch.h) — issue #96.
//
// The correctness this pins: reading a width-w value out of a raw RAM buffer respects width, signedness,
// endianness and — critically — BOUNDS (an addr whose read would run off the end yields none, never a
// wild read); the initial exact-value scan finds exactly the planted addresses and no others; each of the
// four relational filters (increased / decreased / unchanged / changed) narrows a candidate set correctly
// given two snapshots; and successive steps INTERSECT — a dropped address can never come back.
//
// CheatSearch.h is header-only, QtCore-free and does no I/O, so this probe needs no QCoreApplication. Every
// expected value below is HAND-COMPUTED from the byte fixtures (an INDEPENDENT oracle) — never produced by
// calling the search itself. A fixture that is a fixed point of the code under test proves nothing.
//
// Prints CHEATSEARCH-OK on success; any failure prints CHEATSEARCH-FAIL <cond> (line) and exits non-zero.
#include "CheatSearch.h"

#include <cstdio>
#include <cstdint>
#include <vector>

using namespace cheatsearch;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CHEATSEARCH-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// std::optional<int64_t> that holds `want`?  (kept terse for the many readValue assertions)
static bool isv(const std::optional<std::int64_t>& o, std::int64_t want) { return o.has_value() && *o == want; }

int main()
{
    // ---- 1. readValue: width, endianness, signedness, bounds. Fixture bytes are explicit; every expected
    //         number is computed by hand from them. --------------------------------------------------------
    // ram = 01 02 03 04 FF   (5 bytes)
    const std::uint8_t ram[] = { 0x01, 0x02, 0x03, 0x04, 0xFF };
    const std::size_t N = sizeof(ram);

    // 8-bit
    CHECK(isv(readValue(ram, N, 0, Width::W8, false), 0x01));        // = 1
    CHECK(isv(readValue(ram, N, 4, Width::W8, false), 0xFF));        // = 255 unsigned
    CHECK(isv(readValue(ram, N, 4, Width::W8, true), -1));           // 0xFF signed 8-bit = -1
    CHECK(isv(readValue(ram, N, 0, Width::W8, true), 1));            // 0x01 signed = +1

    // 16-bit little-endian: addr0 = 0x0201 = 513
    CHECK(isv(readValue(ram, N, 0, Width::W16, false, /*LE*/true), 0x0201));  // 513
    // 16-bit big-endian at addr0 = 0x0102 = 258
    CHECK(isv(readValue(ram, N, 0, Width::W16, false, /*LE*/false), 0x0102)); // 258
    // 16-bit LE at addr3 = bytes 04 FF -> 0xFF04 = 65284 unsigned; signed = -252
    CHECK(isv(readValue(ram, N, 3, Width::W16, false, true), 0xFF04));        // 65284
    CHECK(isv(readValue(ram, N, 3, Width::W16, true,  true), -252));          // 0xFF04 as int16 = -252

    // 32-bit LE at addr0 = 0x04030201 = 67305985
    CHECK(isv(readValue(ram, N, 0, Width::W32, false, true), 0x04030201));    // 67305985
    // 32-bit BE at addr0 = 0x01020304 = 16909060
    CHECK(isv(readValue(ram, N, 0, Width::W32, false, false), 0x01020304));

    // Bounds: the rail. An address whose width-w read overruns the buffer yields NONE — never a wild read.
    CHECK(!readValue(ram, N, 5, Width::W8, false).has_value());   // addr == len, W8 -> off the end
    CHECK(!readValue(ram, N, 4, Width::W16, false).has_value());  // 4+2 > 5
    CHECK(!readValue(ram, N, 2, Width::W32, false).has_value());  // 2+4 > 5
    CHECK( readValue(ram, N, 1, Width::W32, false).has_value());  // 1+4 == 5, the exact fit, DOES read
    CHECK(!readValue(nullptr, 8, 0, Width::W8, false).has_value()); // null buffer -> none

    // ---- 2. Exact-value initial scan: plant a known 8-bit value at known offsets, scan the whole buffer,
    //         and assert the surviving set is EXACTLY those offsets. Every-byte-offset alignment. ----------
    // Plant 42 (0x2A) at offsets 2 and 7. Nowhere else holds 42.
    std::uint8_t snap[10] = { 0x00, 0x01, 0x2A, 0x03, 0x04, 0x05, 0x06, 0x2A, 0x08, 0x09 };
    {
        const std::vector<std::size_t> got = initialCandidates(snap, 10, Filter::ExactValue, Width::W8,
                                                               /*signed*/false, /*target*/42);
        const std::vector<std::size_t> want = { 2, 7 };
        CHECK(got == want);
    }
    // A target that appears nowhere -> empty set.
    {
        const std::vector<std::size_t> got = initialCandidates(snap, 10, Filter::ExactValue, Width::W8,
                                                               false, 0xEE);
        CHECK(got.empty());
    }

    // ---- 3. Width mismatch: a 16-bit search for 42 must NOT match the single byte 0x2A. At offset 2 the
    //         16-bit LE value is bytes 2A 03 = 0x032A = 810, not 42; nowhere holds the 16-bit value 42
    //         (which would need bytes 2A 00). So an 8-bit search finds {2,7} but a 16-bit search finds {}. --
    {
        const std::vector<std::size_t> got16 = initialCandidates(snap, 10, Filter::ExactValue, Width::W16,
                                                                 false, 42);
        CHECK(got16.empty());  // no aligned OR unaligned 16-bit 42 in the fixture
        // Now plant a real 16-bit 42 = 0x002A little-endian (2A 00) at offset 4, and confirm W16 finds it.
        std::uint8_t s16[8] = { 0, 0, 0, 0, 0x2A, 0x00, 0, 0 };
        const std::vector<std::size_t> hit = initialCandidates(s16, 8, Filter::ExactValue, Width::W16,
                                                              false, 42);
        const std::vector<std::size_t> want = { 4 };
        CHECK(hit == want);
    }

    // ---- 4. Relational filters over two snapshots. 8-bit, one byte per "variable" at offsets 0..3.
    //         prev -> cur chosen by hand so each address exercises a different relation:
    //           addr0: 10 -> 12   increased
    //           addr1: 20 -> 15   decreased
    //           addr2: 30 -> 30   unchanged
    //           addr3: 40 -> 55   increased (and changed)
    // ---------------------------------------------------------------------------------------------------- --
    const std::uint8_t prev[4] = { 10, 20, 30, 40 };
    const std::uint8_t cur[4]  = { 12, 15, 30, 55 };
    // Seed the universe with a relational initial scan (no prev yet): every in-bounds 8-bit addr = {0,1,2,3}.
    const std::vector<std::size_t> all = initialCandidates(prev, 4, Filter::Increased, Width::W8, false, 0);
    CHECK((all == std::vector<std::size_t>{ 0, 1, 2, 3 }));

    {   // Increased: addr0 and addr3
        const std::vector<std::size_t> got = narrow(all, prev, cur, 4, Filter::Increased, Width::W8, false, 0);
        CHECK((got == std::vector<std::size_t>{ 0, 3 }));
    }
    {   // Decreased: addr1 only
        const std::vector<std::size_t> got = narrow(all, prev, cur, 4, Filter::Decreased, Width::W8, false, 0);
        CHECK((got == std::vector<std::size_t>{ 1 }));
    }
    {   // Unchanged: addr2 only
        const std::vector<std::size_t> got = narrow(all, prev, cur, 4, Filter::Unchanged, Width::W8, false, 0);
        CHECK((got == std::vector<std::size_t>{ 2 }));
    }
    {   // Changed: everything except addr2
        const std::vector<std::size_t> got = narrow(all, prev, cur, 4, Filter::Changed, Width::W8, false, 0);
        CHECK((got == std::vector<std::size_t>{ 0, 1, 3 }));
    }

    // ---- 5. Successive steps INTERSECT — a dropped address never re-enters. Start from {0,1,2,3}; step A
    //         = Increased -> {0,3}; step B on THAT set = Decreased over a third snapshot where addr0 falls
    //         but addr3 rises. addr1 fell in this third step too, but it was ALREADY dropped, so it must NOT
    //         reappear. Result = {0}, not {0,1}. -----------------------------------------------------------
    {
        const std::vector<std::size_t> stepA = narrow(all, prev, cur, 4, Filter::Increased, Width::W8, false, 0);
        CHECK((stepA == std::vector<std::size_t>{ 0, 3 }));
        // third snapshot: addr0 12->9 (down), addr1 15->2 (down, but not in stepA), addr3 55->99 (up)
        const std::uint8_t cur2[4] = { 9, 2, 30, 99 };
        const std::vector<std::size_t> stepB = narrow(stepA, cur, cur2, 4, Filter::Decreased, Width::W8, false, 0);
        CHECK((stepB == std::vector<std::size_t>{ 0 }));            // only addr0; addr1 stayed dropped
        CHECK(stepB.size() == 1);
    }

    // ---- 6. Exact-value NARROW step also intersects and re-tests against the CURRENT snapshot. Candidates
    //         {2,7} (from part 2) — after a snapshot where offset 2 still holds 42 but offset 7 changed to
    //         99, an exact-42 narrow keeps only {2}. -------------------------------------------------------
    {
        std::uint8_t later[10] = { 0x00, 0x01, 0x2A, 0x03, 0x04, 0x05, 0x06, 99, 0x08, 0x09 };
        const std::vector<std::size_t> cands = { 2, 7 };
        const std::vector<std::size_t> got = narrow(cands, snap, later, 10, Filter::ExactValue, Width::W8, false, 42);
        CHECK((got == std::vector<std::size_t>{ 2 }));
    }

    // ---- 7. Signed relational: a value crossing zero. As UNSIGNED bytes 0xFF(255) -> 0x00(0) reads as a
    //         DECREASE; as SIGNED int8 -1 -> 0 reads as an INCREASE. Same bytes, signedness flips the verdict.
    {
        const std::uint8_t p[1] = { 0xFF };
        const std::uint8_t c[1] = { 0x00 };
        const std::vector<std::size_t> one = { 0 };
        // unsigned: 255 -> 0 is a decrease
        CHECK((narrow(one, p, c, 1, Filter::Decreased, Width::W8, /*unsigned*/false, 0) == std::vector<std::size_t>{ 0 }));
        CHECK( narrow(one, p, c, 1, Filter::Increased, Width::W8, false, 0).empty());
        // signed: -1 -> 0 is an increase
        CHECK((narrow(one, p, c, 1, Filter::Increased, Width::W8, /*signed*/true, 0) == std::vector<std::size_t>{ 0 }));
        CHECK( narrow(one, p, c, 1, Filter::Decreased, Width::W8, true, 0).empty());
    }

    if (failures == 0) { std::printf("CHEATSEARCH-OK\n"); return 0; }
    std::fprintf(stderr, "CHEATSEARCH: %d failure(s)\n", failures);
    return 1;
}
