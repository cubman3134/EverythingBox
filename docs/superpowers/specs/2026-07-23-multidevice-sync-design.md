# Multi-Device Sync Upgrade (snapshot → per-item merge) — Design

**Date:** 2026-07-23
**Status:** Approved design; plan next.
**Origin:** The replace-everything roadmap's #1 — the user asked "does the Google Drive
sync not work for this?" — answer: it's a snapshot backup (last-writer-wins whole-file),
not a merge; this track upgrades it on the same Drive transport.

## Scope reality (scout, 2026-07-22)

The merge machinery EXISTS IN MINIATURE: `mymediavault-progress.json` already syncs
`resume/*` (merge-by-`ts`, keep newer, never delete — MainWindow::mergeProgress ~:8849)
and `recent/<profile>/items` (union by id, newest ts, cap 40), pushed debounced 15s
after playback (`scheduleProgressSync` armed from `PlaybackSession::resumeSaved`),
pushed on exit, pulled+merged ~1.5s after startup. The heavy bundle
(`mymediavault-sync.zip`) snapshots THE ENTIRE ini (minus `cloud/*`) + addons/themes/
saves/states, applied wholesale ("always take the cloud" at startup — main.cpp ~:68).

## Design

### 1. Generalize the merge file (the progress-json pattern → all per-item stores)

Serialize/merge pairs per store, all riding the existing push/pull plumbing:

| Store | Timestamp | Merge semantics |
|---|---|---|
| resume/* | `ts` (exists) | unchanged: newest-ts wins, never delete |
| recent/<p>/items | `ts` (exists) | unchanged: union by id, newest, cap 40 |
| marks/<p>/items | **add `updatedAt`** (stamp at saveItem + removeTagEverywhere's rewrites) | newest-wins per item; tagVocab/pinnedTags = union minus tombstoned |
| favorites/<p>/items | **add `ts`** (stamp in save()) | union by id newest-wins + tombstones for removes |
| playlists/<p> | **add `updatedAt` per playlist** | newest-wins per PLAYLIST (whole-object — entry-level merge is overkill; a playlist edited on two devices same-interval = newest playlist wins) + tombstones for deletes |
| stats/<p>/items + playstats/<p> | device-namespaced (below) | NO merge needed — each device syncs only its own namespace |

- **Tombstones**: `deleted/<store>/<key> = ts` recorded on remove; merge honors the
  newest of (item.updatedAt, tombstone.ts); compacted after 30 days.
- **Accumulators (the double-count trap)**: ConsumptionStats + PlayStats accrual moves
  to device-namespaced keys (`stats/<profile>/<deviceId>/items/<hash>`, likewise
  playstats). Each device writes ONLY its namespace; sync unions namespaces verbatim
  (no arithmetic merge possible = no double-count by construction); display sums
  across namespaces (categorySeconds/topTitles/profileTotalSeconds grow a
  sum-across-devices pass; the per-device migration folds the EXISTING un-namespaced
  totals into this device's namespace once, stamped).

### 2. Device identity

`device/id` — UUID minted once at first run (Settings accessor), excluded from sync,
used for the accumulator namespaces and merge diagnostics (meta.json gains it).

### 3. Device-local carve-out (the snapshot bundle's settings.json)

Excluded from the synced settings (stay local): `roms/folder`, `emulators/root`,
`emulators/fullscreen`, `player/externalPath`, `player/external` (device-specific:
the TV has no VLC), `netplay/relay`, `display/mode`, `display/tvPromptDone`,
`emu/virtualPad*`, `sync/files/*` (path-keyed offsets; `sync/global/*` DOES sync),
`profiles/current` (active profile is session state; `profiles/list` syncs),
`device/*`, `cloud/*` (already excluded), DownloadsStore + PcGameStore keys
(absolute paths), `library/showHidden` stays SYNCED (a preference, debatable —
decided: sync it; it's user-level). The exclusion list lives in ONE place
(CloudSync) with a comment discipline: every new device-local key must join it.
The bundle keeps carrying addons/themes/saves/states snapshot-style.
`applyBundle` respects the same exclusions on the way IN (a cloud snapshot must
not clobber local device keys).

### 4. Cadence + triggers

The existing 15s debounce extends: mark/favorite/playlist/stats mutations arm the
same scheduler (store-level dirty flags → the serializer includes only-what-changed
is unnecessary — payloads are KB-class; serialize-all is fine). Startup pull+merge
and exit push unchanged. The startup BUNDLE apply keeps "always take cloud" for its
(now-filtered) content; the merge file is authoritative for the per-item stores —
`applyBundle` must NOT write the per-item store keys at all anymore (they belong to
the merge file exclusively; prevents the snapshot clobbering merged state).

### 5. Recorded limitation (not solved here)

Android/TV sign-in: the OAuth loopback flow (QTcpServer + system browser) is
desktop-shaped; signing in ON Android is untested/fragile and stays an open
follow-up (likely a Custom-Tabs/app-link flow or a device-code flow). Desktop↔
desktop syncs day one; Android instances join when their auth path lands.

## Verification

- Probe (`probe_cloudmerge` or extending the shape): the merge pairs per store
  (newer wins, tombstone beats older item, resurrection prevented, vocab union),
  accumulator namespace union (no double-count across three simulated devices),
  the migration fold (existing totals → device namespace, idempotent), the
  exclusion list (serialize → assert device-local keys absent; applyBundle-in
  respects exclusions). All pure-logic testable (the serializers are ini-in/json-out).
- Live (single machine, two INSTANCES: the deployed app + a portable build with its
  own data dir + the same Drive account — the worktree-pipe technique): mark/watch/
  playlist on A → sync → B sees it after pull; delete on B → A honors the tombstone;
  stats accrue on both → totals = sum, no double-count; device-local keys (display
  mode, roms folder) unchanged on both. Byte-level ini inspection for the carve-out.
- Suite + perf gates (the debounce hook cost is the sync-track precedent; startup
  unchanged — the 1.5s pull is already off the critical path).

## Non-goals

- Solving Android OAuth (recorded follow-up); real-time/push sync (the 15s debounce
  + startup is the model); syncing saves/states per-item (bundle snapshot remains);
  E2E encryption; multi-ACCOUNT sharing (one Drive account = one household).
