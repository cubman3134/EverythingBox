# D-pad row bands in the nav ring — the classic Edit Profile picker

**Date:** 2026-07-28
**Area:** `native/src/ui/nav` (NavRing), `native/src/ui/nav/PasscodePad.cpp`, `native/tools/probe_nav.cpp`

## The report

The classic (non-themed) Edit Profile page (`ProfileDialog::showPicker`) was reported as having a D-pad
reachability defect: the "Passcode…" and "Cancel" buttons could not be focused with arrow keys at all, with
`NavRing::pickNext`'s sideways-dominance filter named as the cause:

```cpp
if (orth > primary + 4) continue;   // more sideways than in-direction -> not "that way"
```

## What measurement actually showed

The real `ProfileDialog` was instantiated inside a replica of `MainWindow::showDialogPanel`'s panel shape
(header + `widgetResizable` QScrollArea + 28/24 content margins) at 1280x760, and the full arrow-key
reachability **closure** computed — from the ring's own initial selection, press all four arrows from every
widget reached, until nothing new appears. Measured geometry is within ~10px of the report's.

| press | before |
| --- | --- |
| Down from 🐝 🦖 🐢 🍄 ⭐ 🌈 🍀 | nothing — dead from 7 of 8 columns |
| Down from 🎮 | OK — leaps *past* "Passcode…" |
| Left from 🐝 | **"Passcode…"** |
| Right from OK | **Cancel** |
| Up from "Passcode…" | 👾 — skips a whole icon row |
| Down from "Passcode…" | nothing — no path onward to OK/Cancel |

So the diagnosis of the cause was right and the defect is real, but **"unreachable" was not**: both controls
could be focused. The report's walk (Down from every column, Left/Up from OK) never pressed Left from the
leftmost column, which is the one press that reaches "Passcode…".

The honest statement of the defect: **"Passcode…" is a pocket hanging off one icon.** The natural Down
motion toward it does nothing from seven of eight columns, the eighth skips it, Up from it skips a row, and
it has no forward path to the buttons it sits above. Reachable-by-a-press-nobody-would-try is not reachable.

## Decision: row bands in the ring, not an edge hop owned by the screen

The report proposed an explicit edge hop owned by the screen, on the `PasscodePad::handleNavKey` precedent.
Rejected in favour of correcting the ring's vertical model:

- `ProfileDialog` is a plain `QDialog` inside `MainWindow`'s generic panel ring (`panelRing_` over
  `panelPage_`). It has no `handleNavKey` the way `NavOverlay` subclasses do, so a screen-owned hop needs
  **new nav-kit plumbing** to reach it — and then fixes one screen while leaving the same trap for every
  other classic dialog embedded in a panel.
- The band rule **subsumes** `PasscodePad`'s existing hop. That the #30 special case falls out of the
  general rule for free is the strongest available evidence that the general rule is the right one.
- Rows are a real property of a layout. Reading them off the geometry beats guessing at a threshold.

### The rule

`NavRing::pickNext` resolves Up/Down in two stages:

1. **Find the row.** The candidate whose edge-to-edge gap in the pressed direction is smallest, plus every
   candidate whose vertical extent overlaps that one's. Edges, not centres — a tall control and a short one
   starting at the same y are the same row, and their centres are not.
2. **Choose within the row** using the existing weighted score, so a grid still steps straight down its
   column. The sideways-dominance filter no longer applies to vertical steps: the band has already
   established "that way", and inside one row "more sideways than down" *is* the bug.

Two properties keep the blast radius honest:

- **Left/Right are untouched.** Horizontal runs have no comparable "next column" structure (a vertical list
  is one column of full-width rows), and the dominance filter is load-bearing there — it is what stops a
  header Back button, sitting up-and-slightly-right of a row, winning a Right press. No probe section covers
  that case, so it is not changed without a net. Consequence left standing: Left from OK still drifts up
  into the icon grid rather than reaching "Passcode…". Everything remains reachable.
- **An empty band falls back to the old scoring.** If nothing sits clear of the current widget's own row
  there is no row to step to, so the whole candidate list is scored exactly as before. This makes the change
  strictly **additive**: no move that worked before stops working; rows that were unreachable stop being so.

### After

Down reaches "Passcode…" from all eight columns; Down from "Passcode…" reaches OK; Up from OK returns to
"Passcode…" (skipping no row); Up from "Passcode…" lands on the icon row directly above.

`PasscodePad::handleNavKey`'s `Key_Down` case is deleted, with the history kept as a comment. probe_nav
§22(c) — which walks Down from every column of the pad — therefore becomes a regression test of the ring
rule rather than of a special case that shadowed it.

## The probe (§23)

Asserted against the **real** `ProfileDialog`, not a stand-in layout: the defect was pure geometry, and a
stand-in drifts from the page it is meant to guard until it stops describing it. `probe_nav` gains
`ProfileDialog.cpp` + `ProfileStore.cpp` as sources.

Two kinds of assertion:

- **(a) No D-pad orphans** — the reachability closure over all four arrows. A hand-written walk can only
  prove the paths its author thought of, which is exactly how §22's first draft passed while shipping a
  dead-end corner. This assertion would *not* have caught the reported defect on its own (nothing was
  strictly unreachable), so it is a guard against future layout changes, not the load-bearing one.
- **(b,c) Order** — Down from each of the eight bottom-row columns reaches "Passcode…"; the rows below keep
  going, both ways, skipping nothing. This is the one that names the defect.

§23 also dismisses any overlay left open by a failing earlier section, so its report names its own cause
instead of "nothing is reachable".

## Verification

- Pre-fix behaviour restored two ways (force the vertical path off; make `rowBand` return empty so the
  fallback takes over): **18 failures each**, covering all eight columns, all four ordering assertions, and
  §22(c)'s pad walk.
- A third mutant that also tightens the Left/Right filter strands the controls outright: assertion (a) fires
  and names "Passcode…" and "‹ Back". Neither assertion is vacuous.
- With the fix: `probe_nav` NAV-OK, deterministic over 5 runs; full headless suite **132/132**.

## Out of scope, found in passing

`probe_passcode` is flaky at [`probe_passcode.cpp:394`](../../../native/tools/probe_passcode.cpp)
(`got.lockedUntilMs <= readAt + kMaxLockoutMs`): `readAt` is sampled before `attempts()` takes its own
`currentMSecsSinceEpoch()` for the clamp, so a millisecond tick between them fails the assertion. Observed
1 failure in 4 runs. Unrelated to this change — that target links only `ProfilePasscode.cpp` + Qt6::Core.
