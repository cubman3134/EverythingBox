# Category Playlists + Channels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Category-scoped playlists (one Video playlist mixes episodes+movies), Play-random, and Channel mode — shuffle-bag random autoplay with a cancelable countdown interstitial: the personal TV network.

**Architecture:** PlaylistStore migrates catalogKey→categoryKey (via the existing `mediaCategory()` oracle, legacyKey preserved); per-playlist actions ride the game-action NavMenu precedent; the channel chains at the EOF-gated `queueFinished` seam with a channel-active guard diverting from episode-autoplay; the interstitial is NavConfirm + a live QTimer (its nested loop keeps timers firing). Spec: `docs/superpowers/specs/2026-07-21-category-playlists-channels-design.md`.

**Tech Stack:** Qt 6.8.3, PlaylistStore/NavOverlay kit, headless probes.

## Global Constraints

- Branch: `library/playlists-channels` off main. Standing autonomy through the merge gate.
- ANCHOR ON FUNCTION NAMES. Scout-verified seams (main@ef7c4a0):

| Concern | Anchor |
|---|---|
| Natural-end signal (EOF-only, stop/seek swallowed) | `MpvWidget::onMpvEvent` MPV_EVENT_END_FILE (~:249) → `endReached` → `MainWindow::onTrackEnded` → `PlaybackSession::handleTrackEnd` → `queueFinished` |
| The chain seam (+ episode-autoplay divert guard) | the `queueFinished` connect (`MainWindow.cpp` ~:777 — currently unconditionally tries `tryPlayNextEpisode`) |
| catalogKey composition (SINGLE site) | `HomeView::currentCatalogKey` (~:1565, `aid|catalogId|catalogType`); decomposer `addonForKey` |
| type→category oracle | `HomeView::mediaCategory` (~:1279 — returns `video|audio|game|reading`; NOTE: `game` SINGULAR — the store's category tokens use mediaCategory's outputs VERBATIM; the spec's "games" prose maps to `game`) |
| Category-level surface | `categoryCatalogs(categoryKey)` (~:1323) |
| Playlist UI | `_playlists`/`_playlist`/`_newplaylist` mime routing in `activateItem` (~:2209); `openPlaylistsLevel`/`openPlaylistLevel`/`populatePlaylists`; builders `browse::playlistsCatalog`/`playlistItemsCatalog` (SyntheticCatalogs.cpp ~:108/:132) |
| Per-item action-menu precedent | the game NavMenu (`HomeView.cpp` ~:2418, Play/Favorite/…/Uninstall) |
| Rename/remove/removeItem exist with ZERO callers | `PlaylistStore.h:37-40` |
| Countdown card | `NavConfirm::ask` nested loop keeps timers firing (`NavOverlay.cpp` ~:441); Cancel-focused house shape |
| Migration blast radius | PlaylistStore.{h,cpp} read/write/filter/create + 3 forCatalog callers + openPlaylistLevel's catalogKey read + SyntheticCatalogs signatures |

- Weekend Picks (the user's REAL playlist) must survive migration byte-meaningfully: same id, same items, now category-scoped; the migration probe pins its exact shape class; live verification confirms it intact.
- Channel: video/audio playlists only (games = Play-random only); chains ONLY on natural end (structural via EOF gating + the channel-active flag cleared on every user stop/Back/queue-clear path); shuffle bag = no repeat until exhausted, then reshuffle; bag is session-only; resume applies per item.
- Desktop suite + perf gates; env recipe COMPLETE (QT_PLUGIN_PATH — the 3× false-alarm memory); serial builds; probes registered runner+ci.yml; ini snapshot + byte-identical restore EXCEPT the deliberate migration rewrite (snapshot both states, diff must show ONLY the playlist key-shape change).

---

### Task 1: PlaylistStore category migration + category-level surfacing + probe

**Files:** Modify `native/src/core/PlaylistStore.{h,cpp}` (field `categoryKey` + `legacyKey`, one-time `migrateToCategories()`, `forCategory(categoryKey)` replacing/wrapping `forCatalog`), `native/src/ui/HomeView.cpp/.h` (callers: populatePlaylists/add flows key by `mediaCategory(currentCatalogKey-segment-2)`; openPlaylistLevel's addon resolution — entries carry their own addonId, the playlist-level addon becomes per-entry: verify `playlistItemsCatalog` entries resolve via ENTRY addonId not the playlist's legacy catalog addon — adjust if needed), `native/src/browse/SyntheticCatalogs.{h,cpp}` (builder signatures), `native/tools/probe_playlists.cpp` (create) + CMake/runner/ci.yml.

- [ ] Probe RED: migration table (a v1 ini blob with 2 playlists incl. a Weekend-Picks-shaped one [video catalogKey] + a games one → categoryKey `video`/`game`, legacyKey preserved, ids stable, items untouched); idempotent second run; `forCategory` filtering; create() with a categoryKey; unknown catalogType → `video` (mediaCategory's fallback). Sentinel PLAYLISTS-OK. NOTE: mediaCategory lives in HomeView (not linkable) — EXTRACT the type→category mapping to core (e.g. `core/MediaCategories.h`, a free function) and have HomeView::mediaCategory delegate to it; probe pins the core function.
- [ ] Implement + migrate on store load (once, stamped). Surfacing: the Playlists folder row appears at the CATEGORY level (categoryCatalogs surface) AND stays reachable from catalog levels (same category's lists shown — the add-to-playlist menu now offers all category playlists).
- [ ] Suite green; live: Weekend Picks intact (items + name + opens correctly), visible from the Video category level; add-to-playlist from a DIFFERENT video catalog offers Weekend Picks. Snapshot/diff discipline per constraints. Commit `feat: category-scoped playlists + migration (playlists T1)`.

---

### Task 2: per-playlist actions + Play random

**Files:** Modify `native/src/ui/HomeView.cpp` (playlist-row activation → NavMenu `Open / Play random / Start channel / Rename / Delete` — the game-menu precedent; Start channel row only for video/audio playlists, and only wired in T3 — in T2 it's absent), `native/src/core/PlaylistStore.cpp` if rename/remove need touch-ups (first real callers).

- [ ] Playlist row activate → the menu (default row Open, drilling as today). Play random: uniform pick over entries → the existing per-entry open path (addon resolve / openRecent by path — exactly what playlistItemsCatalog rows do on activate; reuse that resolution, don't duplicate). Rename → Osk::getText prefilled; Delete → NavConfirm Cancel-focused (house pattern) → PlaylistStore::remove.
- [ ] Live: menu on a throwaway playlist (create, rename, random×3 different picks land+play, delete w/ decline+accept); Weekend Picks untouched. Suite green. Commit `feat: playlist action menu + play random (playlists T2)`.

---

### Task 3: Channel mode

**Files:** Modify `native/src/ui/MainWindow.cpp/.h` (channel state: `channelPlaylistId_`, shuffle-bag `QVector<int>` + cursor; the `queueFinished` handler divert; the interstitial helper; clear-on-stop wiring), `native/src/ui/HomeView.cpp` (the Start-channel menu row, video/audio only; signals the id to MainWindow).

- [ ] Start channel: build the bag (indices shuffled — QRandomGenerator::global), play the first pick via the per-entry open path, set channel-active.
- [ ] The `queueFinished` handler: when channel-active → (skip episode autoplay) → next bag pick (exhausted → reshuffle, avoid immediate-repeat of the last item when size>1) → interstitial: NavConfirm::ask("Channel", "Next: <title> — starting in 5 s", {Cancel, Play now}, focus 0, cancel 0) + a QTimer relabeling the message each second and auto-dismissing (accept) at 0 — timers fire inside the nested loop (scout-verified); Cancel/Back → exit channel (flag cleared, stay on the player page stopped or pop — pick the least-surprising, document); timeout/Play-now → play the pick (resume applies naturally).
- [ ] Clear channel-active on EVERY user-stop path: trace where Back/stop from the player lands (goBack's player branch, stopScrobble/clearQueue sites, openHome) — the flag must never survive into unrelated playback; ALSO clear on starting any NON-channel playback (routePlay/openVideoPath entries — a manual play while a channel idles kills the channel).
- [ ] Probe: the shuffle bag as pure logic (extract to a small helper: no repeat until exhausted over N draws×trials, reshuffle-no-immediate-repeat, size-1 sane) — pin in probe_playlists.
- [ ] Live (throwaway playlist of 3 SHORT local videos — generate 5-10s clips via ffmpeg in the scratchpad if the library lacks short ones, add via the normal add flow): Start channel → plays; let one END naturally → interstitial counts down → auto-advances; all 3 air before any repeat; Cancel at the interstitial → channel exits (next natural end of a manually-played video does NOT chain); user Stop mid-item → no chain; audio playlist channel spot-check; games playlist has NO Start-channel row. Suite green. Commit `feat: channel mode — shuffle-bag random autoplay + interstitial (playlists T3)`.

---

### Task 4: close-out — spec status, gates, fable, merge

- [ ] Spec Status → complete (smart playlists recorded future; weighted programming future; games-chaining non-goal restated). Full suite + perf 3 runs. Fable whole-branch (seams: the queueFinished divert vs episode autoplay vs the ext-player routePlay + sync syncKey_ — FOUR tracks now share the play/end pipeline; migration data-safety; interstitial modality vs the input arbitration; channel-flag lifecycle). Fix rounds; merge+push+redeploy.

## Self-Review (done at write time)

- Spec coverage: category scope + migration ✅T1; add-menu category-wide ✅T1; Play random ✅T2 (games included); channel EOF-only chaining + bag + interstitial + resume + exit semantics ✅T3; future smart-playlists preserved (rule field noted in spec, nothing precludes) ✅; Weekend Picks safety ✅T1+constraints.
- Ambiguities resolved: category tokens = mediaCategory outputs verbatim (`game` singular); Start-channel row exists only for video/audio; the mediaCategory extraction to core for probe linkage; interstitial = NavConfirm+timer (subclass only if relabeling proves infeasible — implementer documents).
- Type consistency: `forCategory`, `categoryKey`/`legacyKey`, `migrateToCategories`, the bag helper, channel members — consistent across tasks.
