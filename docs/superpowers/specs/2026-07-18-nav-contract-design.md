# Phase 3, Subsystem A — Themed Nav Contract Design

**Date:** 2026-07-18
**Status:** Subsystem A complete: NavGraph invariants CI-gated (probe_navqml, 100+ CHECKs),
declared-edge screen class, one Back router, two-state components ready for B, watchdog live
(telemetry since 2026-07-19). NavItem per-element registration deferred to B (zone registration
shipped — documented adaptation).
**Builds on:** phase 1 (foundation refactor), phase 2 (perf + polish tracks) — all complete
**Part of:** the phase-3 UI refactor. Decomposition (user-approved): **A** this spec (nav
integrity foundation) → **B** themed-everything, phased: content surfaces (readers:
books/audiobooks/manga/comics, detail pages) first, settings hub last → **D** TV+mobile
adaptivity. B and D get their own specs after A lands. Responsiveness (**C**) is a rule
carried across all of them, not a separate build.

## Problem (user requirements, verbatim intent)

On the themed (couch) surface today, the guarantees the user wants do not structurally
exist: selection can be lost (OSK-close and second-search focus bugs), reachability of
on-screen elements depends on per-screen wiring, text/choice inputs don't uniformly use
the select-then-edit model, Back behavior is per-screen (`themedOnBack_` lambdas), and
an intermittent all-black app state appears with no diagnosis. The user's words are
guarantees — "never lose the selection", "every element", "always" — which demand
enforced invariants, not bug fixes.

## Chosen approach: Nav Contract (invariant-first)

Rejected: fix-list (patches can't deliver "never"); pure-QML FocusScope rewrite
(discards the battle-tested classic nav kit and probe_nav's proven invariants).

## Components

### NavGraph (C++, exposed to QML) — one selection model per screen

- Owns `selectedId`, the registry of interactive elements, movement resolution, and the
  back stack for its screen scope.
- QML elements register via an attached/declared `NavItem` (navId, geometry or
  row/column, onActivate, flags such as `editable`); registration is REQUIRED — an
  interactive element that isn't registered is a CI failure (the harness walks the
  scene; B's migration rule enforces it per-surface).
- **Invariant 1 (selection never lost):** `selectedId` always names a live registered
  element. On removal/rebuild of the selected element, selection deterministically
  reassigns: nearest registered neighbor by geometry, else the screen's declared
  default. Null selection is unrepresentable in the API.
- **Invariant 2 (full reachability):** the movement graph from the default element
  reaches every registered element — resolved over the UNION of spatial geometry and
  the screen's declared edges (below), walked by `NavGraph::validate()`. The harness
  fails a screen whose union graph is partitioned.
- **Invariant 3 (movement is one resolver):** arrows resolve in the shared resolver —
  declared edges first (exact from-zone + key match), then spatial geometry
  (row/column or coordinates) — never per-screen key handling.
  - **Grid intra-zone exception:** the model's strips are 1-D, but a grid/carousel
    surface is a 2-D wrap of one `items` zone. Its intra-zone target index (row-jump =
    ±`gridCols`, ±1 along a row, divider-skip continuing the travel direction) is
    computed QML-side from the grid's own column geometry and handed to the model as a
    `nav.select("items", target)` REQUEST — the model still owns the arbitration
    (clamp into count, snap off dividers), so the QML proposes and the model disposes;
    it never assigns a selection prop directly. This is the one place a surface computes
    geometry the 1-D model can't: transitions BETWEEN zones (grid ↔ buttons, the XMB
    two-cursor switch) remain declared edges resolved wholly inside the model.
- **Declared-edge screen class (two-cursor surfaces):** pure geometry cannot express a
  surface with two independent, always-visible cursors (the XMB category axis + its
  item column, co-located in one grid cell) — spatial crossing moves ONE selection and
  carries its index, but a cursor switch must land on the OTHER cursor's position and,
  for arrow parity, step it in the same press. Such screens declare their transitions
  as edges (`NavGraph::addEdge`): an edge crossing enters the target at its per-zone
  REMEMBERED index (the `NavRing::rememberSelection` concept applied per zone; also
  used by reassignment), and a co-located target whose axis matches the arrow gets the
  fused step. Focus handoffs to spatially real zones (a bottom button bar) are also
  declared edges — entry at memory, no fused step. Both kinds are part of the
  Invariant 2 union, so reachability is still validated, not assumed from key-map
  wiring.
- **Undirected legs vs. directed proof:** `validate()`'s connectivity walk treats every
  declared edge as UNDIRECTED (both endpoints registered), unioned with the geometric
  neighbors — it proves the graph is not partitioned, not that any particular key reaches
  any particular zone. Directed reachability (a specific arrow actually landing on a zone
  from the default) is proven separately by each surface's move-BFS probe: a directed
  breadth-first walk that applies the four real arrows through `move()` and asserts the
  reached set (probe_navqml §9 for the shipped themed shape — the "probe-9" pattern). The
  distinction matters for activation-entered zones: the inline action-chooser (`actions`)
  is reached by ACTIVATION, not an arrow, so it is deliberately excluded from the directed
  arrow-BFS reached set — its connectivity is carried by its declared Esc return edge in
  the undirected `validate()` union, and its entry/return is pinned by explicit asserts,
  not the arrow walk.

### Two-state themed inputs

`ThemedTextField`, `ThemedChoice` (+ future `ThemedToggle` etc.) in
`native/src/theme2/qml/elements/`: navigate = selected (outline, from existing theme
styling), activate = edit (OSK for text on TV; open list for choice), Escape = return
to selected without leaving the screen. Exactly the state machine the classic kit
proved (two-state textboxes/dropdowns), one QML implementation reused everywhere.
Subsystem B may not ship a themed surface using ad-hoc input elements.

### One Back router

- The NavGraph back stack replaces `themedOnBack_` lambdas. Every level push (screen,
  drill, overlay — NavMenu, OSK, Esc menu are levels) registers its pop action.
- Back hotkey (Escape, controller B, Backspace) pops exactly one level; at XMB root it
  opens the exit menu (Resume / Exit My Media Vault). One code path; per-screen Back
  wiring is deleted as surfaces adopt the contract.
- **Invariant 4 (Back terminates):** from any state, repeated Back reaches the root
  exit menu in bounded steps (no cycles, no dead ends).

### Black-frame watchdog + hardening

- Debug-gated (`MMV_UITEST` / Settings ▸ Debug): samples the composited frame ~1/s;
  ≥99%-black visible surface OUTSIDE legitimate contexts (video playing, game running,
  launch handoff, intentional black screens) → logs screen/state/timestamp to the
  existing debug log AND forces a scene refresh (QQuickWidget context-loss kick) as
  recovery. Purpose: diagnose the user's intermittent black state in the act, and
  self-heal it.
- Hardened regardless of watchdog findings: QQuickWidget context-loss/wake handling,
  show()-after-minimize repaint kicks on the themed home.

### CI: probe_navqml

Offscreen QML harness with synthetic screens asserting: Invariant 1 under element
churn (add/remove/rebuild storms), Invariant 2 graph-walk, Invariant 4 Back-chain
termination, and the two-state input transitions. Registered in
`run-headless-probes.sh` AND the CI workflow build-target list. Classic `probe_nav`
stays green untouched — classic surfaces keep their kit until subsystem B retires
them; the contract is the destination and B's migration rule ("a surface is not
'themed' until it registers in the contract and passes probe_navqml") does the rest.

## Responsiveness rule (subsystem C, applied here)

`nav.select` stays instrumented (perf track); a contract-layer change that regresses
median per-keypress latency by >2 ms on the standard baseline route fails and is
reworked — measured via `perfbaseline.py`, same medians discipline as the perf track.

## Coordination

- The user's OSK-focus background session owns today's focus-handoff bug. Before
  execution: check whether it landed on main. If still open, the contract supersedes
  it (its failure mode becomes unrepresentable) — recommend the user close that
  session rather than race it. The Favorites-fix session's territory (FavoritesStore
  writes) is untouched by this spec.
- Existing behavior stays: this subsystem changes HOW guarantees are enforced, not
  what screens look like (that's B). Visible changes limited to: bugs becoming
  impossible (selection/black-state recovery) and the exit menu reliably appearing at
  root Back.

## Exit criteria

probe_navqml green in CI with all four invariants; the XMB home + browse + search +
playlists surfaces migrated onto the contract (the surfaces that exist today —
readers/settings migrate in B); `themedOnBack_` deleted; OSK/second-search focus bugs
closed by construction (regression-probed); watchdog live with at least one week —
or one confirmed catch — of black-state telemetry; nav.select median unchanged
within 2 ms.

All met at close-out EXCEPT the watchdog telemetry soak, which is time-based by design:
**telemetry started 2026-07-19** (watchdog armed + deployed). The remaining exit criterion
is a week of black-state telemetry OR one confirmed catch — whichever comes first. This is
the one open item; **subsystem B may start in parallel** (it does not depend on the soak
concluding), and a confirmed catch during B closes it early.

## After this spec

Subsystem B spec: themed content surfaces (readers, detail pages) on the contract,
then settings; subsystem D spec: TV + mobile adaptivity (Android input model,
form-factor layouts, player UI).

### Carried into B (follow-ups noted at A close-out)

These are known, non-blocking items surfaced during A's reviews — recorded here so B (or a
hardening pass) picks them up rather than rediscovering them:

- **`NavGraph::move()` `QHash::operator[]` → `constFind` hardening.** The movement path
  (`move()` / `crossByEdge`) looks up zones with `m_zones[...]` (operator[]), which
  DEFAULT-INSERTS an empty `Zone{count 0}` on a missing key. It is safe today — `m_zone`
  is always a live zone by invariant, edge targets are `constFind`-guarded before the
  crossing, and `nearestZone` only returns real keys — but operator[] on a lookup is a
  latent hazard: a future caller passing a stale/absent id would silently corrupt the
  registry (a phantom hidden zone) instead of being caught. Switch these read-only
  lookups to `constFind` + an explicit guard.
- **Watchdog false-positive / over-skip caveats.** The black-frame watchdog skips
  `inContent_ || isMinimized() || escMenuVisible()`. Two known edges: (1) a legitimately
  DARK background video played through to a near-black stretch on a themed surface that is
  NOT in a skip context could momentarily sit in the FP window — the 0.99 threshold + the
  2-consecutive-frame gate make this unlikely, but it is why the recovery kick is
  invalidation-only (flicker-free) rather than destructive. (2) `emuPage_` (the
  launch/install wait page) is inside `inContent_`, so the watchdog is fully OFF during a
  launch handoff — a genuine blackout that begins ON the wait page would be OVER-SKIPPED
  and not caught until the surface leaves the content context. Both are acceptable given
  the telemetry-first goal, but B should revisit if a real catch lands in that window.
- **Last-zone / single-input degenerate case.** A registry reduced to ONE zone parks the
  selection there permanently (`removeZone` on the last zone is a refusing no-op), and a
  registry whose every zone is hidden (all counts 0) parks at `(zone, 0)` — the terminal
  state, deliberately not a null. A themed surface that is a SINGLE input field is
  therefore always self-consistent but has no arrow to move to; B's per-element
  registration must ensure such a surface still has a declared default and a Back leg
  (there is nothing for `validate()`'s connectivity walk to partition, so it passes
  vacuously — the guarantee there is "never null", not "reachable from a sibling").
- **`externalEdit` host-owes-`finishEdit` contract.** In the TV/OSK route
  (`externalEdit: true`), activating a `ThemedTextField`/`ThemedChoice` only emits
  `editRequested(navZone)` and goes `externalPending` — it grabs NO focus and opens NO
  inline editor. The HOST then owes exactly one `finishEdit(true|false)` to close the loop
  (`true` commits the host-written value, `false` abandons); until it does, the field
  stays pending (arrows still move the selection away — the pending state does not trap
  navigation). B's surfaces that opt into `externalEdit` MUST honor this contract: a host
  that runs its OSK but never calls `finishEdit` leaves the field permanently pending. The
  contract is probe-pinned (probe_navqml §9/§9b), but the host side is B's responsibility.
