# Unified "Emulation" picker + per-system default — design

**Status:** design, pending user review.

**Goal:** Replace the split emulator/core/backend selection with ONE engine-tagged **Emulation**
picker per game — and a **per-system global default** — so choosing how a game runs (which
emulator, on which engine) is a single choice, and a system's default applies to every game of
that system (including streamed ones) without touching each game.

**Approved shape (user):** one list of engine-tagged run-targets — e.g. `FCEUmm (libretro)`,
`FCEUmm (retropark)`, `Dolphin (standalone)`, `Dolphin (retropark)` — replacing today's separate
Core/Emulator pick and Backend toggle; the default is expressed **per system**.

---

## Current model (origin/main, e2ea20e)

Selection is split three ways:
- **Per-game** `editLaunchOptions` (`MainWindow.cpp:~6100`): a **Core** row (libretro systems) OR
  an **Emulator** row (standalone systems), PLUS a separate **Backend** row (Libretro/RetroPark)
  — libretro systems always, standalone systems only where `retroParkSupportsSystem` (Slice 3b).
- **Global**: a single "Default emulation backend" toggle (`Settings::defaultBackend`,
  `backends/_default`) + per-system libretro-core defaults (`Settings::coreFor`, `cores/<id>`) in
  `presentEmulatorCorePicker`. No per-system standalone-emulator default; no per-system backend UI
  beyond what `backendFor` (`backends/<id>`) stores.
- **Resolution** `GameLauncher::prepareCore`: system → (standalone arm) emulator via
  `resolveEmulatorId` with the 3b RetroPark divert (gated on `retroParkSupportsSystem` +
  resolved backend == RetroPark + vehicle present), OR (libretro arm) core via `resolveCore` +
  backend via `resolveBackend(Settings::backendFor(sys->id), ov)`.
- `LaunchOpts::Override` fields: `core`, `emulatorId`, `extraArgs`, `backend`.
- RetroPark's underlying emulator per system is implicit: NES → FCEUmm shim, gc → Dolphin
  (`retroParkSystemIsPresenting` distinguishes driven vs presenting).

---

## The new model — "emulation targets"

An **emulation target** is one concrete way to run a game on a system: an `(engine, ref)` pair
with a display name. `engine ∈ { Libretro, RetroPark, Standalone }`.

- **Libretro core** → `engine=Libretro`, `ref=<core base name>`, display `"<core> (libretro)"`.
  One target per `sys->cores` entry.
- **RetroPark** → `engine=RetroPark`, `ref=""` (the system implies the underlying: NES→FCEUmm
  shim, gc→Dolphin), display `"<underlying> (retropark)"` (e.g. `FCEUmm (retropark)`,
  `Dolphin (retropark)`). Present only where `retroParkSupportsSystem(sys->id)`.
- **Standalone** → `engine=Standalone`, `ref=<ExternalEmulator id>`, display
  `"<emulator display> (standalone)"`. One per `EmulatorRegistry` entry bound to the system
  (`e.systems.contains(sys->id)`), plus the system's `externalEmulator`.

**Stable target id** (for storage/logging): `"libretro:<core>"`, `"retropark"`,
`"standalone:<emulatorId>"`.

**Enumeration:** a pure helper `emulationTargetsFor(const GameSystem*)` → ordered
`QList<EmulationTarget>`:
- libretro system: its libretro cores, then RetroPark (if supported).
- standalone system: its standalone emulator(s), then RetroPark (if supported).
Lives beside the vocabulary in `EmuBackend.h`/a small `EmulationTarget.h` (Qt-Core only, testable).

### Selection → existing storage (no store migration)

The picker is a **presentation** change; it maps a chosen target onto the EXISTING levers so the
resolvers and `prepareCore` stay intact:
- `libretro:<core>` → `Override.core=<core>`, `Override.backend="libretro"`, `emulatorId=""`.
- `retropark` → `Override.backend="retropark"` (core/emulator left to the system default).
- `standalone:<id>` → `Override.emulatorId=<id>`, `Override.backend=""` (libretro/n-a).
"System default" clears the relevant fields (empty override = inherit).

Per-system default uses the existing `Settings::coreFor`/`backendFor` PLUS a **new**
`Settings::emulatorFor(systemId)` / `setEmulatorFor` (`emulators/<id>`) for the standalone
emulator default (today only the built-in `externalEmulator` + per-game override exist). A chosen
per-system target writes the matching trio (core/emulator/backend).

---

## Changes

### 1. Enumeration + resolution helper (pure, tested)
- `EmulationTarget` struct + `emulationTargetsFor(system)` + `targetToOverrideFields(target)` +
  `resolveEmulationTarget(system, ov, perSystemDefaults)` → the effective target (override →
  per-system default → system built-in). Unit-tested, mutation-covered.

### 2. Per-game picker — `editLaunchOptions`
Replace the **Core** row (libretro) / **Emulator** row (standalone) AND the **Backend** row with
ONE **"Emulation"** row → `NavMenu` of `"System default (<resolved display>)"` + every
`emulationTargetsFor(system)` entry (engine-tagged). The handler writes the Override trio for the
picked target. Keep Extra-args / gfx / pad2key / hooks rows unchanged (per-emulator settings).

### 3. Per-system default — Settings (BOTH builders, GS_TWINS)
Extend `presentEmulatorCorePicker` into a per-system **Emulation** picker: each system → a
`Choice` of its `emulationTargetsFor(...)` (engine-tagged) + "Default" (built-in). Writes the
per-system trio. This **replaces** the single global "Default emulation backend" toggle (retired)
and subsumes "Libretro core per system". Rename the section to "Emulation per system".
`defaultBackend()` stays only as an internal fallback (or is retired — see Open items).

### 4. Rename the settings hub
"Libretro Emulator Settings" → **"Emulation Settings"** (menu item + `openEmulatorSettings`), in
both the themed hub (`:11770`) and classic hub (`:11850`). Its content (per-core options editor
`presentEmulatorCorePicker`/`SettingsDialog`) stays — "you still get per-emulator settings".
"Stand Alone Emulators Settings" stays as-is (or is grouped under the renamed hub — Open items).

### 5. Resolution — `prepareCore`
Keep the existing resolution but source it through `resolveEmulationTarget`: the effective target
decides the arm. `retropark` target → the 3b RetroPark divert (unchanged, still vehicle-gated).
`standalone:<id>` → external emulator. `libretro:<core>` → libretro core. `CorePlan` unchanged.
The `#ifdef EB_HAVE_RETROPARK` / `retroParkSupportsSystem` / vehicle-present / cross-platform
clamp behaviour is preserved (a RetroPark target on a platform/system that can't honour it
degrades to the system's built-in libretro/standalone default — no brick).

---

## Backward compatibility & migration
- No store schema change: existing `Override.core/emulatorId/backend` and
  `Settings::coreFor/backendFor` keep their meaning; the picker just sets them as a unit. Existing
  per-game/per-system choices keep working (a stored `backend=retropark` reads back as the
  RetroPark target; a stored `core=nestopia` as the `libretro:nestopia` target).
- The single global `backends/_default` (default-backend toggle) is retired from the UI; on read,
  if a system has no per-system choice it falls back to its built-in default (libretro core /
  standalone emulator), i.e. RetroPark stays opt-in per system. (Keep reading `backends/_default`
  as a fallback for one release if we want zero behaviour change for anyone who set it.)

## Testing
- `probe_*`: `emulationTargetsFor` (right targets + tags per system kind), `targetToOverrideFields`
  round-trip, `resolveEmulationTarget` precedence (override → per-system → built-in) — RED-first,
  mutation-killed.
- GS_TWINS parity gate stays green (the per-system Emulation picker present in both builders).
- Live EB_UITEST: a game's Emulation picker lists the tagged targets; picking `(retropark)` routes
  to RetroPark; a per-system default of RetroPark makes a fresh/streamed game of that system use it.
- No regression: default (untouched) launches identical to today; NES/GC both still launchable on
  every engine.

## Open items (for review)
1. **Retire `backends/_default` entirely, or keep as a fallback for one release?** (Recommend keep
   as read-fallback, drop the UI.)
2. **Standalone tag wording:** `"Dolphin (standalone)"` vs just `"Dolphin"`. (Recommend tag it, for
   symmetry and to disambiguate `Dolphin (standalone)` vs `Dolphin (retropark)`.)
3. **Per-system default UI scale:** EB has ~30+ systems. The picker lists them all (as
   `presentEmulatorCorePicker` already does for cores) — fine, but confirm.
4. **"Stand Alone Emulators Settings"** — leave separate, or nest under "Emulation Settings"?
