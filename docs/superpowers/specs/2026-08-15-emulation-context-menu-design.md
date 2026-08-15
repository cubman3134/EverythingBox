# Emulation Context Menu (Start button) — Design

**Date:** 2026-08-15
**Status:** Approved (design), spec for implementation

## Goal

Pressing the controller **Start** button while browsing opens a context menu. When a game
(or an emulator library) is focused, that menu offers **Emulation settings** — a drill-in
panel where you pick which emulator/engine runs this content, open that emulator's own
settings, and choose whether your changes apply to **just this game** or **universally**
(the per-system default).

## Scope of v1 vs v2

- **v1 (this spec):** the Start context-menu framework; the Emulation panel; the scope
  toggle; emulator selection; scoped emulator settings — libretro **core options per-game**
  and standalone **graphics/config**; RetroPark selection (no settings button yet).
- **v2 (fast-follow, NOT in this spec):** a **Controller mapping** entry + per-engine rebind
  editor. v1 leaves a clearly-marked seam for it but ships no controller-mapping UI.

## Terminology

- **System / console** = `GameSystem` (`native/src/core/SystemCatalog.h`), keyed by `sys->id`.
- **Engine / run-target** = `EmulationTarget` (`native/src/core/EmulationTarget.h`),
  `EmuEngine{Libretro,RetroPark,Standalone}`.
- **Per-game override** = `LaunchOpts::Override` in `LaunchOptionsStore` (fields
  `core`/`emulatorId`/`backend`), plus the per-game gfx store (`EmuGfxStore`) and a **new**
  per-game core-options store.
- **Per-system default** = `Settings::coreFor/emulatorFor/backendFor` and, for libretro core
  options, `Settings::optionValue/setOptionValue`.

## The scope model

The panel always shows a **scope toggle at the top**: `Universal | This game`.

- **Universal is the default.** A game with no per-game config opens on Universal and shows
  the per-system settings.
- **Switch to "This game":** a per-game config is created, seeded from the current effective
  (universal) values. The game now has an `Override`. Opening this menu again *for this game*
  auto-lands on "This game" — the choice is **sticky per game** (detected by "does an
  Override exist for this game?").
- **Switch back to "Universal":** the game's per-game config is **discarded** — its
  `Override`, per-game gfx, and per-game core options are cleared — and the game reverts to
  the per-system defaults.

"This game" is only offered when a specific game is focused. In a console folder with no game
focused, the panel is Universal-only (no toggle, edits the per-system default).

## Trigger: Start as a context menu

`MainWindow::pollMenuPad()` (native/src/ui/MainWindow.cpp) currently maps `PAD_START` →
`Qt::Key_Escape` (Back/close). v1 adds a **browse-only** branch, evaluated **before** the
generic nav table, that:

1. Fires only when **not in-game** (the existing early-return at the top of `pollMenuPad`
   already guarantees this) **and** no `NavOverlay` is topmost (don't hijack Start while a
   menu/overlay/OSK is already open — there Start keeps its Back/close meaning).
2. Resolves the current **context** (see below). If it yields at least one applicable entry,
   Start opens the context menu (`NavMenu::pick`). Otherwise Start **falls back to today's
   Escape/Back** behavior, so nothing is lost in non-emulation contexts.

`B` remains Back (`PAD_A`→Backspace is the primary "back one level"), so repurposing Start
does not remove controller back-navigation.

The context menu is a small framework: it builds a list of entries from the current context.
In v1 the only entry is **Emulation settings** (present iff the context resolves a
`GameSystem`). It is written so more entries can be added later without touching the trigger.

## Context resolution

A new helper resolves, from the current browse state, one of:

- **`Game`**: a specific game is focused → `{ GameSystem* sys, QString gameKey, QString gamePath }`.
  Reuse `HomeView::themedLeafSystemId(themedDetailIndex_)` / `systemForGameItem` /
  `themedLeafKey` / `themedLeafGamePath`.
- **`Console`**: inside a console folder with no game focused → `{ GameSystem* sys }` derived
  from the current level (`stack_.last().item` → `SystemCatalog::forConsoleName`/`byId`, or the
  synthetic-level system marker).
- **`None`**: not a game/console context → no emulation entry (Start falls back to Back).

This resolver is a pure function of already-available inputs where possible, so its
game/console/none decision is unit-testable.

## The Emulation panel

Built with `ThemedPanelHost::present("Emulation", rows, onAct, onBack)` — the same pattern as
`presentEmulatorCorePicker` — never a `QDialog`/`QMessageBox` (nav-kit rule). Rows, top to
bottom:

1. **Scope** (only in `Game` context): a `Choice`/toggle `Universal | This game`. Initial
   value = `This game` iff an `Override` exists for `gameKey`, else `Universal`. Changing it
   runs the seed/clear logic above, then re-presents the panel (values below re-read at the
   new scope).
2. **Emulator**: a `Choice` whose value is the resolved current target
   (`resolveEmulationTarget(...)` for `This game`, or the per-system default for `Universal`)
   and whose options are "System default" (Game scope only) + `emulationTargetsFor(sys,
   retroParkAvailable)`. Applying it:
   - **This game** → `applyTargetToOverride(target, override)` + save the game's Override.
   - **Universal** → `setSystemEmulationDefault(sysId, target)` (writes
     `Settings::setCoreFor/setEmulatorFor/setBackendFor`).
3. **‹Emulator› settings** (an `Action`), routed by the *currently selected* engine:
   - **libretro** → core options editor (`editCoreOptions`), scope-aware (see below).
   - **standalone** → the EB graphics/config panel (the existing `gfx-*` rows from
     `editLaunchOptions`, extracted into a reusable panel), scope-aware via `EmuGfxStore`
     (per-game) vs the per-system gfx default.
   - **RetroPark** → **omitted** in v1 (no settings surface exists).
4. **Controller mapping** — **omitted in v1** (v2 seam). Do not render a dead row.

## Per-game core options (already built — v1 just wires the editor)

**Correction after code review:** the per-game core-option layer already exists and is already
folded at launch. Issue #95 shipped `Settings::gameHasOption/gameOptionValue/
setGameOptionValue/clearGameOptionValue/gameOptionDelta(token, core, key)`, and the libretro
launch path already folds per-game over per-system (`RetroView.cpp:453` and `:1308`, gated on
an active `gameScope`/`overrideToken_`). So v1 does **not** add a store or a fold. The only
change needed is:

- **Editor scope:** `editCoreOptions` gains a scope + game-identity argument. In `This game`
  scope it reads/writes the per-game delta (`gameOptionValue`/`setGameOptionValue`, keyed by
  the game's `gameToken`); in `Universal` it reads/writes `Settings::optionValue` as today.
  The displayed value is the folded value (per-game delta over per-core baseline over core
  default).
- **Token parity:** the panel MUST derive the game token exactly as the launch path does
  (`Settings::gameToken(PlayStats::identity(...))` → the same value RetroView uses as
  `overrideToken_`), or edits won't match at launch.
- **Clear:** switching a game to Universal clears its deltas via `clearGameOptionValue` over
  `gameOptionDelta(token, core)` keys.

Likewise per-game **graphics** (`EmuGfxStore`, per-game > per-system, resolved in
GameLauncher) and per-game **emulator override** (`LaunchOpts::Override` +
`resolveCore/resolveEmulatorId/resolveBackend`) already exist. v1 wires them into the panel;
it builds no new persistence.

## Storage summary (the per-game bundle — all stores already exist)

| Lever | Per-game (This game) | Per-system (Universal) |
|---|---|---|
| Emulator selection | `LaunchOpts::Override` (`LaunchOptionsStore::set/reset(gameKey)`) | `Settings::coreFor/emulatorFor/backendFor` |
| Graphics/config (standalone) | `EmuGfxStore::set(gameKey)` | `EmuGfxStore::systemDefault(systemId)` |
| libretro core options | `Settings::setGameOptionValue(token, core, key)` (#95) | `Settings::setOptionValue(core, key)` |

Switching to Universal clears all three for that game: `LaunchOptionsStore::reset(gameKey)`,
`EmuGfxStore::set(gameKey, {})`, and `clearGameOptionValue` over `gameOptionDelta(token,
core)`.

## Error handling / edge cases

- **No overlay hijack:** Start over an open `NavOverlay`/OSK keeps Back/close.
- **Empty context:** `None` → Start falls back to Escape/Back; no empty menu.
- **RetroPark selected:** the settings row is absent (not a disabled dead row).
- **System with only one target:** the Emulator choice still renders (shows the single
  target); selection is a no-op write.
- **Console scope (no game):** no scope toggle; edits per-system defaults only.
- **Non-RetroPark builds** (e.g. Android TV, `!kRetroParkBuildAvailable`): RetroPark never
  appears as a target (already handled by `emulationTargetsFor(..., retroParkAvailable)`).

## Testing

Pure logic → headless probes with RED-first + mutation testing; new probe registered in all
three places (native/CMakeLists.txt `add_executable`, run-headless-probes.sh runner, ci.yml
`--target`):

- **Scope resolution:** Override-present→opens `This game`; Override-absent→`Universal`;
  switch-to-This-game seeds an Override; switch-to-Universal clears Override + per-game gfx +
  per-game core options.
- **Context resolver:** focused game→`Game`; console folder→`Console`; non-game→`None`.
- **Core-options fold:** per-game value wins over per-system wins over core default.

The live Start→menu→panel flow, controller navigation of it, and actual per-game vs universal
launch behavior are the user's hands-on verification (a headless probe can't press Start or
launch an emulator).

## Files (anticipated)

- `native/src/core/EmulationScope.h` (new, header-only pure): `enum class EmuScope
  {Universal, ThisGame}`; the context-kind resolver (pure inputs → `Game`/`Console`/`None`);
  `initialScopeForGame(bool hasPerGameConfig)`. No new persistence.
- `native/src/ui/MainWindow.cpp` (modify): `pollMenuPad` Start branch; `openBrowseContextMenu`;
  `presentEmulationPanel(...)`; scope toggle + apply/clear wiring (over the existing stores);
  `editCoreOptions` scope+token param; extract the standalone `gfx-*` rows into a reusable
  scope-aware panel; a `gameHasPerGameConfig`/`clearPerGameBundle` helper over the stores.
- `native/src/ui/HomeView.{h,cpp}` (modify): a `Console`-context accessor (current-level
  system) if not already derivable from `themedLeaf*`.
- `native/tools/probe_emulation_scope.cpp` (new) + registration in the three places
  (native/CMakeLists.txt `add_executable`, run-headless-probes.sh, ci.yml `--target`).
