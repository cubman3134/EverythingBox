# Perf baseline — 2026-07-22 (consumption-stats close-out, T3)

Standard route (perfbaseline.py), Release build of `library/consumption-stats` @ de8c492,
deployed to `C:\MyMediaVault-app`. 3 consecutive runs. Ranked spans, worst/avg ms per run:

| span              | run1 (w/avg) | run2 (w/avg) | run3 (w/avg) | samples |
|-------------------|--------------|--------------|--------------|---------|
| startup.total     | 2355         | 2066         | 2052         | 1       |
| startup.firstpaint| 552          | 588          | 599          | 1       |
| startup.home      | 988          | 628          | 552          | 1       |
| startup.settings  | 168          | 202          | 127          | 1       |
| nav.select        | 432 / 31     | 417 / 30     | 410 / 30     | 67      |
| catalog.load      | 134 / 107    | 76 / 74      | 89 / 80      | 2       |
| open.game         | 70           | 52           | 69           | 1       |
| startup.theme     | 6            | 3            | 3            | 1       |
| startup.addons    | 4            | 6            | 5            | 1       |
| marks.shelves     | 1 / 0        | 0 / 0        | 0 / 0        | 2       |

## Verdict vs the reference band

- **nav.select is FLAT** — the gate invariant. avg 30–31 ms (prior b2t6 avg 28), worst
  410–432 ms is the first-select cold outlier (prior worst 390). The consumption-stats
  accrual seams touch neither `NavGraph::select` nor any per-arrow path; the flat avg
  confirms it.
- **startup.firstpaint** 552 / 588 / 599 — around the reference band (~450–569; 599 is
  ~5% over the top edge, within run-to-run noise).
- **startup.total / startup.home run higher than the b2t6 reference** (~1374–1542 total,
  home 133). This run captured perf **immediately after a full parallel Release build +
  the full probe suite** on the same machine — hot CPU + churned filesystem/thumbnail
  cache inflate cold-start I/O. The b2t6 numbers were single quiescent-machine samples.
  It is **not** a stats regression: `ConsumptionStats` does **zero** startup work —
  it is referenced only by `invalidate()` (profile switch), `openStats()` (on demand),
  and the accrual seams (playback/reading). Nothing in the startup path constructs or
  loads the store, and the HomeView/MainWindow diffs (owned-fetch dedup, playlist add-
  flow stamping, the openStats surface) are all off the startup path.

## Heartbeat double-sync cost (explicit callout)

The accrual hooks `PlaybackSession::persistResume()` — the existing ≥5s-throttled resume
heartbeat. Each heartbeat with ≥1 whole forward-second accrued now calls
`ConsumptionStats::addMediaSeconds()`, which does its own `store().sync()` on the
per-profile ConsumptionStats store in the SAME mymediavault.ini (a second flush of the shared file) — i.e. a **second `QSettings::sync()` per
5s during active video/audio playback**, on top of the pre-existing resume-store sync.

**Not measured by this route:** the standard route opens a libretro **game** (PlayStats
wall-clock spans), never media playback — there are **no `playback.*` spans**, so the
heartbeat cost is outside every measured span.

**Bounded by reasoning:** it is one extra small-ini flush (per-item entries + 3 category
rollups, kilobytes) once per 5000 ms, only while media is playing, on the same thread as
the resume sync the perf track already accepted at the same cadence. Even pessimistically
at a few ms/sync that is <0.1% duty, and it never touches startup / nav / catalog / open.
Playback is decode/IO-bound, not sync-bound — no user-perceptible impact. Follow-up if it
ever matters: coalesce the two syncs, or debounce the accrual flush independently.
