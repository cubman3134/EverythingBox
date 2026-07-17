# Phase 2, Perf Track — Design

**Date:** 2026-07-17
**Status:** Perf track complete: baseline + 2 measured fixes + 9 at-floor verdicts; budgets CI-gated. Polish track next.
**Builds on:** `2026-07-16-foundation-refactor-design.md` (phase 1 complete)

## Problem

The user experiences slowness in all four areas — cold startup, scroll/navigation lag,
content-open latency, and catalog/metadata loads — but nothing is measured, so any fix
order would be a guess. Phase 1 extracted the seams; phase 2's perf track makes the app
fast where it measurably matters.

## Approach (chosen: instrument-first)

Build a lightweight span-tracing harness, capture a baseline over a scripted standard
route, fix hotspots strictly in measured order with before/after evidence per commit,
and lock component-level budgets into a CI probe. Known Qt suspects (synchronous GUI-
thread I/O at startup, per-item `QSettings::sync`, unthrottled poster loads, theme
rebuilds) inform where spans go; numbers alone decide what gets fixed.

Rejected: fixing known suspects unmeasured (risk of optimizing the wrong 20%); full
profiler integration à la Tracy/ETW (heavy, and leaves no CI-gateable artifact).

## Components

### PerfTrace (`native/src/core/PerfTrace.{h,cpp}`)

- RAII span macro `PERF_SPAN("name")` plus explicit `PerfTrace::begin/end` for spans
  that cross functions or threads. `QElapsedTimer`-based.
- Enabled by `MMV_PERF=1` (cached bool; disabled cost = one branch). Safe to leave in
  release builds.
- Output: `<app>/perf_trace.log`, one line per span:
  `ISO-timestamp | span.name | duration_ms | detail` (detail optional: counts, paths'
  file names only — never full paths/URLs with secrets; reuse the logSafeUrl idiom).

### Span map (the four pains)

| Area | Spans |
|---|---|
| Startup | `startup.total` (main() → first paint after show(), zero-timer), children: `startup.settings`, `startup.addons`, `startup.theme`, `startup.home` |
| Scroll/nav | `nav.select` (uitest key inject → selection changed → meta skeleton shown); `thumbs.page` (queue depth + drain per page) |
| Open latency | `open.video` / `open.audio` (openItem → mpv fileLoaded), `open.game` (GameLauncher::open → retro first frame), `open.reader` (open → first page) |
| Catalog | `catalog.load` (requestCatalog → populate done, tagged by level), `search.first` / `search.drain` (fan-out first result / all requests drained) |

### Baseline runner (`native/tools/perfbaseline.py`)

Drives the standard route via the uitest pipe: cold start → home → a console →
50-row scroll → open a local game → exit. Parses `perf_trace.log` into a ranked table.
Baselines and re-runs are committed under `docs/superpowers/perf/` (first:
`2026-07-17-baseline.md`).

### CI gate (`probe_perf`)

Component-level budgets only — deterministic offscreen work, no full-app timing, so CI
never flakes on machine variance: e.g. offscreen 500-item catalog populate, pure
builder mapping costs at scale, `StreamResolver::parseM3u` on a 10k-line playlist.
Budgets are set from measured dev-box numbers with generous headroom (≥3×) and exist to
catch order-of-magnitude regressions, not milliseconds. Registered in
`run-headless-probes.sh` and the CI workflow's build-target list (lesson from plan 2:
CI must BUILD the probe or the gate silently never runs).

## The measure-fix loop

1. Land PerfTrace + spans + baseline runner. Capture and commit the baseline.
2. Rank spans by measured cost on the standard route.
3. One hotspot per task, one commit per fix; the commit message carries before/after
   numbers from a re-run of the same route. A fix that doesn't move its number is
   reverted, not merged.
4. Repeat until exit criteria.

## Rules

- **Behavior preservation:** perf fixes must not change visible behavior or ordering
  (a reordered async load that changes which row renders first is a regression, not a
  win). probe_nav + browse/playback probes green on every commit.
- **Boundaries:** measure up to — never inside — mpv, libretro cores, and Qt
  internals. Addon-server/network latency can be overlapped and cached around (in
  scope) but not accelerated (out of scope).
- **Conflict avoidance:** two independent bug-fix sessions own the FavoritesStore
  write path and the OSK focus handoff; do not touch those areas until their work
  lands on main.

## Exit criteria

Every `startup.*`, `open.*`, and `catalog.*` span has a recorded baseline and either
(a) a fix that measurably improved it, or (b) a written verdict that it is at its
floor (e.g. bounded by mpv init). Component budgets locked into `probe_perf` in CI.

## After this track

The polish track gets its own short spec. Candidate list already observed during
phase 1 (not designed here): toast-duration consistency, the status-bar-vs-Notifier
feedback-boundary decision, themed-home transition smoothness, and the extraction-
debris follow-ups recorded in the phase-1 spec.
