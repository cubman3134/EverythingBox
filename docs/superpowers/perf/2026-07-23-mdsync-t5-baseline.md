# Perf baseline — 2026-07-23 (multi-device sync close-out, mdsync T5)

Standard route (`perfbaseline.py`), Release build of `sync/multidevice` @ 59c1c3c (cadence fix)
temporarily deployed to `C:\MyMediaVault-app` for the run, then the pre-task exe + ini were restored
byte-identical (ini md5 `8817fa31…` verified). 3 consecutive runs. Ranked spans, worst / avg ms per run:

| span               | run1 (w/avg) | run2 (w/avg) | run3 (w/avg) | samples |
|--------------------|--------------|--------------|--------------|---------|
| startup.total      | 1366         | 1518         | 1459         | 1       |
| startup.firstpaint | 438          | 483          | 444          | 1       |
| nav.select         | 424 / 31     | 421 / 30     | 425 / 30     | 67      |
| startup.home       | 148          | 228          | 206          | 1       |
| startup.settings   | 101          | 122          | 135          | 1       |
| catalog.load       | 100 / 85     | 79 / 74      | 79 / 79      | 2       |
| open.game          | 90           | 71           | 83           | 1       |
| startup.theme      | 3            | 3            | 4            | 1       |
| startup.addons     | 3            | 3            | 4            | 1       |
| marks.shelves      | 1 / 0        | 1 / 0        | 0 / 0        | 2       |

## Verdict vs the reference band

- **nav.select is FLAT — the gate invariant.** avg 30–31 ms across all three runs (b2t6 reference
  avg ~28–30; the T3 stats baseline was 30–31). worst 421–425 ms is the usual cold first-select
  outlier (b2t6 worst ~390; T3 worst 410–432). 67 samples/run = the full 50-row scroll + category
  nav completed every run. **The mdsync change-hooks touch neither `NavGraph::select` nor any
  per-arrow path**, so the flat avg is exactly as designed (see below).
- **startup.total 1366 / 1518 / 1459** — squarely in the b2t6 reference band (~1374–1542) and well
  under the T3 hot-machine samples (2052–2355). **startup.firstpaint 438 / 483 / 444** — around the
  reference band (~450–569).
- No SKIPPED route steps; every run ended on themedCategory=Games / themedSelection=Recent.

## The mdsync change-hook cost (explicit callout)

**What was added (T2):** each per-item store (`ItemMarks` / `FavoritesStore` / `PlaylistStore`)
gained a `std::function` change-hook fired at its write funnel; MainWindow registers ONE hook that
marshals onto the GUI thread (`QMetaObject::invokeMethod(..., QueuedConnection)`) and re-arms the
existing 15 s `scheduleProgressSync` debounce. **When it runs:** only on a *store mutation*
(favorite/star, playlist edit, a mark/tag change) — i.e. a user write, not a navigation, scroll,
startup, or catalog step. **Cost per fire:** a queued no-op lambda + `QTimer::start(15000)` (a timer
re-arm) — sub-microsecond, off any measured span.

**Not on any measured span by construction.** The standard route makes zero per-item-store
mutations (it browses, scrolls, opens a local game, exits), so no change-hook fires during the
measured window — consistent with nav.select staying flat. The debounced push itself
(`pushProgressNow`) is the pre-existing resume-sync cadence the perf track already accepted; the
T5 cadence fix (exclude per-item keys from `stateHash` + `buildSettingsJson`) makes it *cheaper*,
not costlier — per-item churn no longer flips the bundle fingerprint, so it can never spuriously
re-serialize/re-upload the heavy state zip.

**Bounded by reasoning:** a store write already did a `QSettings::sync()` before mdsync; the hook
adds one queued timer re-arm on top. Even pessimistically that is far below perceptibility and never
touches startup / nav / catalog / open. Follow-up only if a future high-frequency writer appears:
coalesce the debounce re-arm.

## Note

Cold-start numbers were captured on a machine warm from the full parallel Release build + probe
suite this session; they are already at/under the reference band, so no adjustment is warranted.
