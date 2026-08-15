# Emulation Context Menu (Start) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A controller **Start** press while browsing opens a context menu whose "Emulation
settings" entry drills into a panel to pick the emulator, open that emulator's settings, and
scope changes to **this game** or **universally** — reusing the per-game/per-system stores
that already exist.

**Architecture:** v1 is almost entirely UI wiring over existing machinery. The per-game bundle
(emulator `Override`, gfx `EmuGfxStore`, core-option deltas `Settings::gameOptionValue` #95)
and its launch-time folding already exist. New code: one pure scope/context helper header +
its probe; a Start branch in `pollMenuPad`; a `ThemedPanelHost` panel; and a scope+token
parameter on `editCoreOptions`. No new persistence. Controller mapping is v2 (out of scope).

**Tech Stack:** C++17, Qt6 (Widgets + QML themed surface), the `src/ui/nav` kit
(`ThemedPanelHost`/`NavMenu`/`NavOverlay` — never QDialog/QMessageBox), headless probe gate.

## Global Constraints

- **Nav-kit only:** all on-top UI goes through `src/ui/nav` (`ThemedPanelHost::present`,
  `NavMenu::pick`). No `QDialog`/`QMessageBox`/`QInputDialog`/top-level windows. `probe_nav` gates this.
- **Two settings builders:** any user-facing settings surface reachable from the classic
  Settings dialog must be added to BOTH the themed and the QWidget builder, or it's
  unreachable. (The Start panel itself is a new controller-first surface, not a Settings-dialog
  row, so this applies only if a task also touches the Settings hub.)
- **New probe registered in THREE places:** `add_executable` in `native/CMakeLists.txt`, a
  runner line in `native/tools/run-headless-probes.sh`, and a `--target` in the CI workflow
  (`.github/workflows/ci.yml`). The gate must print `ALL HEADLESS PROBES PASSED`.
- **No AI attribution** in commits/PRs (repo CLAUDE.md).
- **RetroPark gating:** RetroPark only appears as a target where `retroParkAvailable`
  (`kRetroParkBuildAvailable`, `#ifdef EB_HAVE_RETROPARK`) — non-RetroPark builds must be
  unaffected.
- **Token parity:** per-game core-option edits MUST use the same game token the launch path
  uses (`Settings::gameToken(PlayStats::identity(...))`, RetroView's `overrideToken_`).
- **Start is browse-only:** the new Start behavior fires only in the browse UI (the in-game
  early-return in `pollMenuPad` already guarantees not-in-game) and only when no `NavOverlay`
  is topmost; otherwise Start keeps its current Escape/Back meaning.

## File map

- `native/src/core/EmulationScope.h` — NEW, header-only, pure. Scope enum, context-kind
  resolver, initial-scope. QtCore-free where possible (takes plain bools/enums).
- `native/tools/probe_emulation_scope.cpp` — NEW. RED-first + mutation for the pure helper.
- `native/src/ui/MainWindow.{h,cpp}` — MODIFY. Start branch in `pollMenuPad`;
  `openBrowseContextMenu()`; `emuMenuContext()`; `presentEmulationPanel(ctx)`; scope
  apply/clear helpers; `editCoreOptions` scope+token overload; extract `presentEmuGfxPanel`.
- `native/src/ui/HomeView.{h,cpp}` — MODIFY (only if needed). A current-console-system
  accessor for the `Console` context.

---

## Task 1: EmulationScope pure helper + probe

**Files:**
- Create: `native/src/core/EmulationScope.h`
- Create/Test: `native/tools/probe_emulation_scope.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces — Produces:**
```cpp
namespace emuscope {
    enum class Scope { Universal, ThisGame };
    enum class ContextKind { None, Console, Game };
    // A game is focused iff gameSysResolved; else a console folder iff consoleSysResolved; else None.
    ContextKind contextKind(bool gameFocused, bool gameSysResolved, bool consoleSysResolved);
    // The panel opens on ThisGame exactly when the focused game already has any per-game config.
    Scope initialScope(bool hasPerGameConfig);
}
```

- [ ] **Step 1: Write the failing probe** — `native/tools/probe_emulation_scope.cpp`, a
  `main()` returning non-zero on any failed assertion and printing `probe_emulation_scope:
  PASSED` on success (match the existing probe style — see `probe_emutargets.cpp`). Oracles:
  - `contextKind(true, true, _) == Game` (focused game with a resolved system).
  - `contextKind(true, false, true) == Console` (focused row isn't override-capable, but we're
    in a console folder) and `contextKind(false, false, true) == Console`.
  - `contextKind(false, false, false) == None`; `contextKind(true, false, false) == None`.
  - `initialScope(true) == ThisGame`; `initialScope(false) == Universal`.

- [ ] **Step 2: Run it, verify it FAILS to compile** (`EmulationScope.h` not present).

- [ ] **Step 3: Implement `native/src/core/EmulationScope.h`** — header-only `inline`
  functions matching the interface. `contextKind`: return `Game` if `gameFocused &&
  gameSysResolved`; else `Console` if `consoleSysResolved`; else `None`. `initialScope`:
  ternary on `hasPerGameConfig`.

- [ ] **Step 4: Register the probe in THREE places** — `add_executable(probe_emulation_scope
  tools/probe_emulation_scope.cpp)` (+ any `target_link_libraries`/`target_include_directories`
  the sibling probes use) in `native/CMakeLists.txt`; a runner line in
  `native/tools/run-headless-probes.sh`; a `--target probe_emulation_scope` in
  `.github/workflows/ci.yml`. Grep an existing probe name (e.g. `probe_emutargets`) to copy
  all three call sites exactly.

- [ ] **Step 5: Build + run the probe; verify PASS.** Configure/build with the SDL+Qt+mpv
  deploy flags (see the Build note at the plan foot). Run `probe_emulation_scope`; expect
  `PASSED`.

- [ ] **Step 6: Mutation test** — flip `contextKind`'s precedence (return `Console` before
  `Game`) and drop the `gameSysResolved` guard; confirm the probe FAILS each; revert. Record
  the verdict.

- [ ] **Step 7: Commit** — `feat: EmulationScope pure scope/context helper + probe`.

---

## Task 2: Per-game bundle helpers (has / clear) over the existing stores

**Files:**
- Modify: `native/src/ui/MainWindow.h` (declare), `native/src/ui/MainWindow.cpp` (define)

**Interfaces — Produces (private MainWindow methods):**
```cpp
// True if the game already has ANY per-game lever set (emulator override, gfx, or core-option delta).
bool gameHasPerGameConfig(const QString& gameKey, const QString& token, const QString& core) const;
// Discard the game's entire per-game bundle -> reverts to per-system defaults.
void clearPerGameBundle(const QString& gameKey, const QString& token, const QString& core);
```

**Interfaces — Consumes (all existing):** `LaunchOpts::has/reset(gameKey)`; `EmuGfxStore`
`get/set(gameKey)` (all-unset removes) + a way to test emptiness (`EmuGfx::Settings::isEmpty`
or equivalent — check `EmuGfxStore.h`/`EmuSettings.h`); `Settings::gameOptionDelta(token,
core)` + `clearGameOptionValue(token, core, key)`.

- [ ] **Step 1:** Read `EmuGfxStore.h`, `EmuSettings.h`, `Settings.h:434-441`,
  `LaunchOptionsStore.h` to confirm exact method names/emptiness test.

- [ ] **Step 2: Implement `gameHasPerGameConfig`** — returns `LaunchOpts::has(gameKey) ||
  !EmuGfxStore::get(gameKey).isEmpty() || !Settings::gameOptionDelta(token, core).isEmpty()`.
  Guard empty `token`/`core` (a game whose core is unknown just checks the other two).

- [ ] **Step 3: Implement `clearPerGameBundle`** — `LaunchOpts::reset(gameKey)`;
  `EmuGfxStore::set(gameKey, EmuGfx::Settings{})`; for each key in
  `Settings::gameOptionDelta(token, core)`: `Settings::clearGameOptionValue(token, core, key)`.

- [ ] **Step 4: Build** the app target; confirm it links (whole-log error grep = 0). These
  helpers are exercised live via the panel (Task 5); no standalone probe (they are thin store
  orchestration — a probe would just restate the store APIs).

- [ ] **Step 5: Commit** — `feat: per-game bundle has/clear helpers over existing stores`.

---

## Task 3: `editCoreOptions` becomes scope-aware

**Files:**
- Modify: `native/src/ui/MainWindow.h:497`, `native/src/ui/MainWindow.cpp:16088` (`editCoreOptions`)

**Interfaces — Produces:** an overload/param:
```cpp
// scope==ThisGame reads/writes the per-game delta (token, core); Universal reads/writes Settings::optionValue.
void editCoreOptions(const QString& systemId, emuscope::Scope scope, const QString& token);
```
Keep the existing 1-arg `editCoreOptions(systemId)` as `editCoreOptions(systemId,
Scope::Universal, QString())` so the current Settings-hub caller (MainWindow.cpp:16072) is
byte-unchanged.

- [ ] **Step 1:** Read `editCoreOptions` (MainWindow.cpp:16088+) fully — how it resolves the
  core name from `systemId`, lists `CoreOption`s, renders each as a `PanelRow::Choice`, and
  persists via `Settings::optionValue/setOptionValue`.

- [ ] **Step 2: Thread scope/token through.** For each option row:
  - **Displayed value** = folded: `ThisGame && gameHasOption(token, core, key)` →
    `gameOptionValue(token, core, key)`, else `optionValue(core, key)`, else the core's
    default (unchanged fallback).
  - **On change:** `ThisGame` → `setGameOptionValue(token, core, key, val)` (or
    `clearGameOptionValue` when the user picks "Default/inherit"); `Universal` →
    `setOptionValue(core, key, val)` as today.
  - Add a one-line header/info row showing the active scope ("Scope: This game" / "Universal")
    so the user knows which layer they're editing.

- [ ] **Step 3: Build + gate.** No new pure logic to probe (the fold already exists and is
  used at launch); verified live in Task 8. Confirm the app links and the existing
  Universal-path behavior is unchanged (the 1-arg caller still compiles and routes to
  `setOptionValue`).

- [ ] **Step 4: Commit** — `feat: scope-aware editCoreOptions (per-game core-option deltas)`.

---

## Task 4: Extract a reusable, scope-aware standalone-graphics panel

**Files:**
- Modify: `native/src/ui/MainWindow.{h,cpp}` — factor the `gfx-*` rows out of
  `editLaunchOptions` (MainWindow.cpp:6155-6176) into `presentEmuGfxPanel(...)`.

**Interfaces — Produces:**
```cpp
// Present the resolution/aspect/vsync/renderer/MSAA rows for `emulatorId` on `systemId`.
// scope==ThisGame edits EmuGfxStore(gameKey); Universal edits the per-system gfx default.
void presentEmuGfxPanel(const QString& systemId, const QString& emulatorId,
                        emuscope::Scope scope, const QString& gameKey);
```

- [ ] **Step 1:** Read `editLaunchOptions` gfx rows (MainWindow.cpp:6155-6176) and
  `EmuGfxStore.h` (`get(key)`, `set(key,s)`, `systemDefault(systemId)`).

- [ ] **Step 2: Extract** the gfx-row construction into `presentEmuGfxPanel`. `ThisGame` reads
  `EmuGfxStore::get(gameKey)` (falling back to `systemDefault` for display) and writes
  `EmuGfxStore::set(gameKey, s)`; `Universal` reads/writes the per-system default layer. Have
  `editLaunchOptions` call the extracted function (behavior byte-identical for its existing
  per-game path) so there's a single implementation.

- [ ] **Step 3: Build + gate.** Confirm `editLaunchOptions`' gfx rows still behave identically
  (per-game scope). Live gfx verification in Task 8.

- [ ] **Step 4: Commit** — `refactor: reusable scope-aware presentEmuGfxPanel`.

---

## Task 5: `emuMenuContext()` — resolve the browse context

**Files:**
- Modify: `native/src/ui/MainWindow.{h,cpp}`; possibly `native/src/ui/HomeView.{h,cpp}` for a
  console-system accessor.

**Interfaces — Produces:**
```cpp
struct EmuMenuContext {
    emuscope::ContextKind kind = emuscope::ContextKind::None;
    const GameSystem* sys = nullptr;   // resolved system (Game or Console)
    QString gameKey;                   // LaunchOptions key (Game only)
    QString gamePath;                  // local file path (Game only)
    QString token;                     // Settings::gameToken(PlayStats::identity(...)) (Game only)
    QString core;                      // resolved libretro core base name for the game (Game only; may be "")
};
EmuMenuContext emuMenuContext() const;
```

**Interfaces — Consumes (existing):** `themedDetailIndex_`, `HomeView::themedLeafSystemId(idx)`,
`themedLeafKey(idx)`, `themedLeafGamePath(idx)`, `systemForGameItem`; `SystemCatalog::byId/
forConsoleName`; `PlayStats::identity` + `Settings::gameToken`; the resolved core for a game
(same derivation `prepareCore`/`resolveEmulationTarget` uses — reuse, do not reinvent).

- [ ] **Step 1:** Read the focused-leaf accessors (HomeView.cpp:5753-5782, MainWindow.cpp:5653)
  and how `prepareCore`/`resolveEmulationTarget` derive the core + token for a game, so
  `emuMenuContext` reproduces the SAME token and core the launch path uses.

- [ ] **Step 2: Implement Game branch** — when a leaf is focused and `themedLeafSystemId`
  resolves, fill `{Game, sys, gameKey, gamePath, token, core}`.

- [ ] **Step 3: Implement Console branch** — when no override-capable game is focused but the
  current level is a console folder, resolve `sys` from the level (`stack_.last().item` →
  `SystemCatalog`, or a new `HomeView` accessor if not derivable) and return `{Console, sys}`.
  Else `{None}`. Use `emuscope::contextKind` to make the decision.

- [ ] **Step 4: Build + gate.** The context decision is the `emuscope::contextKind` unit
  (Task 1); the accessor wiring is verified live in Task 8.

- [ ] **Step 5: Commit** — `feat: emuMenuContext resolves game/console/none from browse state`.

---

## Task 6: Start → browse context menu (the trigger + framework)

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (`pollMenuPad`, ~2835-2888), `MainWindow.h`.

**Interfaces — Produces:** `void openBrowseContextMenu();` — builds a `NavMenu` whose entries
are context-filtered; v1's only entry is "Emulation settings" (present iff
`emuMenuContext().kind != None`), which calls `presentEmulationPanel(ctx)` (Task 7).

**Interfaces — Consumes:** `emuMenuContext()` (Task 5); `NavMenu::pick`;
`NavOverlay::topmost()`; the `PAD_START` id (=3) and `sendNavKey(Qt::Key_Escape)` fallback.

- [ ] **Step 1:** Read `pollMenuPad` (MainWindow.cpp:2835-2888) — the early in-game return
  (2843), the nav table (2870-2875), the press-edge dispatch (2883). Confirm how a
  press-edge is detected so the Start branch fires once per press.

- [ ] **Step 2: Add the Start branch** BEFORE the generic nav table: on `PAD_START`
  press-edge, if `NavOverlay::topmost()` is non-null → keep today's behavior (send
  `Qt::Key_Escape`); else `openBrowseContextMenu()`. Ensure the generic table no longer also
  fires Escape for that same press (guard/return).

- [ ] **Step 3: Implement `openBrowseContextMenu`** — `auto ctx = emuMenuContext();` build a
  `QVector` of `NavMenu` items; if `ctx.kind != None` add "Emulation settings" →
  `presentEmulationPanel(ctx)`. If the item list is empty, fall back to `sendNavKey(Escape)`
  (never show an empty menu). Present via `NavMenu::pick(tr("Menu"), items, this)`.

- [ ] **Step 4: Build + gate.** Live-verify the Start trigger in Task 8 (a probe can't press a
  pad). Confirm `probe_nav` still passes (no QDialog introduced).

- [ ] **Step 5: Commit** — `feat: Start opens a browse context menu (Emulation settings entry)`.

---

## Task 7: `presentEmulationPanel` — the panel itself

**Files:**
- Modify: `native/src/ui/MainWindow.{h,cpp}`.

**Interfaces — Produces:** `void presentEmulationPanel(const EmuMenuContext& ctx);`

**Interfaces — Consumes:** `ThemedPanelHost::present` (pattern:
`presentEmulatorCorePicker` MainWindow.cpp:16031); `emulationTargetsFor`,
`resolveEmulationTarget`, `applyTargetToOverride`, `setSystemEmulationDefault`
(EmulationTarget.h); `gameHasPerGameConfig`/`clearPerGameBundle` (Task 2); `editCoreOptions(…,
scope, token)` (Task 3); `presentEmuGfxPanel(…, scope, gameKey)` (Task 4);
`kRetroParkBuildAvailable`.

- [ ] **Step 1: Build the row list** via `ThemedPanelHost::present(tr("Emulation"), rows,
  onAct, onBack)`:
  - **Scope** (Game context only): a `Choice` `Universal | This game`, initial value from
    `emuscope::initialScope(gameHasPerGameConfig(ctx.gameKey, ctx.token, ctx.core))`. On change
    to Universal → `clearPerGameBundle(...)` then re-present at Universal. On change to This
    game → seed an Override from the resolved current target
    (`applyTargetToOverride(resolveEmulationTarget(...), ov)`, `LaunchOpts::set(gameKey, ov)`)
    then re-present at This game. (Seeding via the emulator lever is enough to make
    `gameHasPerGameConfig` true and the choice sticky; gfx/core-option deltas are added lazily
    when the user edits them.)
  - **Emulator** (`Choice`): value = current resolved target's `displayName`; options =
    (Game+ThisGame: "System default") + `emulationTargetsFor(ctx.sys, kRetroParkBuildAvailable)`.
    Apply: ThisGame → `applyTargetToOverride` + `LaunchOpts::set(gameKey, ov)`; Universal (or
    Console) → `setSystemEmulationDefault(ctx.sys->id, target)`.
  - **‹Emulator› settings** (`Action`) — routed by the CURRENTLY SELECTED engine:
    libretro → `editCoreOptions(ctx.sys->id, scope, ctx.token)`; standalone →
    `presentEmuGfxPanel(ctx.sys->id, emulatorId, scope, ctx.gameKey)`; RetroPark → **omit the
    row entirely** (no dead row).
  - Do NOT render a "Controller mapping" row (v2).

- [ ] **Step 2: Console context** — no scope row; the Emulator choice edits the per-system
  default; the settings row edits the Universal layer (`scope=Universal`, empty token/gameKey).

- [ ] **Step 3: Build + gate.** `probe_nav` green (nav-kit only). Everything user-facing is
  live-verified in Task 8.

- [ ] **Step 4: Commit** — `feat: presentEmulationPanel (scope toggle + emulator + settings)`.

---

## Task 8: Live proof + review + merge + deploy

- [ ] **Step 1: Full gate** — build the app WITH the deploy flags; run the whole probe suite;
  require `ALL HEADLESS PROBES PASSED` (`probe_emulation_scope`, `probe_nav`, `probe_emutargets`
  all green). Re-run once on a known flake (`probe_uitest`/#164/#180) and say so.

- [ ] **Step 2: Deploy a Release build** to a THROWAWAY test dir (not `C:\EverythingBox-app`
  until the user has verified) OR hand the user the worktree Release exe for hands-on: Start on
  a GameCube game → panel opens; toggle This game / Universal; change emulator; open Dolphin
  (standalone) graphics settings and libretro core options; confirm the sticky-scope behavior
  and that Universal reverts. This is the USER's hands-on verification.

- [ ] **Step 3: Final whole-branch review** (fresh reviewer, most-capable model) over
  `merge-base..HEAD`. Fix Critical/Important findings.

- [ ] **Step 4: Merge + deploy** — per [[retropark-auto-merge-push]]: verify tests, merge to
  main via a throwaway worktree off origin/main, push, deploy the SDL-enabled Release exe to
  `C:\EverythingBox-app` after the user's green light. No AI attribution.

---

## Build note (every build/gate step)

Configure with the deploy flags so SDL (the gamepad path) is compiled in:
```
export PATH="/c/Users/cubma/.cargo/bin:/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 \
  -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib \
  -DSDL2_INCLUDE_DIR=C:/SDL2/include -DSDL2_LIBRARY=C:/SDL2/lib/x64/SDL2.lib \
  -DEB_WITH_RETROPARK=ON
cmake --build build --config Release --target everythingbox   # + probe targets for the gate
```
Grep the WHOLE build log for `: error|error C[0-9]|error LNK`. The headless-probe gate runs
via `native/tools/run-headless-probes.sh` and must print `ALL HEADLESS PROBES PASSED`.

## Self-review notes

- **Spec coverage:** trigger (T6), context (T5), panel+scope+selection (T7), scoped libretro
  settings (T3), scoped standalone gfx (T4), bundle clear/sticky (T2+T7), RetroPark-no-settings
  (T7), non-RetroPark builds (Global Constraints + T7 gating), testing (T1 probe + T8 live).
- **No new store** — corrected from the first spec draft; #95 already shipped per-game core
  options and the launch fold.
- **Controller mapping** is explicitly v2 and appears in no task.
