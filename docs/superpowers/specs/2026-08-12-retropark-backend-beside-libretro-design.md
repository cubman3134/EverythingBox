# RetroPark backend beside libretro — Slice 2 design

**Status:** design approved (view strategy, decomposition, gating decided 2026-08-12); ready for
implementation planning.

**Goal:** Add RetroPark as a first-class emulation backend in EverythingBox, selectable per game
beside the existing libretro path, starting with a live driven-core play surface — and make
RetroPark a *permanent* app dependency (not an opt-in build).

**Context:** This is Slice 2 of the RetroPark integration arc. Slice 1 (merged, commit
`1fd6874`) proved RetroPark's static lib builds + links into EverythingBox and a driven
reference core renders a real frame read back inside EverythingBox's process, via an opt-in
(`EB_WITH_RETROPARK=OFF`) headless probe. Slice 2 turns that spike into a real, user-selectable
backend. Presenting cores (Dolphin/RPCS3, shared-texture GPU interop) are Slice 3 and out of
scope here — everything below is the **driven**-core path (runtime hands back CPU RGBA, owns its
own audio, no GPU interop).

---

## Decisions locked (2026-08-12)

1. **View: a new `RetroParkView` sibling widget**, not a refactor of `RetroView`. `RetroView`
   (~2700 lines, hardwired to `LibretroCore`, with HW render / shaders / netplay / turbo /
   bezels / virtual pad / achievements) stays completely untouched. `RetroParkView` is the
   reusable RetroPark play surface for the whole arc (presenting cores present through it too).
2. **Decomposition: spec both, build 2a first.**
   - **Slice 2a** — permanent-dependency infrastructure + the backend seam + a minimal live
     `RetroParkView` running the *static driven refcore* (animated test pattern, no ROM, no
     audio, no extra DLLs). Proves: pick RetroPark backend for a game → a live RetroPark view
     opens and animates → pause / resume / exit work → RetroPark is linked into the shipped app
     and built in CI.
   - **Slice 2b** — real content + input + audio + savestate/rewind via the libretro shim
     (`libretro_shim` + `fceumm`): play a real NES ROM through RetroPark, with working input,
     RetroPark-owned audio, and save/load-state + rewind wired to the runtime.
3. **Gating: RetroPark is a PERMANENT dependency.** The Slice-1 opt-in `EB_WITH_RETROPARK` gate
   is retired: RetroPark links into the `everythingbox` app target always, is added as a git
   submodule, and CI builds it. The backend picker always offers "RetroPark".

---

## Global constraints

- **Driven-core path only.** No shared textures / `submit_frame` / presenting-core code. The
  host loop is `load_core` → `load_content` → per-frame `rp_runtime_present(out_rgba)` → paint.
- **Do not touch the libretro path.** `RetroView` and `LibretroCore` are unchanged. RetroPark is
  a third branch at the launcher fork, never a modification of the existing two.
- **RetroPark repo is read-only.** It is consumed as a pinned submodule + built via
  `ExternalProject` (Slice 1 established that `add_subdirectory` breaks RetroPark's own build
  because it assumes it is the top-level project — `${CMAKE_SOURCE_DIR}` is used pervasively,
  incl. its Vulkan shader-embed). Do not edit RetroPark to make it embed more nicely; if a real
  RetroPark change is needed, that is a separate change in the RetroPark repo, landed there and
  the submodule bumped.
- **No AI attribution** in commits / PR bodies / issue comments (repo `CLAUDE.md`).
- **Probe-gate discipline:** any new probe registered in all three places (add_executable in
  `native/CMakeLists.txt`, the runner in `native/tools/run-headless-probes.sh`, the `--target`
  list in `.github/workflows/ci.yml`); the gate must print `ALL HEADLESS PROBES PASSED`. CRLF
  repo: `\r?\n` in any gate/regex.
- **Settings parity (GS_TWINS):** any user-facing global/system default setting goes in BOTH
  settings builders (themed PanelRow + classic QWidget). Any modal UI goes through the nav kit
  (`src/ui/nav`), never QDialog/QMessageBox/QInputDialog.
- **CloudSync classification:** the new per-game `backend` override lives in the existing
  `LaunchOpts` store, which is already classified as per-item-synced (`launchopts/`,
  `isPerItemStoreKey`). Adding a field to `LaunchOpts::Override` does not add a new store, so no
  new CloudSync classification is required — but confirm the `LaunchOpts` CloudMerge/serialization
  round-trips the new field, and assert it in `probe_launchopts` (and, if it touches the merge,
  `probe_cloudmerge`).

---

## Architecture

### The backend seam (unchanged fork, third branch)

The launch decision already lives in `GameLauncher::prepareCore(rom, systemHint, key)`
(`native/src/launch/GameLauncher.cpp:176`), which returns a `CorePlan` consumed by
`GameLauncher::open()` (`:346`). Today `open()` forks two ways:

- standalone emulator (child process) when `plan.externalEmulatorId` is set (`:384`);
- else libretro → `finishLibretroLaunch()` (`:428`) → `RetroView::openGame(...)`.

RetroPark is a **third branch on the same fork**:

- `CorePlan` gains a backend discriminator (`enum class EmuBackend { Libretro, RetroPark }`,
  default `Libretro`; standalone remains signalled by `externalEmulatorId` as today, orthogonal).
- `prepareCore()` sets `plan.backend` from a new per-game override resolved by a pure
  `LaunchOpts::resolveBackend(systemDefault, override)`, mirroring the existing
  `resolveCore`/`resolveEmulatorId` resolvers. For a RetroPark plan it still resolves the same
  libretro core id + ROM (RetroPark-driven == running that libretro core via RetroPark's shim in
  2b; in 2a the refcore ignores content), plus the RetroPark core directory/id to load.
- `open()` routes `plan.backend == RetroPark` to a new `finishRetroParkLaunch()` →
  `RetroParkView::openGame(...)`, alongside the existing two branches. `RetroView`/`LibretroCore`
  untouched.

### The per-game lever + defaults

- `LaunchOpts::Override` (`native/src/core/LaunchOptionsStore.h:50`) gains
  `QString backend;` (empty = inherit default). Already per-game, already synced (`launchopts/`).
- A global/system default: `Settings::backendFor(systemId)` (and a plain global default),
  surfaced in BOTH settings builders (GS_TWINS). Default is `Libretro` everywhere, so existing
  behaviour is byte-identical until a user opts a game (or system) into RetroPark.
- Per-game picker: extend the existing per-game launch-options surface (the same place that
  picks core/emulator, issue #51 `LaunchOpts::set`) with a backend choice, through the nav kit.

### `RetroParkView` (new widget)

`native/src/emu/RetroParkView.{h,cpp}`, `class RetroParkView : public QWidget`. Modeled on
`RetroView`'s *shape* (a QWidget with a frame timer, a nav-kit pause menu, an `openGame(...)`
entry and a `stop()`), but backed by an `rp_runtime*` instead of a `LibretroCore`.

- **Runtime lifecycle:** `openGame(...)` creates `rp_runtime_create(RP_GFX_D3D11, nullptr)`,
  `rp_runtime_resize(w,h)`, loads the core (2a: `rp_runtime_load_static_core("refcore_driven")`
  via the StaticCoreRegistry, same as the Slice-1 probe; 2b: `rp_runtime_load_core(shimDir)` then
  `rp_runtime_load_content(romPath)`). `stop()` unloads + destroys.
- **Frame loop:** a `QTimer` at the core's fps calls `rp_runtime_present(rt, buf.data())` into a
  reused `std::vector<uint8_t>`, wraps it as a `QImage(Format_RGBA8888)`, and `update()`s.
  `paintEvent` aspect-fits the QImage into the widget (the same fit math as `RetroView::paintEvent`;
  reuse or duplicate the small helper). No shaders in 2a/2b (RetroView's #99 shader path is not
  shared here — a later polish could bring `ShaderRenderer` to RetroParkView, out of scope now).
- **Present buffer sizing:** allocate for the core's reported max geometry; the driven refcore is
  64×64, real cores larger — size from `rp_runtime_get_status`/av-info geometry after load.
- **Pause / resume / exit:** nav-kit pause menu; "Resume" → `rp_runtime_resume` + restart timer,
  Esc/menu → `rp_runtime_pause` + stop timer, "Exit" → emits `exitRequested()` (same signal
  contract MainWindow already expects from RetroView, so the surrounding show/hide plumbing is
  reused).
- **Input (2b):** map EverythingBox's live input (the `Keymap`/`Gamepad` the libretro path uses,
  or a focused subset) into `rp_input_state` (`keys[256]` VK flags for NES via the shim;
  `pad_buttons`/`pad_axes` abstract pad otherwise) and push each frame with
  `rp_runtime_set_input(port, &in)` before `present`. Two ports.
- **Audio (2b):** none from EverythingBox — RetroPark owns XAudio2 end-to-end. Documented
  limitation: EB's volume/mute does not govern RetroPark audio yet (later polish; possible future
  route through `QAudioSink` if RetroPark exposes a pull/host-sink mode).
- **Savestate / rewind (2b):** pause-menu Save/Load State → `rp_runtime_serialize_size` +
  `rp_runtime_save_state`/`load_state` (to EB's existing per-game state-slot storage). Rewind →
  `rp_runtime_set_rewind(1, cap)` once at load, then `rp_runtime_rewind()` per step-back while a
  rewind key/combo is held; the runtime owns the ring, fed implicitly by the forward `present`
  loop. Hardcore-gated the same way RetroView gates (`blockedInHardcore()`), if RA applies.

### Permanent-dependency infrastructure (Slice 2a foundation)

- **Submodule:** add RetroPark (`https://github.com/cubman3134/RetroPark.git`, pin to a specific
  commit — currently `main`@`aeaeda1`) as a git submodule under e.g. `external/RetroPark`. First
  submodule in the repo; add `.gitmodules`. `RETROPARK_DIR` defaults to the submodule path.
- **Build:** retire the `EB_WITH_RETROPARK` OFF gate. RetroPark is built (via the Slice-1
  `ExternalProject` recipe — `retropark` static lib, `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`,
  per-config `$<CONFIG>` lib path, hand-supplied transitive deps: `Vulkan::Vulkan` +
  Win D3D11/DXGI/XAudio2/ole32/ws2_32) and **linked into the `everythingbox` app target** (the
  #128 app-target rule: RetroParkView.cpp goes in the `everythingbox` source list, not only a
  probe). `find_package(Vulkan REQUIRED)` becomes an unconditional app dependency on desktop.
  Keep it desktop/WIN32-guarded (Android/iOS/non-Win excluded, as Slice 1 did) — RetroPark on
  those platforms is a separate future concern, so the backend simply isn't offered there.
- **CI (`.github/workflows/ci.yml`):**
  - `actions/checkout@v4` gains `submodules: recursive`. **RetroPark is a separate (private)
    repo, so the default `GITHUB_TOKEN` cannot clone it** — add a `token:` (a PAT/deploy-key
    secret with read access to RetroPark) to the checkout, OR make RetroPark public. **This is a
    prerequisite the user must resolve** (provide the secret name, or authorize making RetroPark
    public). Flag it explicitly; do not silently skip.
  - Install the Vulkan SDK on the Windows runner (a setup step) so `find_package(Vulkan)`
    resolves and RetroPark's shaders build.
  - Build RetroPark + `probe_retropark` (now unconditional) and run it in the gate. The probe's
    Slice-1 no-D3D11-device graceful skip (`RETROPARK-OK` on absent device) keeps CI green even
    on a GPU-less runner; on a WARP-capable runner it runs for real.
  - Note CI cost: RetroPark's ExternalProject build is added to every run.
- **Deploy:** 2a adds no runtime DLLs (static-core path). 2b ships the shim: `LibretroShim.dll`
  + its `core.json` + `fceumm_libretro.dll` under the RetroPark core dir that
  `rp_runtime_load_core` reads, deployed to `C:\EverythingBox-app` (Release). Build 2b's core
  targets from the submodule; wire the deploy copy.

---

## Verification

- **Slice 2a:**
  - `probe_retropark` (extended, or a sibling `probe_retropark_loop`): drive a LIVE loop —
    create → static core → N× `present` asserting the animation advances (frame N ≠ frame N-1),
    then `pause` (present repeats the retained frame), `resume` (advances again), destroy.
    Registered in all three places; passes in the gate; graceful skip with no device.
  - Live app proof via the EB_UITEST harness (`EB_UITEST=1`, `native/tools/uitest.py`): launch a
    game with its backend set to RetroPark, confirm a RetroPark view opens and animates, pause,
    resume, exit — no crash, libretro path unaffected for other games.
  - Confirm the default (no override) launch still uses libretro, byte-for-byte.
- **Slice 2b:**
  - Play a real NES ROM (fceumm via the shim) through RetroPark: video, input moves the game,
    audio plays; save-state then load-state restores; rewind steps back. Verified live (the
    user's eyes for gameplay/audio; a probe for the load_core→load_content→present happy path
    with the shim if a committable ROM fixture exists, else documented as live-only).

---

## Open items / risks

- **CI submodule token** (above) — hard prerequisite; needs the user's secret or a public
  RetroPark. Until resolved, CI cannot build RetroPark; 2a's CI wiring is blocked on it (the app
  build + local proof are not).
- **CI build cost/time** — RetroPark's ExternalProject on every CI run; measure and, if heavy,
  consider caching the RetroPark build artifacts by submodule SHA.
- **Audio ownership** — RetroPark-owned XAudio2 is not governed by EB's volume/mute in 2b
  (accepted limitation, later polish).
- **ABI version** — RetroPark core `core.json` declares `abi_version: 4` while the header is at
  `5`; the runtime gates on the compiled-in struct, not the JSON, so this is metadata only.
  Verify the submodule-pinned RetroPark's compiled ABI matches the headers EverythingBox links.
- **Two ports / device model** — 2a needs no input; 2b maps EB input → `rp_input_state`. Keep the
  first pass single-player (port 0), matching the driven surface RetroPark has exercised.
