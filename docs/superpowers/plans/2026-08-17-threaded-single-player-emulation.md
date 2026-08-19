# Threaded Single-Player Emulation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Run the single-player libretro frame loop (core + audio) on a dedicated worker thread so GUI-thread work can no longer starve audio production — eliminating the NES crackle and the ~2% slowdown, which the deployed underrun log proved are the same root cause (GUI thread starved ~15×/sec).

**Architecture:** The `threaded_` worker path already exists for split-screen panes (`stepWorker()` on `emuThread_`, input snapshot under `inputMutex_`, frame handoff under `frameMutex_`, audio sink owned by the worker). Single-player currently forces `threaded_ = false` because save-states, rewind, cheats, achievements, and auto-resume touch the core directly from the GUI thread. This plan (a) separates the concept "runs on a worker thread" (`threaded_`) from "is a split-screen pane" (`splitPane_`, the real reason those features are disabled), (b) adds one `runOnCore()` marshaling primitive so GUI-initiated core operations execute on the worker between frames, (c) moves the per-frame core-touching extras (rewind capture, achievement evaluation) onto the worker loop, and (d) enables threading for single-player **software** cores only (`!hwMode_`); hardware-GL cores keep the current GUI-thread path because their GL context is GUI-thread-bound.

**Tech Stack:** C++/Qt6, `QThread` + `QTimer` affinity, `QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)`, existing RetroView infrastructure.

## Global Constraints

- Only `!hwMode_` (software) cores are threaded in single-player. `setupHwRender()` already sets `threaded_ = false`; that stays.
- Once running threaded, the core is touched **only** from the worker thread. Every GUI-initiated core operation goes through `runOnCore()`.
- Split-screen behavior must be **byte-for-byte unchanged**. All existing restrictions (save-states disabled, no auto-resume) now key off `splitPane_`, which is true exactly where the old split-pane construction set up two panes.
- No ABI changes. No new probe registration unless a task adds one; prefer extending `probe_*` that already exists.
- No AI attribution in commits. Conventional `fix:`/`refactor:` prefixes. Merge to main + deploy per the RetroPark auto-merge/push rule after the final proof.
- Build: `export PATH="/c/Users/cubma/.cargo/bin:/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"` then `cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib -DSDL2_INCLUDE_DIR=C:/SDL2/include -DSDL2_LIBRARY=C:/SDL2/lib/x64/SDL2.lib` then `cmake --build build --config Release --target everythingbox`. Gate: build every `probe_*` in `.github/workflows/ci.yml` then `BUILD_DIR=build/Release QT_QPA_PLATFORM=offscreen bash native/tools/run-headless-probes.sh` → require `ALL HEADLESS PROBES PASSED`.

---

### Task 1: Separate `splitPane_` from `threaded_` (no behavior change)

**Files:** Modify `native/src/emu/RetroView.h`, `native/src/emu/RetroView.cpp`.

**Interfaces:**
- Produces: `bool splitPane_` member (default false), set true only on the split-pane construction/attach path; the save-state/resume guards read it instead of `threaded_`.

**Steps:**
- Add `bool splitPane_ = false;` to RetroView.h near `threaded_`.
- Find where split-pane mode is set up (the caller that constructs a second pane / sets `threaded_ = true` for split screen — search `threaded_ = true` and the split-pane constructor/`attachSplit`-style entry). Set `splitPane_ = true` there, right where `threaded_ = true` is set for split screen.
- Change the guards whose intent is "this is a split pane" (NOT "runs on a worker") from `threaded_` to `splitPane_`:
  - Save-state disabled messages (~lines 2316, 2360: `if (threaded_) { *error = tr("Save states aren’t available in split screen."); ... }`) → `if (splitPane_)`.
  - `offerResume` skip (~line 1440 region, the resume guard) → `splitPane_`.
  - The save-on-exit auto-state (`maybeAutoSaveOnExit`/#93 path ~line 2074 `if (!running_ || threaded_ || ...)`) → use `splitPane_`.
- Leave every guard whose intent is "the core is on a worker thread" (audio sink affinity 2672/2718, `setPaused` 1626, `stopEmu` 1472, `paintEvent` 2464, input path, `stepWorker`, `reschedulePace` 1885) keyed on `threaded_` — unchanged.
- Because single-player is still `threaded_ = false` and split-pane sets both flags true, behavior is identical everywhere.

**Verify:** Full build + gate green. Confirm split-pane still reports both flags true (add a temporary `qInfo` if helpful, remove before commit).

---

### Task 2: `runOnCore()` marshaling primitive + route GUI→core ops through it (still inactive)

**Files:** Modify `native/src/emu/RetroView.h`, `native/src/emu/RetroView.cpp`.

**Interfaces:**
- Produces: `void runOnCore(const std::function<void()>& fn)` — when `threaded_ && emuThread_`, run `fn` on the worker thread via `QMetaObject::invokeMethod(workerCtx_, fn, Qt::BlockingQueuedConnection)` (where `workerCtx_` is an object living on `emuThread_`, e.g. `emuTimer_`); otherwise run `fn` inline on the caller thread. Blocking guarantees `fn` runs serialized between `stepWorker()` frames, never concurrently with `core_.runFrame()`.

**Steps:**
- Add `#include <functional>` if not present; declare and define `runOnCore`.
- Wrap the **core-touching** part (not the file I/O) of these in `runOnCore`:
  - `saveState(int,...)`: `runOnCore([&]{ ok = core_.saveState(data); });` — file write stays on the caller thread afterward.
  - `loadState(int,...)`: read file on caller thread, then `runOnCore([&]{ ok = core_.loadState(data.data(), data.size()); });`
  - Cheat apply/load/freeze/search: the `core_.*` reads/writes → `runOnCore`.
  - Core-option / input-remap apply-mid-game (any `core_.setOption`/`updateControllerPorts` invoked from GUI) → `runOnCore`.
  - `offerResume`'s state restore → `runOnCore`.
- With single-player still `threaded_ = false`, `runOnCore` runs inline — zero behavior change — but the seam is in place and compiles.

**Verify:** Full build + gate green.

---

### Task 3: Move per-frame core-touching extras onto the worker loop

**Files:** Modify `native/src/emu/RetroView.cpp`.

**Steps:**
- Factor the per-frame extras currently in the GUI `tick()` path — `captureRewind()` (when `!fastForward_`), `applyCheats/applyFreezeCheats` cadence, and `ach_->doFrame()` — so that in threaded mode they run inside `stepWorker()` (worker owns the core), gated on `!splitPane_` (split-pane keeps NOT doing resume/rewind/achievements exactly as today).
- Achievement UI side effects must marshal to the GUI: ensure any `showAchievement()`/toast triggered from `ach_->doFrame()` on the worker is posted with `Qt::QueuedConnection` (check the Achievements callback path; the memory read is fine on the worker — it owns the core — but the popup is a widget).
- Rewind playback (consuming one state per tick) must also run on the worker in threaded mode.
- Keep the GUI `tick()` path (non-threaded) exactly as is.

**Verify:** Full build + gate green. Split-pane unchanged.

---

### Task 4: Enable threading for single-player software cores + wire input/menu/teardown

**Files:** Modify `native/src/emu/RetroView.cpp`, `native/src/emu/RetroView.h`.

**Steps:**
- At load/`startEmu`, for the single-player full-screen view, set `threaded_ = true` when `!hwMode_` (software core) and `!splitPane_`. Hardware cores stay `threaded_ = false`.
- Ensure the input path runs for single-player-threaded: `inputTimer_ → pollInput()` snapshots input for the worker (it already exists for split-pane). Confirm the full-screen menu combo (Start+Select) + `handleMenuPad()` still work from `pollInput()` (they do — pollInput handles the combo and menu nav).
- Confirm `setPaused`, `showMenu`/`hideMenu`, and `stopEmu` threaded branches drive `emuThread_`/`emuTimer_` correctly for single-player (they are already threaded-aware).
- Audio sink is created on the worker (`emuThread_` started lambda calls `startAudio`) — already the case in the threaded branch.
- Remove/------- the underrun diagnostic can stay (it will now rarely fire).

**Verify:** Full build + gate green.

---

### Task 5: Live proof + review + merge + deploy

**Steps:**
- Build Release, deploy to `C:\EverythingBox-app`, launch a software NES core (fceumm) and confirm via the app log that `emu: N audio underrun(s)` no longer appears (or is near-zero) and the game runs at full speed with correct-pitch audio.
- Manually verify save state (F2), load state (F4), rewind (R), a cheat, and (if logged in) an achievement still work — these now marshal to the worker.
- Confirm split-screen still works and still (correctly) disables save states.
- Fable review of the whole branch (concurrency focus: every `core_` touch is either on the worker or inside `runOnCore`; no data race on `paused_`/`running_`/counters).
- Merge to origin/main, deploy, report.
