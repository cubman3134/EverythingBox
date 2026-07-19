# Phase 3, Subsystem A — Themed Nav Contract Design

**Date:** 2026-07-18
**Status:** Approved design, pending implementation plan
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

## After this spec

Subsystem B spec: themed content surfaces (readers, detail pages) on the contract,
then settings; subsystem D spec: TV + mobile adaptivity (Android input model,
form-factor layouts, player UI).
