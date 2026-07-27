# Local-Library Resolver Polish — Design

**Date:** 2026-07-24
**Status:** COMPLETE — shipped on `local/resolver-polish`. (1) `normalizeTitle` folds diacritics (NFKD +
drop combining marks) so "Amélie"→"amelie"; (2) `bestMatch` adds subtitle-year disambiguation (the year is in
the aiocatalog search row's subtitle) — a strictly-narrowing check that closes the same-title-different-year
residual (Solaris 1972 vs 2002) with no new request; (3) a new suite-gated integration probe
`probe_showdispatch` machine-pins the `CatalogResolver` typed show-dispatch seam (real `AddonManager` + JsLocal
series fixture, zero network) — the exact C1-class seam a pure probe can't reach; a pre-C1 binary fails it.
Per-task Opus reviews clean (only-narrows verified; genuine C1 reproduction verified). No separate Fable pass
(small, low-risk: T1 isolated pure change, T2 test-only, no cross-task seam); the combined-tree suite
(`probe_resolver` + `probe_showdispatch`) is the integration gate.
**Origin:** The recorded follow-ups from the shipped resolver tracks (movie id-resolver + TV resolution): close
the same-title-different-year mis-match residual, fix diacritic-insensitive matching, and add a machine test
for the async show-dispatch seam where the TV-resolution C1 bug lived (invisible to every pure probe).

## Scope reality (scout, 2026-07-24)

- **`normalizeTitle`** (`CatalogMatch.cpp:6-15`) currently maps any accented letter to a space via
  `[^a-z0-9]+`, so `"Amélie"` → `"am lie"` (not `"amelie"`). Pure, `probe_resolver`-covered
  (`probe_resolver.cpp:30-33`), but the `Amélie`/`amelie` assertion is neutered with `|| true`.
- **The year is already in the search row.** aiocatalog sets each catalog row's `subtitle` to the release
  year (`native/addons/aiocatalog/main.js:86`, `subtitle: year(release_date||first_air_date)`). `MediaDetail`
  has **no typed year** (`AddonModels.h:166-183`) — only a free-text `"Released"` `MediaFact`
  (`main.js:175`) — so a `getMeta` year check would parse a human-text fact string. Since the candidate
  `MediaItem` `bestMatch` already receives carries the year in `subtitle`, a subtitle-year check needs **no
  second request**. `VideoEntry::year` is the local comparison source.
- **`AddonManager`** is a concrete `QObject` with **non-virtual** `requestSearch`/`requestCatalog`/
  `requestMeta`/`sources`/`catalogs` (`AddonManager.h:58-95`) — not subclass-mockable without an interface
  extraction. But `probe_addon.cpp` (`probePrefetch`/`makeFixture`, ~:310-420) already stands up a **real**
  `AddonManager` pointed at an `EB_ADDONS_ROOT` temp dir holding a JsLocal fixture addon (manifest.json +
  main.js, zero network), driving async requests with a `spinUntil` event-loop helper (`:275-281`). JsLocal
  dispatch is async (`QtConcurrent`, `AddonManager.cpp:807-818`); `CatalogResolver::resolved()` debounces
  1500 ms (`CatalogResolver.cpp:173-174`).

## Design

### Item 1 — diacritic-insensitive `normalizeTitle` (pure)

Insert an NFKD-decompose + strip-combining-marks step at the top of `normalizeTitle`, before the `[^a-z0-9]`
replace: `s = s.normalized(QString::NormalizationForm_KD);` then drop each `QChar` whose
`category() == QChar::Mark_NonSpacing` (the combining accents). After decomposition the base ASCII letters
survive the existing filter, so `"Amélie"` → `"amelie"`, `"Pokémon"` → `"pokemon"`, `"WALL·E"` unchanged
(`·` is punctuation, still → space). Then **drop the `|| true`** from `probe_resolver.cpp:33` so the
`Amélie`/`amelie` equivalence is a real assertion, and add `Pokémon`/`pokemon`. Still pure.

### Item 2 — subtitle-year disambiguation in `bestMatch` (movies, pure)

In `CatalogMatch::bestMatch` (movies), when scanning title-match candidates, if the local `want.year > 0`
AND a candidate's `subtitle` contains a 4-digit year, require `abs(candYear - want.year) <= 1` for that
candidate to qualify; a candidate whose subtitle year disagrees is skipped. When a candidate has no parseable
subtitle year (unknown/other addon), fall back to today's behavior (it stays a title-match candidate). This
closes the residual for aiocatalog: `Solaris (2002)` vs a catalog offering only the 1972 `Solaris` (subtitle
`1972`) → year disagreement → skipped → no mis-match. The IMDB-cross-check accept (top of `bestMatch`) and the
ambiguity/`type`/contradicted-tt rules are unchanged; this only *narrows* the title-match set, never widens
it — strictly safer. Year parse: first `\b(19|20)\d{2}\b` in `subtitle`. `bestSeriesMatch` (shows) is
untouched — local shows rarely carry a filename year, so year-verify is movies-only.

Deferred (non-goal): a `getMeta`-based year check (parse the `"Released"` fact) for catalogs whose search
rows don't carry a year in `subtitle` — recorded, not built (aiocatalog, the user's catalog, carries it).

### Item 3 — hermetic show-dispatch test (integration probe)

Extend `probe_addon.cpp` (which already links `AddonManager` + the JS engine and has the fixture/spin
helpers) with a `CatalogResolver` show-dispatch test — NO network, NO mock:

1. `makeFixture` a JsLocal addon whose manifest declares a `series`-typed catalog and whose `getCatalog`
   returns a canned series row (`{ id: "tmdb:tv:1396", type: "series", name: "Breaking Bad" }`) for a query.
2. `qputenv("EB_ADDONS_ROOT", …)`; construct a real `AddonManager mgr;`, a temp-file `LocalResolveCache`,
   and a `CatalogResolver resolver(&mgr, &cache);`.
3. `resolver.enqueue({ one Episode VideoEntry: show="Breaking Bad", season=1, episode=1 })`.
4. `spinUntil` the resolver's `resolved()` fires (allow for the 1500 ms debounce + async dispatch).
5. Assert `cache.seriesIdsByShow()` maps the show key (`showKeyFor` of the entry) → `["tmdb:tv:1396"]` — i.e.
   the typed `requestCatalog` dispatch produced series-typed results that `bestSeriesMatch` accepted and the
   resolver cached. A negative control: a movie-only fixture catalog yields `nomatch` for the show
   (no series-typed result).

This machine-pins the C1 seam (typed show dispatch → series results → match → cache). Sentinel folds into
`probe_addon`'s existing pass/fail. (Rejected: extracting an `AddonManager` interface to mock — a non-trivial
refactor of a shipped class the real-addon harness makes unnecessary.)

## Verification

- **Item 1 + 2:** `probe_resolver` (pure) — the diacritic equivalences (assertion un-defanged); the
  subtitle-year table: `Solaris (2002)` vs a `1972`-subtitle candidate → −1/skip; `±1` tolerance accepted;
  no-subtitle-year candidate still matches on title; an owned year matching wins.
- **Item 3:** `probe_addon` — the show-dispatch harness above (positive: series fixture → cached
  `tmdb:tv:{N}`; negative: movie-only fixture → nomatch). Real `AddonManager`, real JS, zero network.
- Full headless suite + app compile. No Seam A/B change, no resolver behavior change beyond the narrower
  movie match; the show-dispatch probe asserts the *existing* (post-C1-fix) behavior — a regression net, not
  new behavior.

## Non-goals

- `getMeta`-based year verification (deferred; subtitle-year closes it for the user's catalog).
- Extracting an `AddonManager` interface / mock framework (the real-addon harness avoids it).
- Any change to `bestSeriesMatch`, Seam A/B, the cache format, or the addon/sync transports.
- Music/books; the movie/TV resolution behavior itself (this is matching-quality + test polish only).
