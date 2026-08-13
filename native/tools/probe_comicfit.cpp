// Headless check of the two-up (double-page spread) fit-to-page math (src/comic/ComicView.h, the
// comicSpreadScale free function). PURE arithmetic — no widgets, no QApplication, no disk — so it links against
// nothing but the header. Prints COMICFIT-OK on success; any failure prints COMICFIT-FAIL <cond> (line) and
// exits non-zero.
//
// THE BUG IT PINS (manga spread cut off at the bottom): the two-up branch used to fit the spread to the
// viewport WIDTH only. Two portrait pages side by side form a wide-but-tall image; scaled to fill the width,
// its height overflowed the viewport and the bottom of the open book was clipped. comicSpreadScale returns the
// SMALLER of fit-to-width and fit-to-height, so the whole spread fits both dimensions.
//
// ORACLE IS INDEPENDENT OF THE CODE UNDER TEST: every expected value below is computed by hand (or from the
// raw ratios), NEVER by calling comicSpreadScale. The width-only value each "clamp" case rejects is written
// out explicitly, so a revert of the helper to width-only (the mutation) turns these assertions red.
#include "ComicView.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "COMICFIT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool approx(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main()
{
    // ---- Case (a): width-fit OVERFLOWS height — the clamp must bind to HEIGHT ------------------------------
    // Viewport 1280x800; two 700x1000 portrait pages + a 10px gap => totalW = 1410, commonH = 1000.
    //   fit-to-width  = 1280 / 1410 = 0.9078...  -> height would be 1000 * 0.9078 = 907.8 > 800  (CUT OFF)
    //   fit-to-height =  800 / 1000 = 0.8        -> height =            1000 * 0.8   = 800   <= 800 (fits)
    // The helper must return the smaller, 0.8. (Independent oracle: 800/1000 by hand.)
    {
        const double s = comicSpreadScale(1280, 800, 1410, 1000);
        CHECK(approx(s, 0.8));                                   // clamp binds to height, not width
        CHECK(s < 1280.0 / 1410.0 - 1e-9);                      // strictly smaller than the width-only scale
        CHECK(int(1000 * s) <= 800);                            // scaled spread height fits the viewport (no cutoff)
        // Tripwire against a width-only revert: that would return ~0.9078, failing all three above.
        CHECK(!approx(s, 1280.0 / 1410.0));                     // must NOT be the width-only value
    }

    // ---- Case (b): height is ABUNDANT — the clamp must bind to WIDTH (no over-clamping) --------------------
    // Viewport 900x2000; same 700x1000 pages => totalW = 1410, commonH = 1000.
    //   fit-to-width  = 900 / 1410 = 0.6383...   -> height = 1000 * 0.6383 = 638.3 <= 2000 (fits)
    //   fit-to-height = 2000 / 1000 = 2.0        (way more than needed)
    // The helper must return the width scale 900/1410, i.e. it does NOT shrink further than width demands.
    {
        const double s = comicSpreadScale(900, 2000, 1410, 1000);
        CHECK(approx(s, 900.0 / 1410.0));                       // width binds when height is plentiful
        CHECK(int(1000 * s) <= 2000);                           // fits height trivially
        // Tripwire against a height-only implementation: that would return 2.0 here.
        CHECK(!approx(s, 2000.0 / 1000.0));
    }

    // ---- Case (c): a square-ish viewport where the two cases meet -----------------------------------------
    // Viewport 1410x1000; totalW = 1410, commonH = 1000 => both ratios are exactly 1.0. The scaled spread is
    // then 1410x1000, filling the viewport exactly in both dimensions.
    {
        const double s = comicSpreadScale(1410, 1000, 1410, 1000);
        CHECK(approx(s, 1.0));
        CHECK(int(1000 * s) <= 1000);
    }

    // ---- General invariant across a spread of sizes, checked with a hand-rolled min oracle ----------------
    // The helper must equal min(vw/totalW, vh/commonH) for every case, and the scaled height must never exceed
    // the viewport. The oracle recomputes the min directly (not via the function under test), so a mutant that
    // drops either term is caught wherever that term is the binding one.
    struct Case { int vw, vh, totalW, commonH; };
    const Case cases[] = {
        {1920, 1080, 1610, 1200},   // wide desktop, tall pages -> height binds
        {1024,  768, 1410, 1000},   // 4:3 -> height binds
        { 800, 1400, 1410, 1000},   // narrow+tall window -> width binds
        {1600,  900, 1210,  900},   // pages exactly viewport height -> both ~equal
        {1280,  800, 1000, 1500},   // very tall pages -> height binds hard
    };
    for (const Case& c : cases)
    {
        const double oracle = std::min(double(c.vw) / double(c.totalW),
                                       double(c.vh) / double(c.commonH));
        const double s = comicSpreadScale(c.vw, c.vh, c.totalW, c.commonH);
        CHECK(approx(s, oracle));
        CHECK(int(c.commonH * s) <= c.vh);   // the whole spread fits the viewport height in every case
    }

    // ---- Degenerate inputs must not divide by zero (qMax(1, ...) floor) -----------------------------------
    {
        const double s = comicSpreadScale(1000, 1000, 0, 0);   // both dims clamped to a denominator of 1
        CHECK(approx(s, 1000.0));                              // min(1000/1, 1000/1)
    }

    if (failures == 0) std::puts("COMICFIT-OK");
    return failures == 0 ? 0 : 1;
}
