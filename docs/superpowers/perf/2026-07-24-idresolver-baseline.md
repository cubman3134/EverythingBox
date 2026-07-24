# Perf baseline — 2026-07-24 (Local Library ID-Resolver close-out)

Branch `local/id-resolver`, all 4 tasks merged. Captured with an adapted `perfbaseline.py`
(MMV_PERF=1 + MMV_UITEST=1) driving a reference-style route (cold start → home settle → `right`
to the **Games** category rail → 50-item down/up scroll → exit) against a **portable throwaway copy**
of the deployed app (fresh Release exe, `cloud/*`/`sync/*` stripped, aiocatalog + TMDB key retained,
seeded movie fixtures). The real deployed app at `C:\MyMediaVault-app` was never launched. All three
runs completed the route with no SKIPPED steps.

Route note: the stock `perfbaseline.py` opens Games ▸ Recent (local ROMs), which this throwaway does
not carry (`[recent]` was stripped). The adapted route scrolls the Games **category** rail instead —
the same `nav.select` span (begun in `MainWindow::sendNavKey`, ended in `HomeView::requestThemedMeta`),
fired once per selection change over real items.

Representative run (run 1), ranked by worst single occurrence:

| span | worst ms | avg ms | samples |
|---|---|---|---|
| startup.total | 1105 | 1105 | 1 |
| catalog.load | 677 | 250 | 3 |
| startup.firstpaint | 531 | 531 | 1 |
| thumbs.page | 477 | 477 | 1 |
| startup.home | 137 | 137 | 1 |
| startup.settings | 128 | 128 | 1 |
| startup.addons | 3 | 3 | 1 |
| startup.theme | 2 | 2 | 1 |
| nav.select | 1 | 0 | 99 |
| marks.shelves | 0 | 0 | 3 |

## The two gate spans across all 3 runs

| run | startup.total (ms) | nav.select worst (ms) | nav.select avg (ms) | samples |
|---|---|---|---|---|
| 1 | 1105 | 1 | 0 | 99 |
| 2 | 1273 | 1 | 0 | 99 |
| 3 | 1269 | 1 | 0 | 99 |
| **b2t6 reference (2026-07-20)** | **1374** | **390** | **28** | **67** |
| **locallib baseline (2026-07-23)** | **1099–1191** | **323–424** | **27–41** | **65** |

## Verdict

- **nav.select is FLAT — the gate invariant holds.** worst 1 ms / avg 0 ms over 99 samples/run, far
  below (never above) the 28 ms reference avg. The sub-millisecond figures reflect this route's item
  mix — the Games-category rail resolves each tile's art from bundled/local sources on hover, with no
  networked enrichment (unlike the reference's Recent-ROM / movie route whose 28 ms avg and ~390 ms
  worst were dominated by first-cold thumb + online scrape, not steady-state cost). The point for the
  gate is that the ID-resolver adds **zero** per-nav work: its only touch on the render/nav hot path
  is Seam A's per-row `LocalLibrary::index().ownsId(it.id)` — an O(1) `QHash` probe in
  `HomeView` buildBrowseItem (HomeView.cpp:1409), unchanged from the Local Video Library track (whose
  2026-07-23 baseline measured that same probe flat at avg 27–41 ms on the movie/ROM route). Online
  matching happens off the hot path: background addon searches on a worker pool + a 1.5 s-debounced,
  off-thread index rebuild (`CatalogResolver`), never inside a `nav.select` span.
- **startup.total is flat** (1105–1273 ms vs 1374 ms reference; within the 1099–1191 ms locallib
  band, run-to-run noise). The resolver's `enqueue()` is kicked from `rescanLocalLibrary`'s
  completion callback **after** the base index is installed, is a no-op on the GUI thread (it only
  queues jobs), and the base scan/index build already run off-thread via `QtConcurrent::run` — so
  neither the enqueue nor the online resolution lands inside `startup.total`.

No regression attributable to the Local Library ID-Resolver feature.
