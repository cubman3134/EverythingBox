# Unified Emulation Picker — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development.
> Steps use checkbox (`- [ ]`) syntax.

**Goal:** One engine-tagged **Emulation** picker per game + a per-system default, replacing the
split Core/Emulator + Backend selection. Design: `docs/superpowers/specs/2026-08-13-unified-emulation-picker-design.md`.

**Architecture:** A new pure `EmulationTarget` model (engine + ref + tagged display) enumerated
per system; the per-game picker and per-system default write the EXISTING `LaunchOpts::Override`
(`core`/`emulatorId`/`backend`) and `Settings` (`coreFor`/`backendFor`/new `emulatorFor`) levers —
no store migration. `prepareCore` is sourced through `resolveEmulationTarget` but keeps the 3b
RetroPark divert, vehicle-gating, and cross-platform clamp.

## Global Constraints (approved defaults)
- Drop the old global "Default emulation backend" toggle from the UI; **keep reading
  `backends/_default` as a read-fallback** (zero behavior change for anyone who set it).
- Tag every option by engine: `(libretro)` / `(retropark)` / `(standalone)`.
- Per-system picker lists all systems (as the current core picker does).
- "Stand Alone Emulators Settings" stays its own item.
- No `Override`/Settings schema change (reuse existing fields). Default (untouched) launches stay
  byte-identical. RetroPark stays opt-in and degrades to the built-in default where it can't run.
- No AI attribution; CRLF repo; build+gate SYNCHRONOUSLY; #128 app-target rule; GS_TWINS parity
  for the per-system setting; nav kit for the pickers (no QDialog); mutation-test new assertions.

---

### Task 1: EmulationTarget model + enumeration + resolution (pure, RED-first)
**Files:** create `native/src/core/EmulationTarget.h` (+ `.cpp` if needed); test in a new
`native/tools/probe_emutargets.cpp` (Qt6::Core only).
**Interfaces (produce):**
- `enum class EmuEngine { Libretro, RetroPark, Standalone };`
- `struct EmulationTarget { EmuEngine engine; QString ref; QString displayName; QString id; };`
  `id` = `"libretro:<core>"` | `"retropark"` | `"standalone:<emuId>"`.
- `QList<EmulationTarget> emulationTargetsFor(const GameSystem* sys);` — libretro system: each
  `sys->cores` as `"<core> (libretro)"`, then RetroPark (if `retroParkSupportsSystem(sys->id)`) as
  `"<sys->cores[0]> (retropark)"`; standalone system: each bound `EmulatorRegistry` emulator
  (incl. `sys->externalEmulator`) as `"<display> (standalone)"`, then RetroPark (if supported) as
  `"<externalEmulator display> (retropark)"`.
- `void applyTargetToOverride(const EmulationTarget& t, LaunchOpts::Override& ov);` — libretro:
  `ov.core=t.ref; ov.backend="libretro"; ov.emulatorId="";` retropark: `ov.backend="retropark";`
  (leave core/emulatorId empty → system default underlying); standalone: `ov.emulatorId=t.ref;
  ov.backend="";`.
- `EmulationTarget resolveEmulationTarget(const GameSystem* sys, const LaunchOpts::Override& ov,
  /*per-system defaults*/ const QString& coreDefault, const QString& emuDefault, EmuBackend backendDefault);`
  — precedence override → per-system default → system built-in; returns the effective target.

- [ ] Step 1: Write failing `probe_emutargets` assertions: `emulationTargetsFor(nes)` yields
  `{libretro:fceumm, libretro:nestopia, retropark}` with the right tagged displays;
  `emulationTargetsFor(gc)` yields `{standalone:dolphin, retropark}`; `applyTargetToOverride`
  round-trips each engine to the right Override fields; `resolveEmulationTarget` precedence
  (a per-game `backend=retropark` → retropark target; empty ov + per-system core=nestopia →
  `libretro:nestopia`; nothing set → built-in). Hand-computed oracles.
- [ ] Step 2: Run, verify fail. Step 3: Implement the header. Step 4: Run + FULL gate.
- [ ] Step 5: Register `probe_emutargets` in all THREE places. Mutation-kill the enumeration +
  `applyTargetToOverride` + precedence. Step 6: Commit.

### Task 2: `Settings::emulatorFor` per-system standalone default
**Files:** `native/src/core/Settings.h/.cpp`; assert in `probe_emusettings` (or probe_emutargets).
- [ ] `QString Settings::emulatorFor(const QString& systemId)` / `void setEmulatorFor(...)` backed
  by ini key `emulators/<systemId>` (empty = inherit `sys->externalEmulator`). Mirror `coreFor`.
- [ ] Build + gate; mutation-cover the getter/setter round-trip. Commit.

### Task 3: `prepareCore` sourced through `resolveEmulationTarget`
**Files:** `native/src/launch/GameLauncher.cpp` (`prepareCore`).
- [ ] Compute `resolveEmulationTarget(sys, ov, Settings::coreFor(sys->id), Settings::emulatorFor(sys->id),
  Settings::backendFor(sys->id))`; map the effective target onto the existing `CorePlan` fields
  (libretro→`core`/`backend=Libretro`; retropark→`backend=RetroPark` + the 3b divert incl.
  vehicle-gate/clamp/`retroParkSupportsSystem`; standalone→`externalEmulatorId`). Preserve ALL 3b
  behavior (vehicle-gated fallback to external Dolphin; cross-platform `#ifdef EB_HAVE_RETROPARK`
  clamp; NES/gc gating). Default (no override, no per-system default) must produce the identical
  CorePlan as today.
- [ ] Extend the GameLauncher probe (or probe_emutargets) to assert the effective-target→CorePlan
  mapping for the key cases; mutation-kill. Build the `everythingbox` target + gate. Commit.

### Task 4: Per-game Emulation picker — `editLaunchOptions`
**Files:** `native/src/ui/MainWindow.cpp` (`editLaunchOptions` ~6100).
- [ ] Replace the libretro **Core** row + the **Backend** row, AND the standalone **Emulator** row
  + its Backend row, with ONE **"Emulation:"** row (kind `"emulation"`) whose value is the
  resolved target's tagged display + `(default)` marker. Its handler opens a nav-kit `NavMenu` of
  `"System default (<resolved display>)"` + every `emulationTargetsFor(sys)` entry; on pick, write
  `applyTargetToOverride` into the Override via `LaunchOpts::set` (row 0 = clear all three levers).
  Keep Extra-args / gfx / pad2key / hooks rows unchanged. Remove the now-dead `"core"`/`"backend"`
  (and the standalone `"emulator"` row is replaced by `"emulation"`).
- [ ] Build the `everythingbox` target (grep whole log); FULL gate green. Commit.

### Task 5: Per-system Emulation default (both builders) + rename hub
**Files:** `native/src/ui/MainWindow.cpp` (`presentEmulatorCorePicker` ~16096; hubs ~11770/11850;
the classic `SettingsDialog` path; the global default-backend blocks ~12675/13621).
- [ ] Turn `presentEmulatorCorePicker` into a per-system **Emulation** picker: one `Choice` per
  system = `emulationTargetsFor(sys)` (tagged) + "Default"; on pick, write the trio via
  `Settings::setCoreFor`/`setEmulatorFor`/`setBackendFor` (derived from the target). Retire the
  global "Default emulation backend" `choice`/combo from BOTH builders (keep `backends/_default`
  read-fallback in `defaultBackend()`), so GS_TWINS stays balanced (removed from both).
- [ ] Rename "Libretro Emulator Settings" → "Emulation Settings" in the themed hub (`act("libretro",
  ...)` label) and classic hub (`add(tr("Libretro Emulator Settings"), ...)`), keeping the same
  handler. Update "Libretro core per system" label → "Emulation per system".
- [ ] Build + gate; **GS_TWINS parity gate green**. If a probe covers the per-system setting,
  extend it. Commit.

### Task 6: Live proof + review + merge/deploy
- [ ] EB_UITEST: a game's launch options show the "Emulation" row listing tagged targets; picking
  `(retropark)` routes to RetroPark; setting the per-system default for `gc` to `Dolphin (retropark)`
  makes a fresh gc game (incl. a streamed one) launch on RetroPark; a libretro game unaffected by
  default. Screenshots.
- [ ] Fable (or Opus if Fable overloaded) whole-branch review: no regression to default launches,
  GS_TWINS balance, the 3b divert/clamp preserved, no store migration issues, no AI attribution.
- [ ] Fix findings (one worker). Merge to `main` via a throwaway worktree off current origin/main
  (NEVER the shared tree). Deploy Release to `C:\EverythingBox-app` (exe; vehicle/Sys already
  present). Verify the picker live.

## Self-Review
- Covers the spec: model (T1), per-system storage (T2), resolution (T3), per-game picker (T4),
  per-system default + rename (T5), proof (T6).
- No placeholders; approved open-item defaults baked into Global Constraints.
- Type consistency: `EmulationTarget`/`EmuEngine`/`emulationTargetsFor`/`applyTargetToOverride`/
  `resolveEmulationTarget` used identically across tasks; reuses `LaunchOpts::Override` +
  `Settings::coreFor/emulatorFor/backendFor` + the 3b `retroParkSupportsSystem`/vehicle-gate.
