# PS3 Update-Worker Cancel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Back/Esc and the wait-page Stop button cancel an RPCS3 launch while its
pre-boot PS3 update worker (or any emulator's pre-boot BIOS/keys prep) is still running,
instead of being silent no-ops that leave the manager wedged for up to ~30 minutes.

**Architecture:** `EmulatorManager` gains a public `cancelPendingLaunch()` that retires the
per-launch context object (`launchCtx_`) — since merge 339fb0a every pre-boot continuation
(BIOS/keys chains, the RPCS3 worker's boot) is bound to it, so retiring it guarantees the
game never starts — then frees `busy_` and emits the terminal `failed()` signal so the UI
dismisses the wait page. Cancel is gated to the *context-gated phase* (from `launch()`
onward): the install/download machinery's continuations are bound to `this`, not the
context, so freeing `busy_` there would let a new launch interleave with a still-running
download continuation over shared member state. A new `bootPending` signal marks entry to
the cancellable phase; GameLauncher turns it into `waitPage(..., stopVisible=true)` so the
Stop button appears. Because a cancelled launch's worker thread keeps running its
best-effort installs, `runPs3UpdateThenLaunch` gains a per-binDir in-flight guard: a
relaunch that would overlap an in-flight worker skips the update step and boots plain,
so two workers never share `.eb-ps3-updates`, the firmware backoff marker, or
`ps3-updates.json`.

**Tech Stack:** Qt 6.8.3 / C++17, MSVC, CMake ("Visual Studio 18 2026" generator).

## Global Constraints

- **No AI attribution in commits** — no `Co-Authored-By`, no "Generated with" footer (repo CLAUDE.md).
- Conventional commit prefixes (`fix:` here) per CONTRIBUTING.md.
- Build from the worktree's own `build/` dir; if reconfigure is ever needed:
  `cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DEVERYTHINGBOX_BUILD_APP=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib`
  and if CMake complains about `external/RetroPark`, run `git submodule update --init external/RetroPark` first.
- Verification vehicle is the headless probe gate (`BUILD_DIR=build bash native/tools/run-headless-probes.sh`
  with `PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH"`, `QT_QPA_PLATFORM=offscreen`,
  `QT_PLUGIN_PATH=C:/Qt/6.8.3/msvc2022_64/plugins`). EmulatorManager has no probe harness
  (Qt network/process/UI glue — same as the adjacent supersession fix bd2ace2, which shipped
  gate-verified); no new probe is added, so TDD steps are compile-and-gate, not unit tests.
- All comments follow the repo's idiom: explain constraints, not change history.

---

### Task 1: `EmulatorManager::cancelPendingLaunch()` + per-binDir worker guard

**Files:**
- Modify: `native/src/core/EmulatorManager.h` (public API after `closeGame()` at :43, new signal after `failed` at :51, members near `busy_` at :108)
- Modify: `native/src/core/EmulatorManager.cpp` (play() :176-200, install() :202-219, after closeGame() :236, launch() :1214, runPs3UpdateThenLaunch() :1395-1488)

**Interfaces:**
- Consumes: existing `launchCtx_` retirement mechanism (339fb0a), `busy_`, `game_`, `failed(QString)`.
- Produces (Task 2 relies on these exact names):
  - `void EmulatorManager::cancelPendingLaunch();` — public slot-style method.
  - `signals: void bootPending(const QString& displayName);`

- [ ] **Step 1: Declare the API in `EmulatorManager.h`**

After the `closeGame()` declaration (line 43), add:

```cpp
    // Cancel a launch still in its pre-boot phase (BIOS/keys prep, the RPCS3 firmware/update worker):
    // retire the launch context — auto-disconnecting every continuation bound to it, so the game can
    // never start — free the manager, and report the launch as failed so the UI drops the wait page.
    // No-op while the game process is running (terminateGame/closeGame own that) and during the
    // install/download machinery, whose continuations are bound to `this`, not the context — freeing
    // busy_ there would let a new launch interleave with a download continuation over shared members.
    // A cancelled RPCS3 worker keeps running its best-effort installs; runPs3UpdateThenLaunch's
    // per-binDir guard keeps a relaunch from starting a second worker over the same shared state.
    void cancelPendingLaunch();
```

After the `failed` signal (line 51), add:

```cpp
    // The context-gated pre-boot phase began (everything pending now hangs off launchCtx_), so a
    // cancelPendingLaunch() from here on is safe and complete — the host can offer a Stop control.
    void bootPending(const QString& displayName);
```

Near `busy_` (line 108), add two members (and `#include <QSet>` with the other includes at the top):

```cpp
    bool ctxGatedLaunch_ = false; // in the phase where every pending step is bound to launchCtx_
    // binDirs with an RPCS3 update worker still running. A cancelled/superseded launch's worker runs
    // to completion (best-effort installs), so a relaunch could otherwise start a second worker over
    // the same .eb-ps3-updates dir, firmware backoff marker and ps3-updates.json. Touched only on the
    // UI thread (insert at worker start, erase in a QThread::finished continuation bound to `this`).
    QSet<QString> ps3WorkersInFlight_;
```

- [ ] **Step 2: Reset the gate at ownership, set it at `launch()`, implement the cancel**

In `EmulatorManager.cpp`:

(a) In `play()`, at the ownership point (immediately after `launchCtx_ = new QObject(this);`, line 197), and identically in `install()` (after line 217), add:

```cpp
    ctxGatedLaunch_ = false; // install/download continuations bind to `this`; cancel is unsafe until launch()
```

(b) At the very top of `launch()` (line 1215, before the args-template work), add:

```cpp
    // From here on every pre-boot step binds to launchCtx_ (BIOS/keys chains, the RPCS3 worker's boot
    // continuation), so cancelPendingLaunch() is now a complete cancel. Tell the host so it can offer Stop.
    ctxGatedLaunch_ = true;
    emit bootPending(em_.displayName);
```

(c) After `closeGame()` (line 236), add the implementation:

```cpp
// Cancel a pre-boot launch. Only meaningful in the context-gated phase: retiring the context
// auto-disconnects every pending continuation — the same mechanism a superseding play()/install()
// uses — so nothing can start the game afterwards. The worker thread (if any) keeps running its
// idempotent installs, detached: it captures no members, its progress notes carry a QPointer to the
// retired context and drop, and its boot continuation was bound to the context and is now gone.
void EmulatorManager::cancelPendingLaunch()
{
    if (!busy_ || game_ || !ctxGatedLaunch_) return;
    delete launchCtx_;
    launchCtx_ = nullptr;
    ctxGatedLaunch_ = false;
    busy_ = false;
    emit failed(tr("Cancelled launching %1.").arg(em_.displayName));
}
```

- [ ] **Step 3: Per-binDir in-flight guard in `runPs3UpdateThenLaunch()`**

(a) At the top of `runPs3UpdateThenLaunch()` (line 1396, before `const QString rom = rom_;`), add:

```cpp
    // A cancelled or superseded launch's worker may still be running over this install dir. Its
    // shared state (the .eb-ps3-updates staging dir, the firmware backoff marker, ps3-updates.json)
    // is single-writer, so never start a second worker beside it: skip the update step and boot
    // plain — exactly the fallback a failed update takes.
    if (ps3WorkersInFlight_.contains(binDir))
    {
        finishLocalLaunch(program, args, binDir);
        return;
    }
```

(b) Just before `worker->start();` (line 1487), add the bookkeeping — the release is bound to
`this`, NOT `launchCtx_`, so it survives the very supersession/cancel that makes the overlap
possible (delivered queued on the UI thread, where the set lives):

```cpp
    ps3WorkersInFlight_.insert(binDir);
    connect(worker, &QThread::finished, this, [this, binDir] { ps3WorkersInFlight_.remove(binDir); });
```

- [ ] **Step 4: Compile check**

Run:
```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" cmake --build build --config Release --target EverythingBox --parallel
```
Expected: builds clean (warnings-as-before, no new errors).

- [ ] **Step 5: Commit**

```bash
git add native/src/core/EmulatorManager.h native/src/core/EmulatorManager.cpp
git commit -m "fix: cancelPendingLaunch() ends a pre-boot RPCS3 launch; per-binDir guard keeps workers from doubling"
```
(Include whatever the version-bump hook stages, if it fires.)

---

### Task 2: Wire cancel into the wait page (GameLauncher + MainWindow)

**Files:**
- Modify: `native/src/launch/GameLauncher.cpp` (`ensureEmu()` :639-718, `forceCloseEmulator()` :889-893)
- Modify: `native/src/ui/MainWindow.cpp` (waitPageStatus handler :1333-1335)

**Interfaces:**
- Consumes (from Task 1): `EmulatorManager::cancelPendingLaunch()`, `EmulatorManager::bootPending(const QString&)`.
- Produces: no new interfaces — existing `waitPage(text, stopVisible)` semantics now solely own the Stop button's visibility.

- [ ] **Step 1: Route Stop/Back through the cancel**

In `GameLauncher::forceCloseEmulator()` (line 889), replace:

```cpp
void GameLauncher::forceCloseEmulator()
{
    emuUserClosing_ = true;
    if (emu_) emu_->terminateGame();
}
```

with:

```cpp
void GameLauncher::forceCloseEmulator()
{
    emuUserClosing_ = true;
    if (!emu_) return;
    emu_->terminateGame();        // running game: hard kill (no-op pre-boot, game_ is null)
    emu_->cancelPendingLaunch();  // pre-boot: retire the launch instead (no-op once game_ exists)
}
```

MainWindow's emu-page goBack (MainWindow.cpp:2214) already clicks `emuStopBtn_`
(`QAbstractButton::click()` fires even while the button is hidden), so Back/Esc and Stop
both land here — no MainWindow goBack change is needed.

- [ ] **Step 2: Show Stop when the cancellable phase begins**

In `GameLauncher::ensureEmu()`, after the `status` connection (line 651), add:

```cpp
    // The pre-boot prep phase began (BIOS/keys prep, the RPCS3 firmware/update worker — worst case
    // ~30 min): everything pending is now cancellable, so put the Stop button up. Back/Esc and Stop
    // route through forceCloseEmulator -> cancelPendingLaunch until the process actually starts.
    connect(emu_, &EmulatorManager::bootPending, this, [this](const QString& name) {
        emit waitPage(tr("Starting %1…").arg(name), true);
    });
```

- [ ] **Step 3: Stop re-hiding the button on status ticks**

In `MainWindow.cpp` lines 1331-1335, the waitPageStatus handler currently re-hides the Stop
button on every progress line, which would defeat Step 2 the moment the worker posts a note.
Replace:

```cpp
    // Install/launch progress: refresh the wait-page label only when it's already showing — never switch to it.
    // (An install-only flow from Settings ▸ Emulators must leave the user on the settings panel.)
    connect(launcher_, &GameLauncher::waitPageStatus, this, [this](const QString& t) {
        if (emuPage_ && stack_->currentWidget() == emuPage_) { emuLabel_->setText(t); emuStopBtn_->setVisible(false); }
    });
```

with:

```cpp
    // Install/launch progress: refresh the wait-page label only when it's already showing — never switch to it.
    // (An install-only flow from Settings ▸ Emulators must leave the user on the settings panel.)
    // Label only: the Stop button's visibility is owned by waitPage alone, which every phase change
    // emits — hiding it here would strip the cancel control the moment a progress note lands.
    connect(launcher_, &GameLauncher::waitPageStatus, this, [this](const QString& t) {
        if (emuPage_ && stack_->currentWidget() == emuPage_) emuLabel_->setText(t);
    });
```

- [ ] **Step 4: Compile check**

Run:
```bash
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" cmake --build build --config Release --target EverythingBox --parallel
```
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add native/src/launch/GameLauncher.cpp native/src/ui/MainWindow.cpp
git commit -m "fix: Back/Esc and a now-visible Stop cancel a pre-boot emulator launch from the wait page"
```

---

### Task 3: Probe-gate verification

**Files:** none modified (verification only).

- [ ] **Step 1: Build the full CI probe list**

Build the probe targets named in `.github/workflows/ci.yml`'s "Build probes" step (plus
`probe_mpvpreview`) so the gate's stale-binary check passes:

```bash
cd "/c/Users/cubma/Project Goliath/.claude/worktrees/nice-spence-b8c73e" && PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" cmake --build build --config Release --target $(grep -A3 'Build probes' .github/workflows/ci.yml | grep -o 'probe_[a-z0-9_]*' | sort -u | tr '\n' ' ') probe_mpvpreview --parallel
```

(If the grep yields nothing, open the workflow file and copy the target list by hand.)

- [ ] **Step 2: Run the headless probe gate**

```bash
cd "/c/Users/cubma/Project Goliath/.claude/worktrees/nice-spence-b8c73e" && PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH="C:/Qt/6.8.3/msvc2022_64/plugins" BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected: suite PASSES (exit 0). If `probe_navqml`/`probe_formfactor` die rc=127, copy
`C:/Qt/6.8.3/msvc2022_64/bin/Qt6Test.dll` into `build/Release/` and re-run. Never accept a
"hang" claim without the full env recipe above.

- [ ] **Step 3: Commit the plan doc**

```bash
git add docs/superpowers/plans/2026-08-19-ps3-worker-cancel.md
git commit -m "docs: PS3 update-worker cancel implementation plan"
```
