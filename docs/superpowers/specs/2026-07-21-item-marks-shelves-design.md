# Item Marks + Shelves (Playnite track 1: tags/favorites/hidden/completion) — Design

**Date:** 2026-07-21
**Status:** Complete: hidden/completion/tags live per profile; favorites/pinned-tag/hidden shelves + browse filter shipped (2026-07-22).
**Origin:** User request — bring Playnite's library-management strengths to all catalogs.
Roadmap (user-set order): THIS (marks+completion+shelves) → category playlists/channels
→ library importers (Steam first) → consumption stats.

## Decisions (user-set)

- **Per-profile ownership** for ALL of it: favorites, hidden, tags, completion. Each
  profile has its own marks and its own tag vocabulary.
- **Favorites stay in the existing per-profile `FavoritesStore`** (already verb-wired
  into detail/home). The new `ItemMarks` store owns hidden / completion / tags only —
  it does NOT duplicate a favorite bit. The "Favorites" shelf builds FROM
  `FavoritesStore`; every other shelf (pinned tags, Hidden, status filters) builds
  from `ItemMarks`. This keeps one source of truth per concern.

## Design

### ItemMarks store (`native/src/core/ItemMarks.{h,cpp}`, QtCore-only, ini-backed)

- Per (profile, item): `hidden` (bool), `completion`
  (None | InProgress | Finished | Abandoned | Planned), `tags` (QStringList). NO
  `favorite` field — favorites stay in `FavoritesStore`; ItemMarks owns hidden /
  completion / tags only.
- Item keys: the SAME stable keys the app already uses (addon itemId / local path
  keys); hashed internally before use as ini group leaves (the SyncOffsets lesson —
  the store owns key sanitization; MD5-over-UTF8 hex tokens).
- Storage (portable `mymediavault.ini`, namespaced by the active profile id or
  `default`): `marks/<profileId>/items/<itemHash>` → a JSON blob `{ hidden,
  completion, tags }` (one object per item); per-profile tag vocabulary
  `marks/<profileId>/tagVocab` and pinned-tag set `marks/<profileId>/pinnedTags`
  each a JSON array. Empty-key = no-op, no junk keys. Removing a tag from the
  vocabulary strips it from all items + unpins it (per profile).
- In-memory cache over the store for filter-speed (built per profile, invalidated on
  any mark change and on profile switch); a change signal for UI refresh.

### Marking UI (detail surface actions — Nav Contract surfaces only)

- **Favorite** toggle, **Hide**, **Status** (completion Choice), **Tags…** (picker
  overlay: the profile's tags with checkmarks, "New tag…" via OSK, "Pin as shelf" /
  "Unpin" per tag).

### Shelves = pinned tags + automatics (NO filter-builder — deferred)

- Automatics: **Favorites** row per catalog home/browse (shown when non-empty);
  existing Continue row untouched; Finished/Planned exposed as browse filters.
- Custom shelves: any tag can be **pinned**; pinned tags render as home rows in
  their catalog. A full filter-preset builder is explicitly OUT (future).
- **Hidden**: excluded from every row/search for that profile; General gains a
  "Show hidden" toggle + a Hidden shelf visible only while it's on. Hidden is
  personal preference — parental control remains the restricted-profile machinery.

### Browse filtering

- Catalog browse gains a **Filter** action (NavMenu): All / Favorites / by status /
  by tag.

### Application point

- Marks apply at the model-building/aggregation step (hidden filtering + shelf
  construction), via the cache — no per-theme QML work; rows arrive as data like
  every existing row.

## Verification

- Probe (`probe_marks`): store semantics — per-profile isolation (profile A's marks
  invisible to B), tag vocab add/remove-strips, key hashing collision independence,
  hidden/completion/tags round-trips, cache invalidation on change + profile
  switch. Registered runner + ci.yml.
- Live: mark → favorite row appears; tag + pin → shelf row appears; hide → vanishes
  from rows/search, reappears under Show-hidden; completion set + browse filter;
  switch profile → clean slate; switch back → marks intact. Byte-identical ini
  restore EXCEPT the marks written by the walk on a throwaway profile (create +
  delete the throwaway; the real profiles' data untouched).

## Non-goals

- Filter-preset builder (future); shared/global marks; auto-tagging from metadata
  (a future importer/stats synergy); parental gating via hidden.

## Follow-ups (deferred, tracked)

- **Filter-preset builder** — deferred by design (see Non-goals). Browse Filter
  ships as a transient, level-scoped one-shot (All / Favorites / by status / by
  tag); saved multi-criteria presets are out until a future pass.
- **Marks-store GC** — hashed item keys accumulate: an item deleted from a catalog
  (or a source removed) leaves its `marks/<profileId>/items/<itemHash>` blob behind
  forever, since MD5 is one-way and there is no reverse map to a live item. Same
  shape as the **sync-store GC** note — both stores need a future sweep that
  reconciles stored hashes against the live catalog key set and prunes orphans;
  worth doing the two together.
- **Auto-tagging** — deriving tags from catalog metadata/importers (future
  importer/stats synergy); today tags are user-authored only.
- **Paged-catalog shelf presence** — shelf membership is tested against the loaded
  page only (`cat.items`), so a pinned tag whose sole members sit on page ≥2 won't
  surface its shelf until that page loads. Single-shot console libraries load whole
  and are unaffected; a future pass could test presence against a full-catalog key
  index rather than the current page.
- **Catalog-level Show-hidden re-fetch** — toggling General → Show hidden updates
  filtering on the next `populate`, but does not force an in-place re-fetch of an
  already-loaded catalog level; a future pass could re-run the current level so the
  Hidden shelf / hidden rows appear without a manual back-and-re-enter.
