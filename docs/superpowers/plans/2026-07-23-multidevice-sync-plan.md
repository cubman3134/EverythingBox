# Multi-Device Sync Upgrade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Per-item multi-device sync on the existing Drive transport: all per-item stores merge by recency with tombstones, accumulators become device-namespaced (no double-count by construction), device-local keys stop syncing.

**Architecture:** The proven `mymediavault-progress.json` pattern (serialize/merge pairs, 15s debounce, startup pull+merge) generalizes into a probe-linkable `core/CloudMerge` module; MainWindow's existing serializeProgress/mergeProgress move there and grow per-store pairs. Spec: `docs/superpowers/specs/2026-07-23-multidevice-sync-design.md` (the scout's anchor map is in its Scope section).

**Tech Stack:** Qt 6.8.3, QJson, the existing CloudSync Drive plumbing, headless probes.

## Global Constraints

- Branch: `sync/multidevice` off main. Standing autonomy through the merge gate.
- ANCHOR ON FUNCTION NAMES. Key anchors (main@101c1bd): `MainWindow::serializeProgress` (~:8822) / `mergeProgress` (~:8849) / `scheduleProgressSync` (~:8907) / `pullAndMergeProgress` (~:8919); `CloudSync::buildBundle` (~:380, settings.json = whole ini minus cloud/*) / `applyBundle` (~:411) / the progress push/pull (~:553-575); write funnels: `ItemMarks::saveItem` (+ removeTagEverywhere's direct rewrites), `FavoritesStore::save`, `PlaylistStore`'s setValue at plKey (~:108); accrual sites: `ConsumptionStats` writers, `PlayStats::markPlayed/addSession`; `cloudPullAtStartup` (main.cpp ~:59, "always take the cloud").
- Merge semantics VERBATIM from the spec's table (newest-wins per item; playlists whole-object newest-wins; tombstones beat older items; 30-day compaction; accumulators = namespace union, NO arithmetic merge).
- Pure-function discipline: every serialize/merge pair is ini-in/json-out logic in core/CloudMerge — probe-linkable, no MainWindow dependency; MainWindow delegates.
- The exclusion list lives in ONE place (CloudSync) and applies BOTH directions; applyBundle must no longer write ANY per-item store key (the merge file owns them).
- Timestamps: epoch seconds, stamped at every write funnel incl. the removeTagEverywhere rewrite path; migration folds (existing un-namespaced accumulator totals → this device's namespace) are one-time, stamped, idempotent.
- Env recipe COMPLETE everywhere (the 6×-disproven hang false-alarm); serial builds; probes RED-first, runner + ci.yml; app-target linkage same-task; ini snapshot/byte-identical restores on the REAL profile (throwaway/portable instances for mutation tests); Weekend Picks untouched.

---

### Task 1: device identity + timestamps + tombstones

**Files:** Modify `native/src/core/Settings.{h,cpp}` (deviceId() — mint UUID once, key `device/id`), `native/src/core/ItemMarks.{h,cpp}` (updatedAt in the blob + stamps at saveItem AND removeTagEverywhere's rewrites), `native/src/core/FavoritesStore.{h,cpp}` (ts field), `native/src/core/PlaylistStore.{h,cpp}` (updatedAt per playlist, stamped by every mutator); Create `native/src/core/Tombstones.{h,cpp}` (record(store,key), all(store), compact(olderThanDays=30), per-profile-aware where the store is); wire remove-sites: FavoritesStore::remove, PlaylistStore::remove, ItemMarks (hidden isn't a delete — no tombstone; tag deletion tombstones the tag name in vocab-space); Create `native/tools/probe_cloudmerge.cpp` (starts here, grows in T2) + CMake/runner/ci.yml.

**Interfaces (Produces):** `Settings::deviceId()`; blob fields `updatedAt`/`ts` readable by T2's serializers; `Tombstones::{record,all,compact}` with entries `{key, ts}`.

- [ ] Probe RED: deviceId mints once + persists + excluded-shape (just the key name pinned); updatedAt/ts stamped on every funnel (write → read blob → field present + sane); tombstone record/all/compact semantics (compact drops only >30d). Sentinel CLOUDMERGE-OK.
- [ ] Implement; suite green; commit `feat: device id + store timestamps + tombstones (mdsync T1)`.

---

### Task 2: CloudMerge module — the generalized serialize/merge pairs

**Files:** Create `native/src/core/CloudMerge.{h,cpp}`: move serializeProgress/mergeProgress logic here as `serializeAll(QJsonObject&)`/`mergeAll(const QJsonObject&)` covering resume (unchanged semantics), recents (unchanged), marks (newest-updatedAt per item + vocab/pinned union-minus-tombstoned), favorites (union by id newest-ts + tombstones), playlists (whole-object newest-updatedAt + tombstones); Modify `native/src/ui/MainWindow.cpp` (serializeProgress/mergeProgress delegate to CloudMerge; the dirty-trigger extension: mark/favorite/playlist mutations arm scheduleProgressSync — find the cheapest signal/callsite hooks: ItemMarks/FavoritesStore/PlaylistStore have no signals — add a lightweight change-callback the stores invoke, registered by MainWindow, or hook the UI mutation sites; pick the design that keeps stores QtCore-clean and document); probe_cloudmerge grows the full merge matrix.

**Interfaces:** Consumes T1's fields/tombstones. Produces: `CloudMerge::serializeAll/mergeAll` — the ONLY reader/writer of the merge document.

- [ ] Probe RED: per-store pairs (newer wins each direction; tombstone beats older, loses to newer re-add; resurrection prevented; vocab union; playlist whole-object; recents cap; resume never-delete); a three-way sequence test (A-write, B-write, merge both orders → convergent).
- [ ] Implement + wire triggers; suite green; live smoke (single instance): a mark change → the progress push fires within ~15s (log evidence). Commit `feat: CloudMerge generalized per-item merge (mdsync T2)`.

---

### Task 3: device-namespaced accumulators + migration + display sums

**Files:** Modify `native/src/core/ConsumptionStats.{h,cpp}` (accrual writes to `stats/<profile>/<deviceId>/items/<hash>` + rollups likewise; one-time stamped migration folding the legacy un-namespaced keys into this device's namespace; readers — get/categorySeconds/categoryPages/topTitles — sum across device namespaces via childGroups), `native/src/core/PlayStats.{h,cpp}` (same shape: `playstats/<profile>/<deviceId>/<hash>`; profileTotalSeconds sums devices; formatLastPlayed reads max), CloudMerge (serialize the namespaces verbatim — union on merge, never arithmetic), probe extensions.

- [ ] Probe RED: migration fold idempotent (run twice = once); three simulated device namespaces → totals sum exactly, merge union preserves each verbatim (no double-count over repeated merges); the display readers' sums.
- [ ] Implement; suite green (probe_stats + probe_marks unaffected semantics — their asserts may need namespace-aware updates: keep their CONTRACTS identical by pointing them at the same public API, which now sums; adjust fixtures only). Commit `feat: device-namespaced accumulators + migration (mdsync T3)`.

---

### Task 4: the carve-out — exclusions both directions + bundle hands-off

**Files:** Modify `native/src/core/CloudSync.cpp`: ONE exclusion table (the spec's list VERBATIM: roms/folder, emulators/root, emulators/fullscreen, player/externalPath, player/external, netplay/relay, display/mode, display/tvPromptDone, emu/virtualPad*, sync/files/*, profiles/current, device/*, cloud/*, downloads*, pcgames/*; library/showHidden SYNCS) applied in buildBundle's settings.json AND applyBundle; applyBundle additionally skips ALL per-item store prefixes (resume/, recent/, marks/, favorites/, playlists/, stats/, playstats/, deleted/) — the merge file owns them; meta.json gains device/id; probe extension: serialize→excluded absent; apply(fixture-with-excluded-and-store-keys)→local values untouched.

- [ ] Probe RED → implement → suite green. Commit `feat: device-local carve-out + bundle hands-off per-item stores (mdsync T4)`.

---

### Task 5: two-instance live verification + close-out + fable + merge

- [ ] Live (the portable-build technique: the deployed app [instance A, real data, SNAPSHOT ini] + a portable build/Release copy in the scratchpad with its own data dir [instance B] — BOTH signed into the same Drive account; B seeds a fresh profile-set via first-run + pull): A marks/favorites/playlist-edit + watches 30s → push (or Sync now) → B pulls at launch → sees all of it merged; B deletes the favorite → A pulls → tombstone honored, no resurrection; stats accrue on both → A's panel totals = A+B sums; A's display/mode + roms/folder unchanged by B's pushes; the bundle round-trip leaves per-item stores alone (byte-inspect). Restore A's ini byte-identical EXCEPT sync-legitimate merged additions from the test (delete the test artifacts via the UI first, push, THEN restore-verify — leave zero test residue in the cloud copy either: final push after cleanup).
- [ ] Spec status → complete (+ the Android-OAuth follow-up prominent); full suite + perf 3 runs; fable whole-branch (dimensions: merge convergence/edge ordering, the applyBundle hands-off completeness vs "always take cloud", accumulator migration safety on REAL data, exclusion-list completeness vs the ini's real key population, the trigger hooks' cost); fix rounds; merge+push+redeploy.

## Self-Review (done at write time)

- Spec coverage: §1 merge table ✅T1+T2; accumulators ✅T3; §2 device id ✅T1; §3 carve-out ✅T4; §4 cadence/triggers ✅T2; §5 limitation recorded ✅T5 close-out; verification ✅ per-task + T5's two-instance matrix.
- Type consistency: Settings::deviceId, Tombstones::{record,all,compact}, CloudMerge::{serializeAll,mergeAll}, the namespace shapes — consistent across tasks.
- Ambiguities resolved: hidden ≠ delete (no tombstone); playlists merge whole-object; store change-notification design left to T2's implementer with the QtCore-clean constraint stated; probe contracts for stats/marks preserved via the public API.
