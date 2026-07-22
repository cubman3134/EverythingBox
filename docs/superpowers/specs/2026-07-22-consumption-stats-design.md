# Consumption Stats (Playnite roadmap #3) — Design

**Date:** 2026-07-22
**Status:** Complete: per-profile media/reading accrual + Stats surface (themed + classic) shipped 2026-07-22; games via PlayStats display-join; importers ride-alongs landed.
**Origin:** User request — Playnite tracks playtime; generalized: hours watched/listened/
played + pages read, per title + per catalog, per profile, with a stats surface.

## Scope reality (scout, 2026-07-22)

Games ALREADY accrue per-profile: `PlayStats` (`playstats/<profileId>/<sha1>/…`,
total/sessions/last) fed by GameLauncher begin/endPlaySession (libretro + external
emulators) and launchPcExe (pcgame + goggame). Steam/Epic are fire-and-forget
(launcher-owned processes — no sessions; recorded limitation). Media/reading accrue
NOTHING today, and their resume keys are GLOBAL (profile-blind) — accrual must NOT
extend those keys; it gets its own per-profile store.

## Design

### ConsumptionStats store (`native/src/core/ConsumptionStats.{h,cpp}`, per-profile)

- Per-title: `stats/<profileId>/items/<hash>/{mediaSeconds, pagesRead, lastActivity}`
  (hash = the MD5-token pattern; keys = the same identities the seams carry: media
  resume identity, reader path keys, marks-style keyFor where available).
- Per-category rollups: `stats/<profileId>/cat/{video,audio,reading}/{seconds|pages}`
  maintained at accrual time (cheap increments; games rollup computed from PlayStats
  at display time — PlayStats is NOT migrated or duplicated).
- API: `addMediaSeconds(key, category, secs)`, `addPagesRead(key, n)`,
  `get(key)`, `categoryTotals()`, `topTitles(category, n)` (display join with titles
  via the callers' context — the store keeps a `title` field updated on accrual so
  the panel needs no reverse lookup), `invalidate()` (profile switch).

### Accrual seams (NO new timers)

- **Watch/listen**: `PlaybackSession::persistResume()` (the existing ≥5s-throttled
  heartbeat) accrues `clamp(pos - lastAccruedPos, 0, 6s×throttle-factor)` — forward-
  only (seeks don't count), capped so a seek-forward can't dump minutes; category
  from the session's media kind (video vs audio). Per-profile via ProfileStore.
- **Pages read**: the page-turn edges — `PdfView::currentPageChanged`,
  `ComicView::showPage`, EbookView page/chapter advance — counting NEW max-page
  visits only (re-reading a page doesn't double-count; per (profile,title) high-water
  mark stored beside pagesRead).
- **Games**: PlayStats as-is (wall-clock spans). Paused-time subtraction recorded as
  a follow-up (Playnite counts wall time too; not this track).

### The Stats surface

- A themed **Stats panel** (ThemedPanelHost, reached from the Settings hub): per-
  category totals (Watched Xh Ym / Listened / Read N pages / Played Xh from
  PlayStats), then top-5 titles per category (Info rows, formatDuration-style
  display). Per-profile by construction. Classic fallback: a SettingsDialog sub-page
  with the same data (the established dual-surface pattern).
- Stats SHELVES ("Most played" home rows) = recorded follow-up, not this track.

### Ride-alongs (importers-track follow-ups, user-visible gaps)

- `playlistItemsCatalog` handles `epic:`/`gog:` itemIds (dead tiles today — mirror
  the `steam:` branch + addonId stamping so store games in playlists launch).
- Owned-games re-present preserves the grid cursor (no reset mid-navigation).
- In-flight owned-fetch dedup (bool/generation).

## Non-goals

- Pause-aware game time (follow-up); stats shelves (follow-up); time-series/history
  charts (totals only, Playnite-parity); Steam/Epic session timing (launcher-owned);
  cross-profile aggregate views; exporting.

## Verification

- Probe (`probe_stats`): accrual math (forward-only clamp, cap, high-water pages,
  category rollup coherence), per-profile isolation, invalidate, title-field update.
- Live: watch ~30s of a video → stats panel shows it (title + category total); flip
  pages in a comic → pages count; the games row matches PlayStats' existing data;
  profile switch → different stats; playlist epic/gog tile launches (ride-along);
  owned re-present cursor (stub posture — no key). Byte-identical ini restore except
  the throwaway profile's accruals (deleted with it).
- Suite + perf gates (the persistResume hook is on the 5s heartbeat — nav/startup
  untouched; verify).

## Follow-ups (close-out 2026-07-22)

- Pause-aware game time (subtract paused spans; Playnite counts wall time too).
- Stats SHELVES ("Most played" home rows).
- Time-series / history charts (totals-only today, Playnite-parity).
- Playlist-gog registry re-resolve routing: a persisted gog tile carries its exe path
  frozen at add-time; re-resolving via the registry at launch would survive an install
  move/upgrade (nicety, not a correctness gap — the console tile shares the same frozen
  path today).
- Add-flow persistence probe coverage: the epic/gog addon-stamp + gog `url→path` add
  path is validated by probe_importers §9 (store→tile→activation) but the persist round-
  trip (PlaylistStore write-then-reload of the stamped entry) has no dedicated probe.
- Cross-reader open-page inconsistency: the three readers accrue pages on different
  edges (PdfView::currentPageChanged, ComicView::showPage, EbookView page/chapter
  advance); the "current page" semantics at open differ slightly across them — audit for
  a uniform first-visit definition.
- Heartbeat double-sync cost: accrual adds a second QSettings::sync() (the per-profile
  ConsumptionStats store) per ≥5s persistResume heartbeat during media playback, on top
  of the existing resume-store sync. Outside the measured perf route (route opens a game
  via PlayStats, no media playback); bounded by reasoning as negligible (one small extra
  ini flush per 5s, off every hot path — see the T3 baseline note).
