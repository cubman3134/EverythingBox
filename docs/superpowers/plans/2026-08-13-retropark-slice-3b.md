# RetroPark Slice 3b Implementation Plan — Dolphin (GameCube/Wii) presenting core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Play a real GameCube ISO through the RetroPark **Dolphin presenting core** inside
EverythingBox — `RetroParkView` composites Dolphin's Vulkan frames via CPU readback, with
gamepad/keyboard input, audio, and savestate — selectable as the backend for the `gc` system
beside the existing standalone-Dolphin path.

**Architecture:** Built on Slice 3a (proven: a headless `RP_GFX_VULKAN` runtime + a presenting
core render + read back inside EB via `present`→QImage→paint; `rpapi::runtimeApiForCore`
selects the API). 3b loads the **prebuilt** `dolphin_present.dll` (a full Dolphin build) staged
from the user's local RetroPark tree, boots a GC ISO via `load_core`+`load_content`, and reuses
2b's input/audio/savestate host wiring. No GPU interop with Qt.

**Tech stack:** C++17, Qt6 Widgets, RetroPark flat-C host ABI (Vulkan runtime), the prebuilt
Dolphin vehicle.

## Global Constraints

- **The Dolphin vehicle is LOCAL-ONLY.** `dolphin_present.dll` + `Data/Sys` are git-ignored,
  not in the RetroPark submodule, unbuildable by EB, and **never present on CI or another
  machine**. Only `cores/dolphin_present/core.json` is tracked (EB gets it from the submodule).
  So the Dolphin path must **degrade gracefully to absent everywhere the vehicle isn't staged**
  (CI, fresh clones): the `gc` RetroPark backend is simply not offered / `openGame` fails with a
  clear message; the standalone-Dolphin and all other paths are unaffected. EB **never ships**
  Dolphin — it stages the user's own local build on the user's own machine (this also avoids the
  Dolphin-GPLv2 vs EB-GPLv3 *distribution* question — no distribution occurs).
- **Vehicle source:** a CMake cache var `EB_DOLPHIN_VEHICLE_DIR` (default
  `C:/Users/cubma/source/repos/RetroPark/external/dolphin`). `dolphin_present.dll` is at
  `<vehicle>/Binary/x64/dolphin_present.dll`; `Data/Sys` at `<vehicle>/Data/Sys`.
- **Presenting = Vulkan headless.** Reuse 3a: the runtime for a GC/presenting game is created
  `RP_GFX_VULKAN` + null window (`rpapi::runtimeApiForCore`). Driven cores keep D3D11. Never pass
  a real window (readback would be `RP_ERR_UNSUPPORTED`).
- **GC input is the ABSTRACT PAD**, not `keys[]`: `pad_buttons` bitmask (`1u<<RP_PAD_x`:
  A=0,B=1,X=2,Y=3,L=4,R=5,SELECT=6,START=7,L3=8,R3=9,DPAD_UP/DOWN/LEFT/RIGHT=10-13) +
  `pad_axes` (`RP_AXIS_LEFT_X=0,LEFT_Y=1,RIGHT_X=2,RIGHT_Y=3,LEFT_TRIGGER=4,RIGHT_TRIGGER=5`,
  sticks −32768..32767, triggers 0..32767). Dolphin's producer maps the abstract pad to a GC pad.
- **No rewind for Dolphin** — savestates are ~94 MB; a per-frame ring is impractical. Gate
  rewind off for presenting cores. Savestate (F2/F4) still works (Dolphin serialize), namespaced
  under `states/retropark/`.
- Never modify `RetroView`/`LibretroCore` or `external/RetroPark`; no AI attribution; CRLF repo;
  build+gate SYNCHRONOUSLY; #128 app-target rule; GS_TWINS for any user-facing setting; nav kit.
- **Do not regress 2a/2b/3a**: NES (driven/D3D11) and the refcore paths stay byte-behaviorally
  identical; the standalone-Dolphin default (backend unset) still launches external Dolphin.

---

## File Structure

- **Modify** `native/CMakeLists.txt` + a new `native/tools/stage_dolphin_vehicle.cmake` — stage
  the vehicle from `EB_DOLPHIN_VEHICLE_DIR` (graceful absence).
- **Modify** `native/src/core/EmuBackend.h` — `retroParkSupportsSystem` gains `"gc"`; the
  presenting-vs-driven core kind for a system.
- **Modify** `native/src/launch/GameLauncher.cpp` — extend the backend seam to the STANDALONE
  arm so `gc` + backend=RetroPark routes to `finishRetroParkLaunch(presenting=true)` with the
  Dolphin core dir + ISO, instead of the external emulator.
- **Modify** `native/src/emu/RetroParkView.{h,cpp}` — presenting load path (Vulkan runtime +
  `load_core(cores/dolphin_present)` + `load_content(iso)`), abstract-pad input, no-rewind gate.
- **Modify** `native/src/emu/RetroParkInput.h` — an abstract-pad mapper (Qt keys + gamepad →
  `RP_PAD_*`/`RP_AXIS_*`) beside the NES `keys[]` mapper.
- **Modify** the picker (per-game + gating) to offer RetroPark for `gc`.
- **Probe(s)**: a `probe_retropark_dolphin` that DEFERS when the vehicle is absent (CI), asserts
  vehicle-present load path when local.

---

### Task 1: Stage the Dolphin vehicle (configurable path, graceful absence)

**Files:** `native/CMakeLists.txt`, `native/tools/stage_dolphin_vehicle.cmake`

- [ ] **Step 1:** Add `set(EB_DOLPHIN_VEHICLE_DIR "C:/Users/cubma/source/repos/RetroPark/external/dolphin" CACHE PATH "...")`.
- [ ] **Step 2:** POST_BUILD on `everythingbox` (desktop/WIN32): stage into
  `<exeDir>/cores/dolphin_present/`: `core.json` (from the submodule
  `${RETROPARK_DIR}/cores/dolphin_present/core.json`, tracked), and IF
  `${EB_DOLPHIN_VEHICLE_DIR}/Binary/x64/dolphin_present.dll` exists, copy it + recursively copy
  `${EB_DOLPHIN_VEHICLE_DIR}/Data/Sys` → `<exeDir>/cores/dolphin_present/Sys` (match what
  `rp_dolphin` expects for `SetUserDirectory` — verify the expected layout by reading how the
  RetroPark harness runs dolphin_present: `external/RetroPark/external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp`
  `SetUserDirectory`, and the harness `--core` path). If the DLL is absent, stage only `core.json`
  and `message(STATUS ...)` that the Dolphin vehicle is absent (NOT a warning-as-error) — the
  build must still succeed.
- [ ] **Step 3:** Build `everythingbox`; confirm (on this machine, vehicle present) that
  `<exeDir>/cores/dolphin_present/` gets `dolphin_present.dll` + `Sys/` + `core.json`. Grep build
  log for errors. Do NOT commit any binaries.
- [ ] **Step 4: Commit** (`feat: stage the Dolphin presenting vehicle from a configurable path`).

---

### Task 2: Extend the backend seam to the standalone arm (gc → RetroPark)

**Files:** `native/src/core/EmuBackend.h`, `native/src/launch/GameLauncher.cpp`

**Interfaces:** `retroParkSupportsSystem(systemId)` → true for `"nes"` AND `"gc"`. A helper for
the core KIND a RetroPark-backed system uses (nes→driven, gc→presenting) so the launcher/view can
select the runtime API + core dir.

- [ ] **Step 1: Write the failing test** in `probe_launchopts` (or the GameLauncher probe): for a
  STANDALONE system `gc` with `Override{backend="retropark"}`, `prepareCore(...).backend ==
  RetroPark` and the plan carries the ISO path + signals presenting; with no override, `gc` still
  resolves to the external emulator (`externalEmulatorId=="dolphin"`, backend Libretro). NES
  unchanged. `retroParkSupportsSystem("gc")==true`, and non-supported standalone systems clamp.
- [ ] **Step 2: Run, verify fail.**
- [ ] **Step 3: Implement.** `retroParkSupportsSystem` += `"gc"`. In `prepareCore`, BEFORE the
  standalone early-return (`sys->externalEmulator` non-empty), check: if
  `resolveBackend(Settings::backendFor(sys->id), ov) == RetroPark` AND `retroParkSupportsSystem(sys->id)`
  (and `#ifdef EB_HAVE_RETROPARK`), set `plan.backend = RetroPark`, `plan.launchRom = <iso>`, a
  marker that this is a presenting core (e.g. `plan.retroparkPresenting = true` or derive from the
  system), and DO NOT set `externalEmulatorId`/return-to-standalone — fall through so `open()`
  routes to RetroPark. Otherwise keep the existing standalone behavior exactly. Keep the libretro
  arm's 2b logic intact. Apply the same non-supported→clamp guard.
- [ ] **Step 4:** `open()`: the RetroPark branch now also fires for a (former-standalone) gc plan;
  call `finishRetroParkLaunch(...)` passing the presenting flag through.
- [ ] **Step 5:** Build + gate; mutation-kill the gc-routing + clamp assertions.
- [ ] **Step 6: Commit** (`feat: route gc+RetroPark backend to the presenting path (not standalone Dolphin)`).

---

### Task 3: RetroParkView Dolphin load path

**Files:** `native/src/emu/RetroParkView.{h,cpp}`, `native/src/launch/GameLauncher.cpp`

- [ ] **Step 1:** `finishRetroParkLaunch`/`openGame` carry `presenting` (from Task 2). When
  presenting: create the runtime with `rpapi::runtimeApiForCore(presenting)` (→ `RP_GFX_VULKAN`,
  null window, per 3a); resolve the Dolphin core dir `<coresDir>/dolphin_present`; if
  `dolphin_present.dll` is absent there, set `*error` ("Dolphin core not installed — build/stage
  the RetroPark Dolphin vehicle") and fail gracefully (host does not switch page). Else
  `rp_runtime_load_core(dolphinDir)` then `rp_runtime_load_content(isoPath)`; size geometry from
  av-info; run the present loop (reused).
- [ ] **Step 2:** No fceumm self-heal here (Dolphin path); but confirm the Dolphin user/data dir
  (`Sys`) is found — verify dolphin_present locates `Sys` relative to its own DLL dir (read
  rp_dolphin.cpp's user-dir resolution) and stage accordingly in Task 1.
- [ ] **Step 3:** Gate rewind OFF for presenting cores (no `set_rewind`); Save/Load state still
  wired (namespaced `states/retropark/`), tolerant of the ~94 MB size.
- [ ] **Step 4:** Build the `everythingbox` target (grep log). The live boot is Task 6; here prove
  it compiles/links and the absent-vehicle path is graceful.
- [ ] **Step 5: Commit** (`feat: RetroParkView boots the Dolphin presenting core + a GC ISO`).

---

### Task 4: GameCube input — the abstract pad

**Files:** `native/src/emu/RetroParkInput.h`, `native/src/emu/RetroParkView.cpp`

- [ ] **Step 1:** Add a PURE abstract-pad mapper (beside the NES `keys[]` one): Qt keys + the
  physical `Gamepad` → `pad_buttons` (`1u<<RP_PAD_x`) + `pad_axes` (`RP_AXIS_*`). Map a sensible
  keyboard default (e.g. arrows→left stick or D-pad, Z/X/A/S→A/B/X/Y, Enter→Start) AND the
  gamepad (its buttons/sticks → the abstract pad). Sticks are `RP_AXIS_LEFT_X/Y` (analog), NOT
  D-pad. Confirm nothing about Dolphin's expected mapping in rp_dolphin.cpp's input override.
- [ ] **Step 2:** In `feedInput()` (presenting/GC branch), populate `rp_input_state.pad_buttons`
  + `pad_axes` (leave `keys[]` zero) and `set_input(port 0)` before present. NES keeps the
  `keys[]` path. Single-player port 0.
- [ ] **Step 3:** Unit-test the abstract-pad mapper (RED-first, mutation-killed): a few Qt keys +
  gamepad buttons → the exact `RP_PAD_*` bits / `RP_AXIS_*` values, hand-computed oracle.
- [ ] **Step 4:** Build + gate. In-game input feel is Task 6 (live). **Commit**
  (`feat: GameCube abstract-pad input for the RetroPark Dolphin core`).

---

### Task 5: Picker gating for gc + settings

**Files:** the per-game picker + gating (`native/src/ui/MainWindow.cpp`)

- [ ] **Step 1:** The per-game Backend picker offers RetroPark for `gc` (it already keys off
  `retroParkSupportsSystem`, now true for gc — verify the picker shows for gc and writes the
  override). If the Dolphin vehicle is absent at runtime, still offer it (the launch fails
  gracefully with the install message) OR hide it — pick the less-confusing (recommend: offer it;
  the error explains). Keep the global default setting untouched (both builders already in sync).
- [ ] **Step 2:** Build + gate (GS_TWINS green). **Commit** (`feat: offer RetroPark backend for GameCube`).

---

### Task 6: Live proof + review + merge/deploy

- [ ] **Step 1:** Vehicle is staged locally. Live EB_UITEST + a GC ISO (**needs the user to
  provide an ISO path**; read-only; throwaway config): set a gc game's backend to RetroPark,
  launch → Dolphin boots and RENDERS into RetroParkView (screenshot the real game), input moves
  it, audio plays, save/load-state round-trips, exit→Home, no crash. Screenshots to scratchpad.
  (Boot may be slow; allow time.)
- [ ] **Step 2:** Fable whole-branch review — the standalone-arm routing (no regression to
  external Dolphin), the graceful-absent vehicle everywhere, the abstract-pad mapping, no rewind,
  savestate size tolerance, no AI attribution, the local-only/CI-absent handling.
- [ ] **Step 3:** Fix findings (one worker). Merge to `main` (via a throwaway worktree off current
  origin/main — NEVER the shared tree). Deploy: build Release + stage the Dolphin vehicle to
  `C:\EverythingBox-app\cores\dolphin_present\` (the deploy copies the vehicle from
  `EB_DOLPHIN_VEHICLE_DIR`). Verify RetroPark GC boots on the deployed build.

## Self-Review

- Covers the spec: vehicle staging (local-only, graceful), the standalone-arm backend extension
  (the GC-specific launcher change 2a/2b didn't need), the Vulkan presenting load, abstract-pad
  input, no-rewind, picker.
- The one genuine unknown (a GC ISO for the live proof) is flagged as user-provided in Task 6.
- Type consistency: `rpapi::runtimeApiForCore` (3a), `retroParkSupportsSystem` (+gc),
  `RP_PAD_*`/`RP_AXIS_*` (ABI), the presenting flag threaded launcher→view.
