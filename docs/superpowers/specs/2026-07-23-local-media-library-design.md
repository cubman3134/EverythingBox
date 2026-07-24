# Local Media Library Manager (video: movies + TV) — Design

**Date:** 2026-07-23
**Status:** COMPLETE — shipped on `local/video-library` (T1–T6, 6 tasks + a Fable whole-branch round).
`LocalLibrary` pure core (parse + Kodi `.nfo` + scan + `OwnedIndex`) is probe-locked (`probe_locallib`,
RED-first, incl. the year-heuristic + garbage-nfo + `NxNN` + relative-thumb cases); the cached index scans
off-thread at startup; the "Local Library" synthetic folder, the Seam A on-disk badge, and Seam B
prefer-local playback are wired and reviewed. Prefer-local now **short-circuits before stream resolution**
(`HomeView::resolvePlay`), so owned Stremio/IMDB items play offline with no round-trip (the spec's
merge-target). Perf: `nav.select` flat (avg 27–41 ms vs 28 ms reference), startup flat/improved (scan is
off-thread). **Live-verified** (portable throwaway copy of the deployed app, cloud-stripped, fixture
library — real app never touched): the themed Settings section, the browse folder, the drill listing, and
local-file playback reaching mpv. **Recorded honestly as not live-inducible here:** the Seam A badge — the
test machine's enabled movie catalog keys tiles on **TMDB** ids while owned ids are **IMDB**, so no live
tile could match (badge stays covered by `probe_locallib`'s decision assertions + code-walk).

## Post-ship follow-ups (Fable round + live-smoke)

- **The badge/merge lights up only for IMDB-keyed catalogs (Cinemeta/Stremio).** A TMDB-keyed catalog
  tile (owned id is IMDB) won't badge or prefer-local at the tile until an id bridge exists — this is the
  same limitation the deliberately-seamed **network id-resolver** follow-up closes (it would also fill in
  the missing IMDB ids for TMDB tiles). Highest-value next step for merge coverage.
- **Prefer-local `imdbId`-param path (Minor).** `resolvePlay`'s short-circuit checks `it.id` +
  `it.imdbStreamId` but not the separately-carried `imdbId` param on the TMDB-bridged play paths
  (classic detail Play, `onMetaReady`). Owned items from non-Stremio catalogs still go local via the
  `openLibraryItem` backstop after a stream resolves (correct, just not round-trip-free). One-line fold-in.
- **Badge renders only in `theme2/qml/elements/Grid.qml`** — the classic/XMB/carousel delegates receive
  `onDisk`/`onDiskCount` but don't draw it.
- **Resume-key split** for an episode owned via `tvshow.nfo` with no own sidecar: addon route keys
  `tt:S:E`, browse-folder route keys `local:<path>` — progress doesn't transfer between the two entry
  points (NFO'd movies are fine, both key on the tt id).
- **Season-folder-only classification:** an episode file with no `SxxEyy`/`NxNN` marker in its name (only
  the `Show/Season NN/` folder layout) degrades safely to a local-only movie tile.
- **"Malformed nfo logged once"** (spec §edge table) not implemented — a bad nfo degrades silently.

**Original status:** Draft — approved through brainstorming; user-reviewed before plan.
**Origin:** The replace-everything roadmap's #2 — "Local media library (folder scan → matched
metadata, NFO import)." The Plex/Jellyfin half of the app: make files on disk first-class catalog
content alongside addon (Stremio-style) catalogs.

## Decisions (user-set, this brainstorm)

- **Scope = video only (movies + TV).** Music and books are deferred to later tracks that clone
  this exact scan → match → catalog pattern (different matchers: MusicBrainz artist/album/track,
  OpenLibrary author/title). Not built here.
- **Relationship to addon content = merge into addon tiles (Plex/Jellyfin-like).** A film/series you
  own decorates the matching addon tile with an "On disk" badge and prefers the local file on play —
  rather than living only in a separate silo. This reshapes the render + activation layer and adds one
  hard requirement: every mergeable local file must resolve to the same identity the addon tiles use
  (IMDB id for Stremio-style catalogs). Files that cannot resolve an id still appear as first-class
  local tiles in a browse surface.
- **Match/identity source = NFO / sidecar first.** Honor a Kodi-style `.nfo` next to the file
  (`<uniqueid type="imdb">` / `<imdbid>`) before anything online — the GamelistStore/`gamelist.xml`
  pattern applied to A/V. No new API keys, offline, authoritative when present. Files without an id
  degrade to local-only tiles (see the interaction note below). The network id-resolver is a
  deliberately-seamed follow-up, NOT this track.

### Load-bearing interaction (stated honestly)

Merge-into-tiles needs an id; NFO-only supplies an id only for files that carry a `.nfo`. Most casual
libraries lack sidecars, so **Seam A/B merge fires only for NFO-scraped files** in v1. Everything else
still works — it surfaces as a local-only browse tile (filename-derived title, fully playable, with
marks/resume/stats). The design leaves a clean seam so a later network resolver (reusing installed
Stremio meta addons, e.g. Cinemeta) fills in ids with **no data migration** — the index just starts
resolving what was previously local-only. That resolver is the highest-value follow-up.

## Scope reality (architecture map, 2026-07-23)

The universal catalog item is `MediaItem` (`native/src/addons/AddonModels.h:112`) — the same struct for
addon content, ROMs, downloads, and Steam/Epic/GOG games. There is a direct, working, end-to-end
precedent for "local files as catalog items":

- **`RomLibrary`** (`native/src/core/RomLibrary.h`) — configurable root (`Settings::romsFolder`),
  `scan()` walks folders and returns groups; the settings folder-picker block lives at
  `MainWindow.cpp:8213-8246`.
- **`SyntheticCatalogs`** (`native/src/browse/SyntheticCatalogs.h`) — pure builders turning a store's
  list into a `MediaCatalog` (`downloadsCatalog`, `recentsCatalog`, `favoritesCatalog`,
  `steam/epic/gogGamesCatalog`); the `DownloadedItem → MediaItem` mapping sets `url = path`,
  `mime = kind` (`SyntheticCatalogs.cpp:60-69`). Synthetic folders already sit at every catalogue root
  (`HomeView.cpp:4192-4223`).
- **`GamelistStore`** (`native/src/core/GamelistStore.h`) — reads an on-disk `gamelist.xml` sidecar and
  returns a `MediaDetail` with every media role resolved to a local file. This is the NFO-sidecar
  precedent verbatim.
- **`MetaCache`** (`native/src/core/MetaCache.h`) — per-item art/metadata cache, `keyFor(item)` =
  stable id else url; open-ended roles.
- **Categories** — four buckets `video | audio | game | reading` via `core::mediaCategory(type)`
  (`native/src/core/MediaCategories.h:14`). Movies/series map to `video`.
- **Activation** — `openItem → MainWindow::openLibraryItem` (`MainWindow.cpp:6522`) dispatches on
  `mime`/`url`. Local items use `mime` routing tags (e.g. `localgame:<kind>` today,
  `SyntheticCatalogs.cpp:153`).

Nothing here is architecturally new: a new content source wired into the existing
`MediaItem`/`MediaCatalog` spine.

## Design

### Components (each copies a cited precedent)

1. **`Settings::libraryFolder()` / `setLibraryFolder()`** — key `library/folder`, default empty
   (feature dormant when unset). Copy `Settings::romsFolder` (`Settings.cpp:205-213`). Device-local:
   add `library/folder` to the CloudSync carve-out (`CloudSync.cpp:412-413`) so the path never syncs
   (each device points at its own disk).

2. **`LocalLibrary` scanner** (`native/src/core/LocalLibrary.{h,cpp}`) — QtCore-only, **injectable
   root** (constructor/param, the ExternalPlayer/RomLibrary precedent) so it is probe-testable without
   touching the real folder. Responsibilities:
   - Walk the configured folder recursively; recognize a fixed video-extension set
     (`mkv mp4 avi m4v mov webm mpg mpeg ts wmv flv`).
   - Classify each file:
     - **movie** — `Title (Year).ext` or `Title (Year)/Title (Year).ext`.
     - **episode** — path/name carrying `SxxEyy` (case-insensitive), `NxNN`, or a
       `Show/Season NN/...` layout; extract `show`, `season`, `episode`.
   - Produce `LocalEntry { QString path; Kind kind; QString title; int year; QString show;
     int season; int episode; QString imdbId; QString plot; QString thumbPath; }` (imdbId/plot/thumb
     populated by the NFO reader when present).

3. **`NfoReader`** (in `LocalLibrary.cpp` or `native/src/core/NfoReader.{h,cpp}`) — reads the Kodi
   `.nfo` sidecar (`<movie>` / `<episodedetails>`): `<title>`, `<year>`, `<uniqueid type="imdb">tt…`
   (fallback `<imdbid>`), `<plot>`, `<thumb>` (absolute or file-relative). NFO discovery: `<file>.nfo`
   beside the media, then `movie.nfo` in the movie folder. Absent/malformed → returns empty (scan
   falls back to filename parse; malformed logged once, walk never aborts).

4. **`OwnedIndex`** (in-memory, owned by `LocalLibrary` or a thin holder in MainWindow) — built from a
   live scan: `imdbId → QVector<QString> paths` (for Seam A/B) and `QString path → LocalEntry` (for
   browse). Episodes aggregate to their series (`show → owned episode list/count`). Built async at
   startup (off the hot path, the 1.5s-cloud-pull precedent), and rebuilt on folder-change / manual
   "Rescan." **No persisted store** in v1 (games precedent: live scan is fast + self-healing;
   persistence is a "only if a scan measurably lags a browse-open" follow-up).

5. **`browse::localLibraryCatalog(const OwnedIndex&)`** (extend `native/src/browse/SyntheticCatalogs.h`)
   — pure builder returning a `MediaCatalog` of every `OwnedIndex` entry (movies + series). Each
   `LocalEntry → MediaItem`: `type` = `movie`/`series`, `url` = local path (movies; series expandable →
   episode list), `id` = imdb id when known else `local:<path>`, `mime` = `local:video`, thumb from NFO
   thumbPath via MetaCache when present. Surfaced as a synthetic "Local Library" folder at the video
   category root (the Downloaded/Recent/Favorites pattern, `HomeView.cpp:4192-4223`).

### Data flow

1. **Scan → index.** Folder set (or startup with a folder configured) → `LocalLibrary::scan()` walks
   async → per file: filename parse + adjacent `.nfo` via `NfoReader` → `OwnedIndex` populated
   (`imdbId→paths`, `path→entry`, series aggregation). Rebuilt on folder-change / manual Rescan.

2. **Seam A — render-time decoration (the merge).** At the single choke point where a `MediaItem`
   becomes a themed row (the `MediaArt::writeInto`/row-build hook the plan's scout pins), consult
   `OwnedIndex` by `item.id`:
   - Movie tile, id == owned imdb id → set `onDisk` flag (theme renders an "On disk" badge) + stash the
     local path on the row.
   - Series tile, id == a series you own episodes of → badge "On disk (N)".
   - Not owned / id empty → row unchanged (purely additive; un-owned addon tiles untouched).

3. **Seam B — activation prefer-local.** In `openLibraryItem`, a row carrying a stashed local path (or
   `mime local:video`) → play the local file through mpv directly, skipping addon stream resolution.
   Series detail lists episodes; owned episodes badge + prefer local, un-owned resolve via addon as
   today.

4. **Browse surface.** The "Local Library" synthetic folder lists **everything** in `OwnedIndex`,
   including local-only (no imdb id) files that decorate no tile. Fully playable; keyed `local:<path>`
   so marks/resume/stats work (the Steam-item precedent — stable key = free marks/resume).

### Error / edge handling

| Situation | Behavior |
|---|---|
| No folder configured | Feature dormant: empty index, no synthetic folder, no decoration, zero render cost (ROMs precedent). |
| NFO absent / no `<imdbid>` | Filename title+year, local-only tile, key `local:<path>`; playable, marks/resume/stats work; does not merge. |
| Malformed NFO | Treated as absent, logged once, scan continues (never aborts the walk). |
| File moved/deleted | Next scan drops it; index is live/self-healing. |
| Same imdb id, two files | Index keeps a list; v1 prefers the first playable (quality-ranking = follow-up). |
| Unparseable filename | Lands in browse with the raw stem as title; never crashes, never silently dropped. |
| Huge library / slow scan | Async + off the startup hot path; persisted-index follow-up applies only if a scan measurably lags. |

## Verification

- **`probe_locallib`** (pure logic, RED-first — the QtCore-only scanner/reader are fully unit-testable
  over fixture trees; sentinel `LOCALLIB-OK`):
  - Filename-parse table: `Title (2010).mkv` → movie/title/year; `Show/Season 01/Show - S01E02.mkv` →
    episode/show/1/2; `Show.1x03.mp4` → 1/3; junk stem → unrecognized-but-listed.
  - NFO parse: movie.nfo with `<uniqueid type="imdb">` → imdbId; `<episodedetails>`; malformed → empty;
    embedded `<thumb>` path resolved.
  - OwnedIndex build/query over a fixture tree: `imdb→path`, `path→entry`, a missing-nfo local-only
    entry present with empty imdbId, series episode-count aggregation.
  - Seam-A decoration as a pure function: (index + item id) → owned / badge / localPath.
  - Stable-key derivation for local-only items (`local:<path>`).
- **Live (this machine):** point `library/folder` at a small fixture tree → the Local Library folder
  lists them; an NFO'd movie badges + plays from disk; a non-NFO file plays from the browse folder;
  marks/resume land on a local item. Merge-into-an-addon-tile demonstrated where a matching addon tile
  is present, else recorded honestly against a fixture (the game-importers verification posture).
- **Suite + perf gates:** the only steady-state add is Seam A's per-row `OwnedIndex` lookup (an
  in-memory hash probe); the perf run confirms `nav.select` stays flat.

## Non-goals (explicit)

- Music and books (later tracks, same pattern, different matchers).
- The network id-resolver / Cinemeta reuse (the flagged follow-up that lights up merge for non-NFO
  files). Seamed for, not built here.
- Subtitle sidecars / auto-download (roadmap #5).
- Quality-ranking of duplicate ownership.
- On-disk index persistence (games precedent: only if scans measurably lag).
- Any change to the addon transport or the sync/merge transport.
