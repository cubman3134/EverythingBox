# Driven verification: the player ring owns the transport row's arrow keys

**Date:** 2026-08-30
**Branch:** `fix/player-ring-authoritative` (worktree `.claude/worktrees/eloquent-dijkstra-21b4ef`), based on
`feat/player-bar-nav` at `a5bb2b6f`.
**Spec:** `docs/superpowers/specs/2026-08-30-player-ring-authoritative-design.md`
**Plan:** `docs/superpowers/plans/2026-08-30-player-ring-authoritative.md`

**Build driven:** `build/Release/EverythingBox.exe` from this worktree, run **in place**. Nothing was deployed
to `C:\EverythingBox-app` (that instance was not even running). A second app instance belonging to another
worktree (`.claude/worktrees/pbn-main`, pid 13840) was up throughout and was left untouched — this run used a
private pipe name and its own process was stopped by pid, never by image name.

```
PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" EB_UITEST=1 EB_UITEST_PIPE=EB-ringnav \
  ./build/Release/EverythingBox.exe &
EB_UITEST_PIPE=EB-ringnav python native/tools/uitest.py status   # -> ok ready
```

**Test media:** `C:/Users/cubma/Videos/2026-07-10 21-59-06.mp4` — the same file the 2026-08-29 pass used, so
the two records compare directly. `playerDur` read **21.8** on open, so the file genuinely loaded. Playback
was **paused** before every measurement (`playerPermille` held at 525 across a two-second gap), because the
file is 21.8 s long and would otherwise have ended mid-walk.

**Reading focus.** Every observation below is the `playerFocus` field of the UI-test state JSON, which reports
the focused widget's `objectName()` when it is a ring member or the `‹ Back` overlay, and `""` otherwise. The
ten transport buttons were given object names for this pass; before that every button reported `""` and the
row was unreadable.

---

## The ring, and what is visible

`playerRing_` (`native/src/ui/MainWindow.cpp:1232`) declares **12** members, in the row's visual order:

```
prevChapBtn  rewindBtn  playPauseBtn  fastFwdBtn  nextChapBtn  stopBtn
seekBar  muteBtn  volumeBar  speedBtn  subsBtn  moreBtn
```

Three are hidden while a **video** plays, so the ring has **9 visible members** here:

* `prevChapBtn` / `nextChapBtn` — chapter nav, hidden for unchaptered media (`MainWindow.cpp:1180-1181`).
* `stopBtn` — `setVisible(isAudio)` (`MainWindow.cpp:13152`), so it is absent from every video's row.

This resolves the count §16 of the 2026-08-29 pass reported: **9 is correct**, and the design spec's "10 with
the two chapter buttons hidden" missed the audio-only stop button. The spec has been corrected.

The `‹ Back` overlay (`videoBack`) is **not** a ring member. It is a child of the player surface, reachable by
Up, and stepping along the row should never land on it.

---

## Baseline (before the fix)

Measured on the build at commit `9d725426` plus the object-name change — i.e. with the naming in place and the
event-filter fix **not yet applied**.

### B1. Holding Right — period 6

Entering the row (`key down`) landed on `rewindBtn`. Fourteen `key right` presses:

```
rewindBtn -> playPauseBtn -> fastFwdBtn -> speedBtn -> subsBtn -> moreBtn -> seekBar -> muteBtn
          -> volumeBar -> speedBtn -> subsBtn -> moreBtn -> seekBar -> muteBtn
```

The steady cycle is **6** controls:

```
speedBtn -> subsBtn -> moreBtn -> seekBar -> muteBtn -> volumeBar -> speedBtn
```

### B2. Holding Left — period 7

Fourteen `key left` presses from `muteBtn`:

```
muteBtn -> seekBar -> fastFwdBtn -> playPauseBtn -> rewindBtn -> videoBack -> volumeBar -> muteBtn
        -> seekBar -> fastFwdBtn -> playPauseBtn -> rewindBtn -> videoBack -> volumeBar
```

The steady cycle is **7** controls, and it includes `videoBack`, which is not in the ring at all.

### B3. The two directions traverse different sets

| Control | Reached by Right | Reached by Left |
| --- | --- | --- |
| `rewindBtn` | **no** | yes |
| `playPauseBtn` | **no** | yes |
| `fastFwdBtn` | **no** | yes |
| `seekBar` | yes | yes |
| `muteBtn` | yes | yes |
| `volumeBar` | yes | yes |
| `speedBtn` | yes | **no** |
| `subsBtn` | yes | **no** |
| `moreBtn` | yes | **no** |
| `videoBack` (not a ring member) | no | **yes** |

Neither direction covers the 9-member ring. Play/Pause, Rewind and Fast-forward are reachable only by holding
Left — confirming §16 of the 2026-08-29 pass, control for control.

### B4. Down from `‹ Back` ignores `lastPlayerFocus_`

From `seekBar`, `key up` reached `videoBack` — that is `handlePlayerSliderKey`'s `LeaveToBack`, which sets
`lastPlayerFocus_ = seek_` on the way. `key down` then landed on:

```
rewindBtn
```

not `seekBar`. Repeated from a different starting member, Down from `videoBack` landed on `rewindBtn` again.

**This falsifies the comment at `MainWindow.cpp:4198`,** which states that Down comes back to the bar you left
"without this, Up then Down landed on ⏪ instead of the bar you just left". The `lastPlayerFocus_ = bar`
assignment there is inert on this path: `videoBack` is a `QPushButton`, so its Down is consumed by the same
tab-chain walk as every other button's, `stepPlayerFocus(0)` never runs, and Qt hands focus to the next widget
in creation order — which is `rewindBtn`, the very "⏪" the comment says the assignment prevents.

---

## After the fix

Measured on the build at commit `33ce0bd8` plus the `lastPlayerFocus_` amendment described in A4 below.

**6 of 6 checks passed.**

### A1. Holding Right — period 9, the whole ring ✅

Entering the row landed on `rewindBtn`. Eleven `key right` presses:

```
playPauseBtn -> fastFwdBtn -> seekBar -> muteBtn -> volumeBar -> speedBtn -> subsBtn -> moreBtn
             -> rewindBtn -> playPauseBtn -> fastFwdBtn
```

The cycle is **9** — every visible ring member, in the order `playerRing_` declares:

```
rewindBtn -> playPauseBtn -> fastFwdBtn -> seekBar -> muteBtn -> volumeBar -> speedBtn -> subsBtn -> moreBtn -> (wrap)
```

Was 6. `rewindBtn`, `playPauseBtn` and `fastFwdBtn` are now reachable by holding Right, which was the reported bug.

### A2. Holding Left — the exact reverse ✅

Eleven `key left` presses:

```
fastFwdBtn -> playPauseBtn -> rewindBtn -> moreBtn -> subsBtn -> speedBtn -> volumeBar -> muteBtn -> seekBar
           -> fastFwdBtn -> playPauseBtn
```

Same 9 members, same period, reversed. Checks A1 and A2 together are the fix: one set, both directions.

### A3. `‹ Back` is off the row ✅

`videoBack` appears in neither cycle. It was in the middle of the baseline's Left cycle (B2), between
`rewindBtn` and `volumeBar`.

### A4. Up reaches `‹ Back`, Down comes back to where you left ✅ — after an amendment

First measurement, on the fix as originally written:

```
speedBtn --Up--> videoBack --Down--> playPauseBtn
```

Up was right (it never reached `videoBack` from a button before), but Down was not: it landed on whatever
`lastPlayerFocus_` happened to hold — the member the chrome last hid from — rather than on the control just
left.

The cause is the one B4 exposes. `stepPlayerFocus(0)` falls back to `lastPlayerFocus_` when the focused widget
is not a ring member, and `videoBack` never is; `handlePlayerSliderKey`'s `LeaveToBack` therefore records the
bar it is leaving, and nothing recorded the *button* being left, because until this branch a button's Up was
never ours to handle. `handlePlayerRowKey`'s `FocusBack` now makes the same assignment. Re-measured:

```
speedBtn --Up--> videoBack --Down--> speedBtn
```

This also makes the comment at `MainWindow.cpp:4198` true for the first time — see B4, where it was not.

### A5. The bars' contract is unregressed ✅

Seek bar, from Selected:

| Press | `playerFocus` | `barAdjusting` | `playerPermille` |
| --- | --- | --- | --- |
| (arrived) | `seekBar` | `""` | 816 |
| Enter | `seekBar` | `seekBar` | 816 |
| Right | `seekBar` | `seekBar` | **826** |
| Back | `seekBar` | `""` | 826 |

Volume bar, same sequence:

| Press | `playerFocus` | `barAdjusting` | `volume` |
| --- | --- | --- | --- |
| (arrived) | `volumeBar` | `""` | 100 |
| Enter | `volumeBar` | `volumeBar` | 100 |
| Right | `volumeBar` | `volumeBar` | **105** |
| Back | `volumeBar` | `""` | 105 |

Enter goes in, Right steps the value by exactly `kSeekStep` (10) and `kVolumeStep` (5), Back leaves the bar
without leaving the movie, and focus never moves during any of it. Playback was paused throughout, so the
permille figures are the arrow's work and nothing else's.

### A6. Enter and Space still reach the button ✅

The filter claims the four arrows and nothing else:

* Enter on `subsBtn` → `subCard: true` (the Audio & Subtitles panel opened). Back closed it.
* Space on `subsBtn` → `subCard: true` again. Space is Qt's own button press and is untouched.
* Space with nothing in the row focused → `playerPermille` went 132 → 230 (playing), 233 → 233 (frozen
  across one Space), 241 → 340 (advancing again after the second). Pause still toggles.

### A7. The screen agrees with the state

`ring-right-playpause.png` (session scratchpad), taken after stepping **right** onto `playPauseBtn` — a member
the baseline's Right cycle could never reach. The blue focus fill is on the ▶ button, and the row visible in
the shot is the nine controls this record claims: `⏪ ▶ ⏩ | seek | 0:13 / 0:21 | 🔊 volume | 1× | CC | ⚙`,
with no stop button.

---

## Notes

**One test-authoring trap, not an app bug.** The transport chrome auto-hides a few seconds after the last
input, and hiding clears focus. Individual `uitest.py` calls are slow enough (a Python start each) that the
chrome can hide *between* two presses of a sequence, after which the next arrow re-enters the row at
`lastPlayerFocus_` rather than stepping from where the previous one left off — and a `back` pressed while a
*button* holds focus is the player's unified Back, which stops the media and returns home. Two mis-readings in
this pass came from that. Batch presses with `uitest.py keys "…"` (50 ms apart) and read the state once at the
end, or read `playerFocus` before every press.

**Controller input was not exercised.** This worktree's build was configured without SDL2
(`Gamepad: SDL2 not found`), so no pad was attached. It is covered by construction rather than by measurement:
a pad press reaches `sendNavKey`, which delivers the key to the focused widget with
`QCoreApplication::sendEvent` — the same call the UI-test channel makes, and one that runs installed event
filters before the receiver's own `event()`. The keys driven here took the identical path.
