#pragma once
// Emulation Context Menu Task 1 — the pure scope/context helper the Start-menu emulation panel is built on.
//
// Two decisions, both pure (plain bools/enums in, an enum out — no Qt, no Settings, no I/O), so the panel's
// branching logic is unit-testable headlessly and lives in exactly one place:
//
//   * contextKind(...) — given the browse-focus state, is the Start context a focused GAME, a CONSOLE folder, or
//     nothing actionable? A focused game whose system resolved is override-capable (Game). Otherwise, if the
//     current level is a resolvable console folder, the panel edits that console's per-system layer (Console).
//     Otherwise there is nothing to configure (None). The gameSysResolved guard is what keeps a focused-but-
//     unresolvable row from masquerading as a Game — it falls through to the Console/None arms.
//   * initialScope(...) — which layer the panel opens editing. ThisGame exactly when the focused game already
//     carries per-game config (so re-opening the panel lands you back on the layer you were editing); else
//     Universal.
//
// Kept header-only + inline so probe_emulation_scope and MainWindow share one definition with no link unit.
namespace emuscope {

// Which layer of the per-game/per-system store an edit targets.
enum class Scope { Universal, ThisGame };

// What the Start press has in focus, and therefore what the emulation panel can configure.
enum class ContextKind { None, Console, Game };

// A game is the context iff a game leaf is focused AND its system resolved (override-capable). Failing that, a
// resolvable console folder is the context. Failing that, nothing.
inline ContextKind contextKind(bool gameFocused, bool gameSysResolved, bool consoleSysResolved)
{
    if (gameFocused && gameSysResolved) return ContextKind::Game;
    if (consoleSysResolved)             return ContextKind::Console;
    return ContextKind::None;
}

// The panel opens on ThisGame exactly when the focused game already has any per-game config; else Universal.
inline Scope initialScope(bool hasPerGameConfig)
{
    return hasPerGameConfig ? Scope::ThisGame : Scope::Universal;
}

} // namespace emuscope
