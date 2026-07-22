# Consumption Stats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Per-profile consumption stats — watched/listened seconds, pages read, played hours (existing PlayStats) — with a themed Stats panel; plus three importers-track ride-along fixes.

**Architecture:** A ConsumptionStats core store (the PlayStats/ItemMarks per-profile pattern) accrued at existing seams (the persistResume 5s heartbeat, reader page-turn edges) with games read from PlayStats at display; one themed panel + classic sub-page. Spec: `docs/superpowers/specs/2026-07-22-consumption-stats-design.md` (contains the scout's seam map).

**Tech Stack:** Qt 6.8.3, QSettings ini, ThemedPanelHost, headless probes.

## Global Constraints

- Branch: `library/consumption-stats` off main. Standing autonomy through the merge gate.
- ANCHOR ON FUNCTION NAMES. Scout anchors (main@e5d555a): `PlayStats` (core — the scoping template, `playstats/<profileId>/<sha1>/…`; do NOT migrate/duplicate it); `PlaybackSession::persistResume` (~:99, the ≥5s throttled heartbeat at setPosition ~:138 — the media accrual hook; `mediaResumeKey` is GLOBAL, do not extend it); reader edges `PdfView::currentPageChanged` (~:64), `ComicView::showPage` (~:195/pageInfoChanged ~:203), EbookView page/chapter advance (persist ~:553, currentPage/globalPage ~:626/751); the themed panel pattern = any B2 panel (openGeneralSettings) + classic SettingsDialog sub-page push (~:200); Settings-hub row wiring = openSettingsHub's themed rows; `playlistItemsCatalog` steam: branch (SyntheticCatalogs.cpp ~:146) for the epic/gog ride-along; the owned-games re-present + fetch (SteamLibrary/HomeView from the importers track).
- ConsumptionStats: QtCore-only, hashed keys, per-profile (ProfileStore::currentId or "default"), lazy cache + invalidate() (wired at chooseProfile beside ItemMarks::invalidate), linked into the APP target in its own task, empty-key no-op, no junk keys.
- Accrual rules exactly: media forward-only Δpos clamped to [0, 30s] per heartbeat (seeks never dump time); pages = new high-water visits only; categories video|audio|reading (games from PlayStats at display).
- Env recipe COMPLETE everywhere; serial builds; probes RED-first, runner + ci.yml; ini snapshot/restore discipline (throwaway-profile accruals deleted with it); Weekend Picks untouched.

---

### Task 1: ConsumptionStats store + accrual seams + probe

**Files:** Create `native/src/core/ConsumptionStats.{h,cpp}`, `native/tools/probe_stats.cpp`; Modify `native/src/media/PlaybackSession.cpp` (the persistResume hook + a lastAccruedPos_ member + the media-kind category), `native/src/pdf/PdfView.cpp` + `native/src/comic/ComicView.cpp` + `native/src/ebook/EbookView.cpp` (page-turn accrual calls — keep the readers dumb: they call `ConsumptionStats::addPagesRead(key, …)` with their existing path-derived key + title), `native/src/ui/MainWindow.cpp` (chooseProfile invalidate), CMake/runner/ci.yml.

**Interfaces (Produces):**

```cpp
namespace ConsumptionStats {
struct Totals { qint64 mediaSeconds = 0; qint64 pagesRead = 0; qint64 lastActivity = 0; QString title; };
void addMediaSeconds(const QString& key, const QString& category /*video|audio*/, qint64 secs, const QString& title);
void addPagesRead(const QString& key, int page /*current page index*/, const QString& title); // high-water: accrues max(0, page - storedHighWater)
Totals get(const QString& key);
qint64 categorySeconds(const QString& category);   // video|audio
qint64 categoryPages();                            // reading rollup
QVector<QPair<QString,Totals>> topTitles(const QString& category, int n); // reading|video|audio, sorted by the relevant metric
void invalidate();
}
```

- [ ] Probe RED: forward-only clamp (Δ ≤ 30 cap; negative Δ = 0), high-water pages (revisits don't count; regressions don't decrement), rollup coherence (sum of per-title == category rollup after N accruals), per-profile isolation, invalidate, title updates on accrual, empty-key no-op/junk-free. Sentinel STATS-OK.
- [ ] Implement + wire the three seams (persistResume: compute Δ vs lastAccruedPos_, reset lastAccruedPos_ on beginResume/track change; readers: the page edges). Suite green; live smoke: play ~30s video → ini shows the accrual; flip comic pages → pagesRead grows.
- [ ] Commit `feat: ConsumptionStats store + media/reader accrual (stats T1)`.

---

### Task 2: the Stats surface + importers ride-alongs

**Files:** Modify `native/src/ui/MainWindow.cpp` (themed Stats panel — hub row + panel builder: category totals rows [Watched/Listened/Read pages/Played from PlayStats-rollup-at-display], top-5 Info rows per category; classic SettingsDialog sub-page with the same data), `native/src/ui/SettingsDialog.cpp` (classic page), `native/src/browse/SyntheticCatalogs.cpp` (epic:/gog: branches in playlistItemsCatalog mirroring steam: + the add-flow addonId stamping in HomeView), `native/src/ui/HomeView.cpp` (owned re-present cursor preserve — capture/restore the selection index across the re-present; in-flight owned-fetch dedup — a generation/bool).

- [ ] The panel (both surfaces; per-profile by construction; formatDuration display). PlayStats games rollup computed at display: sum over `playstats/<profile>/` totals (add a small `PlayStats::profileTotalSeconds()` helper rather than raw key iteration in the panel).
- [ ] Ride-alongs: epic/gog playlist tiles launch (probe extension: the builder branch table; live: add the real GOG game to a throwaway playlist → tile launches via the monitored path); cursor preserve + fetch dedup (code + the atSteamConsole discipline; stub-posture live check).
- [ ] Live: the panel shows T1's accrued smoke data + the games hours matching PlayStats; profile switch → different totals; classic page renders. Suite green.
- [ ] Commit `feat: Stats panel + importers ride-alongs (stats T2)`.

---

### Task 3: close-out — spec status, gates, fable, merge

- [ ] Spec Status → complete (+ follow-ups: pause-aware game time, stats shelves, time-series). Full suite + perf 3 runs (the heartbeat hook must not move nav/startup — call it out). Fable whole-branch (dimensions: the persistResume hook's cost/correctness on the hot heartbeat; accrual-vs-resume-key identity coherence across profiles; the ride-alongs' cross-track fit; panel data joins). Fix rounds; merge+push+redeploy.

## Self-Review (done at write time)

- Spec coverage: store+seams ✅T1; surface ✅T2; games-from-PlayStats-at-display ✅T2 (no migration); ride-alongs ✅T2; non-goals preserved.
- Type consistency: ConsumptionStats::{Totals,addMediaSeconds,addPagesRead,get,categorySeconds,categoryPages,topTitles,invalidate} + PlayStats::profileTotalSeconds — consistent across tasks.
- Ambiguities resolved: high-water page semantics (regressions never decrement); the 30s per-heartbeat cap; title stored on accrual (no reverse lookups); categories at the seam not the store.
