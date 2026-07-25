# TV / Episode ID-Resolution — Design

**Date:** 2026-07-24
**Status:** COMPLETE — shipped on `local/tv-resolution` (T1–T3 + a whole-branch C1 fix). Series matcher +
`composeEpisodeId` + `buildIndex` TV keys + a show-level cache + group-by-show resolution. **The whole-branch
review + live verify caught a Critical the three green per-task reviews structurally missed:** show jobs first
reused the movie track's untyped `requestSearch`, which aiocatalog defaults to a *movie* search — so no show
ever matched (dead end-to-end). Fixed to a **type-scoped `requestCatalog(source, seriesCatalogId, showTitle)`**
(aiocatalog TMDB `/search/tv`; Cinemeta a well-formed `/catalog/series/…/search`). **Live-verified** (portable
throwaway, aiocatalog): a no-NFO "Breaking Bad" resolved to `tmdb:tv:1396`, the **series tile badged "● 2"**,
and an owned episode **played the local file** via the composed id `tmdb:episode:1396:1:1`. Real app untouched.

## Close-out follow-up

- **Hermetic show-orchestration test.** The async show-job path has no machine test (pure cores are
  probe-covered; the C1 bug lived in the transport seam, invisible to every pure probe). A small
  `catalogReady`-stub test driving a show job through the typed dispatch would pin this seam against
  regression. Not a gate (the live aiocatalog pass covers it now), but worthwhile.
**Origin:** The recorded follow-up from the movies-only id-resolver
(`2026-07-24-local-library-id-resolver-design.md`). That track lit the "On disk" badge on aiocatalog MOVIE
tiles; TV was deferred. This extends resolution to shows so **series tiles badge "On disk (N)"** and **owned
episodes prefer-local from the series detail** — on the user's real aiocatalog (TMDB-namespace) catalog, not
only NFO'd Cinemeta shows.

## Decision (user-set, this brainstorm)

- **Approach = COMPOSE catalog episode ids from the series tile id, not DRILL the series detail.** The scout
  verified both catalogs' episode ids are deterministic functions of the series tile id + (season, episode):
  aiocatalog `tmdb:tv:{N}` → `tmdb:episode:{N}:{s}:{e}`; Cinemeta bare `tt…` → `tt…:{s}:{e}`. One search per
  show yields everything — no drill. (Rejected: DRILL — format-agnostic but heavy for aiocatalog, a two-level
  crawl series → seasons → episodes, N `getDetail` calls per show; buys nothing composition doesn't for the
  two catalogs the user runs.) Tradeoff accepted: a small per-catalog episode-id-shape table (two entries,
  scout-verified); a third catalog with an unknown shape degrades safely (it neither
  badges nor prefers-local — `composeEpisodeId` returns "" so `buildIndex` skips both the episode key and the
  series count — a full, safe fallback that never risks a wrong key or a misleading badge).

## Scope reality (scout, 2026-07-24)

- **Series tile id:** aiocatalog `tmdb:tv:{N}` (`aiocatalog/main.js:84`, `path=="tv"`); Cinemeta bare `tt…`
  (`AddonManager.cpp:384`, verbatim from `metas[].id`). A series search returns the series tile with these ids
  (`parseStremioCatalog`, `AddonManager.cpp:958/384`).
- **Episode tile id (composable):** aiocatalog `tmdb:episode:{N}:{s}:{e}` where `{N}` is the **bare numeric**
  tmdb id = `tmdb:tv:{N}` minus its `tmdb:tv:` prefix (`aiocatalog/main.js:158`, `showId==parts[2]`); Cinemeta
  `tt…:{s}:{e}` unpadded (`parseStremioVideos`, `AddonManager.cpp:436`). Season/episode numbers unpadded on
  both sides (`QString::number` locally, raw TMDB numbers remotely).
- **Current OwnedIndex episode keys** (`LocalLibrary.cpp:193-199`): `pathById[seriesImdbId:season:episode]` +
  `seriesCount[seriesImdbId]` (both `tt`-namespace, unpadded).
- **Seam A** (`HomeView::browseItems`, `:1440-1445`): `ownsId(it.id)` → `onDisk`; `ownedEpisodes(it.id)` →
  `onDiskCount`. Works for a bare-`tt` Cinemeta series tile today; a `tmdb:tv:{N}` aiocatalog tile never
  matches (the gap this closes).
- **Seam B** (`resolvePlay` head `:3081-3091`, mirrored in `openLibraryItem`/`playThemedLeaf`):
  `localPathFor(it.id)` → else `localPathFor(it.imdbStreamId)`. A Cinemeta episode tile's `it.id` is `tt…:s:e`
  → already matches; an aiocatalog episode tile's `it.id` is `tmdb:episode:{N}:{s}:{e}` → matches only once
  that key is indexed (this track).
- **Resolver episode-skip seam:** `CatalogResolver::enqueue` `:38` `if (e.kind != Kind::Movie) continue;`.
  Job model is single-title (`job->movie`, one `requestSearch(s, job->movie.title)` `:74`).
- **Scanned show identity** (`LocalLibrary.h` VideoEntry): `show` (name), `seriesImdbId` (from `tvshow.nfo`,
  often empty), `season`, `episode`. Group scanned episodes by `seriesImdbId` (else `show`) to resolve each
  distinct show once — mirrors how movie resolution feeds `extraMovieIdsByPath` into `buildIndex`.
- **Cinemeta TV already works for NFO'd shows** (series tile `tt…` = `seriesImdbId`; episode `tt…:s:e` =
  OwnedIndex key). The resolver's NEW value: (1) aiocatalog (tmdb) shows badge + play at all; (2) shows with
  NO tvshow.nfo get resolved by title.

## Design

### Components (all extend the shipped resolver; Seam A/B untouched)

1. **`CatalogMatch` — a show matcher.** Add `bestSeriesMatch(showTitle, seriesImdbId, candidates) → int`
   (or extend `bestMatch` with a media-kind param): accept a candidate only when its `type` is `series`/`tv`
   AND (its `tt` id equals `seriesImdbId` when that is non-empty → instant accept) OR (unique normalized show
   title). Contradicted-`tt` skip + ambiguous → −1, verbatim from the movie matcher's safety model. Pure.

2. **`CatalogResolver` — group-by-show + compose.** Extend `enqueue` to also take episodes: group them by
   `seriesImdbId` (else cleaned `show` name); one job per distinct show carrying the title, the `seriesImdbId`,
   and the set of owned `(season, episode)`. `startJob` searches each catalog source once for the show title.
   On a confident `bestSeriesMatch`, **compose** per the matched series tile id's shape:
   - `tmdb:tv:{N}` → series key `tmdb:tv:{N}`; episode keys `tmdb:episode:{N}:{s}:{e}` per owned `(s,e)`.
   - bare `tt…` → series key `tt…`; episode keys `tt…:{s}:{e}` per owned `(s,e)`.
   Record in the cache (below). No confident match → `nomatch` (same timeout-safe rule: only cache `nomatch`
   when searches actually replied; timeout/no-source leaves it uncached to retry).

3. **`LocalResolveCache` — TV entries.** In addition to the movie `path → ids`, cache per show: the resolved
   **series tile id(s)** and, per owned episode, its **composed episode id → the episode's local path**.
   Concretely: reuse the path-keyed map for episodes (each episode file `path → [composedEpisodeId]`), plus a
   show-level map (`seriesImdbId`-or-`show`-key → `[resolvedSeriesTileId]`). Persisted, device-local,
   timeout-safe, same as movies.

4. **`LocalLibrary::buildIndex` — index the resolved TV keys.** Extend the episode branch: from the cache,
   additionally `seriesCount[resolvedSeriesTileId] += 1` per owned episode (so a `tmdb:tv:{N}` series tile
   badges "On disk (N)"), and `pathById[composedEpisodeId] = episodePath` (so an episode tile prefers local).
   The existing `seriesImdbId`-keyed entries remain (NFO'd Cinemeta shows keep working); resolved keys are
   ADDED, first-wins. Movie behavior unchanged.

### Seams — unchanged

- **Seam A:** `ownsId(seriesTile.id)` now finds the resolved `tmdb:tv:{N}` `seriesCount` entry → badge;
  `ownedEpisodes` → the owned-episode count.
- **Seam B:** `localPathFor(episodeTile.id)` finds the composed `tmdb:episode:{N}:{s}:{e}` (or `tt…:s:e`) →
  plays the local file, skipping stream resolution (the shipped prefer-local short-circuit, incl. the
  owned⇒offer-Play gate for detail leaves).

### Data flow

1. Scan → group episodes by show (`seriesImdbId` else `show` name) → one job per distinct show.
2. `requestSearch(source, showTitle)` per source → `bestSeriesMatch`.
3. On match: compose the series tile key + one episode key per owned `(s,e)` → `LocalResolveCache`.
4. Debounced rebuild: `buildIndex` indexes `seriesCount[seriesTileId]` (count) + `pathById[episodeId → path]`.
5. Seam A badges the series tile "On disk (N)"; Seam B prefers local on an owned episode. Progressive (badges
   appear as resolution lands); relaunch is all cache hits (no network).

### Error / edge handling

| Situation | Behavior |
|---|---|
| Show with no `tvshow.nfo` | Grouped by name, resolved by title; gains `tt…` and/or `tmdb:tv:{N}` keys per matched catalog. New win over today. |
| Same-name show collision / show-vs-movie same title | `bestSeriesMatch` requires `type` series/tv + unique title → conservative reject; no mis-badge. |
| Own some episodes, not all | Count = owned only; only owned episodes get keys; un-owned episode tiles untouched. |
| Third catalog, unknown episode-id shape | `composeEpisodeId` returns "" → `buildIndex` skips BOTH the episode key AND `seriesCount`, so the series tile does NOT badge and its episodes fall back to normal resolution — full, safe degradation (under-badge is deliberate: never badge a series whose episodes can't be wired to local). |
| Offline / no catalog source / `resolveOnline` off | No resolution; NFO'd Cinemeta shows still work; zero network (movie-track behavior). |
| Season packs / duplicate episode files | Keyed per distinct `(s,e)`; existing first-copy-wins + distinct-episode count carry over. |
| Contradicted `tt` (tvshow.nfo id ≠ a candidate's `tt`) | That candidate skipped (never wins on title) — the movie matcher's contradicted-`tt` guard, reused. |

## Verification

- **`probe_resolver`** (extended, RED-first, pure): the show matcher (`type` filter; unique-title accept;
  `tt` cross-check; contradicted-`tt` skip; ambiguous → −1); the compose table (aiocatalog
  `tmdb:tv:{N}` → `tmdb:episode:{N}:{s}:{e}`; Cinemeta `tt…` → `tt…:{s}:{e}`; unpadded); and the buildIndex
  TV-key indexing (a resolved series tile id → `ownedEpisodes` count; a composed episode id →
  `localPathFor` = the episode path). All hermetic.
- **Live (portable throwaway, aiocatalog):** an owned show (episodes on disk, no tvshow.nfo) → after resolve,
  the aiocatalog **series tile badges "On disk (N)"** → drill in → an **owned episode prefers-local** (plays
  the on-disk file, not a stream). A non-owned show shows no badge. Real deployed app untouched.
- Suite + app compile; perf unaffected (resolution off the render/hot path; Seam A/B stay O(1)).

## Non-goals

- A standalone TMDB/TVDB client (reuse addon search + composition).
- Drilling series detail for episode ids (composition chosen).
- getMeta year-verification for movie same-title remakes (the movie track's separate follow-up).
- Music/books; NFO write-back; syncing the resolve cache.
- Any change to Seam A/B, the addon transport, or the sync transport.
