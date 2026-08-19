# Media-playback launch supersession: a film, a track or a book supersedes a pending external launch

## The bug

This is the first follow-up chip of `2026-08-19-cross-frontend-launch-supersession.md`
("Media playback is not covered"). That change taught the three in-app **game** frontends to
cancel a still-pending external launch; every non-game surface is still exposed to the
identical failure.

Start a PS3 game. `EmulatorManager::runPs3UpdateThenLaunch`'s worker begins a firmware/update
install — minutes of work, `busy_` true, the wait page showing. `GameLauncher::aboutToLaunch`
has already stopped the player and cleared the audio queue. Press F8 to enter split screen,
F8 again to leave it: `exitSplitScreen()` → `openHome()` lands on Home with the external
launch still pending. Now play a movie, a music track, or open a book.

When the worker finishes, its continuation boots RPCS3 full-screen over the playback, and:

- `GameLauncher`'s `launched` handler minimises the app mid-film;
- it records the stale `pendingEmu*` PS3 entry into Recents and the play session, over what
  was actually being watched or read;
- nothing re-stops the playback — `aboutToLaunch` fired minutes earlier, before the film
  even started, so the film keeps playing behind a minimised window under a full-screen
  emulator.

The readers are exposed the same way. Nothing about the bug is audio-specific: the minimise
and the Recents stomp happen whatever the superseded surface was.

## Decision

Extend the existing supersession idiom to every non-game play surface. The primitive is
**unchanged** — this change adds call sites only.

`GameLauncher::cancelPendingEmulatorLaunch()` already does exactly the right thing: it is
two-phase (demote a still-installing launch so its download completes, cancel-now a
post-install one), emits its terminal `failed()` **queued** so no host teardown re-enters the
superseding caller's stack frame, surfaces the outcome on the toast because the status bar is
hidden app-wide, and no-ops when nothing is pending. None of that needs revisiting.

The stale-Recents half of the bug needs no separate fix, for the same reason it needed none
for games: with the continuation dropped, `launched` never fires for the cancelled launch.

### The helper

One named private helper on `MainWindow`, so the rationale lives in one place and the twelve
callers are one line each:

```cpp
// A non-game play surface is about to own the screen, so an external launch still waiting on
// an install/firmware update must not boot over it minutes from now — the same supersession
// GameLauncher's in-app game tails do. No-op when nothing is pending.
void MainWindow::supersedePendingExternalLaunch();   // → launcher_->cancelPendingEmulatorLaunch()
```

`openGamePath`'s existing split-pane call site keeps its direct
`launcher_->cancelPendingEmulatorLaunch()` and its own game-specific comment: converting it
would churn a reviewed line for no behavioural gain.

## The choke points

Twelve sites, in three groups. The group determines the placement rule.

### 1. Readers — `presentBook()` / `presentPdf()` / `presentComic()` (3 sites)

`MainWindow.cpp:2250`, `:2264`, `:2278`. One call at the top of each, after
`captureReaderOrigin()`.

**Readers count as owning the screen.** The harm is identical to the film case and has
nothing to do with audio: the app minimises itself mid-page and the stale PS3 entry lands in
Recents over the book. Opening a book is as explicit an expression of intent as pressing
play, so the parent plan's "the last expressed intent wins" applies unchanged. The photo
viewer rides along, being `ComicView` in photo mode.

**Why these three functions and not the nine open sites.** `present*` are reached from
exactly nine places — `openDocumentPath`'s three leaves (`:4492`, `:4499`, `:4506`),
`openLibraryItem`'s five (`:11093`, `:11104`, `:11111`, `:11121`, `:11131`), and
`openImagePages`'s `openCbz` (`:11602`) — and every one of them only *after*
`book_`/`pdf_`/`comic_` has accepted the file. Nothing else in the tree calls them.

That placement is the point, not just an economy. `openDocumentPath` and
`openLibraryItem`'s reader leaves can fail with "Can't open PDF: …" and return. Cancelling at
entry would destroy the user's pending launch and then tell them the document cannot be
opened either — they lose both. That is precisely the failure the review of the parent change
already legislated against when it hoisted `finishRetroParkLaunch`'s "no RetroPark in this
build" precondition *above* its supersede. Post-accept placement gives the same guarantee for
free, and gives it structurally: a future reader leaf gets it by calling `present*` at all.

### 2. Media entry points — top of function (6 sites)

- `MainWindow::openVideoPath` (`:3512`)
- `MainWindow::openAudioPath` (`:3614`)
- `MainWindow::openAudio` — the multi-select branch only (`:3588`); the single-select case
  delegates to `openAudioPath`
- `MainWindow::playStream` (`:4366`)
- `MainWindow::openAudioStream` (`:4432`)
- the `StreamResolver::playQueue` IPTV lambda (`:1126`)

Top-of-function placement is the only position that works here, for two reasons:

- **The external-player handoff returns first.** `routePlay()` hands the file to VLC/MPC-HC/an
  Android intent and returns before any of these functions reach their play sink. The user's
  intent to watch is identical on that route, and RPCS3 booting full-screen over VLC is the
  same harm, so the cancel must precede it.
- **Each function's own `splitTarget_` branch returns first** too (`:3516`, `:4358`, `:4436`).

Unlike the readers, these sinks have no synchronous failure to guard against — mpv loads
asynchronously, so there is no "the file was rejected" moment to sit behind.

The three routes that bypass `notePlaybackStart` entirely (`openAudio` multi-select, the IPTV
`playQueue` lambda, and the split branches) are covered here explicitly rather than by
inheritance, which is why they are named individually.

### 3. Catalog and split panes (3 sites)

- **`openLibraryItem`'s `if (splitTarget_)` block** (`:11060`): hoist the `isGame`
  classification the block already computes to the top of the block, then one call guarded on
  `!isGame`. This covers `openBook`/`openPdf`/`openComic`/`openPhoto`/`openVideo` into a pane
  in one line. Post-accept does not apply: a pane takes the screen whether or not the file
  parses (the pane surfaces its own errors and `finishSplitOpen()` switches to the split view
  unconditionally).
- **`openLibraryItem`'s full-screen video leaf** (`:11154`), at the TOP of the leaf rather than
  beside its teardown ritual: this leaf has its own `routePlay()` external-player handoff that
  returns above the ritual, so group 2's rule applies here too. Its audio and audiobook leaves
  need nothing — both delegate to `openAudioStream`, group 2.
- **`openDocumentPath`'s `if (splitTarget_)` branch** (the branch at the top of the function,
  `:4471`), same reasoning as the `openLibraryItem` pane block.

`isGame` must stay excluded in `openLibraryItem`: its game leaf calls `openGamePath` →
`launcher_->open()` → `runEmulator`, where the unchanged `emu_->busy()` refusal is the
behaviour the parent plan deliberately kept. Cancelling first would convert
external-over-external from "refused" into "superseded", letting two RPCS3 install runs race
on the same emulator directory. Everything reachable past the hoisted guard is a media or
reader surface: Steam, PC games, and `.m3u` refs have all returned above it, and remote
documents/ROMs have re-entered through `fetchRemoteDocumentThenOpen`.

## Why not `notePlaybackStart()`

`MainWindow::notePlaybackStart` (`:3947`) is the shared "a play sink was reached" hook and the
obvious candidate. It is the wrong one twice over:

1. **It is reached by `openGamePath`** (`:3968`), before any routing decision. A cancel there
   would let an external launch supersede its own predecessor — explicitly rejected in the
   parent plan ("External-over-external stays blocked").
2. **It is structurally incapable of the coverage.** Every `splitTarget_` branch returns
   before it; all three readers never call it at all; `openAudio`'s multi-select branch and
   the IPTV `playQueue` route drive `setQueue` directly and skip it (both say so in their own
   comments); and on the routes that *do* reach it, the external-player handoff has already
   returned above.

The same disqualification applies to the other tempting shared choke point,
`finishSplitOpen()` (`:7958`): both of `openGamePath`'s split branches reach it, including the
external one at `:3986` that routes back through `open()`.

`PlaybackSession::playRequested` is likewise rejected: it fires on every queue *advance*,
which is not new user intent — the same reason its own comment gives for deliberately not
running the channel guard there.

## Ordering and re-entrancy

Already solved by the parent change, and the reasoning carries over unchanged. The cancel
posts its `failed()` through a queued invocation, so `GameLauncher`'s failed handler →
`waitPageDone()` → `MainWindow::openHome()` lands only after the calling sink has unwound and
set the stack to the player or reader page. `openHome()` is top-gated on the wait page being
current, so it no-ops. Emitted inline it would run a full host teardown — and, with a profile
passcode, spin a nested event loop — inside the superseding sink's own stack frame, which is
the crash-#28 class this repo has a precedent for.

## Files touched

| File | Change |
|---|---|
| `native/src/ui/MainWindow.h` | declare `supersedePendingExternalLaunch()` |
| `native/src/ui/MainWindow.cpp` | implement the helper; 12 call sites in the three groups above; hoist `isGame` in `openLibraryItem`'s split block |

No change to `EmulatorManager`, `LaunchCancel`, or `GameLauncher`.

## Verification

- `BUILD_DIR=build bash native/tools/run-headless-probes.sh` → `ALL HEADLESS PROBES PASSED`.
- Full app build (`--target everythingbox`) clean.

**No new probe.** This change adds no new decision logic; the only pure part of the mechanism,
`LaunchCancel::decide`, is already pinned by `probe_launchcancel` and
`native/tools/mutate-launchcancel.json`, both unaffected. The residual risk is a *future* play
surface forgetting the call, which no probe can catch — and a source-grep gate for it would be
the kind of harness that reports green without meaning it. The `present*` placement is the
real mitigation: it makes the readers correct by construction rather than by convention.

Live check (manual, optional): start a PS3 game so the update worker runs, F8 in and out to
reach Home, play a video — expect the "Cancelled the pending launch of …" toast immediately,
and no RPCS3 window minutes later.

## Deliberately out of scope

Unchanged from the parent plan:

1. **An external launch still does not retire `GameLauncher`'s pending in-app download
   contexts**, so an older in-app launch's late-arriving core/asset download can still cancel a
   newer external launch. The fix must avoid deleting `extractCtx_` from inside its own
   executing continuation — the crash-#28 class again — so it needs deferred destruction, not a
   one-line delete.
2. **The demote arm's surviving download/extract contends with live emulation** on the GUI
   thread.
3. **A running external game is never cancelled by this primitive.** Once `game_` exists,
   ending it is `closeGame()`/`terminateGame()` territory. Starting a film while an external
   game actually runs keeps today's behaviour.
