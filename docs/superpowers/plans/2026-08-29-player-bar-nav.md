# Player Bar Navigation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the TV/movie player's seek bar and volume bar reachable and adjustable with arrow keys and a controller — a focus selector lands on the bar, Enter goes in, arrows move the value, Enter or Back comes out and keeps what you set.

**Architecture:** The player's Left/Right focus ring (`playerButtons_`) is widened from `QVector<QPushButton*>` to a `QVector<QWidget*>` so the two sliders can join it. Because `QSlider::keyPressEvent` consumes arrows before they could reach `MainWindow::keyPressEvent`, the bars' key contract is claimed in `MainWindow::eventFilter` (the same technique the lyric panel and the subtitle panel already use). The decision table itself is a pure header-only component with its own headless probe. The seek bar's Adjusting state is implemented as `setSliderDown(true)`, which makes the keyboard gesture literally the existing mouse-drag gesture.

**Tech Stack:** C++17, Qt 6.8.3 Widgets, CMake (Visual Studio multi-config generator), the repo's headless probe suite.

## Global Constraints

- **No AI attribution in commits.** No `Co-Authored-By: Claude` trailer, no "Generated with Claude Code" line, no tool name anywhere in the message. This overrides any default or global instruction. See repo root `CLAUDE.md`.
- **Conventional commit prefixes** (`feat:`, `fix:`, `docs:`, `refactor:`) per `CONTRIBUTING.md`.
- **The working tree is shared with other sessions.** At the time of writing there are uncommitted changes in `native/src/input/Gamepad.cpp`, `native/src/input/Gamepad.h`, `native/src/theme2/ThemeEngine.cpp`, and a startup pad-logging timer in `native/src/ui/MainWindow.cpp` that belong to another session. **Every `git commit` in this plan MUST use an explicit pathspec** (`git commit -m "…" -- path1 path2`) so that other work is never swept in. Never run a bare `git commit -a`.
- **A repo hook bumps the version on every commit.** `native/CMakeLists.txt` (`project(... VERSION x.y.z ...)`) and `native/src/main.cpp` (`kAppVersion`) will appear in each commit with a patch bump. That is expected; do not revert it and do not fight it.
- **Byte-exact edits to three files.** `native/tools/run-headless-probes.sh` and `.github/workflows/ci.yml` are **CRLF** with very long single lines; `native/CMakeLists.txt` contains a lone CR. Edit these by matching a **mid-line** anchor string with the Edit tool. Never rewrite these files wholesale, never normalise their line endings, and never use a `$`-anchored `sed` expression on them (the `\r` sits between the text and the end of line, so `$` will not match what you expect).
- **Never run a target-less `cmake --build build`.** `native/CMakeLists.txt` declares 50+ probe harnesses; the default target builds all of them. Always name targets.
- **Build synchronously.** Run each build to completion in one command and read its result before continuing. Do not start a build and hand control back to wait for it.
- **`Q_ASSERT` is inert in this build** (`NDEBUG` + `QT_NO_DEBUG` in Release). Probes assert with their own `CHECK` macro that increments a failure counter, never with `Q_ASSERT`.

### Build and gate commands

The tree at `build/` is already configured (Qt at `C:/Qt/6.8.3/msvc2022_64`, mpv at `C:/mpv-dev`, generator "Visual Studio 18 2026"). If it is missing, configure it with:

```bash
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib"
```

Build a probe:

```bash
cmake --build build --config Release --target probe_playerbar
```

Build the app (do this after **every** task that touches `MainWindow.cpp`/`.h` — a probe-only build will not catch an app link or compile error):

```bash
cmake --build build --config Release --target everythingbox
```

Run the full headless suite:

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

The suite's own verdict is `build/headless-probes.verdict`, containing `VERDICT=PASS` or `VERDICT=FAIL`. **Read that file** — the script's exit status is not the suite's verdict:

```bash
cat build/headless-probes.verdict
```

---

## File Structure

| File | Responsibility |
| --- | --- |
| `native/src/ui/PlayerBarNav.h` | **New, header-only.** The pure decision table: what a key means to a transport bar in each of its two states, plus the clamped step arithmetic and the two step constants. No Qt widgets, no signals/slots, so no moc. |
| `native/tools/probe_playerbar.cpp` | **New.** Headless probe for `PlayerBarNav.h`. Prints `PLAYERBAR-OK`. |
| `native/CMakeLists.txt` | Registers the `probe_playerbar` target. |
| `native/tools/run-headless-probes.sh` | Runs `probe_playerbar` in the suite. |
| `.github/workflows/ci.yml` | Builds `probe_playerbar` in CI. |
| `native/src/ui/MainWindow.h` | Ring type widening, the new members and the three new method declarations. |
| `native/src/ui/MainWindow.cpp` | The ring membership, the event-filter hook, the two-state handler, the live-seek rate limiter, the four dependent call sites, the stylesheet, and the two new UI-test state fields. |

`PlayerBarNav.h` is **header-only on purpose**. Adding a new `.cpp` to a probe's source list without also adding it to `qt_add_executable(everythingbox …)` is the issue-#182 trap: the suite prints `ALL HEADLESS PROBES PASSED` while the app fails to link, because nothing in the suite builds the app. A header cannot cause that.

---

## Task 1: The pure decision table and its probe

**Files:**
- Create: `native/src/ui/PlayerBarNav.h`
- Create: `native/tools/probe_playerbar.cpp`
- Modify: `native/CMakeLists.txt` (add the target next to `probe_bulkselect`, around line 2538)
- Modify: `native/tools/run-headless-probes.sh:676` (the `for p in …` loop)
- Modify: `.github/workflows/ci.yml:68` (the `--target` list)

**Interfaces:**
- Consumes: nothing.
- Produces, all in `namespace eb`, all `inline`:
  - `enum class BarAct { NotOurs, Consume, FocusPrev, FocusNext, Enter, Leave, LeaveToBack, LeaveToRow, StepDown, StepUp };`
  - `BarAct barKey(int key, bool adjusting)`
  - `int barStep(int cur, int delta, int step, int lo, int hi)`
  - `constexpr int kVolumeStep = 5;`
  - `constexpr int kSeekStep = 10;`

- [ ] **Step 1: Write the probe (the failing test)**

Create `native/tools/probe_playerbar.cpp`:

```cpp
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
```

- [ ] **Step 2: Register the probe in `native/CMakeLists.txt`**

Use the Edit tool. Match this exact existing block (it is at roughly line 2537):

```
    # Headless test for BulkSelect (the multi-select set + collision-safe reassign target path, issue #65).
    # Header-only unit; links nothing but Qt6::Core.
    add_executable(probe_bulkselect tools/probe_bulkselect.cpp
        src/core/BulkSelect.h)
```

Replace it with:

```
    # The player transport BARS' two-state key contract + arrow-step arithmetic (seek and volume sliders).
    # Header-only unit, so there is no .cpp to also add to the app's source list (the issue-#182 trap);
    # links nothing but Qt6::Core, which it needs only for the Qt::Key_* enum.
    add_executable(probe_playerbar tools/probe_playerbar.cpp
        src/ui/PlayerBarNav.h)
    target_include_directories(probe_playerbar PRIVATE src src/ui)
    target_link_libraries(probe_playerbar PRIVATE Qt6::Core)

    # Headless test for BulkSelect (the multi-select set + collision-safe reassign target path, issue #65).
    # Header-only unit; links nothing but Qt6::Core.
    add_executable(probe_bulkselect tools/probe_bulkselect.cpp
        src/core/BulkSelect.h)
```

- [ ] **Step 3: Register the probe in the suite runner**

`native/tools/run-headless-probes.sh` is CRLF with a single very long `for p in …` line at line 676. Use the Edit tool with this **mid-line** anchor so no line ending is touched.

Find: `"probe_riivolution RIIVOLUTION-OK"; do`

Replace with: `"probe_riivolution RIIVOLUTION-OK" "probe_playerbar PLAYERBAR-OK"; do`

- [ ] **Step 4: Register the probe in CI**

`.github/workflows/ci.yml` is CRLF with a long `--target` line at line 68. Use the Edit tool with this **mid-line** anchor.

Find: `probe_launchcontexts probe_riivolution`

Replace with: `probe_launchcontexts probe_riivolution probe_playerbar`

- [ ] **Step 5: Verify all three registrations landed, and that the shell script still parses**

A merge or a careless edit that eats a closer in this script makes the suite abort mid-run while `grep` still says the gate is present, so parse it explicitly:

```bash
cd "C:/Users/cubma/Project Goliath" && grep -c "probe_playerbar" native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml && bash -n native/tools/run-headless-probes.sh && echo "SCRIPT-PARSES"
```

Expected: a count of `1` or more for each of the three files (CMakeLists will report `3`), then `SCRIPT-PARSES`.

- [ ] **Step 6: Run the probe build to verify it fails**

```bash
cd "C:/Users/cubma/Project Goliath" && cmake --build build --config Release --target probe_playerbar
```

Expected: FAIL — `Cannot open include file: 'PlayerBarNav.h'`. The header does not exist yet.

- [ ] **Step 7: Write the header**

Create `native/src/ui/PlayerBarNav.h`:

```cpp
// PlayerBarNav — what a key means to one of the player's transport BARS (the seek slider and the volume
// slider), and how far one arrow press moves it.
//
// The bars sit in the same Left/Right ring as the transport buttons, but a slider is not a button: it has a
// value as well as a position in the row, so one set of arrows has to serve both. Hence two states.
//
//   SELECTED   focused, and inert. Left/Right walk the ring past the bar, exactly as they do across the
//              buttons, so a bar never obstructs getting to the other end of the row. Enter goes in.
//   ADJUSTING  Left/Right move the bar's VALUE. Enter and Back come back out to Selected.
//
// Two rules in the table below are not obvious and are the ones worth keeping:
//
//   * Up and Down belong to the bar in BOTH states. A QSlider handles Up/Down as value steps and ACCEPTS
//     them, so any key this table declines is a key the slider will silently act on — "step off the bar
//     upward" would have nudged the volume on the way out. The bar therefore claims them and performs the
//     player's own Up/Down (to the ‹ Back overlay, or back down onto the row), leaving Adjusting on the way.
//     PageUp/PageDown/Home/End are swallowed outright for the same reason, having no meaning here.
//
//   * Back is NOT ours while merely Selected. There it has to stay the player's unified Back (stop the media
//     and return home), which is what it does on every other control in the row; claiming it would leave a
//     user who has arrowed onto a bar with no way off the player at all.
//
// Deliberately free of Qt widgets, signals and slots — it needs only the Qt::Key_* enum — so it is
// header-only (no moc) and adds no translation unit that the app's own source list would have to repeat.
// Pinned headlessly by native/tools/probe_playerbar.cpp.
#pragma once
#include <Qt>

namespace eb
{

// What the bar does with a key. Anything but NotOurs is consumed by the bar; NotOurs falls through to the
// player's own handling (Space, F12, the queue menu, the speed keys, and — while Selected — Back).
enum class BarAct
{
    NotOurs,      // not the bar's key: let it through untouched
    Consume,      // a key the slider WOULD act on but the bar does not use: swallow it and do nothing
    FocusPrev,    // Selected + Left: step the transport ring backwards
    FocusNext,    // Selected + Right: step the transport ring forwards
    Enter,        // Selected + Enter: go to Adjusting
    Leave,        // Adjusting + Enter/Back: return to Selected
    LeaveToBack,  // Up: leave Adjusting (if in it) and focus the ‹ Back overlay
    LeaveToRow,   // Down: leave Adjusting (if in it) and re-land on the transport row
    StepDown,     // Adjusting + Left: one step down
    StepUp        // Adjusting + Right: one step up
};

inline BarAct barKey(int key, bool adjusting)
{
    switch (key)
    {
    case Qt::Key_Left:      return adjusting ? BarAct::StepDown : BarAct::FocusPrev;
    case Qt::Key_Right:     return adjusting ? BarAct::StepUp   : BarAct::FocusNext;

    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Select:    return adjusting ? BarAct::Leave : BarAct::Enter;

    // Out of Adjusting; the player's own Back while merely Selected (see the header comment).
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
    case Qt::Key_Back:      return adjusting ? BarAct::Leave : BarAct::NotOurs;

    // Always ours, in both states — a declined Up/Down is a value the slider moves behind the user's back.
    case Qt::Key_Up:        return BarAct::LeaveToBack;
    case Qt::Key_Down:      return BarAct::LeaveToRow;

    // Same reasoning, no meaning here: swallowed rather than declined.
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:       return BarAct::Consume;

    default:                return BarAct::NotOurs;
    }
}

// One arrow press on a bar, clamped to the bar's range. `delta` is -1 or +1. Clamping (rather than wrapping)
// is the point: holding a direction has to come to rest at an end, not roll a full-volume bar around to zero.
inline int barStep(int cur, int delta, int step, int lo, int hi)
{
    const int v = cur + delta * step;
    return v < lo ? lo : (v > hi ? hi : v);
}

// The volume bar is 0..200 (above 100 is software boost), so 5 is 40 presses end to end — 20 across the
// ordinary 0..100 range, with the boost half of the scale costing another 20.
inline constexpr int kVolumeStep = 5;
// The seek bar is 0..1000 permille of the media's duration, so 10 is 1% of duration per press — 100 presses
// end to end whatever the length. Proportional on purpose: the ⏪/⏩ buttons already own the precise 10-second
// jump, so the bar is the "get me roughly there" control and should cost the same effort on a 90-minute film
// and a six-hour audiobook.
inline constexpr int kSeekStep = 10;

} // namespace eb
```

- [ ] **Step 8: Build the probe and run it**

```bash
cd "C:/Users/cubma/Project Goliath" && cmake --build build --config Release --target probe_playerbar && ./build/Release/probe_playerbar.exe; echo "exit=$?"
```

Expected: the build succeeds, the probe prints `PLAYERBAR-OK`, and `exit=0`.

- [ ] **Step 9: Prove the probe can actually fail**

A probe that cannot fail gates nothing. Temporarily break one rule and confirm the probe catches it:

```bash
cd "C:/Users/cubma/Project Goliath" && sed -i 's/    case Qt::Key_Up:        return BarAct::LeaveToBack;/    case Qt::Key_Up:        return BarAct::NotOurs;/' native/src/ui/PlayerBarNav.h && cmake --build build --config Release --target probe_playerbar && ./build/Release/probe_playerbar.exe; echo "exit=$?"
```

Expected: `PLAYERBAR-FAIL eb::barKey(Qt::Key_Up, sel) == BarAct::LeaveToBack (line …)` on stderr and a non-zero `exit=`.

Now restore it:

```bash
cd "C:/Users/cubma/Project Goliath" && sed -i 's/    case Qt::Key_Up:        return BarAct::NotOurs;/    case Qt::Key_Up:        return BarAct::LeaveToBack;/' native/src/ui/PlayerBarNav.h && cmake --build build --config Release --target probe_playerbar && ./build/Release/probe_playerbar.exe; echo "exit=$?"
```

Expected: `PLAYERBAR-OK` and `exit=0`.

- [ ] **Step 10: Run the full headless suite**

```bash
cd "C:/Users/cubma/Project Goliath" && BUILD_DIR=build bash native/tools/run-headless-probes.sh > /dev/null 2>&1; cat build/headless-probes.verdict
```

Expected: `VERDICT=PASS`. If a probe binary is reported as not built, build it by name and re-run — do not run a target-less build.

- [ ] **Step 11: Confirm the suite actually ran the new probe**

Registering a target is not the same as running it; `probe_addon` was maintained for a long time while wired into neither the runner nor CI, so every assertion in it gated nothing.

```bash
cd "C:/Users/cubma/Project Goliath" && grep -c "PLAYERBAR-OK" build/headless-probes.log
```

Expected: `1` or more. A `0` means the runner edit did not take effect — go back to Step 3.

- [ ] **Step 12: Commit**

```bash
cd "C:/Users/cubma/Project Goliath" && git add native/src/ui/PlayerBarNav.h native/tools/probe_playerbar.cpp && git commit -m "feat: pure key contract and step arithmetic for the player's transport bars" -- native/src/ui/PlayerBarNav.h native/tools/probe_playerbar.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml native/src/main.cpp
```

Confirm the commit contains only those files plus the hook's version bump:

```bash
cd "C:/Users/cubma/Project Goliath" && git show --stat --oneline HEAD
```

Expected: `PlayerBarNav.h`, `probe_playerbar.cpp`, `native/CMakeLists.txt`, `run-headless-probes.sh`, `ci.yml`, `native/src/main.cpp`. If `Gamepad.cpp`, `Gamepad.h`, `ThemeEngine.cpp` or `MainWindow.cpp` appear, the pathspec was wrong — `git reset --soft HEAD~1` and redo the commit with the explicit pathspec.

---

## Task 2: The bars join the ring and answer keys

**Files:**
- Modify: `native/src/ui/MainWindow.h` — line 1577 (`playerButtons_`), line 1582 (`lastPlayerFocus_`), and the private-method block near line 515
- Modify: `native/src/ui/MainWindow.cpp` — lines ~1170–1202 (widget setup + ring), ~2550 region (`eventFilter`), ~3461 (UI-test state), ~3980 (`stepPlayerFocus`), ~4127 (`applyFormFactorWidgets`), ~4251 (`hideMediaControls`), ~22685 and ~22692 (skip chip)

**Interfaces:**
- Consumes from Task 1: `eb::BarAct`, `eb::barKey(int, bool)`, `eb::barStep(int, int, int, int, int)`, `eb::kVolumeStep`, `eb::kSeekStep`.
- Produces, as private members of `MainWindow`:
  - `QVector<QWidget*> playerRing_` (renamed from `playerButtons_`)
  - `QPointer<QWidget> lastPlayerFocus_` (type widened)
  - `QPointer<QSlider> adjustingBar_`
  - `QTimer* liveSeekTimer_`, `QElapsedTimer liveSeekClock_`
  - `bool handlePlayerSliderKey(QSlider* bar, int key)`
  - `void setBarAdjusting(QSlider* bar, bool on)`
  - `void liveSeek()`
- Produces, in the UI-test state JSON for the player page: `"playerFocus"` (a string — `"seekBar"`, `"volumeBar"`, or `""`) and `"barAdjusting"` (a string — `"seekBar"`, `"volumeBar"`, or `""`).

- [ ] **Step 1: Add the includes and the new members to `MainWindow.h`**

Confirm `<QElapsedTimer>` is included; add it if not. Check first:

```bash
cd "C:/Users/cubma/Project Goliath" && grep -n "#include <QElapsedTimer>\|#include <QPointer>\|#include <QSlider>" native/src/ui/MainWindow.h
```

If `<QElapsedTimer>` is absent, add it beside the other Qt includes at the top of `MainWindow.h`.

Then, in `MainWindow.h`, replace this block (line 1577 onward):

```cpp
    QVector<QPushButton*> playerButtons_; // transport buttons in Left/Right arrow-nav order
    // Where the transport cursor was when the chrome auto-hid. hideMediaControls() has to clear focus (a
    // hidden button must not hold it), which otherwise made the next arrow press re-enter at an END of the
    // row — you were on the volume and came back to skip-back. Restored on the next entry, so a bar that
    // hides under you and comes straight back feels continuous rather than reset.
    QPointer<QPushButton> lastPlayerFocus_;
```

with:

```cpp
    // Transport controls in Left/Right arrow-nav order. QWidget, not QPushButton: the seek and volume BARS
    // are members too (see PlayerBarNav.h for the two-state contract that lets a slider share the arrows).
    QVector<QWidget*> playerRing_;
    // Where the transport cursor was when the chrome auto-hid. hideMediaControls() has to clear focus (a
    // hidden button must not hold it), which otherwise made the next arrow press re-enter at an END of the
    // row — you were on the volume and came back to skip-back. Restored on the next entry, so a bar that
    // hides under you and comes straight back feels continuous rather than reset.
    QPointer<QWidget> lastPlayerFocus_;
    // The bar currently in its Adjusting state (arrows move its VALUE), or null. Only ever seek_ or volume_.
    QPointer<QSlider> adjustingBar_;
    // Live-seek rate limit while the seek bar is being arrowed — see liveSeek().
    QTimer* liveSeekTimer_ = nullptr;
    QElapsedTimer liveSeekClock_;
```

- [ ] **Step 2: Declare the three new methods in `MainWindow.h`**

Find this line (around line 515):

```cpp
    void stepPlayerFocus(int dir); // arrow-key focus across the transport buttons (dir +1/-1, or 0 = enter row)
```

Replace with:

```cpp
    void stepPlayerFocus(int dir); // arrow-key focus across the transport controls (dir +1/-1, or 0 = enter row)
    // The transport BARS' two-state key contract (PlayerBarNav.h). Returns true when the key was claimed.
    // Called from eventFilter, not keyPressEvent — see the call site for why.
    bool handlePlayerSliderKey(QSlider* bar, int key);
    void setBarAdjusting(QSlider* bar, bool on); // enter/leave a bar's Adjusting state
    void liveSeek();                             // rate-limited seek while the seek bar is being arrowed
```

- [ ] **Step 3: Include the new header in `MainWindow.cpp`**

Find the line that includes `SeekSlider.h` (near the other local `ui/` includes) and add the new header next to it:

```bash
cd "C:/Users/cubma/Project Goliath" && grep -n '#include "SeekSlider.h"\|#include "ui/SeekSlider.h"' native/src/ui/MainWindow.cpp
```

Add `#include "PlayerBarNav.h"` immediately after that include, matching the surrounding quoting style exactly.

- [ ] **Step 4: Give the bars object names, an explicit focus policy, and the event filter**

In `MainWindow.cpp`, find:

```cpp
    seek_ = new SeekSlider(Qt::Horizontal, mediaControls_);
    seek_->setRange(0, 1000);
    seek_->setCursor(Qt::PointingHandCursor);
    seek_->setToolTip(tr("Click or drag the bar to jump to that point"));
```

Replace with:

```cpp
    seek_ = new SeekSlider(Qt::Horizontal, mediaControls_);
    seek_->setObjectName(QStringLiteral("seekBar"));
    seek_->setRange(0, 1000);
    seek_->setCursor(Qt::PointingHandCursor);
    seek_->setToolTip(tr("Click or drag the bar to jump to that point, or arrow onto it and press Enter"));
    // Both bars are arrow-reachable ring members now. Stated rather than inherited: the default comes from a
    // style hint, and a bar that cannot take focus would silently drop out of the ring on some styles.
    seek_->setFocusPolicy(Qt::StrongFocus);
    // The bars' keys are claimed in eventFilter, because a focused QSlider consumes the arrows itself — see
    // the filter's own comment. Nothing here works without this line.
    seek_->installEventFilter(this);
```

Then find:

```cpp
    volume_ = new QSlider(Qt::Horizontal, mediaControls_);
    volume_->setRange(0, 200); // 0..200%: above 100% is software amplification ("boost"), VLC-style
    volume_->setFixedWidth(120);
    volume_->setToolTip(tr("Volume"));
```

Replace with:

```cpp
    volume_ = new QSlider(Qt::Horizontal, mediaControls_);
    volume_->setObjectName(QStringLiteral("volumeBar"));
    volume_->setRange(0, 200); // 0..200%: above 100% is software amplification ("boost"), VLC-style
    volume_->setFixedWidth(120);
    volume_->setToolTip(tr("Volume"));
    volume_->setFocusPolicy(Qt::StrongFocus);   // see seek_ above
    volume_->installEventFilter(this);
```

- [ ] **Step 5: Put the bars in the ring**

Find:

```cpp
    // Order for Left/Right arrow navigation across the transport (chapter buttons skipped while hidden).
    // (skipChip_ joins and leaves this ring with its own visibility — see showSkipChip/hideSkipChip.)
    playerButtons_ = { prevChap, rewind, playPause, fastFwd, nextChap, stop, muteBtn_, speedBtn_, subsBtn,
                       moreBtn };
```

Replace with:

```cpp
    // Order for Left/Right arrow navigation across the transport (chapter buttons skipped while hidden).
    // (skipChip_ joins and leaves this ring with its own visibility — see showSkipChip/hideSkipChip.)
    // The two BARS sit where the layout puts them — seek_ after stop, volume_ after the speaker — because the
    // ring's order is the row's visual order, and an arrow that skipped a control it passed over would read
    // as the bar being broken rather than as the ring being clever.
    playerRing_ = { prevChap, rewind, playPause, fastFwd, nextChap, stop, seek_, muteBtn_, volume_,
                    speedBtn_, subsBtn, moreBtn };
```

- [ ] **Step 6: Create the live-seek timer**

Find:

```cpp
    connect(seek_, &QSlider::sliderPressed, this, [this] { sliderDown_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, &MainWindow::onSeekReleased);
```

Replace with:

```cpp
    connect(seek_, &QSlider::sliderPressed, this, [this] { sliderDown_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, &MainWindow::onSeekReleased);
    // The trailing half of the live-seek rate limit (see liveSeek): whatever position the last arrow press
    // left the handle on gets seeked to, even when that press fell inside the quiet window.
    liveSeekTimer_ = new QTimer(this);
    liveSeekTimer_->setSingleShot(true);
    connect(liveSeekTimer_, &QTimer::timeout, this, [this] {
        if (duration_ <= 0.0) return;
        liveSeekClock_.restart();
        player_->setPosition(seek_->sliderPosition() / 1000.0 * duration_);
    });
```

- [ ] **Step 7: Widen `stepPlayerFocus`**

Find:

```cpp
// Move keyboard focus across the visible transport buttons (dir +1/-1), or land on the row (dir 0).
void MainWindow::stepPlayerFocus(int dir)
{
    QVector<QPushButton*> vis;
    for (QPushButton* b : playerButtons_) if (b && b->isVisible()) vis.push_back(b);
```

Replace with:

```cpp
// Move keyboard focus across the visible transport controls (dir +1/-1), or land on the row (dir 0).
void MainWindow::stepPlayerFocus(int dir)
{
    QVector<QWidget*> vis;
    for (QWidget* b : playerRing_) if (b && b->isVisible()) vis.push_back(b);
```

Then, further down in the same function, find:

```cpp
    int idx = vis.indexOf(qobject_cast<QPushButton*>(focusWidget()));
```

Replace with:

```cpp
    int idx = vis.indexOf(focusWidget());
```

- [ ] **Step 8: Write the two-state handler**

Add these three functions immediately **after** `MainWindow::stepPlayerFocus` (i.e. after its closing brace, before `MainWindow::showEvent`):

```cpp
// Enter or leave a transport bar's Adjusting state.
//
// For the SEEK bar this is the whole trick: setSliderDown drives the slider through exactly the states a
// mouse drag drives it through. Down emits sliderPressed -> sliderDown_, which is what makes onPosition stand
// off both the handle and the time readout while the user aims; up emits sliderReleased -> onSeekReleased,
// which clears the latch and commits the final position. So the keyboard/controller gesture IS the mouse
// gesture, with no second path to keep in step with the first.
void MainWindow::setBarAdjusting(QSlider* bar, bool on)
{
    if (!bar) return;
    // Only one bar can be in hand at a time; arriving at one while another is still latched leaves that one
    // with its slider held down forever, which silently kills the seek readout for the rest of playback.
    if (on && adjustingBar_ && adjustingBar_ != bar) setBarAdjusting(adjustingBar_, false);

    if (on) adjustingBar_ = bar;
    else if (adjustingBar_ == bar) adjustingBar_ = nullptr;

    if (bar == seek_)
    {
        // Stop any pending trailing seek BEFORE releasing: onSeekReleased is about to commit the exact
        // position, and a shot still in flight would afterwards drag playback back to where the bar was left.
        if (!on && liveSeekTimer_) liveSeekTimer_->stop();
        seek_->setSliderDown(on);
    }
    // Re-run the stylesheet for the [adjusting="true"] rule — a dynamic property does not restyle on its own.
    bar->setProperty("adjusting", on);
    bar->style()->unpolish(bar);
    bar->style()->polish(bar);
    bar->update();
}

// Seek while the seek bar is being arrowed. Deliberately NOT wired into the sliderMoved handler, which a
// MOUSE drag also runs: the mouse keeps its commit-on-release behaviour, and only the key path seeks live.
//
// A held direction repeats every 160 ms (pollMenuPad's hold-repeat), and on a network stream a seek per
// repeat is a re-buffer per repeat. So: seek at once, then at most once per 250 ms, with a trailing shot so
// the position the user actually came to rest on always lands.
void MainWindow::liveSeek()
{
    if (duration_ <= 0.0) return;
    if (!liveSeekClock_.isValid() || liveSeekClock_.elapsed() >= 250)
    {
        liveSeekClock_.restart();
        player_->setPosition(seek_->sliderPosition() / 1000.0 * duration_);
        return;
    }
    liveSeekTimer_->start(250 - int(liveSeekClock_.elapsed()));
}

// The transport bars' two-state key contract (the table lives in PlayerBarNav.h). Returns true when the key
// was claimed and must not travel any further.
bool MainWindow::handlePlayerSliderKey(QSlider* bar, int key)
{
    if (!bar || !stack_ || stack_->currentWidget() != playerPage_) return false;
    const eb::BarAct act = eb::barKey(key, adjustingBar_ == bar);
    if (act == eb::BarAct::NotOurs) return false;

    revealMediaControls();   // every key the bar claims is activity: the chrome must not fade mid-adjust

    switch (act)
    {
    case eb::BarAct::Consume:     return true;                             // swallowed on purpose
    case eb::BarAct::FocusPrev:   stepPlayerFocus(-1); return true;
    case eb::BarAct::FocusNext:   stepPlayerFocus(+1); return true;
    case eb::BarAct::Enter:       setBarAdjusting(bar, true);  return true;
    case eb::BarAct::Leave:       setBarAdjusting(bar, false); return true;
    case eb::BarAct::LeaveToBack:
        setBarAdjusting(bar, false);
        if (videoBack_) videoBack_->setFocus(Qt::TabFocusReason);          // the player's own Up
        return true;
    case eb::BarAct::LeaveToRow:
        setBarAdjusting(bar, false);
        stepPlayerFocus(0);                                                // the player's own Down
        return true;
    case eb::BarAct::StepDown:
    case eb::BarAct::StepUp:
    {
        const int delta = (act == eb::BarAct::StepUp) ? +1 : -1;
        if (bar == volume_)
        {
            // setValue is enough: the valueChanged handler applies it to mpv, unmutes if it was muted,
            // redraws the speaker glyph and persists player/volume — which is why backing out keeps it.
            volume_->setValue(eb::barStep(volume_->value(), delta, eb::kVolumeStep, 0, 200));
        }
        else
        {
            // setSliderPosition, not setValue: with the slider held down this is the same move a drag makes,
            // so the existing sliderMoved handler repaints the preview time for free.
            seek_->setSliderPosition(eb::barStep(seek_->sliderPosition(), delta, eb::kSeekStep, 0, 1000));
            liveSeek();
        }
        return true;
    }
    case eb::BarAct::NotOurs:     break;   // returned above
    }
    return false;
}
```

- [ ] **Step 9: Hook the handler into `eventFilter`**

In `MainWindow::eventFilter`, find this existing block:

```cpp
    // The classic queue list's edit gesture (issue #193). Claimed here, before the list's own type-ahead
    // search eats the letter — the same reason the lyric panel's Left/Esc is claimed above.
    if (obj == playlist_ && event->type() == QEvent::KeyPress)
    {
        const int k = static_cast<QKeyEvent*>(event)->key();
        if (k == Qt::Key_M || k == Qt::Key_Menu) { showQueueMenu(); return true; }
    }
```

Insert immediately **after** it:

```cpp
    // The transport BARS' two-state arrow contract (the seek and volume sliders). Claimed HERE and not in the
    // player's key switch for the same reason the subtitle panel's buttons are claimed below: a focused
    // QSlider handles Left/Right/Up/Down itself and ACCEPTS them, so the key never propagates to
    // MainWindow::keyPressEvent. Written in keyPressEvent, this whole feature would be unreachable code —
    // and worse than unreachable, because the slider would be quietly moving its own value instead.
    if ((obj == seek_ || obj == volume_) && event->type() == QEvent::KeyPress
        && handlePlayerSliderKey(static_cast<QSlider*>(obj), static_cast<QKeyEvent*>(event)->key()))
        return true;
```

- [ ] **Step 10: Fix the three call sites that assumed the ring held buttons**

**(a)** In `applyFormFactorWidgets`, find:

```cpp
    const QSize floorSz = hit > 0 ? QSize(hit, hit) : QSize(0, 0);
    for (QPushButton* b : playerButtons_) if (b) b->setMinimumSize(floorSz);
```

Replace with:

```cpp
    const QSize floorSz = hit > 0 ? QSize(hit, hit) : QSize(0, 0);
    // Only the ring's BUTTONS take the square floor. A bar must not be squared: volume_ is a fixed 120px wide
    // and seek_ is the row's stretch item, so a 44x44 minimum would either fight the fixed width or blow the
    // bar's height out. The bars get a minimum HEIGHT instead — see the seek_ line below, and volume_'s twin.
    for (QWidget* w : playerRing_) if (auto* b = qobject_cast<QPushButton*>(w)) b->setMinimumSize(floorSz);
```

Then find, a few lines below:

```cpp
    if (seek_) seek_->setMinimumHeight(hit); // desktop: 0 (no change); mobile: a grabbable track
```

Replace with:

```cpp
    if (seek_) seek_->setMinimumHeight(hit);   // desktop: 0 (no change); mobile: a grabbable track
    if (volume_) volume_->setMinimumHeight(hit); // same treatment; it is a ring member and a touch target too
```

**(b)** In `hideMediaControls`, find:

```cpp
        // Remember the transport button first — clearing focus is what loses the place (see lastPlayerFocus_).
        if (auto* b = qobject_cast<QPushButton*>(fw); b && playerButtons_.contains(b)) lastPlayerFocus_ = b;
        fw->clearFocus();
```

Replace with:

```cpp
        // Leave any bar's Adjusting state BEFORE clearing focus. A seek bar left latched down would stop
        // onPosition updating the handle and the time readout for the rest of playback — a four-second walk
        // away mid-adjust would look like the transport had died.
        if (adjustingBar_) setBarAdjusting(adjustingBar_, false);
        // Remember the transport control first — clearing focus is what loses the place (see lastPlayerFocus_).
        if (playerRing_.contains(fw)) lastPlayerFocus_ = fw;
        fw->clearFocus();
```

**(c)** In `showSkipChip` / `hideSkipChip`, find and replace these two lines:

```cpp
    if (!playerButtons_.contains(skipChip_)) playerButtons_.push_back(skipChip_);
```
becomes
```cpp
    if (!playerRing_.contains(skipChip_)) playerRing_.push_back(skipChip_);
```

and

```cpp
    playerButtons_.removeAll(skipChip_);   // out of the ring the moment it stops being reachable
```
becomes
```cpp
    playerRing_.removeAll(skipChip_);   // out of the ring the moment it stops being reachable
```

- [ ] **Step 11: Expose the new state to the UI-test channel**

In the player-page branch of the UI-test state JSON, find:

```cpp
            o.insert(QStringLiteral("volume"), volume_ ? volume_->value() : 0);
```

Replace with:

```cpp
            o.insert(QStringLiteral("volume"), volume_ ? volume_->value() : 0);
            // Bar navigation (arrow/controller reachability of the two sliders): which ring member holds
            // focus, and which bar — if any — is in its Adjusting state. Only the two BARS carry object
            // names, so a focused transport button reports "" here; that is enough to assert the thing this
            // exists for, which is that arrowing along the row lands ON a bar and that Enter goes into it.
            QWidget* pf = focusWidget();
            o.insert(QStringLiteral("playerFocus"),
                     (pf && playerRing_.contains(pf)) ? pf->objectName() : QString());
            o.insert(QStringLiteral("barAdjusting"),
                     adjustingBar_ ? adjustingBar_->objectName() : QString());
```

- [ ] **Step 12: Confirm no reference to the old name survives**

```bash
cd "C:/Users/cubma/Project Goliath" && grep -rn "playerButtons_" native/src native/tools; echo "hits=$?"
```

Expected: no output and `hits=1` (grep found nothing). Any hit is a site the rename missed.

- [ ] **Step 13: Build the app**

```bash
cd "C:/Users/cubma/Project Goliath" && cmake --build build --config Release --target everythingbox 2>&1 | tail -20
```

Expected: a successful link, ending in `EverythingBox.vcxproj -> …\EverythingBox.exe`. Fix any compile error before continuing; do not proceed on a partial build.

- [ ] **Step 14: Run the full headless suite**

```bash
cd "C:/Users/cubma/Project Goliath" && BUILD_DIR=build bash native/tools/run-headless-probes.sh > /dev/null 2>&1; cat build/headless-probes.verdict
```

Expected: `VERDICT=PASS`. `probe_nav` and `probe_uitest` are the two most likely to react to this change; if either fails, read `build/headless-probes.log` for the assertion.

- [ ] **Step 15: Commit**

```bash
cd "C:/Users/cubma/Project Goliath" && git commit -m "feat: make the player's seek and volume bars reachable by arrow keys and controller" -- native/src/ui/MainWindow.cpp native/src/ui/MainWindow.h native/CMakeLists.txt native/src/main.cpp
```

Then confirm the other session's files stayed out:

```bash
cd "C:/Users/cubma/Project Goliath" && git show --stat --oneline HEAD && git status --short
```

Expected: the commit lists only `MainWindow.cpp`, `MainWindow.h`, `native/CMakeLists.txt`, `native/src/main.cpp`. `git status` should still show `Gamepad.cpp`, `Gamepad.h` and `ThemeEngine.cpp` as modified — they must NOT have been committed.

---

## Task 3: The selector you can see

Without this task the feature works but is invisible: the two sliders are unstyled, so a focused one on the dark transport bar has essentially no focus indication and the Adjusting state looks identical to the Selected state.

**Files:**
- Modify: `native/src/ui/MainWindow.cpp:1102` (the `mediaControls_` stylesheet)

**Interfaces:**
- Consumes from Task 2: the `adjusting` dynamic property that `setBarAdjusting` writes to each bar.
- Produces: nothing other tasks depend on.

- [ ] **Step 1: Extend the transport stylesheet**

Find this block:

```cpp
    mediaControls_->setStyleSheet(QStringLiteral(
        "#mediaControls { background: rgba(20,20,24,0.85); border-radius: 10px; }"
        "#mediaControls QLabel { color: #e8e8e8; }"
        "#mediaControls QPushButton { background: transparent; color:#e8e8e8; border:none; border-radius:6px;"
        " min-width:34px; min-height:32px; padding:2px 6px; font-weight:bold; }"
        "#mediaControls QPushButton:hover { background: rgba(255,255,255,0.14); }"
        "#mediaControls QPushButton:pressed { background: rgba(255,255,255,0.22); }"
        "#mediaControls QPushButton:focus { background: rgba(90,140,255,0.80); border-radius:6px; }")); // arrowed-to
```

Replace with:

```cpp
    mediaControls_->setStyleSheet(QStringLiteral(
        "#mediaControls { background: rgba(20,20,24,0.85); border-radius: 10px; }"
        "#mediaControls QLabel { color: #e8e8e8; }"
        "#mediaControls QPushButton { background: transparent; color:#e8e8e8; border:none; border-radius:6px;"
        " min-width:34px; min-height:32px; padding:2px 6px; font-weight:bold; }"
        "#mediaControls QPushButton:hover { background: rgba(255,255,255,0.14); }"
        "#mediaControls QPushButton:pressed { background: rgba(255,255,255,0.22); }"
        "#mediaControls QPushButton:focus { background: rgba(90,140,255,0.80); border-radius:6px; }" // arrowed-to
        // The two BARS are arrow-reachable ring members, so they need the row's two focus states drawn on
        // them as well. Styling them at all means drawing the groove and handle by hand — a system-drawn
        // slider has no focus indication this row can use, which is the whole reason the bars looked dead to
        // a remote even once they were reachable. The transparent border in the BASE rule is load-bearing:
        // the two states below add a 2px border, and without a same-width transparent one here the bar would
        // change size the instant it took focus and shove the rest of the row sideways.
        "#mediaControls QSlider { border:2px solid transparent; border-radius:6px; padding:0px 2px; }"
        "#mediaControls QSlider::groove:horizontal { height:6px; background: rgba(255,255,255,0.22);"
        " border-radius:3px; }"
        "#mediaControls QSlider::sub-page:horizontal { background:#e8e8e8; border-radius:3px; }"
        "#mediaControls QSlider::handle:horizontal { background:#e8e8e8; width:12px; margin:-5px 0;"
        " border-radius:6px; }"
        // SELECTED: focused, inert. An outline only — the row's buttons fill on focus, but a filled BAR would
        // hide the very thing it is drawing (where its handle sits).
        "#mediaControls QSlider:focus { border:2px solid rgba(90,140,255,0.90); }"
        // ADJUSTING: arrows are moving the value. Filled in the row's focus blue, with a white handle, so the
        // two states cannot be confused at couch distance.
        "#mediaControls QSlider[adjusting=\"true\"] { background: rgba(90,140,255,0.80);"
        " border:2px solid #ffffff; }"
        "#mediaControls QSlider[adjusting=\"true\"]::handle:horizontal { background:#ffffff; width:16px;"
        " border-radius:8px; }"));
```

- [ ] **Step 2: Build the app**

```bash
cd "C:/Users/cubma/Project Goliath" && cmake --build build --config Release --target everythingbox 2>&1 | tail -20
```

Expected: a successful link. A stylesheet is a runtime string, so a mistake in it will not fail the build — Step 3 of Task 4 is what actually checks it.

- [ ] **Step 3: Commit**

```bash
cd "C:/Users/cubma/Project Goliath" && git commit -m "feat: draw the selected and adjusting states on the player's transport bars" -- native/src/ui/MainWindow.cpp native/CMakeLists.txt native/src/main.cpp
```

---

## Task 4: Driven verification on the real app

The probe suite cannot see focus, styling, or what a person experiences — every fault this project's rules are written from was green under the probes and found by someone driving the app. This task is that pass, and it is not optional.

**Files:**
- Create: `docs/superpowers/verification/2026-08-29-player-bar-nav.md`
- Screenshots: write them to the scratchpad directory, not the repo root.

**Interfaces:**
- Consumes from Task 2: the `playerFocus` and `barAdjusting` fields in the UI-test state JSON.
- Produces: nothing other tasks depend on.

- [ ] **Step 1: Deploy and launch the built app under the UI-test channel**

Per the project's deploy convention, deploy the **Release** binary (the debug DLLs are not present in the deploy directory). Close any running instance first, then launch with the test channel enabled:

```bash
cd "C:/Users/cubma/Project Goliath" && EB_UITEST=1 ./build/Release/EverythingBox.exe &
```

Wait for the channel:

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py status
```

Expected: `ok ready`.

- [ ] **Step 2: Open a video and reveal the transport chrome**

Open any local video file the machine has (use one from the user's library; if none is obvious, ask). Then:

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py key down && python native/tools/uitest.py state
```

Expected: the state JSON reports `"mediaControls": true`, a non-zero `"playerDur"`, and the new `"playerFocus"` and `"barAdjusting"` keys are present.

- [ ] **Step 3: Walk the ring onto the volume bar**

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py walk 12 right
```

Expected: across the twelve steps, `playerFocus` reads `"seekBar"` at one point and `"volumeBar"` at another. **This is the core assertion of the whole feature** — before this change neither string could ever appear, because neither bar could hold focus.

- [ ] **Step 4: Enter the volume bar, change it, and back out**

Stop on the volume bar (repeat `key right`/`key left` until `state` shows `"playerFocus": "volumeBar"`), record the starting volume, then:

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py state && python native/tools/uitest.py key enter && python native/tools/uitest.py state
```

Expected: `"barAdjusting"` flips from `""` to `"volumeBar"`, and `"volume"` is unchanged (entering must not move the value).

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py keys "left left left" && python native/tools/uitest.py state
```

Expected: `"volume"` is exactly 15 lower than it was, and `"barAdjusting"` is still `"volumeBar"`.

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py key back && python native/tools/uitest.py state
```

Expected: `"barAdjusting"` is `""`, `"playerFocus"` is still `"volumeBar"`, `"volume"` is **unchanged** by the back-out (this is "back out of it keeps your setting"), and playback is still running — Back must not have exited the player.

- [ ] **Step 5: Check the two states are visibly different**

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py shot "$SCRATCH/barnav-1-volume-selected.png"
```

(substituting the session scratchpad path for `$SCRATCH`), then `key enter`, then another shot to `barnav-2-volume-adjusting.png`. **Open both images and look at them.** Confirm the Selected shot shows an outline around the volume bar and the Adjusting shot shows it filled blue with a white handle. A stylesheet typo produces a bar that looks identical in both states and no command will report it.

- [ ] **Step 6: Repeat for the seek bar, including the live seek**

Walk to `"playerFocus": "seekBar"`, `key enter`, then:

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py state && python native/tools/uitest.py keys "right right right right right" && python native/tools/uitest.py state
```

Expected: `"playerPermille"` rises by 50 (five presses × 10), and the picture has actually moved — confirm with a screenshot that the frame on screen is not the frame from before the presses. Then `key back` and check `"barAdjusting"` is `""` and `"playerPermille"` stayed where it was left rather than snapping back.

- [ ] **Step 7: Check the idle-hide case**

This is the one that would strand the seek bar's latch. Enter the seek bar, then wait five seconds without pressing anything (the chrome hides after four):

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py state
```

Expected: `"mediaControls": false` and `"barAdjusting": ""`.

Then confirm the transport recovered rather than dying: press a key to bring the chrome back, and read `state` twice a couple of seconds apart.

```bash
cd "C:/Users/cubma/Project Goliath" && python native/tools/uitest.py key down && python native/tools/uitest.py state
```

Expected: `"playerPermille"` is **advancing between the two reads**. If it is frozen, `sliderDown_` was left latched and Task 2 Step 10(b) did not take effect.

- [ ] **Step 8: Check the ring is not a trap**

From a bar, confirm every exit works: `key up` reaches the ‹ Back overlay; `key down` returns to the row; `key left`/`key right` while merely Selected move `playerFocus` off the bar **without** changing `volume` or `playerPermille`. That last one is the silent-value-change bug the header warns about — verify it explicitly by reading `state` before and after.

- [ ] **Step 9: Check the setting survives a restart**

Set the volume to a distinctive value via the bar, back out, close the app, relaunch, open a video, and read `state`.

Expected: `"volume"` is the value that was set. (It is written to `player/volume` on every step by the existing `valueChanged` handler; this confirms the arrow path really goes through it.)

- [ ] **Step 10: Write the verification record**

Create `docs/superpowers/verification/2026-08-29-player-bar-nav.md` recording, for each step above, the command run and the actual observed values — not "passed". Note anything that behaved unexpectedly even if it was not a failure.

- [ ] **Step 11: Commit**

```bash
cd "C:/Users/cubma/Project Goliath" && git add docs/superpowers/verification/2026-08-29-player-bar-nav.md && git commit -m "docs: hardware verification pass for player bar navigation" -- docs/superpowers/verification/2026-08-29-player-bar-nav.md native/CMakeLists.txt native/src/main.cpp
```

---

## Self-Review

**Spec coverage.** Every section of `docs/superpowers/specs/2026-08-29-player-bar-nav-design.md` maps to a task: the two-state behaviour and the step table → Task 1 (the pure table) and Task 2 Step 8 (the handler); live rate-limited seeking → Task 2 Steps 6 and 8; the ring widening → Task 2 Steps 1, 5, 7; keys claimed in the event filter → Task 2 Step 9; seek reusing the mouse drag → Task 2 Step 8 (`setBarAdjusting`); the pure component and its three-place probe registration → Task 1 Steps 1–4; styling → Task 3; the four dependent call sites → Task 2 Step 10; the driven pass the probes cannot do → Task 4. The spec's "out of scope" list (`MediaPane`, the mouse drag, the ⏪/⏩ buttons) is respected — no task touches them.

**One addition beyond the spec:** the `playerFocus` and `barAdjusting` UI-test state fields (Task 2 Step 11). The spec calls for a driven verification pass but the existing state dump exposes no way to assert *which* control holds focus, so Task 4 would have had nothing to check but screenshots. Two fields, both read-only.

**Type consistency.** `playerRing_` is `QVector<QWidget*>` in the header and at all five use sites. `lastPlayerFocus_` is `QPointer<QWidget>` and is assigned a `QWidget*`. `adjustingBar_` is `QPointer<QSlider>`; `setBarAdjusting` and `handlePlayerSliderKey` both take `QSlider*`, and the `eventFilter` call site casts to `QSlider*` (valid — `seek_` is a `SeekSlider`, which derives from `QSlider`). `eb::barKey`, `eb::barStep`, `eb::kVolumeStep` and `eb::kSeekStep` are spelled identically in the header, the probe and the handler. `BarAct::Consume` is defined in Task 1 and handled in Task 2's switch; every other enumerator likewise appears in both.

**Known gap, deliberately left.** `Task 4 Step 2` needs a real video file, which this plan cannot name. The implementer should use one from the user's library and ask if none is obvious.
