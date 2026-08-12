# RetroPark Slice 2a Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make RetroPark a permanent EverythingBox dependency and let a game be launched with a
`RetroPark` backend (beside libretro), opening a live `RetroParkView` that runs the static driven
reference core (animated test pattern) with working pause/resume/exit — libretro path untouched.

**Architecture:** RetroPark is added as a git submodule and built (via the Slice-1 `ExternalProject`
recipe) + linked into the `everythingbox` app target, unconditionally on desktop/Windows (the
Slice-1 `EB_WITH_RETROPARK` opt-in gate is retired). A new `EmuBackend` discriminator flows from a
per-game `LaunchOpts::Override::backend` (resolved by a pure `resolveBackend`) through
`GameLauncher::CorePlan` to a third branch in `GameLauncher::open()`, which routes RetroPark games
to a new `RetroParkView` sibling widget driving `rp_runtime`. `RetroView`/`LibretroCore` are never
modified.

**Tech Stack:** C++17, Qt6 Widgets, CMake + `ExternalProject`, RetroPark flat-C host ABI
(`rp_runtime_*`), Vulkan SDK (RetroPark link dep), D3D11 (driven runtime device).

## Global Constraints

- **Driven-core path only.** Host loop is `rp_runtime_create(RP_GFX_D3D11, nullptr)` → `resize` →
  `load_static_core("refcore_driven")` → per-frame `rp_runtime_present(out_rgba)` → paint. No
  shared textures / presenting-core code (that is Slice 3).
- **Never modify `RetroView` or `LibretroCore`.** RetroPark is a third branch, never an edit to the
  existing two launch branches.
- **RetroPark repo is read-only**, consumed as a pinned submodule + `ExternalProject` build. Do not
  `add_subdirectory` it (breaks its build — it assumes it is top-level; Slice 1 established this).
- **No AI attribution** in commits / PR / issue comments (repo `CLAUDE.md`).
- **A new probe is registered in THREE places** (`add_executable` in `native/CMakeLists.txt`, the
  runner list in `native/tools/run-headless-probes.sh`, the `--target` list in
  `.github/workflows/ci.yml`) or it silently does not run. Gate must print
  `ALL HEADLESS PROBES PASSED`. CRLF repo: use `\r?\n` in any gate/regex.
- **Any new `.cpp` used by the app goes in the `everythingbox` app target** (the #128 trap), not
  only a probe. Build the `everythingbox` target and grep the WHOLE log for
  `: error|error C[0-9]|error LNK`.
- **A user-facing setting goes in BOTH settings builders** (themed PanelRow + classic QWidget); the
  GS_TWINS parity gate enforces it. **Modal UI goes through the nav kit** (`src/ui/nav`), never
  QDialog/QMessageBox/QInputDialog.
- **`LaunchOpts` is already per-item-synced** (`launchopts/`, `isPerItemStoreKey`). Adding a field
  to `Override` adds no new store, but the new field MUST round-trip through `get`/`set` and the
  CloudMerge serialization — assert it in `probe_launchopts` (and `probe_cloudmerge` if the merge
  path serializes `Override`).
- **Assertions are proven by mutation testing** (`native/tools/mutate.py`): break the guarded
  behaviour, show the assertion fails. Fixtures must not be fixed points of the code under test.
- **Default is `Libretro` everywhere.** Until a user opts a game/system into RetroPark, every
  launch behaves byte-identically to today.

---

## File Structure

- **Create** `native/src/core/EmuBackend.h` — `enum class EmuBackend { Libretro, RetroPark }` +
  `backendToString(EmuBackend)` / `backendFromString(const QString&)` (unknown → `Libretro`).
  One responsibility: the backend vocabulary, shared by the store, the launcher, and the view.
- **Create** `native/src/emu/RetroParkView.h` / `.cpp` — the RetroPark play-surface QWidget.
- **Create** `native/tools/probe_retropark_loop.cpp` — headless live-loop probe.
- **Modify** `native/src/core/LaunchOptionsStore.h/.cpp` — `Override::backend` + `resolveBackend`.
- **Modify** `native/src/core/Settings.h/.cpp` — `backendFor(systemId)` + a global default.
- **Modify** `native/src/launch/GameLauncher.h/.cpp` — `CorePlan::backend`, `prepareCore` wiring,
  `open()` third branch, `finishRetroParkLaunch`, `showRetroParkRequested`.
- **Modify** `native/src/ui/MainWindow.cpp` — a RetroParkView page shown on
  `showRetroParkRequested`, hidden on the view's `exitRequested`.
- **Modify** the per-game launch-options nav surface + BOTH settings builders — backend picker.
- **Modify** `native/CMakeLists.txt` — submodule build, retire the gate, link into the app target,
  register the loop probe.
- **Modify** `.github/workflows/ci.yml` — submodule checkout + Vulkan SDK + build (Task 6, gated on
  the token prerequisite).
- **Add** `.gitmodules` + submodule at `external/RetroPark`.

---

### Task 1: Make RetroPark permanent (submodule + unconditional app link)

**Files:**
- Create: `.gitmodules`, submodule `external/RetroPark`
- Modify: `native/CMakeLists.txt` (retire `EB_WITH_RETROPARK`; unconditional desktop build+link)

**Interfaces:**
- Produces: the `retropark` static lib linked into the `everythingbox` app target, and
  `probe_retropark` built unconditionally on desktop/Windows. `RETROPARK_DIR` now defaults to
  `${CMAKE_SOURCE_DIR}/../external/RetroPark` (the submodule).

- [ ] **Step 1: Add the submodule.**
  `git submodule add https://github.com/cubman3134/RetroPark.git external/RetroPark`, then
  `cd external/RetroPark && git checkout aeaeda1` (pin), `cd -`, `git add .gitmodules external/RetroPark`.
  (If `external/` is not the repo convention, use the sibling dir the repo already favours; keep it
  out of `native/`.)
- [ ] **Step 2: Retire the opt-in gate in `native/CMakeLists.txt`.** Remove
  `option(EB_WITH_RETROPARK ... OFF)` and the `if(EB_WITH_RETROPARK ...)` guard. Keep the desktop
  guard: wrap the RetroPark block in `if(WIN32 AND NOT ANDROID AND NOT IOS)`. Set
  `RETROPARK_DIR` default to the submodule path. Keep the Slice-1 `ExternalProject_Add(retropark_ext ...)`
  recipe verbatim (`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, `--target retropark`, per-config
  `$<CONFIG>` lib path, `BUILD_BYPRODUCTS`).
- [ ] **Step 3: Link `retropark` into the `everythingbox` app target.** On desktop/Windows, add to
  the `everythingbox` target: `add_dependencies(everythingbox retropark_ext)`,
  `target_link_libraries(everythingbox PRIVATE "${EB_RETROPARK_LIB}" Vulkan::Vulkan d3d11 dxgi dxguid d3dcompiler xaudio2 ole32 ws2_32)`,
  `target_include_directories(everythingbox PRIVATE ${RETROPARK_DIR}/include ${RETROPARK_DIR}/src)`,
  and `target_compile_definitions(everythingbox PRIVATE EB_HAVE_RETROPARK)`. `find_package(Vulkan REQUIRED)`
  is now unconditional on desktop. Keep `probe_retropark` building unconditionally too (drop its
  option guard).
- [ ] **Step 4: Build + verify.**
  `export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"`;
  `cmake -S native -B <builddir> --preset <repo preset or existing flags>`;
  `cmake --build <builddir> --config Release --target everythingbox --target probe_retropark`.
  Grep the WHOLE log for `: error|error C[0-9]|error LNK` — expect none. The app links `retropark`
  even though no app code uses it yet (a linked-but-unused lib is fine).
- [ ] **Step 5: Gate.** `BUILD_DIR=<builddir> bash native/tools/run-headless-probes.sh` →
  `ALL HEADLESS PROBES PASSED` (probe_retropark now unconditional; the runner's `findexe` guard from
  Slice 1 still works and now always finds it).
- [ ] **Step 6: Commit.** `git add .gitmodules external/RetroPark native/CMakeLists.txt` +
  `git commit -m "feat: make RetroPark a permanent dependency (submodule + app link)"`.

---

### Task 2: Backend vocabulary + store field + pure `resolveBackend` (RED-first)

**Files:**
- Create: `native/src/core/EmuBackend.h`
- Modify: `native/src/core/LaunchOptionsStore.h` (`:50` `struct Override`; add `resolveBackend` decl),
  `native/src/core/LaunchOptionsStore.cpp` (persist `backend`; implement `resolveBackend`)
- Modify: `native/src/core/Settings.h/.cpp` (`backendFor` + a global default; precedent `coreFor` at
  `Settings.h:373`)
- Test: `native/tools/probe_launchopts.cpp` (extend), and `probe_cloudmerge` if `Override` merges

**Interfaces:**
- Produces:
  - `enum class EmuBackend { Libretro, RetroPark };` + `QString backendToString(EmuBackend)` +
    `EmuBackend backendFromString(const QString&)` (unknown/empty → `Libretro`), in `EmuBackend.h`.
  - `LaunchOpts::Override` gains `QString backend;` (empty = inherit).
  - `EmuBackend LaunchOpts::resolveBackend(EmuBackend defaultBackend, const Override& ov);` — pure:
    if `ov.backend` is a recognised backend string, return it; else `defaultBackend`. (Mirror
    `resolveCore` at `LaunchOptionsStore.h:70`: an unknown/retired value must NOT error — it falls
    back to the default.)
  - `EmuBackend Settings::backendFor(const QString& systemId);` (user's per-system default; global
    default when unset) — mirror `coreFor`.

- [ ] **Step 1: Write the failing test** (in `probe_launchopts.cpp`): assert
  `resolveBackend(EmuBackend::Libretro, ov)` returns `RetroPark` when `ov.backend=="retropark"`,
  `Libretro` when `ov.backend==""`, and the passed default when `ov.backend=="nonsense"`; and that
  an `Override{ backend="retropark" }` written via `set(key, ov)` and re-read via `get(key)`
  round-trips `backend`. Compute expected values by hand (independent oracle), not by calling the
  function under test.
- [ ] **Step 2: Run it, verify it fails to compile / fails** (symbols not defined yet).
- [ ] **Step 3: Implement** `EmuBackend.h`, the `Override::backend` field + its persistence in
  `get`/`set` (match the existing key/serialization style for `core`/`emulatorId`), `resolveBackend`,
  and `Settings::backendFor` (+ global default).
- [ ] **Step 4: Run tests, verify pass.** Then run the FULL gate.
- [ ] **Step 5: Mutation-test the new assertions** (`native/tools/mutate.py` over the changed
  region): flip `resolveBackend`'s branch / the round-trip and show KILLED. Report verdicts.
- [ ] **Step 6: CloudSync check.** If `Override` is serialized in the CloudMerge path, assert in
  `probe_cloudmerge` that the merged `Override` preserves `backend`; confirm `launchopts/` stays
  `isPerItemStoreKey`. If `Override` is not in the merge path, note that in the report.
- [ ] **Step 7: Commit** (`feat: EmuBackend vocabulary + per-game backend override + resolveBackend`).

---

### Task 3: `CorePlan::backend` + launcher plumbing (branch to a RetroPark stub)

**Files:**
- Modify: `native/src/launch/GameLauncher.h` (`CorePlan` at `:38`; declare `finishRetroParkLaunch`,
  signal `showRetroParkRequested`), `native/src/launch/GameLauncher.cpp` (`prepareCore` at `:176`;
  `open()` at `:346`)

**Interfaces:**
- Consumes: `EmuBackend`, `LaunchOpts::resolveBackend`, `Settings::backendFor` (Task 2).
- Produces: `CorePlan` gains `EmuBackend backend = EmuBackend::Libretro;`. `prepareCore` sets it for
  libretro systems (standalone systems keep `externalEmulatorId`, backend left `Libretro`).
  `open()` gains a third branch: `plan.backend == EmuBackend::RetroPark` (and not standalone) →
  `finishRetroParkLaunch(plan, launchRom, recentTitle, thumb, key)`.

- [ ] **Step 1: Write the failing test.** In a GameLauncher probe (extend the existing one, or
  `probe_launchopts` if `prepareCore` is testable there; if `prepareCore` needs `SystemCatalog`,
  drive it through the same harness other GameLauncher probes use). Assert: with a per-game override
  `backend="retropark"` on a libretro system, `prepareCore(...).backend == EmuBackend::RetroPark`
  and `.core`/`.launchRom` are still resolved as before; with no override, `.backend == Libretro`;
  a standalone system is unchanged (`externalEmulatorId` set, backend `Libretro`).
- [ ] **Step 2: Run it, verify it fails.**
- [ ] **Step 3: Implement.** Add `CorePlan::backend`. In `prepareCore`, after the libretro core is
  resolved (`GameLauncher.cpp:289-314`), set
  `plan.backend = LaunchOpts::resolveBackend(Settings::backendFor(sys->id), ov)` using the `ov`
  already fetched at `:181`. Declare `finishRetroParkLaunch` + `showRetroParkRequested`; in `open()`
  add the third branch (before/after the existing libretro branch at `:428`) that calls a
  `finishRetroParkLaunch` STUB (which for now just emits `showRetroParkRequested()`; the view lands
  in Task 4). Keep the standalone branch (`:384`) and libretro branch (`:428`) intact.
- [ ] **Step 4: Run tests + full gate, verify green.**
- [ ] **Step 5: Mutation-test** the `prepareCore` backend assignment (flip default vs resolved) —
  KILLED.
- [ ] **Step 6: Commit** (`feat: CorePlan backend discriminator + RetroPark launch branch`).

---

### Task 4: `RetroParkView` live driven surface + MainWindow page + loop probe

**Files:**
- Create: `native/src/emu/RetroParkView.h/.cpp`, `native/tools/probe_retropark_loop.cpp`
- Modify: `native/CMakeLists.txt` (RetroParkView.cpp → `everythingbox` target; register
  `probe_retropark_loop` in all three places), `native/src/launch/GameLauncher.cpp`
  (`finishRetroParkLaunch` calls the view), `native/src/ui/MainWindow.cpp` (RetroParkView page)

**Interfaces:**
- Consumes: `rp_runtime_*` (retropark.h), `rp::StaticCoreRegistry`, the renamed driven getter
  `refcore_driven_static_get_core_abi` (compile `${RETROPARK_DIR}/cores/refcore_driven/RefCoreDriven.cpp`
  into the app target the same way the probe does, with
  `-Drp_get_core_abi=refcore_driven_static_get_core_abi`, so the app has the static core to load in 2a).
- Produces: `class RetroParkView : public QWidget` with `void openGame(const QString& coreOrId, const QString& romPath, const QString& title, const QString& systemId, const QString& gameKey, QString* error);`
  (2a ignores `romPath` — loads the static refcore), `void stop();`, signals `void exitRequested();`
  `void gameStopped();` (mirroring RetroView so MainWindow reuses the same show/hide contract).

- [ ] **Step 1: Write the failing loop probe** `probe_retropark_loop.cpp`: register the static core,
  `rp_runtime_create(RP_GFX_D3D11,nullptr)` → `resize(64,64)` → `load_static_core("refcore_driven")`;
  present frame A, present frame B, assert `B != A` (animation advanced — compare buffers);
  `rp_runtime_pause`, present twice, assert the two are equal (paused repeats the retained frame);
  `rp_runtime_resume`, present, assert it differs again; destroy. Print `RETROPARK-LOOP-OK` on
  success; graceful skip (still print the token) if `rp_runtime_create` returns null (no device).
- [ ] **Step 2: Register the probe in all THREE places** and run it — verify it fails first (not
  yet built), then builds and passes once wired.
- [ ] **Step 3: Implement `RetroParkView`.** QWidget with a `QTimer` present loop into a reused
  `std::vector<uint8_t>` sized to the core geometry, wrap as `QImage(Format_RGBA8888)`, `update()`;
  `paintEvent` aspect-fits into the widget (reuse the fit math from `RetroView::paintEvent`
  `:2350` — duplicate the small helper; do NOT include RetroView). Nav-kit pause menu (`src/ui/nav`):
  Resume → `rp_runtime_resume` + timer start; Esc/menu → `rp_runtime_pause` + timer stop; Exit →
  emit `exitRequested()`. `stop()` → `rp_runtime_unload_core` + `rp_runtime_destroy` + emit
  `gameStopped()`. No input/audio/shaders in 2a.
- [ ] **Step 4: Add `RetroParkView.cpp` (+ the RefCoreDriven.cpp static-core source) to the
  `everythingbox` app target** in `native/CMakeLists.txt` (#128). Wire `finishRetroParkLaunch` to
  construct/show the view via `showRetroParkRequested` and `MainWindow` to add a RetroParkView page,
  shown on that signal and hidden on `exitRequested` (mirror the RetroView page plumbing —
  `GameLauncher.cpp:440` `showRetroRequested` + MainWindow's RetroView page).
- [ ] **Step 5: Build the `everythingbox` target**, grep the whole log for errors — none. Run the
  full gate → `ALL HEADLESS PROBES PASSED` (incl. `probe_retropark_loop`).
- [ ] **Step 6: Commit** (`feat: RetroParkView live driven surface + loop probe + launch wiring`).

---

### Task 5: Backend picker UI (per-game + global/system default in both settings builders)

**Files:**
- Modify: the per-game launch-options nav surface (where core/emulator is picked, the `LaunchOpts::set`
  writer), BOTH settings builders in `native/src/ui/MainWindow.cpp` (themed PanelRow + classic QWidget)

**Interfaces:**
- Consumes: `EmuBackend`, `LaunchOpts::get/set`, `Settings::backendFor`/setter.

- [ ] **Step 1:** Add a per-game "Backend" choice (Libretro / RetroPark) to the launch-options nav
  menu, writing `Override::backend` via `LaunchOpts::set`, through the nav kit (no QDialog).
- [ ] **Step 2:** Add a global/system default "Emulation backend" setting to BOTH settings builders
  (themed + classic), writing `Settings::setBackendFor`/global — the ROMs-folder precedent for a
  two-builder setting.
- [ ] **Step 3: Build + run the GS_TWINS parity gate** — it must pass (setting present in both).
  Round-trip the setting.
- [ ] **Step 4: Commit** (`feat: RetroPark backend picker (per-game + settings default)`).

---

### Task 6: CI wiring (BLOCKED on the submodule token prerequisite)

**Files:** Modify `.github/workflows/ci.yml`

- [ ] **Step 1 (PREREQUISITE — user):** RetroPark is a separate private repo; the default
  `GITHUB_TOKEN` cannot clone it as a submodule. Resolve ONE of: (a) make `cubman3134/RetroPark`
  public, or (b) provide a repo secret (PAT/deploy key with read access) to pass as `token:` on the
  checkout. **Do not proceed until this is answered.**
- [ ] **Step 2:** `actions/checkout@v4` → add `submodules: recursive` (+ `token:` if 1b).
- [ ] **Step 3:** Add a Windows-runner step to install the Vulkan SDK (so `find_package(Vulkan)`
  resolves); build the app + `probe_retropark` + `probe_retropark_loop` targets; run them in the
  gate (the graceful no-device skip keeps a GPU-less runner green).
- [ ] **Step 4:** Note the added CI build cost; if heavy, cache the RetroPark ExternalProject build
  by submodule SHA (follow-up, not required to land).
- [ ] **Step 5: Commit** (`ci: build RetroPark submodule + run its probes`).

---

### Task 7: Live proof + close-out

- [ ] **Step 1:** EB_UITEST live proof (`EB_UITEST=1`, `native/tools/uitest.py`): set a game's
  backend to RetroPark, launch it, confirm the RetroPark view opens and animates (state/shot),
  pause, resume, exit — no crash. Confirm a different game with default backend still launches
  libretro. Follow the read-only-real-saves + throwaway-ini safety rules.
- [ ] **Step 2:** Whole-branch review (final code-reviewer subagent on the most capable model).
- [ ] **Step 3:** Merge to `main`, push, deploy Release to `C:\EverythingBox-app`, close-out.

## Self-Review

- Spec coverage: submodule+permanent (T1), backend model (T2), plan plumbing (T3), view+probe (T4),
  picker (T5), CI (T6), proof (T7) — all spec sections covered; 2b is a separate later plan.
- Placeholder scan: interfaces carry concrete signatures + file:line anchors; the one genuine
  unknown (CI token) is an explicit human prerequisite, not a placeholder.
- Type consistency: `EmuBackend` used identically across `EmuBackend.h`, `Override::backend`
  (string) ↔ `resolveBackend`→`EmuBackend` ↔ `CorePlan::backend`→`RetroParkView`. `resolveBackend`
  and `backendFor` return `EmuBackend`; `Override::backend` stores its string form.
