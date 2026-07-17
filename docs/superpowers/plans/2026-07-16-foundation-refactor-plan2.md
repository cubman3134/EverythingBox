# Foundation Refactor — Plan 2 (debris cleanup + async browse providers)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close out phase 1: clean the plan-1 extraction debris, then pull HomeView's remaining content providers (playlists, Steam, cross-addon search) into focused probe-tested modules and collapse its duplicated reset/injection boilerplate.

**Architecture:** Continues plan 1's proven pattern — pure catalog builders in `native/src/browse/` + HomeView's marker dispatch + `showSyntheticCatalog` as the one shared reset path — instead of the spec's virtual `BrowseLevel` interface. Same goal (one small file per content type, one shared load path, `Notifier` for feedback), materially lower regression risk; Task 7 records the supersession in the spec. Behavior preservation remains the governing constraint.

**Tech Stack:** Qt 6 Widgets C++ (existing style); headless `probe_*` pattern; `MMV_UITEST=1` + `native/tools/uitest.py` live verification.

**Scope note:** The spec's "addon catalogs provider" is HomeView's core Level machinery itself — already a single code path; extracting it buys nothing this phase. LibraryView's duplicated request idiom (LibraryView.cpp:83,169,175,218) is noted for a possible plan 3, not touched here. RetroView (src/emu) exposed no duplication from plan 1 — the spec's step-6 sweep resolves to Task 1's debris list.

## Global Constraints

- Qt 6, C++17, match existing comment density/naming (trailing `_` members, explanatory block comments).
- NEVER QDialog/QMessageBox/QInputDialog/top-level windows — feedback via nav kit / Notifier.
- One seam per commit. Move code, don't rewrite — bodies transfer verbatim except named substitutions.
- Build: `cmake --build build --config Release [--target <t>]` from repo root. Probes need `PATH` prefixed with `/c/Qt/6.8.3/msvc2022_64/bin` (+ `/c/mpv-dev` for the full runner) and `QT_QPA_PLATFORM=offscreen` when run directly.
- Live verification: deploy Release exe over the existing exe in `C:\MyMediaVault-app` (keep name), launch detached with `MMV_UITEST=1`, drive only via `python native/tools/uitest.py`. Never SendKeys/focus. Kill the app afterward; restore any seeded user data.
- All scout line references below are against main @ 1edad4a — re-grep anchors before editing.

---

### Task 1: Extraction-debris cleanup (plan-1 review findings)

**Files:**
- Modify: `native/src/ui/Notifier.h`, `native/src/ui/Notifier.cpp`
- Modify: `native/src/ui/MainWindow.h`, `native/src/ui/MainWindow.cpp`
- Modify: `native/src/launch/GameLauncher.h`, `native/src/launch/GameLauncher.cpp`
- Modify: `native/src/media/PlaybackSession.h`, `native/src/media/PlaybackSession.cpp`
- Modify: `native/tools/probe_playback.cpp` (one new assert)
- Modify: `native/tools/run-headless-probes.sh` (skip-message phrasing)

**Interfaces:**
- Produces: `PlaybackSession::setQueue(const QStringList& files, int startIndex, const QStringList& titles = {}, const QString& resumeKey = QString())` — new trailing arg; empty = current behavior (resume keyed by file path); non-empty = the queue's first-played track is resume-keyed by `resumeKey` instead (kills the begin-after-setQueue re-key fragility). `GameLauncher::CorePlan` gains `int errorMs = 6000;`.

The fix list (each item is small; do them all, one logical group per checkbox):

- [ ] **Step 1: Notifier fixes.**
  - Reparent the window notice back to the central area: in MainWindow's constructor, `new Notifier(this, this)` → `new Notifier(centralWidget(), this)` (call site is after `setCentralWidget` — verify; if not, move it after). Restores the pre-refactor 56px-above-central-bottom anchoring.
  - `Notifier::setPlayerHost`: guard a second call — `if (playerNotice_) { playerNotice_->deleteLater(); playerNoticeTimer_->deleteLater(); }` before re-creating (comment: single caller today; guard keeps a future second caller from leaking).
  - MainWindow: null-guard `notifier_` in `notify`/`hideNotice` slot bodies and the `resizeEvent`/`moveEvent` reposition calls (`if (notifier_) ...`) — restores the old defensive posture of the most-called feedback path.
  - Delete/replace the two stale comments: the "top-level Tool window (not a child widget)" block above MainWindow.h's `notify` slot (describe reality: child-widget overlay owned by Notifier, floats over the central area), and MainWindow.cpp's moveEvent comment "the notice is a separate top-level window; keep it stuck to us as we drag" (the reposition there is now only needed for DPI-change edge cases — say that, or remove the call if reading shows it's fully vestigial; if removing, keep resizeEvent's).

- [ ] **Step 2: GameLauncher fixes.**
  - Restore per-site toast durations: add `int errorMs = 6000;` to `CorePlan`; `prepareCore` sets 7000 for the archive-extract failure, 6000 for no-system and core-download failures (match the original values at old MainWindow.cpp:1723/1751/1794 — verify each in `git show 99e7462:native/src/ui/MainWindow.cpp` if unsure); `open()` and MainWindow's split router use `plan.errorMs` instead of hardcoded durations.
  - Restore the lost log line: `glLog(QStringLiteral("game: launching in split pane"));` in MainWindow's split router just before `splitTarget_->openGame(...)` — MainWindow has no glLog; use the mwLog equivalent there (grep for the local log helper name in MainWindow.cpp).
  - Kill the double resolution on split→external: the router already holds `plan` — pass it through by calling `launcher_->runEmulator(*em, plan.launchRom, title, thumb, key, plan.systemId)` directly (mirroring what `open()` would do — verify `open()`'s external branch does nothing else material besides the registry lookup + Android guard the router already performed; if it does more, keep the `open()` call and drop this sub-item with a note).
  - Remove the redundant `SystemCatalog::byId(plan.systemId)` re-lookup in `open()` if `prepareCore` can hand back what's needed (add `const GameSystem* sys` to CorePlan only if it stays a plain borrowed pointer with catalog-static lifetime — it does, SystemCatalog entries are static; otherwise skip with a note).
  - `CoreManager::ensureCore`'s dead `parent` param (`Q_UNUSED` at CoreManager.cpp:114): remove the parameter and update all call sites (grep `ensureCore(`).

- [ ] **Step 3: PlaybackSession re-key hardening.** Add the `resumeKey` arg to `setQueue` (see Interfaces); inside, when non-empty, `playIndex`'s initial `beginResume(tracks_[index])` for the STARTING track uses the key instead (simplest faithful mechanic: after the existing `playIndex(startIndex)` call inside `setQueue`, re-key exactly as the callers do today — move the `beginResume(resumeKey)` inside; behavior identical, fragility gone). Update the two call sites (`playStream` keeps its own `beginResume(rkey)` — it doesn't queue; `openAudioStream` passes `rkey` as the new arg and drops its trailing `beginResume`; the StreamResolver `playQueue` connect passes nothing). Add one probe assert: `setQueue({"a.mp3"}, 0, {}, "stable-id")` → persist → new session `beginResume("stable-id")` → `takeResumeSeek()` returns the saved position. Fix MainWindow's imprecise HLS-routing comment at `openStreamUrl` while in the file (plan-1 minor #5).

- [ ] **Step 4: Runner phrasing.** In `run-headless-probes.sh`, change the new loop's `echo "SKIP: $1 (not built)"` to the file's older `(skip) <name> not built` vocabulary.

- [ ] **Step 5: Build + verify.** Full Release build; run the probe suite runner (all green, incl. the new re-key assert — expect probe_playback 10/10); live smoke: launch app, one toast visible and correctly anchored above the bottom of the central area in windowed mode (screenshot), one audio open + resume round-trip. 

- [ ] **Step 6: Commit** — `cleanup: plan-1 extraction debris (notice parenting, error durations, resume re-key, dead params, stale comments)`

---

### Task 2: Route Steam + playlist populates through showSyntheticCatalog

**Files:**
- Modify: `native/src/ui/HomeView.cpp` (`populateSteamGames` ~1470-1494, `populatePlaylists` ~1528-1552, `populatePlaylistItems` ~1571-1588)

**Interfaces:** none new — consumes `showSyntheticCatalog(const MediaCatalog&)` (plan 1).

- [ ] **Step 1:** In each of the three populates, replace the trailing open-coded reset block (`pendingReqId_ = -1; loading_ = false; hasMore_ = false; currentPage_ = 1;` + `hideMeta()` + grid show/hide + `populate(cat, false)`) with `showSyntheticCatalog(cat);`. Byte-compare first: if any of the three blocks differs from showSyntheticCatalog's body in ANY way (ordering, an extra line), STOP and report the difference instead of unifying.
- [ ] **Step 2:** Build app; run probe_browse + probe_nav (green). Live: drive Steam console list and a Playlists folder — render + drill as before (screenshots).
- [ ] **Step 3: Commit** — `refactor: Steam + playlist levels share showSyntheticCatalog's reset path`

---

### Task 3: Playlist catalog builders → src/browse (pure) + probe

**Files:**
- Modify: `native/src/browse/SyntheticCatalogs.h`, `native/src/browse/SyntheticCatalogs.cpp`
- Modify: `native/src/ui/HomeView.cpp` (populatePlaylists/populatePlaylistItems become one-liners; interactive functions stay)
- Modify: `native/tools/probe_browse.cpp` (new asserts)

**Interfaces:**
- Produces (namespace `browse`): `MediaCatalog playlistsCatalog(const QList<Playlist>& all, const QString& catalogKey)` — the playlist list for one catalogue + the trailing synthetic `_newplaylist` row, exactly as populatePlaylists builds today; `MediaCatalog playlistItemsCatalog(const Playlist& p)` — PlaylistEntry→MediaItem mapping incl. the `steam:` and local-`path` special cases. Verify the real `Playlist`/`PlaylistEntry` fields against `native/src/core/PlaylistStore.h` before writing probe initializers.
- HomeView passes `PlaylistStore::forCatalog(key)` / a `PlaylistStore::get` result; any addon-dependent field the mapping needs (scout says entry.addonId is stored on the entry, and addon resolution happens at ACTIVATION time via addonForKey, not at mapping time — verify; if mapping needs the addon, take it as a parameter, keep the builder pure).

- [ ] **Step 1:** Write the failing probe asserts in probe_browse.cpp: build a 2-playlist list → `playlistsCatalog` returns 3 items (2 + New-playlist row, correct marker mimes/ids matching what activation dispatches on — copy the exact marker strings from the moved code); a playlist with one addon entry, one `steam:` entry, one local-path entry → `playlistItemsCatalog` maps each variant's url/id/mime/type/thumbnail exactly as the moved loop does. Run: probe fails to build (functions absent).
- [ ] **Step 2:** Move the two loop bodies verbatim into SyntheticCatalogs.cpp (store list → parameter; `tr` → `QObject::tr`). HomeView populates become one-liners through `showSyntheticCatalog(browse::playlistsCatalog(PlaylistStore::forCatalog(key), key))` etc.
- [ ] **Step 3:** Build; probe_browse green (BROWSE-OK with the new asserts); app builds.
- [ ] **Step 4:** Live: Playlists folder renders, drilling a playlist renders its items, New-playlist row still triggers the Osk prompt (interactive path untouched). Screenshots.
- [ ] **Step 5: Commit** — `refactor: playlist levels as pure probe-tested catalog builders`

---

### Task 4: Steam games builder → src/browse (pure) + probe

**Files:**
- Modify: `native/src/browse/SyntheticCatalogs.h/.cpp`, `native/src/ui/HomeView.cpp` (`populateSteamGames`), `native/tools/probe_browse.cpp`

**Interfaces:**
- Produces: `MediaCatalog browse::steamGamesCatalog(const QList<SteamGame>& installed, const QString& query)` — the SteamGame→MediaItem mapping (`id="steam:"+appid`, mime `steamgame`, poster via `SteamLibrary::posterUrl`) + the in-console query filter, verbatim from populateSteamGames ~1470-1494. Verify `SteamGame` fields in `native/src/core/SteamLibrary.h`. `posterUrl` is a static/pure lookup (verify) — if it needs no I/O it stays inside the builder; if it does I/O, take a `std::function<QString(const SteamGame&)> poster` parameter defaulting to it.
- HomeView: `populateSteamGames()` becomes `showSyntheticCatalog(browse::steamGamesCatalog(SteamLibrary::installedGames(), stack_.last().query));` (query source verbatim from the current code). `requestSteamMeta`, launch, achievements stay in HomeView — out of scope.

- [ ] **Step 1:** Failing probe asserts: 2 installed games + query filter matching 1 → catalog of 1 with exact id/mime mapping; empty query → both.
- [ ] **Step 2:** Move verbatim; rewire; build; BROWSE-OK.
- [ ] **Step 3:** Live: Steam console renders installed games; open one game's detail (meta fetch still works). Screenshot.
- [ ] **Step 4: Commit** — `refactor: Steam games level as a pure probe-tested catalog builder`

---

### Task 5: SearchAggregator — the cross-addon fan-out out of HomeView

**Files:**
- Create: `native/src/browse/SearchAggregator.h`, `native/src/browse/SearchAggregator.cpp`
- Modify: `native/src/ui/HomeView.h/.cpp` (drop `searchAllReqs_/searchAllReqSrc_/searchAllSeen_/searchAllCat_/searchAllQuery_` + `startSearchEverything`; rewire `searchEverything`, `loadTop`'s `_search` branch, and the merge branch of `onCatalogReady` ~3488-3512)
- Modify: `native/CMakeLists.txt` (app source list; add SearchAggregator.cpp to probe_browse's sources if the probe covers its pure dedup helper — see Step 1)

**Interfaces:**
- Produces: `class SearchAggregator : public QObject` —
  - `SearchAggregator(AddonManager* mgr, QObject* parent)`; connects itself to `AddonManager::catalogReady` and filters by its own reqId set (AddonManager reqIds are globally unique positive ints, so self-filtering is safe alongside HomeView's `pendingReqId_` — the current merge branch already relies on exactly this).
  - `void start(const QString& query)` — verbatim port of startSearchEverything's fan-out loop (enabled sources × catalogs, page 1), clearing prior state first. `void cancel()` — clears the req set (results then ignored).
  - `bool active() const` (reqs in flight); `const MediaCatalog& accumulated() const` (for Back-repopulation and re-population).
  - signals: `resultsAppended(const MediaCatalog& add, bool firstBatch)` — the per-reply deduped, source-tagged additions; `finished(int totalResults)` — req set drained.
  - Dedup key + skip rules (info/rechdr/empty rows, "title|type" lowercase, `sourceAddonId` tagging) move verbatim.
- HomeView keeps: the `_search` Level push, grid population (connects `resultsAppended` → `populate(add, append=!items_.isEmpty())` after its usual `generation_`/reset prep — reproduce the exact current sequencing: `startSearchEverything`'s resets at ~1418-1443 stay in HomeView as the pre-`start()` prep, minus the fan-out loop), the no-results toast on `finished(0)`, and `loading_` mirroring (`loading_ = agg_->active()` after `start`; set false in the `finished` handler).
- CRITICAL sequencing to preserve: results that arrive for a STALE search (user typed again / navigated away) must be ignored exactly as today — today that works because startSearchEverything clears the req sets. `start()`/`cancel()` reproduce it. `loadTop`'s Back branch re-fires `start(...)` (not `accumulated()`) — preserve that (yes, it re-queries; that is today's behavior).

- [ ] **Step 1:** Failing probe (extend probe_browse): the dedup/skip rule as a small pure static — `static bool SearchAggregator::acceptResult(const MediaItem&, QSet<QString>& seen)` used by the merge path; probe asserts: duplicate title|type rejected case-insensitively, `_open`/info/rechdr synthetic rows skipped, distinct types with same title accepted. (The async fan-out itself is verified live — don't fake AddonManager in the probe.)
- [ ] **Step 2:** Implement; move the merge branch out of `onCatalogReady` (HomeView's handler shrinks to the non-search path — byte-preserve ITS logic); rewire searchEverything/loadTop; delete the five members.
- [ ] **Step 3:** Build; probes green; grep `searchAll` in HomeView.cpp → zero hits.
- [ ] **Step 4:** Live: cross-addon search from the search box ("everything" search): results stream in from multiple sources, no dupes, opening a result routes through its source addon; search again mid-flight (type a new query quickly) — no stale results bleed in; Back re-runs the search. Screenshots.
- [ ] **Step 5: Commit** — `refactor: extract SearchAggregator — cross-addon search fan-out out of HomeView, dedup probe-tested`

---

### Task 6: Table-driven synthetic-folder injection in populate()

**Files:**
- Modify: `native/src/ui/HomeView.cpp` (the ~3541-3648 injection blocks inside `populate`)

**Interfaces:** internal only — a file-local table `struct SyntheticFolder { const char* markerPrefix; ... std::function<bool()> hasAny; std::function<MediaItem()> makeRow; }` (or the simplest shape that fits — implementer's call, kept file-local). Each current block ("Recent"/"Downloaded"/"Playlists" at root; per-console "Recent"/"Favorites"/"Downloaded") becomes one table row; the guard conditions (`!stack_.last().detail && query.isEmpty() && !recentView_`, per-console variants) must be preserved EXACTLY per block — if the guards differ between blocks (they do: root vs per-console), model that in the table rather than unifying conditions. Byte-preserve each row's marker mime strings, titles, and icon types.

- [ ] **Step 1:** Read all injection blocks; write the table + loop; delete the blocks. If any block resists the table shape without behavior change, leave that block open-coded and say so.
- [ ] **Step 2:** Build; probe_nav + probe_browse green. Live: at a video catalogue root — Recent/Downloaded/Playlists folders appear exactly as before; in a game console — Recent/Favorites/Downloaded appear; folders absent where stores are empty (the hasAny guards). Screenshots.
- [ ] **Step 3: Commit** — `refactor: table-driven synthetic-folder injection in HomeView::populate`

---

### Task 7: Close-out — suite, smoke, spec update

**Files:**
- Modify: `docs/superpowers/specs/2026-07-16-foundation-refactor-design.md`

- [ ] **Step 1:** Full Release build; full runner (all probes); record `wc -l native/src/ui/HomeView.cpp native/src/ui/MainWindow.cpp`.
- [ ] **Step 2:** Live smoke across the plan-2 surface: search-everything, playlists (browse + create), Steam console, synthetic folders, plus one plan-1 regression pass (audio queue, game launch). Screenshots.
- [ ] **Step 3:** Spec edits: Status → `Phase 1 complete (plans 1+2): MainWindow seams, browse providers, shared reset path`; in the HomeView section, replace the `BrowseLevel` interface sketch with a short paragraph recording the superseding pattern (pure builders in src/browse + marker dispatch + showSyntheticCatalog; rationale: equal extensibility, no rewrite of the Level machinery) and note LibraryView request-idiom sharing as a plan-3 candidate.
- [ ] **Step 4: Commit** — `docs+refactor: phase-1 close-out — spec reflects the shipped browse-provider pattern`
