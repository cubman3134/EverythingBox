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

*(pending — Task 4)*
