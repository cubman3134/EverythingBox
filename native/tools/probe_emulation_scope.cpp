// Headless check of the pure scope/context helper (src/core/EmulationScope.h, Emulation Context Menu Task 1) —
// the two pure decisions the Start-menu emulation panel is built on: which browse context is in focus
// (contextKind) and which layer the panel opens editing (initialScope). Both take plain bools/enums, so there is
// no Qt at all here — but a QCoreApplication is kept for parity with the sibling probes and the offscreen QPA gate.
//
// It pins:
//   * contextKind — a focused game with a resolved system is Game; a focused-but-unresolvable row (or nothing
//     focused) inside a resolvable console folder is Console; nothing resolvable is None. The gameSysResolved
//     guard is what separates Game from Console, so a mutant that returns Console before Game (precedence flip)
//     or drops the guard is caught.
//   * initialScope — the panel opens on ThisGame exactly when the focused game already carries per-game config,
//     else Universal.
//
// Prints "probe_emulation_scope: PASSED" on success; any failure prints EMUSCOPE-FAIL <cond> (line) and exits
// non-zero.
//
// FIXTURES ARE HAND-COMPUTED ORACLES: every expected enum value is a literal derived from the documented rules,
// never produced by re-running the function under test, so an assertion cannot pass merely by echoing the code.
#include "EmulationScope.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "EMUSCOPE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using emuscope::Scope;
using emuscope::ContextKind;
using emuscope::contextKind;
using emuscope::initialScope;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- contextKind ----
    // A focused game with a resolved system -> Game, regardless of whether a console system also resolves
    // (the third arg is a don't-care here). Two literals, both Game.
    CHECK(contextKind(true, true, true)  == ContextKind::Game);
    CHECK(contextKind(true, true, false) == ContextKind::Game);

    // A focused row that is NOT override-capable (gameSysResolved=false) but we ARE inside a resolvable console
    // folder -> Console. Also: nothing focused at all, but a resolvable console folder -> Console.
    CHECK(contextKind(true, false, true)  == ContextKind::Console);
    CHECK(contextKind(false, false, true) == ContextKind::Console);

    // Nothing resolvable -> None. A game focused but with an unresolvable system AND no console folder -> None too
    // (the guard: gameFocused alone is not enough without gameSysResolved, and no console fallback exists).
    CHECK(contextKind(false, false, false) == ContextKind::None);
    CHECK(contextKind(true, false, false)  == ContextKind::None);

    // ---- initialScope ----
    // ThisGame exactly when the focused game already has per-game config; Universal otherwise.
    CHECK(initialScope(true)  == Scope::ThisGame);
    CHECK(initialScope(false) == Scope::Universal);

    if (failures == 0) std::printf("probe_emulation_scope: PASSED\n");
    else               std::fprintf(stderr, "probe_emulation_scope: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
