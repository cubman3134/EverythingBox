# Local Library Network ID-Resolver (movies) — Design

**Date:** 2026-07-24
**Status:** COMPLETE — shipped on `local/id-resolver` (T1–T5 + a Fable whole-branch round). `CatalogMatch`
(pure, probe-locked), `LocalResolveCache` (persisted device-local JSON), and `CatalogResolver` (async,
throttled, over `requestSearch`/`catalogReady`) are wired; `buildIndex` indexes resolved ids; the
`resolveOnline` toggle + "Re-match" action are live. **Live-verified (the payoff the shipped local-library
track could not produce):** on a portable throwaway with the real `aiocatalog` addon + TMDB key, a fixture
"Interstellar (2014)" (no NFO) resolved live via aiocatalog's TMDB search → `tmdb:movie:157336` cached in
`localresolve.json` → the **real aiocatalog tile lit the "On disk" badge** (`resolver-badge.png`); sibling
Interstellar-named tiles correctly did not. Toggle-off (no resolution) / Re-match verified. Perf flat
(`nav.select` ≤1 ms; resolution is off-thread + debounced). Real deployed app untouched.

## Close-out record — deviations, residual risk, and follow-ups

**Two plan-time deviations (from the addon-search reality):**
- **No year in matching.** Search-result `MediaItem`s carry no year field, so `bestMatch` = unique-normalized-title
  + IMDB cross-check (+ a skip for a `tt…` candidate that *contradicts* a known NFO imdb). A same-title film
  with ≥2 candidates → conservative −1.
- **No `getMeta`.** Searching every installed movie-catalog source already yields each catalog's own tile-id
  space; the winner's id is stored directly.

**Residual mis-match risk (recorded honestly — the earlier "safety is preserved" was too strong):** when a
catalog's search returns only ONE film sharing the local title but of a *different year* (a partial-coverage
catalog that has the wrong "Solaris" but not yours), and the local file has no NFO imdb, the single title hit
is accepted → the wrong tile badges and Seam B would play the wrong local file. Bounded to same-title-different-
year + partial catalog + no NFO. The **getMeta per-candidate year check is the follow-up that closes it**; the
contradicted-`tt` skip already prevents the Cinemeta-id variant.

**Follow-ups (recorded, not built):**
- **Owned ⇒ offer "Play" regardless of stream provider (strong follow-up).** A metadata-only catalog tile
  (aiocatalog with no stream/debrid addon) offers no Play action, so Seam B prefer-local can't be triggered
  from it even when the file is owned — the badge shows but the tile can't play locally. With a stream provider
  present (any setup that can play aiocatalog at all) Play appears and Seam B redirects to the local file, so
  this is an enhancement, not a break. Surfacing Play whenever the item is owned makes owned-but-unstreamable
  films playable from their tile.
- getMeta year-verification (closes the residual above); diacritic-insensitive `normalizeTitle` (Amélie ≠
  Amelie today — a missed badge, never a wrong one); `clearCacheAndRequeue` skip-in-flight (avoid a redundant
  double-queue); prune cache entries for vanished files (currently monotonic — harmless, buildIndex only reads
  scanned paths).

**Original status:** Draft — approved through brainstorming; user-reviewed before plan.
**Origin:** The recorded highest-value follow-up from the local video library (roadmap #2, shipped v0.5.1).
The shipped merge (Seam A badge + Seam B prefer-local) fires only when a browsed catalog tile's `id`
matches an `OwnedIndex` key. NFO-first matching supplies bare IMDB `tt…` keys, but the user's real catalog
(`aiocatalog`) emits `tmdb:movie:{id}` tile ids — so the badge never lights up. This resolver bridges local
files to the id space the catalogs actually use.

## Decisions (user-set, this brainstorm)

- **Resolver source = reuse installed catalog addons' search.** `AddonManager::requestSearch(source, title)`
  returns real tiles with their real ids; matching a local movie against those results and indexing the
  returned id guarantees a byte-identical match to whatever catalog the user browses — and reuses the TMDB
  key `aiocatalog` already holds. No new API key, no hard-coded id format. (Rejected: a standalone C++ TMDB
  client — needs a second key and must hard-code `aiocatalog`'s `tmdb:movie:{id}` composite, fragile if the
  addon changes. Rejected: hybrid — sum of both, more surface than a first cut warrants.)
- **Scope = movies only.** TV (resolve show → tmdb/tt → compose per-episode keys + series-tile count) is the
  immediate next track, reusing this resolver + cache. Owned episodes still browse + play locally today.

## Scope reality (scout, 2026-07-24)

- **A TMDB-catalog tile's `id` is a `tmdb:`-composite, kept verbatim** from the addon: movies
  `tmdb:movie:{tmdbId}`, shows `tmdb:tv:{id}`, episodes `tmdb:episode:{show}:{s}:{e}`
  (`native/addons/aiocatalog/main.js`; parsed verbatim at `AddonModels.cpp:186` / `AddonManager.cpp:384`).
  **Never a bare IMDB `tt` at the tile level.** Cinemeta tiles, by contrast, are bare `tt…`. The IMDB id for
  a TMDB item appears only after a `getMeta` round-trip, in `MediaDetail::imdbStreamId` (`AddonModels.cpp:246`;
  `aiocatalog` builds it from TMDB `external_ids`, unpadded `tt…:s:e`).
- **`requestSearch(LoadedAddon*, query)`** (`AddonManager.h:93`) is a title→tiles primitive returning a
  `MediaCatalog` of `MediaItem`s (ids included) via `catalogReady`. It is **per-source** — only `LibraryView`
  uses it, one source at a time; no host-level merged fan-out exists, so the resolver iterates
  `mgr_->sources()` itself. `requestMeta(src, item)` (`AddonManager.h:94`) enriches an id you already have
  (→ `metaReady(MediaDetail)`), used here to capture the winner's `imdbStreamId`.
- **No C++ TMDB/OMDb client exists** — movie metadata is entirely addon-sourced. The only IMDB↔TMDB bridge is
  inside the `aiocatalog` JS addon (reached via `getMeta`).
- **OwnedIndex key consumption (the shipped Seam A/B, what the resolver must feed):** `ownsId(it.id)` at
  `HomeView.cpp:1409`; `localPathFor(it.id)` → `localPathFor(it.imdbStreamId)` at `HomeView.cpp:3054-3056`
  and `MainWindow.cpp:6570-6572`. Movie keys today = bare `tt…` (`LocalLibrary.cpp:184`). To badge a TMDB
  tile, `tmdb:movie:{id}` must be an OwnedIndex key → local path.
- **API-key/settings precedents:** app-level keys use `Settings` getter/setter + a masked `textf`/`QLineEdit`
  settings row (e.g. `steam/apikey`, `Settings.cpp:115`). Toggle rows use `toggle(id,label,value)` in the
  themed builder. (This track needs only a toggle, not a key.)

## Design

### Components (each isolated + testable)

1. **`CatalogMatch` (pure — `native/src/core/CatalogMatch.{h,cpp}`, probe-tested).**
   `bestMatch(const LocalLibrary::VideoEntry& want, const QVector<MediaItem>& candidates) → int` (index of
   the accepted candidate, or −1). Strict acceptance:
   - If `want.imdbId` is non-empty and a candidate's IMDB id equals it → instant accept (highest confidence).
   - Else accept the candidate whose **normalized title** equals `want.title` **and** whose year is within ±1
     of `want.year` (when `want.year > 0`; if `want.year == 0`, require a unique normalized-title match).
   - Normalization: lowercase, strip punctuation, collapse whitespace, drop a leading article
     (`the/a/an`). Remake collision (same title, different year) with no year agreement → −1.
   - Ambiguous (≥2 candidates tie after the rules) → −1. **Conservative reject beats mis-badge.**

2. **`LocalResolveCache` (persisted — `native/src/core/LocalResolveCache.{h,cpp}`).** A device-local JSON in
   `AppPaths::dataDir()/localresolve.json`: `path → { size, mtime, ids:[…], status: matched|nomatch, ts }`.
   API: `entry(path) → optional`, `putMatched(path, size, mtime, ids)`, `putNoMatch(path, size, mtime, ts)`,
   `isFresh(path, size, mtime)` (true when the record matches current size+mtime AND — for `nomatch` — is
   within the retry-after window, default 14 days), `save()/load()`. **Not synced** (derived, re-resolvable;
   ids are global but the cache is a device-local convenience — added to the `library/` non-sync discipline;
   the resolve JSON lives outside the sync bundle). This is the ONLY new persistence — `LocalLibrary`'s scan
   stays live.

3. **`CatalogResolver` (QObject — `native/src/core/CatalogResolver.{h,cpp}`, holds `AddonManager*`).** The
   async engine. `enqueue(const QVector<LocalLibrary::VideoEntry>& movies)`: for each not `isFresh` in the
   cache (and only when `Settings::resolveOnline()`), queue it. Drains the queue throttled (`maxActive ≈ 2`,
   the `GameMetaAggregator` precedent): per movie, `requestSearch` across the movie-catalog `sources()`,
   collect `catalogReady` results, run `CatalogMatch::bestMatch`; on accept, `requestMeta(match)` → capture
   `imdbStreamId`; record `{ matchTileId (e.g. tmdb:movie:…), imdbStreamId (tt…) }` via `putMatched`; else
   `putNoMatch`. Emits `resolved()` (debounced) when a batch lands. Maps `requestSearch`/`requestMeta`
   `reqId → pending entry` to correlate async replies.

4. **Settings toggle `library/resolveOnline`** (default **true**): `Settings::resolveOnline()/setResolveOnline()`
   + a themed `toggle("library.resolveonline", tr("Match local files to online catalogs"), …)` row beside the
   Local Library folder picker, plus a "Re-match online" `action` that clears the cache and re-runs. Off ⇒
   resolver fully dormant (today's NFO-only behavior, zero network).

5. **`LocalLibrary::buildIndex` grows** to index, per movie: the NFO `imdbId` (as today) **plus** every id in
   that file's `LocalResolveCache` `matched` record → the same local path. Episodes unchanged. Seam A
   (`ownsId(tile.id)`) and Seam B (`localPathFor`) are **untouched** — a `tmdb:movie:123` tile id now simply
   finds a key.

### Data flow

1. **Scan (local, fast):** `scanFolder` → entries. `buildIndex` indexes NFO ids + any **cached** resolved ids
   → install the base index. NFO'd and previously-resolved movies badge instantly; **zero network on relaunch.**
2. **Enqueue:** movies not `isFresh` in the cache (and `resolveOnline` on) → `CatalogResolver::enqueue`.
   Episodes skipped.
3. **Resolve (async, throttled):** `requestSearch` per source → `bestMatch` → on accept `requestMeta` →
   record `{tmdb:movie:…, tt…}`; on reject `putNoMatch(retry-after)`.
4. **Refresh progressively:** each landed resolve → rebuild the index with the new keys → `onLocalLibraryChanged()`
   (debounced: one rebuild per batch). Badges pop in on aiocatalog **and** Cinemeta tiles as resolves complete;
   the UI never blocks.
5. **Persist:** cache written as results land → next launch is all cache hits (no network, no rate-limit
   exposure). "Rescan" re-checks mtimes; "Re-match online" force-clears the cache.
6. **Seams unchanged:** `ownsId(tile.id)` now hits a `tmdb:movie:…` key; Seam B `localPathFor` likewise → the
   local file plays. No render/activation-path change.

### Error / edge handling

| Situation | Behavior |
|---|---|
| Offline / addon error / search timeout | Resolve doesn't complete; base index (NFO + cached) still serves; NFO'd movies still badge IMDB tiles. Entry stays uncached, retries next scan. Never blocks the UI. |
| Wrong-match risk | Strict `bestMatch` (title+year, or IMDB cross-check). Ambiguous / remake-without-year / title-only → no match, no badge. Conservative reject beats mis-badge (and mis-play). |
| `resolveOnline` off | Resolver dormant; NFO-only behavior; zero network. |
| Rate limits | Throttle (maxActive≈2) + persisted cache = one-time per file; `nomatch` carries a retry-after window so a transient failure isn't cached forever. |
| File moved/renamed (path/mtime change) | Cache miss → re-resolve. Stale entries for vanished files never consulted; pruned on rewrite. |
| No installed catalog addon | Nothing to search → every movie is a `nomatch`; NFO ids still work. Degrades to today's behavior. |

## Verification

- **`probe_resolver`** (pure, RED-first, sentinel `RESOLVER-OK`): the `bestMatch` table — exact title+year
  accept; year mismatch reject; article/punctuation/case normalization; IMDB cross-check instant-accept;
  remake disambiguation by year; ambiguous → −1. Plus `LocalResolveCache` read/write, `isFresh`
  size+mtime invalidation, and the `nomatch` retry-after window (all hermetic; `QTemporaryDir` for the cache
  file).
- **Live (finally inducible — the payoff):** with `aiocatalog` installed and a fixture movie whose title+year
  match a real TMDB entry, drive a scan → watch the resolve land → the **badge appears on the actual
  aiocatalog tile**, and activating that tile **plays the local file** (prefer-local). This is the demo the
  shipped track couldn't produce, because resolution now targets the real tile id space. Portable-throwaway
  technique, real deployed app untouched.
- **Suite + perf:** resolution is fully off the render/hot path (async, background, debounced); Seam A/B stay
  O(1) hash lookups. Perf gate confirms `nav.select` flat.

## Non-goals (explicit)

- TV / episode resolution (the immediate next track — reuses `CatalogMatch` + `LocalResolveCache` +
  `CatalogResolver`, adds show-resolution and per-episode key composition).
- A standalone C++ TMDB/OMDb client (we reuse installed addons' search + getMeta bridge).
- Write-back of resolved ids into `.nfo` sidecars.
- Music / books.
- Syncing the resolve cache across devices (each device resolves once; the cache is derived/device-local).
- Any change to Seam A / Seam B, the addon transport, or the sync transport.
