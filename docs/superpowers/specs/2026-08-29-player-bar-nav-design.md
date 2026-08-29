# Arrow/controller navigation for the player's seek and volume bars

## Problem

On the TV/movie player, the seek bar and the volume bar are unreachable without a
mouse.

The player's Left/Right focus ring is `playerButtons_`, a `QVector<QPushButton*>`
(`native/src/ui/MainWindow.cpp:1202`). Because it holds buttons, the two sliders in
the transport row — `seek_` (a `SeekSlider`) and `volume_` (a `QSlider`) — cannot be
members of it and are never focused by an arrow key or a controller.

Even if one were focused, it would not help: on the player page Left/Right
unconditionally call `stepPlayerFocus(±1)` (`MainWindow.cpp:3949`), so arrows would
walk past a focused bar rather than move it. The comment above that switch claims
"a focused seek slider keeps Left/Right for scrubbing" — that is stale; nothing
implements it.

Consequences today:

* Volume can only be muted (`muteBtn_` **is** in the ring) or left where it is.
  There is no way to set a level from the couch.
* The playback position can only be nudged by the ⏪/⏩ buttons in 10-second steps.
  The progress bar itself cannot be grabbed at all.

The keyboard and the controller converge on the same handler — the pad's
`sendNavKey` delivers synthetic arrow keys to the window, which reach
`MainWindow::keyPressEvent` — so a single implementation covers both input paths.

## Behaviour

Each of the two bars has two states.

**Selected** — the bar is focused and carries a visible outline, the same way a
focused transport button does. Left/Right continue to walk the ring past it, so the
bars do not obstruct traversal of the row. Enter enters the bar.

**Adjusting** — the bar is filled with the transport row's blue highlight. Left and
Right now move the bar's *value*. Enter and Back both return to Selected. Up and
Down also leave Adjusting and then perform their usual player action (Up focuses the
‹ Back overlay, Down re-lands on the transport row).

Every key therefore either adjusts the bar or leaves it. There is no key that
strands the user inside a bar.

Backing out never reverts a change:

* Volume already persists to `player/volume` in the ini on every `valueChanged`, so
  the value the user left is the value that is kept and restored next launch.
* Seek is applied as the user moves, and the final resting position is committed on
  the way out.

### Steps

| Bar | Range | Step per press | Effect |
| --- | --- | --- | --- |
| `volume_` | 0–200 (above 100 is software boost) | ±5 | 20 presses end to end |
| `seek_` | 0–1000 permille of duration | ±10 | 1% of duration; 100 presses end to end |

The seek bar is deliberately proportional rather than an absolute number of
seconds. ⏪/⏩ already own the precise 10-second jump; the bar is the "get me roughly
there" control, so it should cost the same effort on a 90-minute film and a
six-hour audiobook.

### Live seeking, rate-limited

Each arrow press seeks immediately — the user watches the picture follow the bar.

A held direction repeats every 160 ms (the pad's hold-repeat rate,
`MainWindow.cpp:3756`), and on a network stream a seek per repeat is a re-buffer per
repeat. The seek is therefore rate-limited to at most one per 250 ms, with a
trailing fire so the final resting position always lands. A tap seeks at once; a
hold seeks about four times a second instead of six or more.

## Design

### The ring widens to widgets

`playerButtons_` becomes a `QVector<QWidget*>` ring. `seek_` joins it after `stop`
and `volume_` after `muteBtn_`, which is where they sit in the layout — the ring's
order must keep matching the visual order.

`stepPlayerFocus` changes its element type and is otherwise unchanged.
`lastPlayerFocus_` becomes a `QPointer<QWidget>`.

### Keys are claimed in the event filter, not the key switch

`QSlider::keyPressEvent` consumes Left/Right/Up/Down before they can propagate to
`MainWindow::keyPressEvent`, so the logic cannot live in the player's key switch.

It goes in `MainWindow::eventFilter`, which both sliders are registered with — the
same place, and for the same reason, the classic lyric panel claims Left/Esc
(`MainWindow.cpp:2550`). A single `handlePlayerSliderKey(QWidget* bar, int key)`
returns whether it handled the key.

The player's key switch in `keyPressEvent` needs no change: when a bar has focus the
filter has already claimed the key, and when it does not, the switch behaves exactly
as it does today.

### Seek adjust mode reuses the mouse drag

Entering Adjusting on the seek bar is `seek_->setSliderDown(true)`. That emits
`sliderPressed`, which already sets `sliderDown_`, which already makes `onPosition`
stand off both the handle and the time readout (`MainWindow.cpp:22878`).

Each arrow is `setSliderPosition(value ± 10)`. Because the slider is down, that
emits `sliderMoved`, whose existing handler already repaints the preview time.

Leaving Adjusting is `setSliderDown(false)`, which emits `sliderReleased` →
`onSeekReleased`, which already clears `sliderDown_` and commits the position.

So the keyboard/controller adjust gesture *is* the mouse drag gesture, with no
parallel code path to keep in sync.

The live seek is the one thing layered on top, and it is invoked from the key
handler only — deliberately **not** from the `sliderMoved` handler, which mouse
drags also run. The mouse keeps its present commit-on-release behaviour.

The volume bar needs no equivalent latch: its `valueChanged` handler already applies
to mpv, updates the speaker glyph, and persists.

### Pure component and probe

Per CONTRIBUTING's "a new pure component gets a probe, registered in three places",
the decision logic is extracted rather than left inline in `MainWindow`:

`native/src/ui/PlayerBarNav.h`, header-only, containing

* the state machine: `(key, adjusting) → action`, where the actions are
  focus-previous, focus-next, enter, leave, step-down, step-up, leave-then-pass-through,
  and not-ours; and
* the clamped step math: `(current, delta, step, lo, hi) → value`.

It is header-only for the same reason `SeekSlider.h` is: no signals or slots, so no
moc, and — because nothing new is added to a `.cpp` source list — no exposure to the
issue-#182 trap where a probe links a translation unit the app does not.

`native/tools/probe_playerbar.cpp` prints `PLAYERBAR-OK` and returns 0. It is
registered in all three required places:

1. `add_executable(probe_playerbar …)` in `native/CMakeLists.txt`;
2. the no-argument probe loop in `native/tools/run-headless-probes.sh`;
3. the `--target` list in the "Build probes" step of `.github/workflows/ci.yml`.

### Styling

Both sliders are currently unstyled — the `#mediaControls` stylesheet
(`MainWindow.cpp:1102`) rules only `QLabel` and `QPushButton`. A focused slider on
that dark bar therefore has essentially no focus indication, which is the visible
half of this feature.

The stylesheet gains explicit `QSlider` groove and handle rules, a `:focus` state
(outline, matching the buttons' focus treatment) and an `[adjusting="true"]` state
(filled with the row's `rgba(90,140,255,0.80)` highlight). The Adjusting state is
driven by a dynamic property on the widget plus `style()->unpolish()` /
`style()->polish()`.

This does take these two sliders off the native style. That is unavoidable: a
system-drawn slider gives no usable indication of either new state on this
background.

### Sites the widened ring forces to change

Three existing call sites assume every ring member is a `QPushButton`:

* **`applyFormFactorWidgets` (`MainWindow.cpp:4127`)** floors every ring member to a
  square `QSize(hit, hit)` on touch form factors. Squaring a slider would wreck the
  bar's layout, so the loop casts to `QPushButton` and skips anything else;
  `volume_` instead gets a `setMinimumHeight(hit)` beside the one `seek_` already
  has on the following line.
* **`hideMediaControls` (`MainWindow.cpp:4251`)** records the focused widget as
  `lastPlayerFocus_` only when it is a `QPushButton` in the ring. It records any ring
  member, so the chrome re-opens on the bar the user was on.
* **`showSkipChip` / `hideSkipChip` (`MainWindow.cpp:22685`, `:22692`)** push and
  remove `skipChip_` from the ring; only the container's element type changes.

`hideMediaControls` additionally **leaves Adjusting** before it clears focus. Without
that, a four-second idle hide during an adjust would strand `sliderDown_` latched
true, and `onPosition` would stop updating the handle and the time readout for the
rest of playback.

## Testing

`probe_playerbar` covers the pure half: every key in both states maps to the right
action, and the step math clamps at both ends of both ranges.

The full headless suite (`native/tools/run-headless-probes.sh`) covers regressions.

The probe suite cannot see focus, styling, or what a user experiences, so the
feature is confirmed by a driven pass with `EB_UITEST=1` and
`native/tools/uitest.py`: reveal the chrome, walk the ring to each bar, enter it,
step it, back out, and screenshot each state — plus a check that the volume set this
way survives a restart, and that an idle chrome hide during an adjust leaves the
player in a working state.

## Out of scope

* The split-view pane's own volume slider (`MediaPane`), which is a separate surface
  with its own bar.
* Any change to the mouse drag behaviour of either bar.
* Any change to the ⏪/⏩ 10-second seek buttons.
