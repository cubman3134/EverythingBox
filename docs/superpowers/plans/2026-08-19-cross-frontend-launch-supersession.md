# Cross-frontend launch supersession: cancel a pending external launch when an in-app frontend starts

## The bug

While `EmulatorManager::runPs3UpdateThenLaunch`'s worker runs (minutes of firmware/update
installs; `busy_` true, wait page shown), the F8 split-screen shortcut is live on every page.
F8 enters split screen from the emu wait page; F8 again exits via `exitSplitScreen()` →
`openHome()`, landing the user on Home with the external launch still pending. In-app
libretro (`retro_`), RetroPark, and split-pane launches from there never consult
`emu_->busy()`, so when the worker finishes, its `launchCtx_`-bound continuation boots RPCS3
full-screen on top of the running in-app game — and `GameLauncher`'s `launched` handler
records the stale `pendingEmu*` PS3 entry into Recents and the play session over the game
actually being played.

## Decision: supersession (option b), not blocking (option a)

Two candidate shapes were on the table:

- **(a) Block** in-app launches (and F8?) while `emu_->busy()`, with feedback.
- **(b) Supersede**: a real host-side invalidation that cancels the pending external launch
  when another frontend starts.

Chosen: **(b)**. Reasons:

- Blocking locks the user out of playing *anything* for the duration of a firmware install
  (bounded at 10 minutes per step). They already expressed intent by navigating away and
  starting another game; the last expressed intent wins.
- Supersession is this codebase's established idiom for exactly this class of race:
  `GameLauncher::extractCtx_`/`launchCtx_` and `EmulatorManager::launchCtx_` (bd2ace2) all
  retire a context so a stale continuation drops instead of booting over its replacement.
  This change extends the same idiom across the frontend boundary.
- Blocking F8 specifically would gratuitously remove split screen during multi-minute
  installs. F8 is navigation, not a launch; once every *launch* site supersedes, F8 needs no
  gate. F8 stays untouched.
- One `cancelPendingLaunch()` primitive also serves the sibling task (the wait-page Stop
  button is dead during the worker phase — it only calls `terminateGame()`, and there is no
  `game_` process yet).

Kept as-is, deliberately:

- **External-over-external stays blocked.** `runEmulator`'s `emu_->busy()` refusal is
  unchanged. Superseding one external launch with another would let two RPCS3 CLI install
  runs (the orphaned worker's and the new launch's) race on the same emulator directory.
- **Install-only flows are never cancelled.** A Settings ▸ Emulators download
  (`launchAfterInstall_` false) boots nothing, so there is nothing to supersede, and the
  user wants that download to finish.
- **A running game is never cancelled by this primitive.** Once `game_` exists,
  ending it is `closeGame()`/`terminateGame()` territory (destructive, save-affecting).
  An in-app launch while an external game is actually running keeps today's behaviour.

## Why the cancel is two-phase

The external-launch pipeline has two ownership regimes, and a correct cancel differs per
phase:

1. **Install chain** (`startInstall` → fetch → download → extract → `finishInstall`): every
   continuation is connected to `this`, not `launchCtx_`. Retiring the context does nothing
   here, and clearing `busy_` would let a new `play()` of the same emulator race the
   still-running download/extract on the same directory. The correct cancel is a **demote**:
   set `launchAfterInstall_ = false` so the chain completes as an install-only flow —
   `finishInstall` then emits `installed()` and clears `busy_` itself — and nothing boots.
   The download is not wasted.
2. **Launch phase** (`launch()` onward: BIOS/keys chains, the PS3 update worker's boot
   continuation): every async continuation is parented/bound to `launchCtx_` (that is
   bd2ace2's design). Retiring the context orphans them all; but nothing else will ever
   clear `busy_` (the task's "silent auto-disconnect wedges busy_ forever" observation), so
   the cancel must also **clear `busy_` and emit a terminal signal** so the wait page is
   dismissed and the manager is reusable.

A new `bool installing_` member tracks which regime owns the flow: set true in
`startInstall()`, false at the top of `launch()` (both of its callers — `play()` direct and
`finishInstall` — converge there). `play()`/`install()` reset it at entry for hygiene.
Install-chain failure paths leave it stale-true, which is harmless: they clear `busy_`, and
the decision gate checks `busy_` first.

## The primitive

### `native/src/core/LaunchCancel.h` (new, pure, header-only)

```cpp
namespace LaunchCancel {
enum class Action { None, DemoteToInstall, CancelNow };
inline Action decide(bool busy, bool gameRunning, bool launchAfterInstall, bool installing)
{
    if (!busy || gameRunning || !launchAfterInstall) return Action::None;
    return installing ? Action::DemoteToInstall : Action::CancelNow;
}
}
```

Pure so it is probe-testable without a window (house rule: a new pure component gets a
probe). The comments in the header must carry the phase reasoning above — the
Demote/CancelNow distinction is the load-bearing part.

### `EmulatorManager::cancelPendingLaunch()` (new, public)

```cpp
// Cancel a launch that is pending but has not yet spawned the emulator process — the seam
// an in-app frontend uses to supersede a still-installing/updating external launch (and the
// wait-page Stop button during the worker phase). Returns true if there was one to cancel.
bool EmulatorManager::cancelPendingLaunch()
{
    switch (LaunchCancel::decide(busy_, game_ != nullptr, launchAfterInstall_, installing_))
    {
    case LaunchCancel::Action::None:
        return false;
    case LaunchCancel::Action::DemoteToInstall:
        // The install chain (bound to `this`, not launchCtx_) still owns the flow; it will
        // clear busy_ itself at finishInstall. Demote it to install-only so nothing boots.
        delete launchCtx_; launchCtx_ = new QObject(this);
        launchAfterInstall_ = false;
        rom_.clear(); extraArgs_.clear();
        emit failed(tr("%1 launch cancelled — its download finishes in the background.")
                        .arg(em_.displayName));
        return true;
    case LaunchCancel::Action::CancelNow:
        // Post-install: every pending continuation (BIOS/keys chains, the PS3 update
        // worker's boot) is launchCtx_-bound; retiring the context drops them. Nothing else
        // will clear busy_ after that, so do it here and emit the terminal signal so the
        // wait page is dismissed.
        delete launchCtx_; launchCtx_ = new QObject(this);
        busy_ = false;
        emit failed(tr("%1 launch cancelled.").arg(em_.displayName));
        return true;
    }
    return false; // unreachable
}
```

`failed()` is the right terminal signal: `GameLauncher`'s handler already dismisses the wait
page, stops the (not-yet-started) hotkey/pad2key watches, and surfaces the message. It does
NOT run the `finished()` bookkeeping (`endPlaySession`, `firePostHook`, restore) — correct,
because no game ever ran. `emulatorInstallFailed` reaching the themed Emulators panel is
top-gated on `emInstallId_`, which is only set by Settings-initiated installs — and those
are `launchAfterInstall_` false, i.e. never cancellable, so the panel never sees this.

Known residual (same one the existing supersession in `play()` already accepts, bd2ace2):
cancelling during the PS3 worker phase orphans the worker thread. Its ctx-guarded `note`
lambdas drop silently and it self-deletes, but its bounded RPCS3 `--installfw`/`--installpkg`
child may still be running; a *new* external launch started inside that window could race
it. Bounded (10 min per step), rare, and strictly no worse than today.

### `GameLauncher::cancelPendingEmulatorLaunch()` (new, public)

```cpp
bool GameLauncher::cancelPendingEmulatorLaunch()
{
    if (!emu_ || !emu_->cancelPendingLaunch()) return false;
    glLog(QStringLiteral("emu: pending launch superseded by another frontend"));
    // The failed() path's statusMessage lands on the app-wide-hidden status bar; the toast
    // is the visible channel. pendingEmu* is accurate here: a true cancel means a launch
    // was genuinely pending, and its runEmulator set these. Empty title = a bare
    // "open the emulator UI" run.
    emit notifyUser(pendingEmuTitle_.isEmpty()
                        ? tr("Cancelled the pending emulator launch.")
                        : tr("Cancelled the pending launch of “%1”.").arg(pendingEmuTitle_),
                    kFeedbackStandard);
    return true;
}
```

## Call sites (the supersession points)

Every place an in-app frontend actually starts a game:

1. **`GameLauncher::finishLibretroLaunch`** — first line, before `aboutToLaunch()`:
   `cancelPendingEmulatorLaunch();`
2. **`GameLauncher::finishRetroParkLaunch`** — same, first line.
3. **`MainWindow::openGamePath` split-pane branch** — after the `plan.error` and
   `externalEmulatorId` sub-branches (both return), before the `ensureCoreThen` block:
   `launcher_->cancelPendingEmulatorLaunch();` (the external sub-branch routes back through
   `open()` → `runEmulator`, whose busy-refusal is the correct behaviour there).

Placing 1–2 in the tails (not at `open()`'s top) keeps external launches on the unchanged
busy-refusal path and means the cancel fires exactly when an in-app surface is about to own
the screen. Signal ordering is safe: the cancel's `failed()` → `waitPageDone()` →
`openHome()` all run synchronously before the tail's own `showRetro*Requested()`.

The stale-Recents half of the bug needs no separate fix: with the continuation dropped,
`launched` never fires for the cancelled launch, so the stale `pendingEmu*` entry is never
recorded.

## Probe: `probe_launchcancel`

`native/tools/probe_launchcancel.cpp` — asserts `LaunchCancel::decide`'s full truth table:

- `busy == false` → `None` for all 8 combinations of the other three;
- `busy && gameRunning` → `None` (all 4 combinations behind it);
- `busy && !gameRunning && !launchAfterInstall` → `None` (both `installing` values);
- `busy && !gameRunning && launchAfterInstall && installing` → `DemoteToInstall`;
- `busy && !gameRunning && launchAfterInstall && !installing` → `CancelNow`.

Prints `LAUNCHCANCEL-OK`, returns 0. Pure header include, no Qt linkage needed beyond what
the probe pattern uses. Registered in **all three places** (CONTRIBUTING):

1. `add_executable(probe_launchcancel …)` in `native/CMakeLists.txt`;
2. the `for p in "probe_… …-OK"` runner loop in `native/tools/run-headless-probes.sh`;
3. the `--target` list in `.github/workflows/ci.yml`'s "Build probes" step.

Mutation matrix `native/tools/mutate-launchcancel.json` (committed, run via
`native/tools/mutate.py --spec`), all `expect: killed`:

- `!busy` → `busy`;
- drop the `gameRunning` disjunct;
- `!launchAfterInstall` → `launchAfterInstall`;
- swap `DemoteToInstall`/`CancelNow` in the ternary.

## Files touched

| File | Change |
|---|---|
| `native/src/core/LaunchCancel.h` | new pure decision helper |
| `native/src/core/EmulatorManager.h` | declare `cancelPendingLaunch()`, `installing_` member |
| `native/src/core/EmulatorManager.cpp` | implement the primitive; `installing_` transitions in `play()`/`install()`/`startInstall()`/`launch()` |
| `native/src/launch/GameLauncher.h` | declare `cancelPendingEmulatorLaunch()` |
| `native/src/launch/GameLauncher.cpp` | implement wrapper; call in both in-app tails |
| `native/src/ui/MainWindow.cpp` | call in the split-pane launch branch |
| `native/tools/probe_launchcancel.cpp` | new probe |
| `native/tools/mutate-launchcancel.json` | mutation matrix |
| `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml` | probe registration (3 places) |

## Verification

- `BUILD_DIR=build bash native/tools/run-headless-probes.sh` → `ALL HEADLESS PROBES PASSED`.
- `native/tools/mutate.py --spec native/tools/mutate-launchcancel.json` → all mutants killed,
  none NOT APPLIED.
- Full app build (`--target everythingbox`) clean.

## Coordination note for the sibling task

The sibling task (wait-page Stop is dead during the worker phase) should route its Stop
button through the same `cancelPendingLaunch()` when `busy_ && !game_`, falling back to
`terminateGame()` once a process exists. The primitive is public on both `EmulatorManager`
and (as `cancelPendingEmulatorLaunch()`) `GameLauncher` for exactly that reuse.
