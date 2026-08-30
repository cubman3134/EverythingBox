// Headless check of PlayerBarNav (src/ui/PlayerBarNav.h) — the two-state key contract for the player's
// transport BARS (the seek slider and the volume slider), and the clamped arrow-step arithmetic.
//
// The bars have two states. SELECTED: focused but inert, arrows walk the transport ring past them. ADJUSTING:
// arrows move the bar's value. This pins the whole table, because the states differ on only some keys and it
// is exactly the shared keys that make a bar feel like a trap when they are wrong:
//
//   * Left/Right are ring movement while Selected and value steps while Adjusting;
//   * Enter goes IN from Selected and comes OUT from Adjusting;
//   * Back comes out of Adjusting but is NOT ours while merely Selected — there it stays the player's unified
//     Back (stop + return home), which is what it does on every other transport control;
//   * Up/Down ALWAYS belong to the bar, in BOTH states. This is the non-obvious one: a QSlider treats Up/Down
//     as value steps and accepts them, so a key the bar declined would silently move a bar the user only meant
//     to step off. Same reason PageUp/PageDown/Home/End are swallowed rather than declined.
//
// Prints PLAYERBAR-OK on success; any failure prints PLAYERBAR-FAIL <cond> and exits non-zero.
#include "PlayerBarNav.h"

#include <Qt>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PLAYERBAR-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    using eb::BarAct;
    const bool sel = false;   // Selected: focused, inert
    const bool adj = true;    // Adjusting: arrows move the value

    // 1. Left/Right: ring movement while Selected, value steps while Adjusting.
    CHECK(eb::barKey(Qt::Key_Left,  sel) == BarAct::FocusPrev);
    CHECK(eb::barKey(Qt::Key_Right, sel) == BarAct::FocusNext);
    CHECK(eb::barKey(Qt::Key_Left,  adj) == BarAct::StepDown);
    CHECK(eb::barKey(Qt::Key_Right, adj) == BarAct::StepUp);

    // 2. Enter/Return/Select: in from Selected, out from Adjusting. All three spellings, because a remote
    //    and a pad both arrive as Key_Select while a keyboard sends Return or Enter.
    for (int k : { Qt::Key_Return, Qt::Key_Enter, Qt::Key_Select })
    {
        CHECK(eb::barKey(k, sel) == BarAct::Enter);
        CHECK(eb::barKey(k, adj) == BarAct::Leave);
    }

    // 3. Back (all three spellings): out of Adjusting; not ours while Selected, where it must stay the
    //    player's unified Back. A bar that swallowed Back while merely focused would strand the user on the
    //    transport row with no way off the player at all.
    for (int k : { Qt::Key_Escape, Qt::Key_Backspace, Qt::Key_Back })
    {
        CHECK(eb::barKey(k, adj) == BarAct::Leave);
        CHECK(eb::barKey(k, sel) == BarAct::NotOurs);
    }

    // 4. Up/Down belong to the bar in BOTH states — see the header comment. Up goes to the ‹ Back overlay,
    //    Down re-lands on the transport row, and either one leaves Adjusting on the way.
    CHECK(eb::barKey(Qt::Key_Up,   sel) == BarAct::LeaveToBack);
    CHECK(eb::barKey(Qt::Key_Up,   adj) == BarAct::LeaveToBack);
    CHECK(eb::barKey(Qt::Key_Down, sel) == BarAct::LeaveToRow);
    CHECK(eb::barKey(Qt::Key_Down, adj) == BarAct::LeaveToRow);

    // 5. QSlider's OTHER value keys are swallowed in both states, never declined: declining them hands the
    //    slider a key it would act on, which is the same silent-value-change bug as Up/Down above.
    for (int k : { Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End })
    {
        CHECK(eb::barKey(k, sel) == BarAct::Consume);
        CHECK(eb::barKey(k, adj) == BarAct::Consume);
    }

    // 6. Anything else falls through untouched, in both states (F12 screenshot, M queue menu, S skip, [ and ]
    //    speed — every player shortcut has to keep working while a bar holds focus).
    for (int k : { Qt::Key_Space, Qt::Key_F12, Qt::Key_M, Qt::Key_S, Qt::Key_I,
                   Qt::Key_BracketLeft, Qt::Key_BracketRight, Qt::Key_Menu, Qt::Key_A })
    {
        CHECK(eb::barKey(k, sel) == BarAct::NotOurs);
        CHECK(eb::barKey(k, adj) == BarAct::NotOurs);
    }

    // 7. Step arithmetic clamps at both ends of both ranges and never overshoots.
    CHECK(eb::barStep(100, +1, eb::kVolumeStep, 0, 200) == 105);
    CHECK(eb::barStep(100, -1, eb::kVolumeStep, 0, 200) == 95);
    CHECK(eb::barStep(198, +1, eb::kVolumeStep, 0, 200) == 200);   // clamps, does not wrap to 203
    CHECK(eb::barStep(2,   -1, eb::kVolumeStep, 0, 200) == 0);     // clamps, does not go negative
    CHECK(eb::barStep(200, +1, eb::kVolumeStep, 0, 200) == 200);   // already at the top: idempotent
    CHECK(eb::barStep(0,   -1, eb::kVolumeStep, 0, 200) == 0);     // already at the bottom: idempotent
    CHECK(eb::barStep(500, +1, eb::kSeekStep, 0, 1000) == 510);
    CHECK(eb::barStep(500, -1, eb::kSeekStep, 0, 1000) == 490);
    CHECK(eb::barStep(995, +1, eb::kSeekStep, 0, 1000) == 1000);
    CHECK(eb::barStep(5,   -1, eb::kSeekStep, 0, 1000) == 0);

    // 8. The step sizes themselves are the design's numbers, not whatever happened to be typed: 40 presses
    //    across the volume range, 100 across the seek range (1% of duration per press).
    CHECK(eb::kVolumeStep == 5);
    CHECK(eb::kSeekStep == 10);
    CHECK(200 / eb::kVolumeStep == 40);   // 0..200 in half-steps of the 0..100 scale
    CHECK(1000 / eb::kSeekStep == 100);

    if (failures) { std::fprintf(stderr, "PLAYERBAR-FAIL %d check(s)\n", failures); return 1; }
    std::printf("PLAYERBAR-OK\n");
    return 0;
}
