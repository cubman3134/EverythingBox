# Make `playerRing_` authoritative for the transport row's arrow keys

## Problem

The player's transport row declares a Left/Right focus ring, `playerRing_`
(`native/src/ui/MainWindow.cpp:1232`), walked by `MainWindow::stepPlayerFocus`
(`MainWindow.cpp:4080`). For the row's **buttons** that ring is largely nominal.

A focused `QAbstractButton` handles Left/Right/Up/Down itself, by walking the tab
chain, and **accepts** them. The key therefore never propagates to
`MainWindow::keyPressEvent`, and the four arrow cases there (`MainWindow.cpp:4052-4055`)
are unreachable from a focused button. The tree already names this mechanism, in the
comment above the subtitle panel's own filter (`MainWindow.cpp:2675`) — it is why that
panel claims its arrows in `eventFilter` instead.

The tab chain follows widget **creation** order. `playerRing_` follows the row's
**visual** order. Those two differ: `speedBtn_` … `moreBtn` are constructed at
`MainWindow.cpp:1135-1149`, before `seek_` (`:1185`), `muteBtn_` (`:1200`) and
`volume_` (`:1203`). So traversal is asymmetric. Measured on a driven run
(`docs/superpowers/verification/2026-08-29-player-bar-nav.md` §16, with per-step
montages):

```
Right, period 6:  seekBar -> 🔊 -> volumeBar -> 1× -> CC -> ⚙ -> seekBar
Left,  period 7:  seekBar -> ⏩ -> ▶ -> ⏪ -> ‹ Back -> volumeBar -> 🔊 -> seekBar
```

Neither is the modular cycle over `playerRing_`. That ring declares 12 members, of
which the two chapter buttons are hidden for unchaptered media (`MainWindow.cpp:1180-1181`),
leaving 10 visible; §16 states 9, which the driven pass below settles by enumerating
rather than counting.

The two directions traverse different **sets**: Play/Pause, Rewind and Fast-forward
are reachable by holding Left but never by holding Right, and the `‹ Back` overlay —
which is not a ring member at all — appears in the Left cycle.

Today `playerRing_` genuinely governs only three things: entering the row
(`stepPlayerFocus(0)` and `lastPlayerFocus_`), steps taken **from a slider** (which
reach `stepPlayerFocus` through `handlePlayerSliderKey`, `MainWindow.cpp:4181`), and
skip-chip membership (`MainWindow.cpp:22938`).

The bug is latent as well as present. Anyone adding a transport control must place it
correctly in **both** orders, or the row behaves differently entering a bar than
leaving one — with no compile error and no test failure, because the headless suite
builds no `MainWindow` anywhere in this repo and cannot see focus at all.

## Behaviour

After this change, one order exists. On the player page, an arrow key pressed on any
member of `playerRing_` — button, bar or the skip chip — and on the `‹ Back` overlay
does exactly what the player's own arrow contract says:

| Key | Action |
| --- | --- |
| Left | `stepPlayerFocus(-1)` — the previous **visible** ring member, wrapping |
| Right | `stepPlayerFocus(+1)` — the next visible ring member, wrapping |
| Up | focus the `‹ Back` overlay |
| Down | `stepPlayerFocus(0)` — re-land on the row |

Holding Right and holding Left therefore enumerate the same set in opposite order,
with the same period. `‹ Back` is reachable only by Up, never by stepping along the
row, because it is not a ring member.

Nothing else about the row changes. A focused button still activates on
Enter/Space, the bars keep their existing two-state contract (`PlayerBarNav.h`) —
which already routes Left/Right to `stepPlayerFocus` while Selected and to the bar's
value while Adjusting — and Tab keeps walking the tab chain, which is ordinary Qt
behaviour and is not what a remote or a controller sends.

## Design

Four pieces, mirroring how the bars were done.

### 1. A pure row table, beside the bars' one

`eb::rowKey` joins `eb::barKey` in `native/src/ui/PlayerBarNav.h`:

```cpp
enum class RowAct { NotOurs, FocusPrev, FocusNext, FocusBack, FocusRow };
inline RowAct rowKey(int key);
```

Left → `FocusPrev`, Right → `FocusNext`, Up → `FocusBack`, Down → `FocusRow`,
everything else → `NotOurs`. Deliberately free of Qt widgets, like the rest of that
header, so it needs no moc and no new translation unit.

This is the change's test signal. The headless suite cannot see focus, but it can pin
what an arrow **means** on the row, and `probe_playerbar` (`native/tools/probe_playerbar.cpp`)
already exists and is already registered in all three places CONTRIBUTING requires —
so extending it adds a gate without adding a probe.

The table is small and its content is not surprising. It earns its place by being the
only written-down statement of the row's key contract, and by failing loudly if a
future edit routes Up somewhere else.

### 2. `MainWindow::handlePlayerRowKey(int key)`

The widget half, shaped like `handlePlayerSliderKey`: a player-page gate, then
`revealMediaControls()` for any key it claims (an arrow is activity; the chrome must
not fade out from under a user who is navigating), then the four actions. Returns
`true` when the key was claimed.

`keyPressEvent`'s four arrow cases are replaced by a call into it. That is the point
of the extraction: **one** definition of what an arrow means on the player page,
reached both from the filter and from the old fallthrough path (which still matters —
it is how a key arrives when nothing in the row holds focus).

### 3. One install loop

Immediately after the `playerRing_` assignment (`MainWindow.cpp:1232`), a loop
installing `this` as an event filter on every member. Written as a loop over the ring
rather than as a list of button names, so a control added to `playerRing_` is armed
automatically. That is the latent trap closing, rather than only this instance of it
being fixed.

Three members already carry the filter for other reasons — `seek_` (`MainWindow.cpp:1197`)
and `volume_` (`:1209`) for the bars' key contract, `skipChip_` (`:1318`) for its
hover/focus lifetime — and `installEventFilter` de-duplicates, so the loop is safe over
all of them. `videoBack_` is already filtered too (`:1273`, to keep the overlay alive
while hovering it), so it needs no new install either; it simply is not a ring member,
and so is named explicitly in the filter clause.

The `eventFilter` clause sits beside the bars' clause and delegates to it first, so a
focused slider keeps its own two-state contract and only non-slider members reach
`handlePlayerRowKey`.

### 4. Object names on the ring's buttons

`playerFocus` in the UI-test state reports `objectName()` and is gated on ring
membership (`MainWindow.cpp:3556`). Only the two bars carry names, so **every button
reports `""`** — a driven pass cannot tell ▶ from ⚙, and therefore cannot verify this
change at all.

Each of the ten transport buttons gets a stable name: `prevChapBtn`, `rewindBtn`,
`playPauseBtn`, `fastFwdBtn`, `nextChapBtn`, `stopBtn`, `muteBtn`, `speedBtn`,
`subsBtn`, `moreBtn`. `skipChip_` (`MainWindow.cpp:1305`) and `videoBack_` (`:1264`)
already carry names, for their own `#id` stylesheet rules — which is also the one
constraint on the new names: none may collide with an existing `#id` selector, or it
silently restyles the button.

The gate widens with them. `videoBack_` is not a ring member, so `playerFocus` would
report `""` for it however it is named — and checks 3 and 4 below are precisely about
where `‹ Back` is and is not. The state reports the overlay too, alongside the ring.
The comment at `MainWindow.cpp:3552` explaining the `""` goes with the reason for it.

## Alternatives rejected

**`setTabOrder` mirroring the ring.** Less invasive, and it would make the two
directions agree. But it leaves two orders to keep in sync — the failure this change
exists to remove — and it does not fix the `‹ Back` overlay appearing in the Left
cycle, because `videoBack_` is a child of `player_`, not of `mediaControls_`, and so
sits in the chain regardless of how the row's own children are ordered.

**Reordering construction** so creation order matches visual order. A single block
move, no new code. Rejected for the same two reasons, plus a third: the skip chip
joins the ring at runtime and would land in the wrong place in the chain.

## Risk and verification

This changes behaviour a driven verification pass has already signed off
(`docs/superpowers/verification/2026-08-29-player-bar-nav.md`, 15 of 15), so the probe
suite alone is not sufficient evidence. The headless suite builds no `MainWindow`
anywhere in this repo; this bug is invisible to it, and so would a regression be.

**Headless.** `probe_playerbar` extended to pin `eb::rowKey`, including that it
declines Enter, Space and Back — a row table that claimed Back would strand the user
on the player, the same failure `barKey` avoids while Selected.

**Driven.** `EB_UITEST=1` with `native/tools/uitest.py`, on a build from this
worktree run in place (never deployed over `C:\EverythingBox-app`), reading
`playerFocus` from the player state JSON:

1. Holding Right from the seek bar enumerates **every visible ring member** and
   returns to the seek bar. The expected sequence is read off `playerRing_` rather
   than assumed, and the observed period replaces §16's 6.
2. Holding Left enumerates the same members in exactly reversed order, same period —
   replacing §16's 7. Checks 1 and 2 together are the fix: same set, both directions.
3. `‹ Back` appears in neither cycle.
4. Up from a button focuses `‹ Back`; Down from `‹ Back` returns to the row, at the
   member `lastPlayerFocus_` names.
5. The bars' existing contract is unregressed: Enter on a bar still enters Adjusting,
   Left/Right there still move the value, Back still leaves Adjusting.
6. Enter on a button still activates it, and Space still toggles pause.

Check 4 also settles an open question: the comment at `MainWindow.cpp:4198` assumes
Down from `‹ Back` runs `stepPlayerFocus(0)`, which cannot be true if the overlay's
own arrows are consumed by the tab chain. The pre-change behaviour is measured and
recorded either way.
