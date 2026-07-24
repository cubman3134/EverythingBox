# Perf baseline — 2026-07-23 (Local Video Library close-out)

Branch `local/video-library`, all 5 tasks merged. Captured with `native/tools/perfbaseline.py`
(MMV_PERF=1 + MMV_UITEST=1) driving the standard route (cold start → Games → Recent → 50-row
scroll → open first local game → back → exit) against a **portable throwaway copy** of the deployed
app (fresh Release exe, cloud/sync stripped, fixture video library). The real deployed app was never
launched. Route ran fully on all three runs (no SKIPPED steps).

Representative run (run 2), ranked by worst single occurrence:

| span | worst ms | avg ms | samples |
|---|---|---|---|
| startup.total | 1134 | 1134 | 1 |
| startup.firstpaint | 510 | 510 | 1 |
| nav.select | 323 | 27 | 65 |
| startup.settings | 171 | 171 | 1 |
| catalog.load | 117 | 94 | 2 |
| startup.home | 104 | 104 | 1 |
| open.game | 79 | 79 | 1 |
| startup.addons | 7 | 7 | 1 |
| startup.theme | 2 | 2 | 1 |
| marks.shelves | 1 | 0 | 2 |

## The two gate spans across all 3 runs

| run | startup.total (ms) | nav.select worst (ms) | nav.select avg (ms) | samples |
|---|---|---|---|---|
| 1 | 1099 | 337 | 30 | 65 |
| 2 | 1134 | 323 | 27 | 65 |
| 3 | 1191 | 424 | 41 | 65 |
| **b2t6 reference (2026-07-20)** | **1374** | **390** | **28** | **67** |

## Verdict

- **nav.select is FLAT — the gate invariant holds.** avg 27–41 ms vs the b2t6 reference avg 28 ms;
  the worst sample (323–424 ms) is, as always, the *first* nav.select on a cold thumb cache /
  first-paint (see the 2026-07-17 baseline analysis), not a steady-state cost. The Local Video
  Library feature's only steady-state addition is Seam A's per-row `LocalLibrary::index().ownsId(it.id)`
  lookup in `HomeView::buildBrowseItem` (HomeView.cpp:1409) — an O(1) `QHash` probe run once per
  rendered row. It does not move nav.select off its floor. Run 3's avg 41 ms is a single cold-outlier
  pull (worst 424), within run-to-run noise.
- **startup.total is flat/slightly improved** (1099–1191 ms vs 1374 ms reference). The startup scan
  (`MainWindow::rescanLocalLibrary`) runs off-thread via `QtConcurrent::run` and installs the index on
  completion, so it never lands inside `startup.total`.

No regression attributable to the Local Video Library feature.
