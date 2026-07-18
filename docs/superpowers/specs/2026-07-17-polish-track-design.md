# Phase 2, Polish Track — Design

**Date:** 2026-07-17
**Status:** Polish track complete: 19 fixed, 4 skipped by user; feedback policy landed. Phase 2 complete.
**Builds on:** `2026-07-17-perf-track-design.md` (perf track complete), `2026-07-16-foundation-refactor-design.md` (phase 1 complete)

## Problem

The app works and is now fast, but it feels rough in three user-confirmed ways: abrupt
screen cuts, blank/empty states that show nothing, and inconsistent feedback (toast vs
status bar vs silence, scattered durations). The user also wants a systematic audit to
find jarring moments they haven't consciously catalogued.

## Scope decisions (user-set)

- **Surface:** the themed home (couch/controller experience) first; classic widgets only
  where feedback consistency demands it.
- **Bar:** "fix what's jarring" — no aesthetic overhaul. The theme's look stays.
- **Approach:** audit → user triage → fix (mirrors the perf track's measure-first
  pattern, adapted for taste: screenshots do the reporting, the user keeps the taste
  calls via a cheap checklist triage).

## The audit (one document, three sweeps)

Output: `docs/superpowers/polish/2026-07-17-jank-inventory.md` — ranked items, each
with evidence (screenshot pair or code cite), a one-line proposed fix, and a cost tag
(trivial / small / medium).

1. **Flow sweep** — a scripted uitest route walks every themed-home surface (XMB root,
   each category, browse levels, metadata/detail panel, search, playlists, player
   entry/exit, game launch/exit, Esc menu), capturing a screenshot at each step and
   rapid shot pairs across each transition to catch hard cuts and flashes. Where
   mpv/QML surfaces screenshot black (known grab limitation), the uitest state JSON
   records what was showing instead.
2. **Empty-state matrix** — every synthetic folder and browse level captured both
   populated and empty. Empty states are produced without touching user data
   (scoped filters/markers, or a throwaway profile if one can be created and removed
   cleanly); the matrix records what renders: blank grid, message, or nothing.
3. **Feedback-event inventory** — code-derived: every `notifier_->notify`,
   `statusMessage`, `statusBar()->showMessage`, and known silent-failure site greped
   and tabulated with channel, duration, and trigger. The table proposes ONE channel
   and duration per event class — this is where the status-bar-vs-Notifier boundary
   gets decided concretely, as data rather than doctrine.

The accumulated debris from the three merged branches folds into the inventory
pre-tagged (from the phase-1/2 ledgers): toast-duration scatter (4500/6000/7000),
stale comments, unwired `nextTrack`/`prevTrack` slots, `SearchAggregator::cancel()`
unwired, `openLibraryItem` audio branch missing its stable resumeKey, empty-playlist
drill showing nothing, split-external status/toast asymmetry, probe_perf restart-bound
loosening, baseline-runner end-state assert.

## Triage gate

The inventory goes to the user before any fix lands:

- **Taste items** (transitions, empty-state wording, anything visual): user marks
  fix / skip per item.
- **Objective consistency items** (feedback channel/duration policy, unwired slots,
  stale text, debris): pre-approved as a block — no taste in them.

## Fix rules

- Precise "jarring" boundary: adding a short standard fade (~150 ms, one shared
  constant) where a hard cut exists is IN; changing layout, colors, spacing, or the
  theme's look is OUT.
- Empty states get one consistent, quiet message row (component reused everywhere,
  styled from existing theme values — no new visual language).
- All user-feedback events route through `Notifier` per the inventory's policy table;
  durations come from one constants header (ends the scatter). Status-bar usage
  survives only where the policy table explicitly keeps it (e.g. transient progress
  while a panel is open) — decided in the inventory, not ad hoc.
- One fix per commit; each visual fix carries a before/after screenshot pair committed
  to `docs/superpowers/polish/`; probe_nav + the full probe suite green on every
  commit.
- Hands off areas owned by running sessions (FavoritesStore write path, OSK focus
  handoff) and the two chip follow-ups (poster-cache eviction, async ensureBios).

## Exit criteria

Every triaged-fix item closed with before/after evidence; every skipped item recorded
as skipped with the user's call; the feedback policy table implemented and the
constants header in place; suite green.

## After this track

Phase 2 complete. Remaining known backlog (plan-3 candidates recorded in earlier
specs): LibraryView request-idiom sharing, addon-console grid read-side poster warming,
open.reader measurement route.
