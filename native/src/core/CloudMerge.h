// The generalized multi-device merge document (mdsync T2). One small JSON file rides the existing Drive
// "continue watching" push/pull plumbing (MainWindow::pushProgressNow / pullAndMergeProgress, 15s debounce +
// startup pull) and now carries EVERY per-item store — resume, recents, item marks, favourites and playlists —
// each merged by recency with deletion tombstones so two devices' edits combine instead of clobbering.
//
// serializeAll(root) fills `root` with the whole document; mergeAll(root) folds a remote document into the
// local ini. This is the ONLY reader/writer of the merge document: MainWindow::serializeProgress/mergeProgress
// are thin wrappers over these two.
//
// Pure ini-in / json-out (QtCore + QSettings over the shared portable everythingbox.ini, same posture as the
// other core stores) — NO MainWindow/UI dependency, so it links into headless probes. It reads/writes the ini
// DIRECTLY (not through the per-profile store front-ends, which are current-profile-scoped) so one pass covers
// EVERY profile at once. The only cross-store call is ItemMarks::invalidate() after a merge (its static cache
// would otherwise be stale) — a core dep, not UI.
//
// Document shape (all keys optional; a missing key merges as empty, so an old resume-only file still applies):
//   {
//     "resume":  { "<hash>": {pos,dur,ts,title}, ... },              // newest-ts wins, never delete
//     "resumeTombs": [{key,ts}],                                     // GLOBAL, key = <hash> (issue #150)
//     "recent":  { "<profile>/items": "<list-json-string>", ... },   // union by id, newest, cap 40
//     "recentTombs": { "<profile>": [{key,ts}] },                    // key = the entry's key-else-path (#150)
//     "marks":   { "<profile>": { "items": {"<hash>": <blob>}, "tagVocab": [...], "pinnedTags": [...],
//                                 "vocabTombs": [{key,ts}], "pinnedTombs": [{key,ts}] } },
//     "favorites":{ "<profile>": { "items": [<fav>...], "tombs": [{key,ts}] } },
//     "bookmarks":{ "<profile>": { "items": [<bookmark>...], "tombs": [{key,ts}] } },  // per-book reading marks (#136)
//     "playlists":{ "<profile>": { "items": [<playlist>...], "tombs": [{key,ts}] } },
//     "presets":  { "<profile>": { "items": [<preset>...],   "tombs": [{key,ts}] } },   // saved filters (#184)
//     "metaoverrides": { "<hash>": {title,subtitle,overview,image,updatedAt}, ... },  // GLOBAL, like resume
//     "missed":  { "<profile>": { "<showHash>": <unixSecs> } },       // per-show dismissal watermarks (#25)
//     "stats":    { "<profile>": { "<device>": { "items/<hash>": <blob>, "cat/<cat>/..": <n> } } },
//     "playstats":{ "<profile>": { "<device>": { "<hash>/total": <n>, "<hash>/last": <n>, ... } } }
//   }
//
// Merge semantics (verbatim from the design table):
//   * resume     — per hash keep the newer ts; never delete a local entry on ABSENCE. A clear is a TOMBSTONE
//                  in the global "resume" namespace (issue #150), stamped at the clear; it suppresses any copy
//                  stamped at-or-before it, on this device and on every peer, and a strictly-newer position
//                  beats it so a clear is not permanent. Deliberately NOT #132's husk: a resume clear fires on
//                  every finished episode, so husks would grow with playback and ride the document for ever,
//                  while compact(30) costs only a position a 31-day-dormant peer brings back — and a
//                  month-old playback point is stale anyway. See remoteReplaces for the rule in full.
//   * recent     — per profile union local+remote by stable id (key else path), keep newest ts, cap 40. An
//                  EXPLICIT remove/clear is tombstoned per profile (issue #150) and suppressed before the cap;
//                  a CAP EVICTION is not — it is the list running out of room, not a user forgetting
//                  something, and dating it would make the cap permanent.
//   * marks items— per hash keep the newer updatedAt; never delete (hidden/clear is not a tombstoned delete).
//   * tagVocab   — union(local,remote) MINUS tags tombstoned in vocab space (a deleted tag stays gone).
//   * pinnedTags — union(local,remote) MINUS tags tombstoned in EITHER vocab space (a deleted tag can't be a
//                  shelf anywhere) OR pinned space (a bare unpin retires the shelf without deleting the tag).
//   * favorites  — union by itemId keep newest ts; a tombstone with ts >= the item's ts suppresses it (a
//                  newer re-add — ts strictly greater — beats an older tombstone; resurrection prevented).
//   * bookmarks  — per stable id keep newest ts; tombstone-vs-ts exactly as favourites (issue #136). The id is
//                  the book+position (BookmarkStore::idFor), so the same passage on two devices folds to one
//                  row; a remove tombstones so a peer cannot resurrect it.
//   * playlists  — WHOLE-OBJECT per id keep newest updatedAt; tombstone-vs-updatedAt as favourites.
//   * presets    — per stable id keep newest ts; tombstone-vs-ts exactly as favourites (issue #184). A rename is
//                  an id-stable name edit, not a delete+add, so it folds onto the one id; a delete tombstones so
//                  a peer cannot resurrect it. A legacy #63 row with no id gets a deterministic name-derived id.
//   * metaoverrides — per hash keep the newer updatedAt; never delete. There are deliberately NO tombstones:
//                  "reset to scraped" writes an EMPTY record with a fresh stamp (a husk), so the reset is
//                  itself the newest record and propagates; a deletion would be resurrected by any peer that
//                  still held the old correction.
//   * missed     — per profile, per show: keep the LARGER stamp. No tombstones, no husks and no tie-break,
//                  because the store's only mutation is raising a number: `max` is commutative, associative
//                  and idempotent, so both merge orders converge with nothing left undecided, and "cleared"
//                  is never spelled as a removal (0/absent means never dismissed). Records are COLLECTED
//                  rather than husked — see MissedDismiss.h for why that is safe here and not for #24.
//   * stats/playstats — device-namespaced accumulators: UNION of namespaces, VERBATIM replace of each REMOTE
//                  namespace (a device only ever writes its own), local namespace untouched — NEVER arithmetic,
//                  so repeated merges can't double-count.
// EQUAL-TIMESTAMP TIE-BREAK (mdsync T3, carried from the T2 review): where two devices edited the same key at
// the same second, newest-wins is undecided; a UNIFORM order-independent comparator — the lexically-greater
// canonical JSON value bytes — decides for resume/recents/marks/favorites/playlists, so both merge orders
// converge (this deliberately supersedes the divergent legacy ties: four stores kept-local, recents `>=`).
// Every merge imports the remote tombstones locally (faithful ts) so a device re-propagates a peer's deletion,
// then Tombstones::compact(30) bounds the deleted/* footprint.
#pragma once

class QJsonObject;

namespace CloudMerge
{
    // Serialize the ENTIRE local per-item state (all profiles) into `root`. Caller does QJsonDocument(root).
    void serializeAll(QJsonObject& root);

    // Merge a remote document `root` into the local ini (recency + tombstones), then compact tombstones.
    void mergeAll(const QJsonObject& root);
}
