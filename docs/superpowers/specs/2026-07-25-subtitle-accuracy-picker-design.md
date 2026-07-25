# Subtitle Accuracy + Picker (roadmap #5 gap-fill) — Design

**Date:** 2026-07-25
**Status:** Complete — live-verified against the user's real OpenSubtitles account (2026-07-25).
**Origin:** Roadmap #5 ("subtitle auto-download"). A scout found **most of #5 is already shipped**, so this is a
targeted gap-fill, not a build.

## Already shipped — do NOT rebuild

- **A real OpenSubtitles REST v1 client** (`native/src/core/SubtitleFetcher.{h,cpp}`): `Api-Key` header,
  `/login` bearer token (in-memory), `/subtitles` search, `/download` + link GET to disk, VIP `base_url`
  re-point, 401/403 token reset. Credentials are user-supplied (`subs/osApiKey|osUser|osPass`), never embedded.
- **Settings + UI on both surfaces**: `subs/onByDefault`, `subs/language` (single), the three credential
  fields — themed rows (`MainWindow.cpp:8062-8070`, handlers `:8236-8244`) and QWidget (`:8560-8629`).
- **Auto-fetch on play**: `armSubtitleFetch` (`MainWindow.cpp:6192-6205`) arms `subCtx_`; the real trigger is
  the `MpvWidget::fileLoaded` lambda (`MainWindow.cpp:293-321`), which fetches **only when no usable sub track
  already exists** (`hasSub`, computed in `MpvWidget.cpp:279-309`) and then `player_->addSubtitle(srt)`.
- **Player subtitle API**: `subtitleTracks()`/`setSubtitleTrack()`/`cycleSubtitle()`, `addSubtitle()` via mpv
  `sub-add … select` (`MpvWidget.cpp:471-476`), delay/scale; the overlay already has track selection, sync
  steppers, `📂 Load from file…`, and a one-click OpenSubtitles button (`MainWindow.cpp:6311+`).
- **Sidecar `.srt` auto-load**: mpv is initialized with `sub-auto=fuzzy` (`MpvWidget.cpp:56-59`), so a
  `Movie.srt` beside `Movie.mkv` **already works with zero code** — and it correctly suppresses a redundant
  download, because the sidecar appears in `track-list` so `hasSub` is true.

## The gaps this closes (user-selected: accuracy bundle + picker; multi-language explicitly skipped)

1. **Local-library files never get an exact match.** `armSubtitleFetch` reads `item.imdbStreamId`, but
   `localLibraryCatalog` (`SyntheticCatalogs.cpp:81-99`) puts the imdb id in `it.id` and never sets
   `imdbStreamId` — so every local file falls through to the *title-query* fallback
   (`LocalLibrary::displayTitle`, e.g. `"Blade Runner (1982)"`) instead of `imdb_id=`.
2. **No caching — every replay re-downloads.** `SubtitleFetcher::download` (`:210-235`) writes
   unconditionally; no existence check, no key→file memo. Replays burn the OpenSubtitles daily quota.
3. **No results picker.** The manual button fires one `fetch()` and silently accepts the client's pick; the
   parsed candidate array (`SubtitleFetcher.cpp:160-176`) is discarded.
4. **No `moviehash` search** — the exact-release match, and the one thing that meaningfully fixes *sync* on
   local rips. (The `QCryptographicHash` include at `SubtitleFetcher.cpp:16` is unused and is **not** the seam:
   the OSDb hash is not a cryptographic digest.)

## Design

### 1. `SubtitleHash` (pure — `native/src/core/SubtitleHash.{h,cpp}`)

The OpenSubtitles **OSDb hash**: a 64-bit value = **filesize + the sum of all little-endian `uint64` words
across the first 64 KiB and the last 64 KiB** of the file, rendered as **16 lowercase hex digits**. Files
smaller than 128 KiB have no valid hash.

```cpp
namespace SubtitleHash {
    QString ofFile(const QString& path);                       // "" when unreadable or < 128 KiB
    QString ofBytes(const QByteArray& head, const QByteArray& tail, qint64 size); // pure, probe-testable
}
```
`ofFile` reads only the two 64 KiB windows (never the whole file) and delegates to `ofBytes`, which is the
pure, fixture-testable core.

### 2. `SubtitleCache` (persisted — `native/src/core/SubtitleCache.{h,cpp}`)

Device-local JSON at `AppPaths::dataDir()/subtitles.json`, modeled on `LocalResolveCache`:
```cpp
class SubtitleCache {
public:
    explicit SubtitleCache(QString filePath);
    void    load();  void save() const;
    QString lookup(const QString& key) const;               // "" if absent OR the file no longer exists
    void    put(const QString& key, const QString& srtPath); // overwrite-on-put (the picker's choice wins)
    void    clear();
    static QString keyFor(const QString& identifier, const QString& lang);  // "<identifier>|<lang>"
};
```
`lookup` returns empty when the recorded `.srt` has been deleted behind our back, so a stale entry
self-heals into a re-fetch.

### 3. `SubtitleFetcher` — priority chain, cache-awareness, `searchList`

- **`fetch(imdbStreamId, title, lang, localPath, done)`** gains `localPath` (empty for streams). Chain,
  first hit wins:
  1. **Cache** — `keyFor(hash|imdbStreamId|title, lang)`; a live hit returns the path with **zero network**.
  2. **moviehash** — only when `localPath` is non-empty and hashable → `moviehash=<hash>`.
  3. **IMDB** — `imdb_id=` (movie) / `parent_imdb_id`+`season_number`+`episode_number` (episode), as today.
  4. **Title `query=`** — the existing last resort.
  Every success writes the cache entry under the identifier that matched.
- **`searchList(imdbStreamId, title, lang, localPath, done)`** — runs the same chain but returns the parsed
  candidates (language, release/file name, download count, `file_id`) instead of auto-picking. A separate
  `downloadChoice(fileId, lang, key, done)` fetches one and writes the cache under `key`.

### 4. IMDB plumbing for local items (the cheap win)

In `browse::localLibraryCatalog` (`SyntheticCatalogs.cpp:86-95`), also set `it.imdbStreamId`:
- movie with a known imdb id → that `tt…`;
- episode with a known `seriesImdbId` → `seriesImdbId:season:episode` (unpadded) — **exactly** the format
  `SubtitleFetcher` already parses into `parent_imdb_id`/`season`/`episode`;
- otherwise leave empty (title fallback, as today).
`it.id` is unchanged (it remains the OwnedIndex/merge key).

### 5. Picker UI

In the existing subtitle overlay (`MainWindow::showSubtitleMenu`), add **"Search subtitles…"** beside the
current one-click button: it calls `searchList`, shows a themed list of candidates (language · release ·
download count), and on selection downloads via `downloadChoice` → `player_->addSubtitle(path)`. The chosen
entry **overwrites** the cache for that key, so a corrected pick sticks on replay.

## Data flow

Play → (unchanged) `armSubtitleFetch` arms `subCtx_`; local items now carry `imdbStreamId`, and the item's
local path rides along for hashing → `fileLoaded` fires only when no usable track exists → `fetch` walks
cache → hash → imdb → title → `sub-add`. Manual → "Search subtitles…" → `searchList` → user picks →
`downloadChoice` → `sub-add` + cache overwrite.

## Error / edge handling

| Situation | Behavior |
|---|---|
| File < 128 KiB or unreadable | No hash; fall through to imdb/title. Never an error surface. |
| Not configured (missing key/user/pass) | Feature dormant exactly as today; the picker shows an "add your OpenSubtitles credentials in Settings" line instead of an empty list. |
| Network / 401 / 403 | Existing behavior (token cleared, re-login next fetch); the picker shows "couldn't reach OpenSubtitles". Never hangs. |
| Zero results at every tier | One quiet notice; **nothing cached**, so a later retry isn't suppressed. |
| Cached `.srt` deleted behind our back | `lookup` returns empty ⇒ treated as a miss ⇒ re-fetch ⇒ cache updated. |
| Download-limit / quota error | Surface THAT message specifically (it is the one the user can act on), not a generic failure. |
| Streams (no local bytes) | No hash tier; cache + imdb + title apply unchanged. |
| A sidecar `.srt` or embedded track exists | `hasSub` true ⇒ auto-fetch skipped entirely (shipped behavior, preserved). |

## Verification

- **`probe_subs`** (new, pure, RED-first, sentinel `SUBS-OK`): OSDb hash over a synthetic fixture with known
  bytes (a value computed by an independent implementation in the probe itself), the `< 128 KiB` → empty case,
  and endianness; `SubtitleCache` round-trip / hit / miss / missing-file invalidation / put-overwrite;
  `keyFor` composition; the priority-chain decision as a pure function (cache-hit short-circuits; hash only
  when a local path exists; imdb before title); the local-item `imdbStreamId` composition (movie vs episode
  vs unknown).
- **Live (genuinely testable here):** a local-library file with **no** sidecar → play → a subtitle is fetched
  and appears; replay → the cache short-circuits (no second download); open the picker → choose a different
  candidate → it loads and **sticks** on replay. **Gated on the user's OpenSubtitles credentials being
  configured** — if they are not, verify the dormant path + fixtures and record the live pass as user-gated.
- Suite + app compile. No perf run (fetching is off the render path, on the file-loaded event).

## Live verification outcome (2026-07-25)

Everything under "Verification" ran against the real API with the user's credentials; **nothing was deferred**.
Two OpenSubtitles downloads were spent, the agreed budget.

- Auto-fetch fell through the moviehash tier to the IMDB tier and attached the `.srt`. ✅
- Replay hit the cache with **zero network**. ✅
- The picker downloaded a **non-default** candidate and that choice **survived a replay** (the overwrite works). ✅
- The sticky "Searching…" notice cleared on Back. ✅
- A plain `Open Video…` file showed the "no title or IMDB id to match on" wording — not misleading credential advice. ✅

**Two defects the code reviews could not see, both found here:**

1. **The picker's rows were not all on screen.** `kMaxCandidates = 20` was chosen as a *display* bound on the
   belief that NavMenu shows every row; in fact NavMenu never scrolled, so only 12 of 20 rendered at
   1280×760 while the ring walked invisibly to row 20. Fixed in the nav kit itself (NavMenu now clamps and
   scrolls) rather than by guessing a smaller cap — the cap is now only a quota/parse bound. Re-verified
   live at 1280×760, 1204×583 and 980×620; short menus are byte-for-byte unchanged in appearance.
2. **The log never named the matching tier**, so a title-tier hit on the wrong cut was indistinguishable
   from an exact moviehash match without comparing subtitle runtime to video runtime by hand. The chain now
   logs the tier it matched on and each tier it skipped.

**Known gap (not a regression — recorded as follow-up):** the picker is only armed on the
`openLibraryItem` route. Reaching a video through **Recents** goes via `openVideoPath`, which clears
`subCtx_`, so the panel reports "no title or IMDB id to match on" even for a file that has both. The message
is honest about the state but the state is avoidable.

**Cosmetic, accepted:** with the list scrolled to the top, the bottom-most partially-visible row sits flush
with the card's inner edge. This is ordinary scroll-viewport behaviour and doubles as a "more below" cue;
rounding to whole rows is not well-defined here because rows word-wrap to variable heights.

## Non-goals

- Multi-language preference (user explicitly skipped; the API would accept CSV).
- Subtitle editing / resync beyond the shipped delay+scale controls.
- Non-OpenSubtitles providers; burning-in or transcoding subtitles.
- Changing the shipped auto-fetch gate, the credentials UI, or `sub-auto=fuzzy` sidecar loading.
