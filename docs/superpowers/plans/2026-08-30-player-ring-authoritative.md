# Player ring authoritative — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `MainWindow::playerRing_` the only order that decides where an arrow key moves focus on the media player's transport row, so holding Left and holding Right traverse the same controls in opposite directions.

**Architecture:** A focused `QAbstractButton` consumes Left/Right/Up/Down by walking Qt's tab chain, so the ring's own `stepPlayerFocus` is never reached from a button and the tab chain (creation order) wins over the ring (visual order). The fix claims those four keys in `MainWindow::eventFilter` on every ring member — the pattern the subtitle panel and the two transport bars already use — routing them through a single pure key table, `eb::rowKey`, that sits beside the bars' `eb::barKey` in `PlayerBarNav.h`.

**Tech Stack:** C++17, Qt 6.8.3 (Widgets), MSVC / Visual Studio 18 2026, CMake. Headless probes are plain executables printing `<NAME>-OK`. Driven verification uses `EB_UITEST=1` plus `native/tools/uitest.py`.

**Spec:** `docs/superpowers/specs/2026-08-30-player-ring-authoritative-design.md`

## Global Constraints

- **Branch:** `fix/player-ring-authoritative`, already checked out in this worktree, based on `feat/player-bar-nav` (`a5bb2b6f`). Do not rebase it and do not touch any other worktree.
- **No AI attribution** in any commit message or body — no `Co-Authored-By`, no "Generated with", no tool name. Repo root `CLAUDE.md` overrides any global instruction that says otherwise.
- **Conventional commit prefixes** (`feat:`, `fix:`, `docs:`, `test:`) per `CONTRIBUTING.md`.
- **Every source file touched here is CRLF** (`native/src/ui/MainWindow.cpp`, `native/src/ui/MainWindow.h`, `native/src/ui/PlayerBarNav.h`, `native/tools/probe_playerbar.cpp`). Use the **Edit tool** for all of them. A `sed -i` / Python rewrite normalises the file to LF and the change looks fine while silently rewriting the whole file.
- **A `pre-commit` hook bumps the patch version** in `native/CMakeLists.txt` and `native/src/main.cpp` and adds them to your commit. That is expected. Do not revert it, and do not stage those two files yourself. If `git status` shows them staged *before* you commit, run `git reset HEAD -- native/CMakeLists.txt native/src/main.cpp` first.
- **Build (this worktree's `build/` is already configured):**
  ```
  cmake --build build --config Release --target <target> --parallel
  ```
  Only if a configure is needed:
  ```
  cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DEVERYTHINGBOX_BUILD_APP=ON \
    -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 \
    -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib
  ```
- **Run builds and the gate synchronously to completion.** Read the exit code, then grep the whole log. Never arm a monitor and hand back mid-build. If a build exceeds the tool timeout it moves to the background — wait for *that* notification.
- **The gate** (`CONTRIBUTING.md`): `BUILD_DIR=build bash native/tools/run-headless-probes.sh`, run from the repo root, must end `ALL HEADLESS PROBES PASSED`.
- **Never deploy over `C:\EverythingBox-app`.** The driven runs in Task 4 use the worktree build **in place**, on a private pipe name, so a normally-running instance is untouched.

## File Structure

| File | Responsibility | Change |
| --- | --- | --- |
| `native/src/ui/PlayerBarNav.h` | Pure key tables for the player's transport — no Qt widgets, header-only, no moc | Add `eb::RowAct` + `eb::rowKey` beside the existing `eb::BarAct` + `eb::barKey` |
| `native/tools/probe_playerbar.cpp` | Headless gate for that header | Add checks for `rowKey` |
| `native/src/ui/MainWindow.h` | Declarations | Add `handlePlayerRowKey` |
| `native/src/ui/MainWindow.cpp` | The row's widgets, the filter, the key handlers, the UI-test state | Object names, filter install loop, `eventFilter` clause, `handlePlayerRowKey`, `keyPressEvent` collapse, widened `playerFocus` gate |
| `docs/superpowers/verification/2026-08-30-player-ring-authoritative.md` | The driven evidence, before and after | Create |

---

### Task 1: The row's pure key table

`eb::rowKey` is what an arrow *means* on the row. It carries no Qt widgets, so the headless suite can pin it — which is the whole point: this repo builds no `MainWindow` headlessly and cannot see focus, so without this table the change would ship with no automated signal at all.

**Files:**
- Modify: `native/src/ui/PlayerBarNav.h` (append inside `namespace eb`, after `kSeekStep`)
- Test: `native/tools/probe_playerbar.cpp` (append sections 9 and 10, before the `if (failures)` line)

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class eb::RowAct { NotOurs, FocusPrev, FocusNext, FocusBack, FocusRow };` and `inline eb::RowAct eb::rowKey(int key);`, both in `native/src/ui/PlayerBarNav.h`. Task 3 calls `eb::rowKey`.

- [ ] **Step 1: Write the failing test**

Edit `native/tools/probe_playerbar.cpp`. Insert immediately **before** the final `if (failures)` line (currently after section 8's `CHECK(1000 / eb::kSeekStep == 100);` and the blank line that follows it):

```cpp
    // 9. The transport ROW's table — every ring member that is not a bar (the transport buttons and the skip
    //    chip), plus the ‹ Back overlay above the row. The four arrows are the RING's, so that playerRing_
    //    and not Qt's creation-order tab chain decides where Left and Right go. Unlike a bar there is no
    //    second state: a button has a position in the row and nothing else.
    CHECK(eb::rowKey(Qt::Key_Left)  == eb::RowAct::FocusPrev);
    CHECK(eb::rowKey(Qt::Key_Right) == eb::RowAct::FocusNext);
    CHECK(eb::rowKey(Qt::Key_Up)    == eb::RowAct::FocusBack);
    CHECK(eb::rowKey(Qt::Key_Down)  == eb::RowAct::FocusRow);

    // 10. Everything else is declined, and the three groups are declined for three different reasons:
    //     * Enter/Return/Select and Space are how a focused button is PRESSED. Claiming them would not make
    //       the row asymmetric, it would make it dead.
    //     * Back, in all three spellings, must stay the player's unified Back — exactly the rule barKey
    //       applies to a merely Selected bar, and for the same reason: a row that swallowed Back would leave
    //       the user with no way off the player.
    //     * The player's own shortcuts (F12, M, S, I, [ and ]) have to keep working while the row has focus.
    //       PageUp/PageDown/Home/End are declined rather than swallowed here, unlike on a bar: a QPushButton
    //       does not act on them, so there is no silent value change to defend against.
    for (int k : { Qt::Key_Return, Qt::Key_Enter, Qt::Key_Select, Qt::Key_Space,
                   Qt::Key_Escape, Qt::Key_Backspace, Qt::Key_Back,
                   Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End,
                   Qt::Key_F12, Qt::Key_M, Qt::Key_S, Qt::Key_I, Qt::Key_Menu,
                   Qt::Key_BracketLeft, Qt::Key_BracketRight })
        CHECK(eb::rowKey(k) == eb::RowAct::NotOurs);

```

- [ ] **Step 2: Run the build to verify it fails**

```bash
cmake --build build --config Release --target probe_playerbar --parallel
```

Expected: **compile error**, `'rowKey': is not a member of 'eb'` (and `'RowAct': is not a member of 'eb'`). If it compiles, the edit did not land — check you edited the probe and not the header.

- [ ] **Step 3: Write the minimal implementation**

Edit `native/src/ui/PlayerBarNav.h`. Append inside `namespace eb`, immediately after the `inline constexpr int kSeekStep = 10;` line and its comment, and before the closing `} // namespace eb`:

```cpp

// ---- The transport ROW ---------------------------------------------------------------------------------
//
// What an arrow means to a ring member that is NOT a bar — a transport button, or the skip chip — and to the
// ‹ Back overlay above the row.
//
// It exists for the same reason barKey does, and is claimed at the same kind of place: an event filter on the
// widget. A focused QAbstractButton handles Left/Right/Up/Down by walking Qt's TAB CHAIN and ACCEPTS them, so
// an arrow pressed on a button never reached MainWindow::keyPressEvent and playerRing_ decided nothing for it.
// The tab chain follows widget CREATION order while the ring follows the row's VISUAL order, and those two
// differ — so Left and Right traversed different SETS of controls, and ⏪ ▶ ⏩ were reachable by holding Left
// and never by holding Right. Routing every arrow through this table makes the ring the only order there is.
//
// Only the four arrows are ours. Enter and Space must reach the button — they are how it is pressed — and Back
// must stay the player's unified Back, the same rule barKey applies to a merely Selected bar and for the same
// reason: claiming it would strand a user on the row with no way off the player.
enum class RowAct
{
    NotOurs,     // not an arrow: let it through (Enter/Space press the button, Back leaves the player)
    FocusPrev,   // Left:  the previous VISIBLE ring member, wrapping
    FocusNext,   // Right: the next visible ring member, wrapping
    FocusBack,   // Up:    the ‹ Back overlay, which is not a ring member and is reachable no other way
    FocusRow     // Down:  re-land on the row
};

inline RowAct rowKey(int key)
{
    switch (key)
    {
    case Qt::Key_Left:  return RowAct::FocusPrev;
    case Qt::Key_Right: return RowAct::FocusNext;
    case Qt::Key_Up:    return RowAct::FocusBack;
    case Qt::Key_Down:  return RowAct::FocusRow;
    default:            return RowAct::NotOurs;
    }
}
```

- [ ] **Step 4: Extend the header's own file comment**

Still in `native/src/ui/PlayerBarNav.h`, the file opens `// PlayerBarNav — what a key means to one of the player's transport BARS (the seek slider and the volume`. Add one sentence to the end of that opening paragraph block, immediately before the line beginning `// The bars sit in the same Left/Right ring`:

```cpp
// The row's own arrow table (rowKey, at the bottom of this file) lives here too: same transport, same ring,
// and the two tables have to be read against each other, since Left and Right mean one thing on a bar and
// another on the button beside it.
```

- [ ] **Step 5: Run the probe to verify it passes**

```bash
cmake --build build --config Release --target probe_playerbar --parallel && ./build/Release/probe_playerbar.exe
```

Expected: build succeeds; the probe prints `PLAYERBAR-OK` and exits 0. Any `PLAYERBAR-FAIL <cond> (line N)` means the table disagrees with the checks — fix the table, not the checks.

- [ ] **Step 6: Commit**

```bash
git add native/src/ui/PlayerBarNav.h native/tools/probe_playerbar.cpp
git commit -m "feat: pure key table for the transport row's arrow keys"
```

---

### Task 2: Name the row's controls, and measure the bug

Instrumentation only — no behaviour change. Today `playerFocus` in the UI-test state reports `objectName()` gated on ring membership, and only the two bars carry names, so **every button reports `""`**: a driven pass cannot tell ▶ from ⚙ and therefore cannot verify Task 3 at all. This task makes the row legible and then records the *pre-fix* traversal, which is the baseline Task 4 measures against — and which settles whether §16's "9 visible members" or the code's 10 is right.

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (after the `playerRing_` assignment at `:1232`; and the `playerFocus` block at `:3550-3557`)
- Create: `docs/superpowers/verification/2026-08-30-player-ring-authoritative.md`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: object names `prevChapBtn`, `rewindBtn`, `playPauseBtn`, `fastFwdBtn`, `nextChapBtn`, `stopBtn`, `muteBtn`, `speedBtn`, `subsBtn`, `moreBtn` on the ring's buttons; `playerFocus` in the UI-test state additionally reports `videoBack`. Task 4 asserts on exactly these strings.

- [ ] **Step 1: Name the ring's buttons**

Edit `native/src/ui/MainWindow.cpp`. The ring assignment currently reads:

```cpp
    playerRing_ = { prevChap, rewind, playPause, fastFwd, nextChap, stop, seek_, muteBtn_, volume_,
                    speedBtn_, subsBtn, moreBtn };
```

Insert immediately **after** it:

```cpp
    // Stable names for the ring's buttons. They are what the UI-test state's `playerFocus` reports, and
    // without them a driven pass sees "" for every button and cannot tell ▶ from ⚙ — which is to say it
    // cannot check the row's traversal at all, the one thing about this row worth checking. The two bars and
    // the skip chip are already named (their #id stylesheet rules), which is also the constraint on these:
    // no name here may collide with an #id selector in a stylesheet, or it silently restyles the button.
    prevChap->setObjectName(QStringLiteral("prevChapBtn"));
    rewind->setObjectName(QStringLiteral("rewindBtn"));
    playPause->setObjectName(QStringLiteral("playPauseBtn"));
    fastFwd->setObjectName(QStringLiteral("fastFwdBtn"));
    nextChap->setObjectName(QStringLiteral("nextChapBtn"));
    stop->setObjectName(QStringLiteral("stopBtn"));
    muteBtn_->setObjectName(QStringLiteral("muteBtn"));
    speedBtn_->setObjectName(QStringLiteral("speedBtn"));
    subsBtn->setObjectName(QStringLiteral("subsBtn"));
    moreBtn->setObjectName(QStringLiteral("moreBtn"));
```

- [ ] **Step 2: Report the ‹ Back overlay in the UI-test state**

Still in `native/src/ui/MainWindow.cpp`, replace this block (currently at `:3550-3557`):

```cpp
            // Bar navigation (arrow/controller reachability of the two sliders): which ring member holds
            // focus, and which bar — if any — is in its Adjusting state. Only the two BARS carry object
            // names, so a focused transport button reports "" here; that is enough to assert the thing this
            // exists for, which is that arrowing along the row lands ON a bar and that Enter goes into it.
            QWidget* pf = focusWidget();
            o.insert(QStringLiteral("playerFocus"),
                     (pf && playerRing_.contains(pf)) ? pf->objectName() : QString());
```

with:

```cpp
            // Transport navigation: which ring member holds focus, and which bar — if any — is in its
            // Adjusting state. Every ring member carries an object name now, so the whole row is legible and
            // not just the two bars. The ‹ Back overlay is reported alongside them although it is NOT a ring
            // member, because "Up leaves the row for Back, and stepping along the row never lands there" is
            // exactly the property a driven pass has to be able to see.
            QWidget* pf = focusWidget();
            const bool onTransport = pf && (playerRing_.contains(pf) || pf == videoBack_);
            o.insert(QStringLiteral("playerFocus"), onTransport ? pf->objectName() : QString());
```

- [ ] **Step 3: Build the app**

```bash
cmake --build build --config Release --target EverythingBox --parallel
```

Expected: `Build succeeded`, exit 0. Grep the whole log for `error` before believing it.

- [ ] **Step 4: Launch the build in place, on a private pipe**

Never deploy this over `C:\EverythingBox-app`. From the repo root:

```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" EB_UITEST=1 EB_UITEST_PIPE=EB-ringnav \
  ./build/Release/EverythingBox.exe &
```

Then confirm it is up:

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py status
```

Expected: `ok ready`. (`ok starting` means wait and retry; a connection error means the app died — read its stderr.)

- [ ] **Step 5: Open the test media and land on the player**

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py open "C:/Users/cubma/Videos/2026-07-10 21-59-06.mp4"
```

Then:

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
```

Expected: the state JSON reports a non-zero `playerDur` (≈21.8 for this file — proof it really loaded, not just that a page opened). If the file is missing, substitute any local video and record which one in the doc.

- [ ] **Step 6: Record the pre-fix Right cycle**

Enter the row, then step right 14 times, printing `playerFocus` at each step:

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key down
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py walk 14 right
```

Record the `playerFocus` value after each press, in order. Expected (this is the **bug**, so a short cycle is the correct observation here): a repeating cycle much shorter than the ring, per §16 of `docs/superpowers/verification/2026-08-29-player-bar-nav.md` — around `seekBar → muteBtn → volumeBar → speedBtn → subsBtn → moreBtn → seekBar`. Write down what you actually see, not what is predicted.

- [ ] **Step 7: Record the pre-fix Left cycle**

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py walk 14 left
```

Record the sequence. Expected: a *different* set from Step 6, including `playPauseBtn`, `rewindBtn`, `fastFwdBtn` and `videoBack` — none of which Step 6 visited.

- [ ] **Step 8: Record what Down from ‹ Back does today**

With focus on `videoBack` (step left until `playerFocus` reads `videoBack`), press Down once and read the state:

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key down
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
```

Record the resulting `playerFocus`. This settles the open question in the spec: the comment at `MainWindow.cpp:4198` assumes this runs `stepPlayerFocus(0)` and lands on `lastPlayerFocus_`; if it instead lands on `rewindBtn` the tab chain is answering and that comment is wrong.

- [ ] **Step 9: Write the baseline into the verification doc**

Create `docs/superpowers/verification/2026-08-30-player-ring-authoritative.md` with a header naming the branch, the exact build and launch commands used, the media path and its `playerDur`, and a `## Baseline (before the fix)` section containing the three recorded sequences from Steps 6, 7 and 8 verbatim, plus the **count of distinct visible ring members** each direction reached. State explicitly whether that count is 9 or 10, resolving §16.

- [ ] **Step 10: Stop the app and commit**

Close the driven instance (its own window Back, or kill the process you launched — never touch the `C:\EverythingBox-app` instance).

```bash
git add native/src/ui/MainWindow.cpp docs/superpowers/verification/2026-08-30-player-ring-authoritative.md
git commit -m "test: name the transport row's controls so a driven pass can read them"
```

---

### Task 3: Claim the row's arrows in the event filter

The behaviour change. Three edits, all in `MainWindow.cpp` plus one declaration.

**Files:**
- Modify: `native/src/ui/MainWindow.h` (beside `handlePlayerSliderKey`, `:518`)
- Modify: `native/src/ui/MainWindow.cpp` (install loop after `:1232`; `eventFilter` clause after the bars' clause at `:2627`; new method after `handlePlayerSliderKey`; `keyPressEvent` arrow cases at `:4052-4055`)

**Interfaces:**
- Consumes: `eb::RowAct` and `eb::rowKey(int)` from Task 1; the object names from Task 2.
- Produces: `bool MainWindow::handlePlayerRowKey(int key);` — returns `true` when the key was claimed. Task 4 verifies its effect, not its signature.

- [ ] **Step 1: Declare the handler**

Edit `native/src/ui/MainWindow.h`. After the existing line:

```cpp
    bool handlePlayerSliderKey(QSlider* bar, int key);
```

add:

```cpp
    // The same job for the row's BUTTONS (and the ‹ Back overlay): arrows claimed before the button's own
    // tab-chain walk can eat them. Also the single definition keyPressEvent's arrow cases call.
    bool handlePlayerRowKey(int key);
```

- [ ] **Step 2: Install the filter on every ring member**

Edit `native/src/ui/MainWindow.cpp`. Immediately after the ten `setObjectName` lines added in Task 2 (i.e. still directly below the `playerRing_` assignment), insert:

```cpp
    // The ring's arrow keys are claimed in eventFilter, because a focused QAbstractButton handles
    // Left/Right/Up/Down by walking Qt's TAB CHAIN and ACCEPTS them — see the filter's own comment. Written
    // as a loop over the ring rather than as a list of button names, so a control ADDED to playerRing_ is
    // armed by the same line that puts it in the ring: there is no second place to remember, which is the
    // trap this fix exists to close. installEventFilter de-duplicates, so the members that already carry
    // this filter for other reasons (the two bars, the skip chip) are unaffected.
    for (QWidget* w : playerRing_) if (w) w->installEventFilter(this);
```

- [ ] **Step 3: Claim the keys in the filter**

Still in `native/src/ui/MainWindow.cpp`, find the bars' clause in `eventFilter`:

```cpp
    if ((obj == seek_ || obj == volume_) && event->type() == QEvent::KeyPress
        && handlePlayerSliderKey(static_cast<QSlider*>(obj), static_cast<QKeyEvent*>(event)->key()))
        return true;
```

Insert immediately **after** it:

```cpp
    // The transport ROW's arrows: every ring member that is not a bar (the buttons and the skip chip), plus
    // the ‹ Back overlay above the row. Claimed HERE for exactly the reason the bars' clause above and the
    // subtitle panel's below are: a focused QAbstractButton handles Left/Right/Up/Down by walking the tab
    // chain and ACCEPTS them, so the key never reached MainWindow::keyPressEvent and playerRing_ governed
    // nothing for a button. The tab chain runs in CREATION order and the ring in the row's VISUAL order, and
    // those differ — measured, holding Right cycled six controls and holding Left seven, over different
    // sets, so ⏪ ▶ ⏩ could be reached only by going left. The sliders are excluded because their own
    // two-state contract, handled above, already routes Left/Right here while Selected.
    if (event->type() == QEvent::KeyPress && !qobject_cast<QSlider*>(obj))
        if (auto* w = qobject_cast<QWidget*>(obj); w && (w == videoBack_ || playerRing_.contains(w))
            && handlePlayerRowKey(static_cast<QKeyEvent*>(event)->key()))
            return true;
```

- [ ] **Step 4: Write the handler**

Still in `native/src/ui/MainWindow.cpp`, insert immediately **after** the closing `}` of `MainWindow::handlePlayerSliderKey` (the function ending `    return false;\n}` after the `case eb::BarAct::NotOurs:` line):

```cpp

// The transport ROW's arrow contract (the table lives in PlayerBarNav.h) — the buttons' half of what
// handlePlayerSliderKey does for the bars. Returns true when the key was claimed and must not travel further.
//
// This is also the ONE definition of what an arrow means on the player page: keyPressEvent's arrow cases call
// it too, so the two ways a key can arrive — a filter on the focused control, and the page's own handler when
// nothing in the row holds focus — cannot come to disagree about where the key goes. That mattered: the
// disagreement they used to have IS this bug, in the form of Qt's tab chain answering instead of the ring.
bool MainWindow::handlePlayerRowKey(int key)
{
    if (!stack_ || stack_->currentWidget() != playerPage_) return false;
    const eb::RowAct act = eb::rowKey(key);
    if (act == eb::RowAct::NotOurs) return false;

    revealMediaControls();   // an arrow is activity: the chrome must not fade out mid-traversal

    switch (act)
    {
    case eb::RowAct::FocusPrev: stepPlayerFocus(-1); return true;
    case eb::RowAct::FocusNext: stepPlayerFocus(+1); return true;
    case eb::RowAct::FocusBack: if (videoBack_) videoBack_->setFocus(Qt::TabFocusReason); return true;
    case eb::RowAct::FocusRow:  stepPlayerFocus(0);  return true;
    case eb::RowAct::NotOurs:   break;   // returned above
    }
    return false;
}
```

- [ ] **Step 5: Collapse `keyPressEvent`'s arrow cases onto it**

Still in `native/src/ui/MainWindow.cpp`, replace these four lines:

```cpp
        case Qt::Key_Right: revealMediaControls(); stepPlayerFocus(+1); return;
        case Qt::Key_Left:  revealMediaControls(); stepPlayerFocus(-1); return;
        case Qt::Key_Up:    revealMediaControls(); if (videoBack_) videoBack_->setFocus(Qt::TabFocusReason); return;
        case Qt::Key_Down:  revealMediaControls(); stepPlayerFocus(0); return;
```

with:

```cpp
        // One arrow contract, two ways in. This path carries a key that reached the page because nothing in
        // the row holds focus; the filter carries the far more common one, pressed ON a focused control.
        case Qt::Key_Right: case Qt::Key_Left: case Qt::Key_Up: case Qt::Key_Down:
            handlePlayerRowKey(e->key());
            return;
```

- [ ] **Step 6: Build the app and the probe**

```bash
cmake --build build --config Release --target EverythingBox probe_playerbar --parallel
```

Expected: `Build succeeded`, exit 0. Grep the whole log for `error C` — a warning-free build is not the bar, a clean exit code plus no error lines is.

- [ ] **Step 7: Run the full gate**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected: the run ends `ALL HEADLESS PROBES PASSED`. If probes report "not built" / "not rebuilt", build the full CI probe list from `.github/workflows/ci.yml`'s "Build probes" step first and re-run. If a Qt probe exits 127, put `/c/Qt/6.8.3/msvc2022_64/bin` on `PATH` — that is a missing DLL, not a failure.

- [ ] **Step 8: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "fix: the player's transport row steps its own ring, not Qt's tab chain"
```

---

### Task 4: Driven verification

The probe suite cannot see focus — this repo builds no `MainWindow` headlessly — so it is not evidence for this change. This task produces the evidence, and finishes the doc Task 2 started.

**Files:**
- Modify: `docs/superpowers/verification/2026-08-30-player-ring-authoritative.md`

**Interfaces:**
- Consumes: the object names from Task 2, the behaviour from Task 3, the baseline sequences already written into the doc.
- Produces: a completed verification record.

- [ ] **Step 1: Launch the fixed build in place**

```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" EB_UITEST=1 EB_UITEST_PIPE=EB-ringnav \
  ./build/Release/EverythingBox.exe &
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py status
```

Expected: `ok ready`. Never deploy over `C:\EverythingBox-app`.

- [ ] **Step 2: Open the same media**

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py open "C:/Users/cubma/Videos/2026-07-10 21-59-06.mp4"
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
```

Expected: the same non-zero `playerDur` as the baseline run. Use the identical file, or the comparison is not one.

- [ ] **Step 3: Check 1 — the Right cycle covers the whole ring**

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key down
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py walk 14 right
```

Record every `playerFocus`. **Pass:** the sequence enumerates all **9** visible ring members in the order declared at `MainWindow.cpp:1232` — `rewindBtn, playPauseBtn, fastFwdBtn, seekBar, muteBtn, volumeBar, speedBtn, subsBtn, moreBtn` — and wraps. Three declared members are absent because they are hidden on a video: `prevChapBtn` and `nextChapBtn` (unchaptered media) and `stopBtn` (`setVisible(isAudio)`, `MainWindow.cpp:13152`). **Fail:** any short cycle, or any of the nine never reached.

- [ ] **Step 4: Check 2 — the Left cycle is the exact reverse**

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py walk 14 left
```

**Pass:** the same members, same period, in reversed order. **Fail:** a different set or a different period. Checks 3 and 4 together are the fix; either alone is not.

- [ ] **Step 5: Check 3 — ‹ Back is not on the row**

Read the two sequences from Steps 3 and 4. **Pass:** `videoBack` appears in neither. **Fail:** it appears in either, which means something still walks the tab chain.

- [ ] **Step 6: Check 4 — Up and Down**

From a known member (step right until `playerFocus` reads `speedBtn`):

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key up
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key down
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
```

**Pass:** the first state reads `playerFocus: "videoBack"`; the second returns to the row at `speedBtn` (the member `lastPlayerFocus_` holds). Record both, and say plainly whether this differs from the Task 2 Step 8 baseline — that is the answer to the open question about the comment at `MainWindow.cpp:4198`.

- [ ] **Step 7: Check 5 — the bars' contract is unregressed**

Step onto `seekBar`, then:

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key enter
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key right
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key back
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
```

**Pass:** after Enter, `barAdjusting` reads `seekBar`; after Right, `playerPermille` has risen by exactly 10 (`eb::kSeekStep`) while `playerFocus` still reads `seekBar`; after Back, `barAdjusting` is `""` and `playerFocus` is still `seekBar` — Back left the bar, it did not leave the movie. Repeat the Enter/Right/Back triple on `volumeBar`, where `volume` must rise by exactly 5 (`eb::kVolumeStep`).

Note: `playerPermille` also drifts on its own while the file plays, so read it immediately before and after the Right press, and treat "rose by roughly 10" as the pass — a jump of 100 or a value that did not move at all are the failures this distinguishes.

- [ ] **Step 8: Check 6 — Enter and Space still reach the button**

The state carries no `paused` flag, so these are read indirectly. First, Enter on a button whose effect the state *does* report — step right until `playerFocus` reads `subsBtn`, then:

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key enter
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key back
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
```

**Pass:** `subCard` reads `true` after Enter and `false` after Back — the button was pressed, not swallowed.

Then Space, read off playback progress:

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state   # note playerPermille
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state   # a second or two later: it has advanced
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key space
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state   # frozen: same value
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py key space
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py state   # advancing again
```

**Pass:** `playerPermille` advances, freezes across the first Space, and advances again after the second. **Fail:** either key does nothing — the filter is claiming more than the four arrows. (This media is 21.8 s long, so ~47 permille per second: a frozen readout is unmistakable, but do not let it run to the end mid-check — seek back to the start first if it is close.)

- [ ] **Step 9: Take a screenshot of the row with focus on a stepped-to button**

Step right until `playerFocus` reads `playPauseBtn` — a member the *old* Right cycle could never reach, which is what makes this the shot worth taking — then:

```bash
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py shot "C:/Users/cubma/AppData/Local/Temp/claude/C--Users-cubma-Project-Goliath--claude-worktrees-eloquent-dijkstra-21b4ef/d10df4a8-86fd-4164-b2fb-538dd445cc08/scratchpad/ring-right-playpause.png"
```

Open it and look at it. **Pass:** the focus fill is visibly on the button `playerFocus` names. This is the one check that catches "the state says the right thing but the screen does not".

- [ ] **Step 10: Complete the verification doc**

Add an `## After the fix` section to `docs/superpowers/verification/2026-08-30-player-ring-authoritative.md` recording all six checks with their observed sequences and a pass/fail line each, a `## Summary` line of the form `N of 6 checks passed`, and the screenshot path. If any check failed, say so plainly and stop — do not commit a pass claim you did not observe.

- [ ] **Step 11: Stop the app, re-run the gate, commit**

Close the driven instance. Then, from the repo root:

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected: `ALL HEADLESS PROBES PASSED`.

```bash
git add docs/superpowers/verification/2026-08-30-player-ring-authoritative.md
git commit -m "docs: driven verification that the player ring owns the row's arrows"
```
