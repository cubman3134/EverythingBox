# RetroPark Slice 2b Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play a real NES ROM through the RetroPark backend end to end — `RetroParkView` loads
the libretro shim (FCEUmm) + the ROM, feeds controller/keyboard input, plays audio, and supports
save/load-state + rewind — proving RetroPark's DRIVEN real-content pipeline inside EverythingBox.

**Architecture:** Slice 2a built the seam + `RetroParkView` running the static driven refcore.
2b extends `RetroParkView` to the DYNAMIC core path: `rp_runtime_load_core("<app>/cores/libretro_shim")`
then `rp_runtime_load_content(romPath)`, driving the same `present` loop. Input is captured and
pushed via `rp_runtime_set_input`; audio is owned by RetroPark's own XAudio2 (nothing to wire on
the EB side); save/load-state + rewind call the runtime directly. The build additionally builds
RetroPark's `LibretroShim` target and the deploy ships `cores/libretro_shim/` (shim + core.json +
a copy of EB's existing `fceumm_libretro.dll`).

**Tech Stack:** C++17, Qt6 Widgets, RetroPark flat-C host ABI (`rp_runtime_load_core`,
`load_content`, `set_input`, `save_state`/`load_state`, `set_rewind`/`rewind`), CMake ExternalProject.

## Global Constraints

- **NES / FCEUmm only.** RetroPark's shipped `libretro_shim` `core.json` hardwires
  `libretro_core: fceumm_libretro.dll`. So the RetroPark backend can drive REAL content only for
  NES in 2b. The backend picker must only OFFER RetroPark for NES systems (or, if offered
  elsewhere, `openGame` must fail with a clear "RetroPark supports NES only for now" message and
  NOT switch to the page). Broader systems need a RetroPark-side shim change — explicitly deferred.
- **Driven-core path only** (no presenting cores / shared textures — Slice 3).
- **Never modify `RetroView`/`LibretroCore`; RetroPark repo is READ-ONLY** (consumed via the
  submodule + ExternalProject; if the shim genuinely needs a change, that is a separate RetroPark
  commit + submodule bump, not an edit here).
- **Do not regress Slice 2a.** The static-refcore path stays working (e.g. as a fallback when no
  ROM / a non-shim core id); 2b adds the real-content path beside it.
- **No AI attribution**; CRLF repo (`\r?\n`); build+gate SYNCHRONOUSLY; grep the WHOLE log; the
  #128 app-target rule; nav kit / QFrame-overlay precedent for any menu; **never touch the user's
  real ROMs/saves except read-only**, and the live proof uses a throwaway config + a test/owned
  NES ROM (read-only).
- **Audio ownership:** RetroPark owns XAudio2 end to end. EB's volume/mute does NOT govern it
  (accepted 2b limitation; note it). Do not try to route RetroPark audio through `QAudioSink`.
- **Deploy adds runtime DLLs this time:** `cores/libretro_shim/{LibretroShim.dll, core.json,
  fceumm_libretro.dll}` must be present beside the app for `load_core` to succeed.

---

## File Structure

- **Modify** `native/CMakeLists.txt` — build RetroPark's `LibretroShim` (+ `retropark_libretro_convert`)
  via the existing `retropark_ext` ExternalProject; add a post-build/deploy step (or documented
  copy) that places `cores/libretro_shim/{LibretroShim.dll,core.json}` + a copy of the repo's
  `cores/fceumm_libretro.dll` beside the app exe.
- **Modify** `native/src/emu/RetroParkView.h/.cpp` — real-content load path, input capture +
  `rp_runtime_set_input`, save/load-state + rewind, geometry from the shim's av-info.
- **Modify** `native/src/launch/GameLauncher.cpp` — `finishRetroParkLaunch` passes the resolved
  core + real ROM path (already carries them) and the shim dir; gate RetroPark to NES.
- **Modify** the backend picker (Task 5's surfaces) — only offer RetroPark where supported (NES).
- **Modify** `native/tools/probe_retropark_loop.cpp` (or a new `probe_retropark_content`) — a
  headless real-content proof IF a tiny committable/free NES test ROM is available; else document
  as live-only.
- **Modify** `native/tools/run-headless-probes.sh` + the `retropark-windows` CI job if a content
  probe is added.

---

### Task 1: Build + deploy the libretro shim (FCEUmm)

**Files:** `native/CMakeLists.txt`

- [ ] **Step 1:** In the `retropark_ext` ExternalProject `BUILD_COMMAND`, also build the shim:
  `--target LibretroShim` (it links `retropark_libretro_convert`, built transitively). Add its
  output (`${EB_RETROPARK_BUILD_DIR}/$<CONFIG>/cores/libretro_shim/`) as a known dir.
- [ ] **Step 2:** Add a post-build copy on the `everythingbox` target (desktop/WIN32) that stages
  `cores/libretro_shim/{LibretroShim.dll, core.json}` from the ExternalProject output AND copies
  the repo's already-shipped `fceumm_libretro.dll` (find where EB stages its cores;
  `C:\EverythingBox-app\cores\fceumm_libretro.dll` is the deployed precedent) into
  `cores/libretro_shim/` beside the shim, next to the built exe — so a dev/run build has the shim
  discoverable at `<exeDir>/cores/libretro_shim`.
- [ ] **Step 3: Build** the `everythingbox` target; grep the whole log for errors. Confirm the
  three shim files land in `<buildOutputDir>/cores/libretro_shim/`.
- [ ] **Step 4: Commit** (`feat: build + stage the RetroPark libretro shim (FCEUmm) beside the app`).

---

### Task 2: `RetroParkView` real-content load path

**Files:** `native/src/emu/RetroParkView.h/.cpp`, `native/src/launch/GameLauncher.cpp`

**Interfaces:** `openGame` already takes `(coreOrId, romPath, title, systemId, gameKey, error)`.

- [ ] **Step 1:** In `RetroParkView::openGame`, branch: if `romPath` is non-empty AND the system is
  NES (or `coreOrId` maps to the shim), load the DYNAMIC shim:
  `rp_runtime_load_core(rt, "<appDir>/cores/libretro_shim")` (resolve the app dir the same way EB
  resolves its cores dir — reuse that helper), then `rp_runtime_load_content(rt, romPath.toUtf8())`.
  On any failure, set `*error`, tear down (destroy+null), and return (host shows the message, does
  not switch pages). Keep the static-refcore path as the fallback when there is no ROM.
- [ ] **Step 2:** Size `buf_`/geometry from the loaded core's av-info geometry (query after load —
  the runtime re-queries `get_av_info` on `load_content`; use the runtime status/geometry call the
  Slice-1/2a code already uses, or the max-geometry the ABI exposes), not the hardcoded 64×64.
- [ ] **Step 3:** In `GameLauncher::finishRetroParkLaunch`, pass the resolved libretro core id +
  the real `launchRom` into `retroPark_->openGame(...)` (they are already in the `CorePlan`).
- [ ] **Step 4:** Build the app; grep for errors. Full gate green.
- [ ] **Step 5: Commit** (`feat: RetroParkView plays real content via the FCEUmm shim`).

---

### Task 3: Input — feed EB input to the driven core

**Files:** `native/src/emu/RetroParkView.{h,cpp}`

**Interfaces:** `rp_runtime_set_input(rt, port, const rp_input_state*)`; `rp_input_state{ uint8_t
keys[256]; int16_t pad_axes[8]; uint16_t pad_buttons; }` (VK-code flags in `keys[]` for NES via the
shim — the shim maps arrows/Z/X/Enter/Shift from `keys[]`, NOT the abstract pad).

- [ ] **Step 1:** Capture live input in `RetroParkView` (keyboard held-keys via key events; the
  physical `Gamepad` the libretro path uses — reuse `Gamepad`/`Keymap` if cleanly reusable, else a
  minimal keyboard map). Each `tick()` BEFORE `present`, build an `rp_input_state` for port 0:
  set the VK-code bytes the shim reads (Up/Down/Left/Right, Z=A, X=B, Enter=Start, Shift=Select —
  confirm the exact VKs from RetroPark's `LibretroShim.cpp` NES mapping), and call
  `rp_runtime_set_input(rt, 0, &in)`.
- [ ] **Step 2:** Single-player only (port 0) in 2b — match the surface RetroPark has exercised.
- [ ] **Step 3:** Build + gate. Input movement is human-verified in Task 6 (live).
- [ ] **Step 4: Commit** (`feat: RetroParkView feeds keyboard/pad input to the driven core`).

---

### Task 4: Audio (verify) + save/load-state + rewind

**Files:** `native/src/emu/RetroParkView.{h,cpp}`

- [ ] **Step 1: Audio** — RetroPark owns XAudio2; confirm audio plays with no EB-side wiring (it
  should, once content is loaded). Add NOTHING unless it doesn't; document the volume/mute
  limitation in a comment.
- [ ] **Step 2: Save/Load state** — add to the pause menu (or quick keys mirroring RetroView's
  F2/F4) actions that call `rp_runtime_serialize_size` + `rp_runtime_save_state(buf,size)` /
  `rp_runtime_load_state(buf,size)`, persisting to EB's existing per-game state storage (find how
  RetroView writes state slots — `saveState(slot)`/`loadState(slot)`, `showStateSlots` — and reuse
  the same on-disk location/naming, or a RetroPark-namespaced sibling). Do NOT collide with
  libretro state files for the same game (use a distinct suffix/dir so a game played on both
  backends keeps separate states).
- [ ] **Step 3: Rewind** — `rp_runtime_set_rewind(rt, 1, cap)` once after load; while a rewind
  key/combo is held, call `rp_runtime_rewind(rt)` per step-back (the runtime owns the ring, fed by
  the forward `present` loop). Mirror RetroView's rewind trigger where sensible.
- [ ] **Step 4:** Build + gate. Round-trip a save/load in a headless content probe if a test ROM
  exists (assert `serialize_size>0`, save→advance→load restores); else live-only (Task 6).
- [ ] **Step 5: Commit** (`feat: RetroParkView audio + savestate + rewind via the runtime`).

---

### Task 5: Gate the backend picker to supported systems

**Files:** the Task-5(2a) picker surfaces (`native/src/ui/MainWindow.cpp` per-game + settings)

- [ ] **Step 1:** Only offer the RetroPark backend option for NES systems in the per-game picker
  (and, if the global default is RetroPark, non-NES games silently fall back to libretro — reuse
  the resolve/clamp pattern: extend `resolveBackend` or the launcher to treat RetroPark on a
  non-NES system as Libretro, so a global RetroPark default never bricks non-NES launches). Keep it
  data-driven (a supported-systems check), not a hardcoded string sprinkle.
- [ ] **Step 2:** Build + gate (GS_TWINS still green). Add/adjust a `probe_launchopts` assertion
  for the non-NES→Libretro clamp if practical (mutation-kill it).
- [ ] **Step 3: Commit** (`feat: offer RetroPark backend only where the shim supports the system`).

---

### Task 6: Live proof + close-out

- [ ] **Step 1:** Build Release; deploy the shim (`cores/libretro_shim/…`) beside a THROWAWAY app
  copy. Live EB_UITEST + a real/owned/free NES ROM (read-only): launch on RetroPark → game renders
  → input moves the game → audio plays → save-state → load-state restores → rewind steps back →
  exit→Home, no crash. Capture screenshots. (This step needs the user's eyes for gameplay/audio.)
- [ ] **Step 2:** Whole-branch review (fable) — check the state-file non-collision, the non-NES
  clamp, the shim-load failure path, no 2a regression, no AI attribution. Fix findings (one worker).
- [ ] **Step 3:** Merge to `main`, push, deploy Release + the shim DLLs to `C:\EverythingBox-app`,
  close-out. (Deploy now includes `cores/libretro_shim/` — verify it lands.)

## Self-Review

- Covers the spec's 2b bullet (real content + input + audio + savestate/rewind via the shim).
- The FCEUmm-only constraint is made explicit and handled (picker gating + non-NES→Libretro clamp).
- No placeholders; the one genuine unknown (a committable NES test ROM for a headless content probe)
  is flagged with a live-only fallback.
- Type consistency: `rp_input_state`/`set_input`, `save_state`/`load_state`/`serialize_size`,
  `set_rewind`/`rewind`, `load_core`+`load_content` match the RetroPark ABI mapped in the arc notes.
