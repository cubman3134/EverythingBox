# Catalog Prefetch + Embedded Scraper Credentials — Design

**Date:** 2026-07-20
**Status:** Approved design (user-set scope), pending implementation plan
**Relation:** Independent feature track requested mid-B2; runs on its own branch off main
while B2 (themed settings) is paused at its Task-2/Task-3 boundary. B2 resumes after this
merges (low overlap: this touches AddonManager/HomeView catalog machinery + the
ScreenScraper addon; B2's remaining work is MainWindow settings panels).

## Feature 1: Startup catalog prefetch (user requirement)

Menus must not load-on-entry anymore. Decisions (user-set):

- **Depth: catalogs + first page.** At startup, prefetch every enabled addon's catalog
  LIST and each catalog's FIRST PAGE of items. Menu navigation and the first screen of
  every drill render instantly from the warm store; deeper pages still stream on demand.
- **Refresh: startup + addon changes + TTL.** The store re-pulls (a) at every launch,
  (b) whenever an addon is added/removed/enabled/disabled/reconfigured, (c) in the
  background when the app stays open past a TTL (~30 min per entry, staggered — no
  synchronized re-pull storm).

### Architecture

- **`CatalogStore`** (new, `native/src/addons/CatalogStore.{h,cpp}`): owns the warm data
  — per source: catalog list; per catalog: first-page `MediaCatalog` + fetch timestamp.
  API: `catalogs(sourceId)`, `firstPage(sourceId, catalogId)` (hit = instant value,
  miss = nullopt), `refreshAll()`, `refreshSource(id)`, `invalidate(...)`; signals
  `catalogReady/pageReady` for in-flight UI updates. Fetches ride the EXISTING
  AddonManager request machinery (requestCatalog + catalogReady, globally-unique
  reqIds — the SearchAggregator self-filtering precedent) at low concurrency (2-3
  in flight) so startup isn't a request storm; prefetch traffic must not starve
  user-initiated requests (user requests issue immediately; prefetch queue yields).
- **Read path**: HomeView's `issueRequest`/`loadTop` consult the store first — a warm
  hit populates synchronously (no loading state, no spinner) and NO network request
  is issued for catalog lists + first pages within TTL; page>1 requests stream as
  today. `nav.select`/`catalog.load` perf spans naturally measure the win.
- **Refresh triggers**: launch (after addon load, off the startup critical path — the
  prefetch must NOT regress `startup.total`/`firstpaint`: it starts after first
  paint); AddonManager add/remove/enable/reconfigure signals → `refreshSource`;
  per-entry staggered TTL timer (~30 min ± jitter).
- **Perf gates**: `startup.total`/`startup.firstpaint` medians unchanged (±20%);
  `catalog.load` for warm hits ~0 ms (the headline win, measured); memory bounded
  (first pages only; no image prefetch beyond what MetaCache already does).
- Offline/backend-down: prefetch failures are silent (log only) — the store serves
  stale-if-available, and the on-entry path falls back to today's live request +
  existing error toasts. Never a new error surface at startup.

## Feature 2: Embedded ScreenScraper developer credentials (user requirement)

Remove the user-visible requirement to enter ScreenScraper devid/devpassword; ship them
built-in "like RetroBat" — obfuscated in the binary, never in the public repo.

- **Source of truth**: the user's existing devid/devpassword, currently stored in the
  app's local settings on this machine, are copied ONCE into a gitignored local secrets
  file (`native/secrets/screenscraper.secrets`, key=value; `native/secrets/` gitignored
  with a committed README naming the format). Credentials never appear in chat, in
  committed files, or in build logs.
- **Build**: a CMake step generates an obfuscated header (XOR + split constants — the
  RetroBat-class best-effort obfuscation, explicitly NOT cryptography) from the secrets
  file into the build tree. Secrets file absent → the header carries empty creds and
  the build prints one loud STATUS line (public/CI builds degrade gracefully).
- **Runtime**: `AddonContext` exposes `builtinCredential(key)` (deobfuscates on
  demand, returns empty when not embedded). The ScreenScraper addon resolves creds:
  embedded first → stored user settings fallback (hidden from the settings UI). The
  manifest's devid/devpassword fields are removed from the visible addon-settings
  form; the addon's existing fail-loudly behavior (memory: devid required) now fires
  only when BOTH sources are empty.
- **Scope**: ScreenScraper only (the other keyed providers keep user-supplied keys —
  their terms differ; extending later is a one-key-per-line addition to the same
  mechanism).

## Verification

- Prefetch: probe-level — CatalogStore unit probe (hit/miss/TTL/invalidate semantics,
  stale-serve, queue-yields-to-user-request ordering) with a stubbed request layer;
  live — cold launch then IMMEDIATE menu walk with the network traced (warm hits issue
  zero requests; spans show catalog.load ~0), addon disable→refresh observed, TTL
  fire observed with a shortened test TTL env knob (`MMV_PREFETCH_TTL_S` override).
- Creds: build with secrets → addon works with the manifest fields absent (live scrape
  against ScreenScraper on a real ROM); build without secrets → loud STATUS + stored-
  settings fallback still works; `strings`-grep the built exe for the plaintext devid
  (must NOT appear — obfuscation working); repo grep (must never appear).
- Standard gates: full suite, perfbaseline (startup unchanged, catalog.load improved),
  no regressions on the themed surfaces.

## Exit criteria

Menu navigation issues zero network requests for catalog lists + first pages within
TTL (measured); addon changes + TTL refresh observed live; ScreenScraper scrapes with
no user-entered dev credentials and no plaintext creds in repo or binary strings;
startup medians unchanged.
