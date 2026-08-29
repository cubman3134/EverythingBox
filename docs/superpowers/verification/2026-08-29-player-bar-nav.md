# Driven verification: arrow/controller navigation of the player's transport bars

**Date:** 2026-08-29
**Branch:** `feat/player-bar-nav` (worktree `.claude/worktrees/player-bar-nav`, head `c180b6a6`)
**Build driven:** `build/Release/EverythingBox.exe` from this worktree, run **in place**. Nothing was
deployed to `C:\EverythingBox-app`; that instance was left running and untouched throughout (confirmed by
`Get-Process EverythingBox` after the restart step — pid 44080 at `C:\EverythingBox-app` was still up).

```
PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" EB_UITEST=1 EB_UITEST_PIPE=EB-playerbar \
  ./build/Release/EverythingBox.exe &
EB_UITEST_PIPE=EB-playerbar python native/tools/uitest.py status   # -> ok ready
```

**Settings scope.** This build keeps its settings in `build/Release/everythingbox.ini`, beside the exe —
separate from `C:\EverythingBox-app\everythingbox.ini`. The volume values written below went only to the
worktree copy (verified by `grep -i volume` on both files).

**Onboarding.** The worktree's ini was fresh, so the app came up on "Welcome to EverythingBox" →
"Who's using EverythingBox?" (only row: `＋ Create New Profile`). No config was edited; the profile picker
was simply bypassed — `uitest.py open <path>` routed straight to `openVideoPath` and landed on the player.

**Test media:** `C:/Users/cubma/Videos/2026-07-10 21-59-06.mp4` (quoted everywhere; the path has spaces).
`playerDur` read **21.8** on every open, so the file genuinely loaded. It is a 21.8 s screen capture of an
Unreal editor session, which is short: several timing checks below seek back to the start first, and
"advancing" deltas are ~47 permille per second, which makes a frozen readout unmistakable.

**Screenshots** are in the session scratchpad
`C:\Users\cubma\AppData\Local\Temp\claude\C--Users-cubma-Project-Goliath\e91ff38d-5fca-4451-97ba-0e3b077a76e2\scratchpad\`
(paths given per check). Every one was opened and looked at, and the colour claims below are pixel samples,
not impressions.

---

## Summary

**15 of 15 checks passed.** No failures.

Two things behaved in ways worth writing down even though neither fails a check — one cosmetic
(§3, a blue wash inside the Selected outline that the stylesheet does not ask for), and one structural
(§16, the row's Left/Right traversal is asymmetric and `⏪ ▶ ⏩` cannot be reached by pressing Right).
§16 is code-attributed to pre-existing `QAbstractButton` behaviour, not to this branch.

---

## 1. The bars are reachable at all — PASS

```
uitest.py key down            # reveal the chrome
walk 20 right                 # press right 20x, reading state each step
```

Observed `playerFocus` across the twenty steps:

```
 1 ""   2 ""   3 "1×"  4 "CC"  5 ""   6 "seekBar"
 7 ""   8 "volumeBar"  9 "1×" 10 "CC" 11 ""  12 "seekBar"
13 ""  14 "volumeBar" 15 "1×" 16 "CC" 17 ""  18 "seekBar"
19 ""  20 "volumeBar"
```

`"seekBar"` at steps 6/12/18 and `"volumeBar"` at 8/14/20. Both strings appear, repeatedly, in a stable
cycle. Before this change neither could ever be reported. This is the core of the feature and it works.

## 2. The transport row does not shift between the three states — PASS

Three shots of the **volume** bar (unfocused with focus parked on `CC`, Selected, Adjusting):

- `barnav-0-unfocused.png`
- `barnav-1-volume-selected.png`
- `barnav-2-volume-adjusting.png`

Column extents of landmarks either side of the bar, measured by scanning the row band for non-background ink:

| shot | seek groove x | speaker glyph x | `1×` label x |
|---|---|---|---|
| unfocused | (188, 874) | (938, 953) | (1117, 1132) |
| selected | (188, 874) | (938, 953) | (1117, 1132) |
| adjusting | (188, 874) | (938, 953) | (1117, 1132) |

Byte-identical. The volume widget's own *visible ink* grows from (980, 1091) unfocused to (976, 1095) in
both focused states — that is the 2 px border appearing inside the box the transparent base border already
reserved, and its neighbours on both sides did not budge.

Repeated for the **seek** bar, using three shots of the *same paused frame* (`seek-A-unfocused.png`,
`seek-B-selected.png`, `seek-C-adjusting.png`) so the picture could not confound the measurement:

```
rewind(43,59)  play(98,108)  ffwd(146,162)  time(865,915)
speaker(939,953)  1x(1117,1131)  gear(1220,1237)
```

— identical in all three. The whole-image difference between the unfocused shot and each focused one is
bounded to `(184, 700, 1200, 736)`: the seek bar's own box plus the `CC` button that lost focus, and
**nothing above y=700 changed at all**. The transparent base border is doing its job.

## 3. Selected is an outline, not a fill — PASS (with an unexplained tint)

`barnav-1-volume-selected.png`, zoomed 8× to `zoom-vol-selected.png`. Clearly a ring: a blue border with
the groove (white sub-page, grey add-page) and the white handle plainly visible inside it.

Vertical pixel strip through the handle column (x=1035) of the volume bar:

| | unfocused | Selected | Adjusting |
|---|---|---|---|
| border row (y=705/706) | (17,17,20) — no border | **(84,130,236)** blue | **(255,255,255)** white |
| interior (y=708) | (17,17,20) | **(32,42,67)** | **(75,115,208)** |

**Surprise worth recording.** Selected does not only draw an outline — it also tints the interior. Measured
over a black frame the interior goes (17,17,20) → (32,42,67); solving for alpha against the rule's
`rgba(90,140,255,0.90)` gives a=0.205, 0.203, 0.200 on the three channels, i.e. the focus blue at ~20 %.
Confirmed independently on the seek bar over an identical paused frame: (21,21,24) → (35,45,70) outside vs
inside focus, and the groove's add-page composites over that tint too ((72,72,75) → (83,91,111)).

The stylesheet's `:focus` rule (`MainWindow.cpp:1109`) sets **only** a border, so this wash is a Qt QSS
rendering side-effect of adding a border on `:focus`, not something the rule asks for. It is **not** the
`[adjusting="true"]` rule leaking: the wash tracks focus, and the unfocused shot with `adjusting` also
false shows no wash. It does not fail this check — visually it still reads as an outline and the groove and
handle stay fully legible — but somebody should know it is there.

## 4. Adjusting's border is WHITE, not blue — PASS

Left border pixel of the volume bar at y=718:

- Selected: **(84, 130, 236)** — blue
- Adjusting: **(255, 255, 255)** — pure white

Top border at (1035, 707): Selected (32,42,67)/blue edge, Adjusting **(255,255,255)**. The source-order
assumption holds — `[adjusting="true"]` (line 1112) wins over `:focus` (line 1109) at equal specificity.

## 5. Adjusting is not identical to Selected — PASS

They differ on every axis measured: border white vs blue, interior (75,115,208) vs (32,42,67), handle
(255,255,255) vs (232,232,232) and visibly wider. `state` corroborates: `barAdjusting` was `"volumeBar"`
for the Adjusting shot and `""` for the Selected one, so the state machine and the pixels agree.
See `zoom-vol-selected.png` vs `zoom-vol-adjusting.png`, and `z-seek-B.png` vs `z-seek-C.png`.

## 6. The handle is not vertically clipped — PASS

Vertical strip at x=1035 through the volume handle:

```
y=705,706  border
y=707..709 interior
y=710      (223,223,224)   <- handle top, antialiased
y=711..724 (232,232,232)   <- handle body
y=725      (223,223,224)   <- handle bottom, antialiased
y=726..728 interior
y=729,730  border
```

Widget box is y=705..730 (26 px, matching `playerSeekRect` height 26); the handle occupies y=710..725,
16 px tall, with 3 px of clearance above and below inside the 22 px content box. In the Adjusting strip the
handle runs the same y=710..725 in pure white. The 8× zooms show fully rounded ends on both — no flat
slicing. `margin:-5px 0` on the 6 px groove is safe here.

## 7. Volume: enter, step, back out, and the value sticks — PASS

```
state -> {"playerFocus":"volumeBar","barAdjusting":"","volume":100, ...}
key enter
state -> {"playerFocus":"volumeBar","barAdjusting":"volumeBar","volume":100, ...}   # entry did NOT move it
keys "left left left"
state -> {"playerFocus":"volumeBar","barAdjusting":"volumeBar","volume":85, ...}    # delta exactly -15
key back
state -> {"playerFocus":"volumeBar","barAdjusting":"","volume":85,"page":"QSplitter"}
```

Entry: 100 → 100. Three lefts: 100 → **85**, exactly −15. Back-out: `barAdjusting` `""`, `playerFocus`
still `"volumeBar"`, volume still **85**, and `page` still the player page — Back left the bar, not the
movie.

## 8. Controller Back and keyboard Escape, as separate paths — PASS (both halves, both paths)

**Injected controller Back** (`uitest.py key back`, which rides `sendNavKey` and hits the clause added at
`MainWindow.cpp:2867`):

- from Adjusting → `barAdjusting` `""`, `playerFocus` `"volumeBar"`, `page` still `QSplitter` (the player).
- a **second** `back`, now merely Selected → `page` becomes `"HomeView"`. The app-wide unified Back is
  intact; the bar only claims Back while it is actually adjusting.

**Keyboard Escape** (`uitest.py key escape`), tested separately on a fresh open:

```
adjusting                 -> {"playerFocus":"volumeBar","barAdjusting":"volumeBar","playerPermille":49}
after escape #1           -> {"playerFocus":"volumeBar","barAdjusting":"",         "playerPermille":54}
after escape #2           -> page "HomeView"
```

Same two-step shape, and `playerPermille` 45→49→54 across it shows playback ran on through the first
Escape.

## 9. The latch — PASS on all three exit routes

The bug being regression-checked: a seek bar left `setSliderDown(true)` stops `onPosition` updating the
handle and the time readout for the rest of playback. `barAdjusting == ""` does not prove the latch came
off, so each route below re-opens the video and watches the number **move** over ~2 s.

| exit route from seekBar-Adjusting | page after | re-opened `playerPermille` over 2 s |
|---|---|---|
| `key back` ×2 (goBack path) | `HomeView` | 165 → **259** (Δ 94) |
| click the on-screen `‹ Back` overlay at (55, 41) **while still Adjusting** | `HomeView` | 370 → **468** (Δ 98) |
| page change — `open` a PDF while still Adjusting (`stack_` currentChanged) | `ReaderChromeHost` | 584 → **678** (Δ 94) |

All three advance at the expected ~47 permille/s. Nothing latched.

## 10. Idle auto-hide mid-adjust — PASS

Entered Adjusting on the seek bar, then waited 5.5 s without pressing anything:

```
adjusting, then untouched -> {"playerFocus":"seekBar","barAdjusting":"seekBar","mediaControls":true, "playerPermille":63}
after 5.5s idle           -> {"playerFocus":"",       "barAdjusting":"",        "mediaControls":false,"playerPermille":138}
```

`mediaControls` **false**, `barAdjusting` **`""`**. Then, with the chrome still hidden,
`playerPermille` 142 → **236** over 2 s. After `key down` woke the chrome, 250 → **344** over 2 s. The
transport recovered rather than dying, and the wake put focus back on `"seekBar"` (`lastPlayerFocus_`
doing its job).

## 11. Left/Right on a merely-Selected bar must NOT change its value — PASS

**Volume**, arrowing right-then-left-then-left straight across the Selected bar:

```
on volumeBar (Selected)      volume 85
after right (off the bar)    volume 85
after left  (back on it)     volume 85
after left  (off the other)  volume 85      -> four identical reads
```

**Seek.** Playback moves this number on its own, so this was redone **with playback paused** (clicked the
⏸ button; confirmed static: 320 at t and 320 at t+1.5 s):

```
on seekBar (Selected)   playerPermille 320
keys "right left left"  playerPermille 320      -> exactly unchanged
```

(The unpaused run agreed but only within drift: 179 → 198 over 0.40 s of wall clock against a measured
free-run rate of ~49 permille/s. The paused run is the one that proves it.)

## 12. Up and Down always leave the bar — PASS

From `volumeBar`, from **both** states:

| start | after `key up` | after `key down` | volume |
|---|---|---|---|
| Selected | `focusText "‹ Back"`, `barAdjusting ""` | back in the row | 85 → 85 → 85 |
| Adjusting | `focusText "‹ Back"`, `barAdjusting ""` | back in the row | 85 → 85 → 85 |

Up reaches the `‹ Back` overlay from both states and drops Adjusting on the way; Down returns to the row;
neither touches the value.

**Observation (not a failure):** Down lands on `⏪` (the leftmost transport button — verified by looking at
`ring-after-up-down.png`, where the rewind glyph carries the focus fill), not back on the bar you came
from. That is `stepPlayerFocus(0)`'s documented fallback firing because `videoBack_` is not in the ring and
`lastPlayerFocus_` was unset; the brief only asked that Down return to the row, which it does.

**Also checked — leaving an Adjusting *seek* bar via Up commits rather than reverts.** Paused at 48, entered
Adjusting, `keys "right right right"` → **78** (exactly +30), `key up` → `barAdjusting ""`, focus `‹ Back`,
`playerPermille` **77**, still 77 a further 1.2 s later. 78 → 77 is a one-permille rounding of the committed
mpv position; a revert would have gone back to 48.

## 13. Seek: live movement — PASS

Paused, seek bar Selected at **320**:

```
key enter                                -> barAdjusting "seekBar", playerPermille 320
keys "right right right right right"     -> playerPermille 370        (delta +50, exactly 5 x 10)
```

The picture actually moved, not just the number: comparing the video area (0,0)-(1280,690) of
`barnav-3-seek-before.png` against `barnav-5-seek-after.png` — both taken while **paused**, so any change
is the seek —

```
difference bbox = (0, 24, 1280, 690)
pixels differing by more than 8 levels: 169824 of 883200 = 19.23%
```

Then `key back` → `barAdjusting ""`, `playerPermille` **370**, and still **370** 1.5 s later. The position
stayed where it was left; no snap-back.

## 14. Mouse + keyboard mix — PASS

`playerSeekRect` = `184 705 675 26`. Entered Adjusting on the seek bar, then synthesised a real click on
the groove at 70 % — (656, 718):

```
adjusting              -> {"barAdjusting":"seekBar","playerPermille":370}
after click            -> {"barAdjusting":"",       "playerPermille":707}
```

`barAdjusting` went to `""` on the click (the `sliderReleased` → `setBarAdjusting(false)` guard), and the
click seeked to 707 — 0.70 of the bar, as asked. Arrowing afterwards did **not** scrub: `keys "right right"`
moved focus (`playerFocus` `"seekBar"` → `"volumeBar"`) and left `playerPermille` at **707**. Still 707 a
further 1.5 s later; the handle is not fighting playback.

## 15. Volume survives a restart — PASS

Set via the bar: Enter on `volumeBar`, `keys "left left left left left left left left"` → **85 → 45**
(exactly −40), `key back` → `barAdjusting ""`, volume 45. `build/Release/everythingbox.ini` then read
`volume=45`.

The process was killed (`Stop-Process -Id 8632 -Force`) and relaunched from the same binary and pipe. After
re-opening the video:

```
{"playerFocus":"","barAdjusting":"","volume":45,"playerDur":21.8, ...}
```

**45.** The arrow path really does go through the existing `valueChanged` handler that persists
`player/volume`.

---

## 16. Finding: the row's Left/Right traversal is asymmetric, and `⏪ ▶ ⏩` cannot be reached by pressing Right

Not one of the assigned checks, and **not introduced by this branch**, but it sits squarely on the feature's
subject ("the ring widening") and a reader of `playerRing_` would not predict it, so it is recorded here.

Walking **right** from the seek bar cycles with period **6** (montage: `montage-right.png`, one shot per
step, focus fill visible in each):

```
seekBar -> 🔊 mute -> volumeBar -> 1× -> CC -> ⚙ -> seekBar
```

Walking **left** from the seek bar cycles with period **7** (montage: `montage-left.png`):

```
seekBar -> ⏩ -> ▶ -> ⏪ -> ‹ Back -> volumeBar -> 🔊 mute -> seekBar
```

The two directions traverse **different sets**. `⏪ ▶ ⏩` and the `‹ Back` overlay appear only when walking
left; `1× CC ⚙` only when walking right. Neither is the modular cycle over
`playerRing_` (`MainWindow.cpp:1221`), which has 9 visible members here.

**Mechanism** (the code says so itself, at `MainWindow.cpp:2655`):

> *"A focused QAbstractButton handles Left/Right/Up/Down by walking the tab chain and ACCEPTS them, so the
> key never propagated to MainWindow::keyPressEvent"*

So the row runs on two different mechanisms depending on what has focus:

- focus on a **button** → Qt consumes the arrow and walks the **tab/focus chain** (widget creation order);
  `stepPlayerFocus` is never called.
- focus on a **bar** → `sendNavKey` step 4.9 (`MainWindow.cpp:2867`) intercepts first, so
  `handlePlayerSliderKey` → `stepPlayerFocus(±1)` — the intended ring.

Creation order in `mediaControls_` is `… subs(1129) … more(1140), seek_(1176), time, mute(1189),
volume(1192)`, with `videoBack_` earlier as a child of `player_`. That predicts exactly the two observed
cycles, step for step, including `⚙ → right → seekBar` (tab chain) and `⏪ → left → ‹ Back` (tab chain
running off the front of the row's children).

**Attribution.** The tab-chain consumption predates this branch, and both sliders were already
tab-focusable (`QSlider` defaults to `Qt::WheelFocus`), so `⚙ → right` landed on the seek bar on `main`
too. What changed for the better is what happens once you are there: on `main` the next Right would have
been eaten by `QAbstractSlider` and **scrubbed the video**, which is the bug this feature exists to fix; now
the bar is inert until you press Enter (§11). This is a code-level attribution — a `main` build was not
compiled and driven for a side-by-side comparison.

**Consequence for a remote user:** Play/Pause, Rewind and Fast-forward are reachable by holding Left but
never by holding Right. Worth a follow-up decision; out of scope for this branch.

---

## Screenshots

All under
`C:\Users\cubma\AppData\Local\Temp\claude\C--Users-cubma-Project-Goliath\e91ff38d-5fca-4451-97ba-0e3b077a76e2\scratchpad\`:

| file | what it shows |
|---|---|
| `barnav-0-unfocused.png` | transport row, focus on `CC`, both bars unfocused |
| `barnav-1-volume-selected.png` | volume bar Selected (blue outline) |
| `barnav-2-volume-adjusting.png` | volume bar Adjusting (blue fill, white border + handle) |
| `zoom-vol-selected.png`, `zoom-vol-adjusting.png` | 8× zooms of the above two |
| `seek-A-unfocused.png`, `seek-B-selected.png`, `seek-C-adjusting.png` | seek bar, three states, same paused frame |
| `z-seek-A.png`, `z-seek-B.png`, `z-seek-C.png` | 3× zooms of the above |
| `barnav-3-seek-before.png`, `barnav-4-seek-adjusting.png`, `barnav-5-seek-after.png` | §13 live-seek before/during/after |
| `ring-left-of-seek.png`, `ring-after-up-down.png` | §12, §16 focus landings |
| `montage-right.png`, `montage-left.png` | §16 step-by-step focus walks |
