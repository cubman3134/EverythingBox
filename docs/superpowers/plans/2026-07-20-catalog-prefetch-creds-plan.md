# Catalog Prefetch + Embedded Scraper Creds Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Menus never load-on-entry (warm first pages from a startup prefetch riding the existing 30-min catalog cache), and ScreenScraper works with zero user-entered developer credentials (compiled-in, obfuscated, never in the repo).

**Architecture:** Scout-corrected: catalog LISTS are already synchronous manifest data — the win is FIRST PAGES. AddonManager's existing `catalogCache_` (30-min TTL, keyed id|catalog|query|page|filters) becomes the warm store: a `CatalogPrefetcher` fills it post-first-paint at low concurrency and re-sweeps on triggers; a new synchronous `cachedCatalog()` peek lets HomeView populate warm hits with zero spinner and zero request. Creds ride a new compiled-in `builtinCredential` channel (the hidden-settings alternative is cosmetic — JS can read its own config keys). Spec: `docs/superpowers/specs/2026-07-20-catalog-prefetch-scraper-creds-design.md`.

**Tech Stack:** Qt 6 C++ (AddonManager/HomeView), Duktape JS addon (`native/addons/screenscraper/main.js`), CMake `configure_file` (first use in this repo), probe pattern.

## Global Constraints

- Branch `feature/prefetch-creds` off MAIN (B2 is paused on `themed/b2-plan`; do not touch its unmerged work). Commit, do NOT push.
- Startup budget: `startup.total`/`startup.firstpaint` medians unchanged (±20%) — the prefetch starts AFTER first paint and must never block the GUI thread.
- Prefetch must yield to user requests: user-initiated `requestCatalog` calls issue immediately; the prefetch queue holds ≤3 in flight (JsLocal requests consume global-thread-pool slots — an unthrottled sweep floods it; scout §1).
- Offline/backend-down: prefetch failures log-only; the on-entry path falls back to today's live request + existing toasts. No new startup error surface.
- Credentials NEVER in chat, committed files, build logs, or plaintext binary strings. `native/secrets/` is gitignored with a committed README.md (format doc only).
- Machine facts (all tracks): build `cmake --build build --config Release [--target <t>]`; probes need Qt bin (+ `/c/mpv-dev`) on PATH, `QT_QPA_PLATFORM=offscreen`, **and `QT_PLUGIN_PATH=C:\Qt\6.8.3\msvc2022_64\plugins`** (the offscreen QPA plugin isn't deployed beside the probes — without it they block on a modal dialog; this explained three sessions of phantom failures). Deploy to `C:\MyMediaVault-app` keeping the exe name; `MMV_UITEST=1` + uitest.py only; protect user data; `rtk` proxy normal.
- Scout anchors (verified 2026-07-20, branch tip): `catalogs()` sync/manifest AddonManager.cpp:698-713; `catalogCache_`/`CatalogCacheEntry`/TTL AddonManager.h:195-198 (`kCatalogCacheTtlMs` 30 min); cache-populate lambda .cpp:497-508 (stores only reqIds in `pendingCatalogKey_` — prefetch requests cache naturally); cache-hit queued re-emit .cpp:836-844; `reload()` clears .cpp:599; concurrency = unthrottled QtConcurrent per request .cpp:784-808; `sourcesChanged()` emits .cpp:537/586/1507/1522 but **`setEnabled` (.cpp:1550-1554) emits NOTHING** (landmine); enable toggle call site LibraryView.cpp:178-186; HomeView `issueRequest` .cpp:3567-3580 (the warm-peek slot), `onCatalogReady` .cpp:3582-3611, `buildTabs` sync catalogs read .cpp:857-887; post-paint hook main.cpp:220 (the startup.total zero-timer); ScreenScraper = bundled JsLocal `native/addons/screenscraper/{main.js,manifest.json}`, creds = 4 manifest fields (ssid/sspassword user + devid/devpassword dev) → `addoncfg/screenscraper/<key>` via getConfig (main.js:91-94, AddonContext.cpp:37-51), fail-loudly devid check main.js:100-105; JS globals bound JsAddon.cpp:102-107 (`js_getConfig` :81-84 is the pattern); AddonSettingsDialog renders every manifest field, no hide flag (AddonModels.h:14-20); NO configure_file precedent in CMakeLists; reqIds globally unique (one shared `reqCounter_`); stale-disabled-addon cache landmine (no isEnabled guard on cache/serve).

---

### Task 1: Prefetcher core + cache peek + the enable/disable signal gap

**Files:**
- Create: `native/src/addons/CatalogPrefetcher.{h,cpp}`
- Modify: `native/src/addons/AddonManager.{h,cpp}` (peek API + enabled-guard + the missing signal), `native/src/addons/AddonContext.cpp` only if the fixture needs it (it shouldn't)
- Modify: `native/tools/probe_addon.cpp` (+ a tiny fixture addon under the probe's control), `native/CMakeLists.txt` if probe sources change

**Interfaces (Task 2 consumes verbatim):**

```cpp
// AddonManager additions:
std::optional<MediaCatalog> cachedCatalog(LoadedAddon* src, const QString& catalogId,
                                          const QString& query, int page,
                                          const QMap<QString,QString>& filters) const;
// Synchronous peek into catalogCache_: returns the entry iff present, within TTL, AND the
// source is currently enabled (closes the stale-disabled landmine for ALL cache serving —
// also add the isEnabled guard to the async cache-hit path at .cpp:836-844).
void sourceEnabledChanged(const QString& id, bool enabled);   // NEW signal
// setEnabled() now emits it (and invalidates that source's catalogCache_ entries on disable).

// CatalogPrefetcher (owned by MainWindow, ctor'd after addons_):
CatalogPrefetcher(AddonManager* mgr, QObject* parent);
void start();          // post-first-paint kick: sweep enabled sources × catalogs(s), page 1
void resweep();        // re-run (triggers); skips entries still comfortably within TTL
// Internals (probe-pinned): FIFO queue, ≤3 in flight, user requests unaffected (the
// prefetcher just calls mgr_->requestCatalog and tracks ITS OWN reqIds; results land in
// catalogCache_ via the existing ctor lambda — no new storage); staggered TTL resweep
// timer (~25 min ± 0-5 min jitter per sweep, env override MMV_PREFETCH_TTL_S scales it);
// wired to sourcesChanged + sourceEnabledChanged -> resweep (disable also just stops
// serving via the peek guard); failures logged (mwLog idiom), never retried within a sweep.
```

- [ ] **Step 1 (RED):** extend `probe_addon.cpp` with a fixture JsLocal addon (written to a temp dir by the probe: minimal manifest with 2 catalogs + a main.js serving a 3-item page) and asserts: `cachedCatalog` miss → nullopt; after a `requestCatalog` completes → hit with the same items; TTL expiry (inject a short TTL — make `kCatalogCacheTtlMs` overridable via `MMV_PREFETCH_TTL_S` in AddonManager for testability) → miss again; disabled source → nullopt even when cached + the async hit path also refuses; `setEnabled(false)` emits `sourceEnabledChanged`; CatalogPrefetcher on the fixture: sweep enqueues source×catalogs page-1, ≤3 in flight at any poll, all land in cache; resweep skips fresh entries (request count doesn't grow); sentinel stays `ADDON-OK`.
- [ ] **Step 2:** implement; build; GREEN; probe_nav/probe_navqml untouched-green (env recipe incl. QT_PLUGIN_PATH).
- [ ] **Step 3: Commit** — `feat: CatalogPrefetcher + cache peek + enable/disable signal (menus stop loading-on-entry, part 1)`

---

### Task 2: Warm read path + startup kick + live verification

**Files:**
- Modify: `native/src/ui/HomeView.cpp` (`issueRequest` warm peek), `native/src/ui/MainWindow.cpp` (ctor: prefetcher after `addons_`; post-paint `start()` — chain a `QTimer::singleShot(0, ...)` AFTER the startup.total end at main.cpp:220's pattern, but from MainWindow so it owns the object; verify ordering: paint first, prefetch after)
- Modify: `native/src/core/UiTestServer.cpp` if a request-counter readout helps verification (optional, gated like the rest)

**Mechanics:** in `issueRequest` (HomeView.cpp:3567), BEFORE the loading_/spinner block: peek `cachedCatalog(top.addon, top.catalogId, top.query, page, top.filters)`; warm hit → `PerfTrace::begin/end("catalog.load")` around a synchronous `populate(cat, append)` with `loading_` never set (no spinner, no pendingReqId_); miss → today's async path unchanged. Page>1 requests benefit automatically when cached but are NOT prefetched. The themed XMB first-drill path flows through the same issueRequest — verify, don't duplicate.

- [ ] **Step 1:** implement the peek + kick.
- [ ] **Step 2: Live verification (the headline):** deploy; cold launch; wait for the prefetch sweep to drain (watch the addon/debug log); then walk EVERY category and drill into 6+ catalogs across addons — `catalog.load` spans all ~0-2 ms (warm), NO "Loading…" spinner appears (screenshots), and the addon log shows ZERO new catalog requests during the walk (the requests all happened at sweep time). Then: disable an addon in Library → its rows vanish + peek refuses; re-enable → resweep warms it; `MMV_PREFETCH_TTL_S=60` run → observe a resweep re-fetch after expiry. Backend-down honesty: if the remote addon host is down, note which sources could be exercised (the bundled/local ones always can).
- [ ] **Step 3: Startup gate:** `perfbaseline.py` 3 runs — `startup.total`/`startup.firstpaint` medians within ±20% of the current baseline (record the table); `nav.select` unchanged.
- [ ] **Step 4:** full suite; commit — `feat: warm catalog read path + post-paint prefetch kick (menus stop loading-on-entry, part 2)`

---

### Task 3: Embedded ScreenScraper developer credentials

**Files:**
- Create: `native/secrets/README.md` (committed: format doc `devid=...\ndevpassword=...`, no values), `native/cmake/GenerateSecrets.cmake` or inline CMake block, `native/src/addons/BuiltinSecrets.h.in` (template → generated header in the build tree)
- Modify: `native/CMakeLists.txt` (the configure step — FIRST configure_file in this repo; wire the generated header's include dir), `.gitignore` (`native/secrets/*` except README.md)
- Modify: `native/src/addons/AddonContext.{h,cpp}` (`QString builtinCredential(const QString& key) const` — deobfuscate on demand), `native/src/addons/JsAddon.cpp` (bind `builtinCredential` global, mirroring `js_getConfig` at :81-84 + registration at :102-107)
- Modify: `native/addons/screenscraper/main.js` (devid resolution: `builtinCredential('devid')` first, `getConfig('devid')` legacy fallback; same for devpassword; the fail-loudly check now fires only when BOTH are empty), `native/addons/screenscraper/manifest.json` (REMOVE the devid/devpassword settings fields — ssid/sspassword remain)

**Obfuscation (explicitly best-effort, commented as such):** the CMake step reads `native/secrets/screenscraper.secrets`, XORs each byte with a fixed rolling key, emits the result as two split `constexpr unsigned char` arrays in the generated header; `builtinCredential` re-joins + de-XORs at call time. Secrets file absent → empty arrays + one loud CMake `STATUS` line (`ScreenScraper builtin credentials NOT embedded — secrets file missing; addon falls back to user settings`).

**One-time local step (the implementer does this on this machine):** read the user's existing values from `mymediavault.ini` (`addoncfg/screenscraper/devid` + `devpassword` — the app's own store, already on this machine) and write `native/secrets/screenscraper.secrets`. NEVER echo the values into the report, logs, or chat — verify presence by length only.

- [ ] **Step 1:** CMake step + header + AddonContext + JS binding; build with secrets present (verify the STATUS line does NOT print) and without (rename the file; STATUS prints; build still succeeds; restore).
- [ ] **Step 2: Verification:** live scrape: clear the stored devid/devpassword from a COPY of the config (back up ini; clear keys; run; ScreenScraper metadata still resolves on a real ROM hover/detail — the embedded path; restore ini byte-identical). `strings`-grep the built exe for the devid plaintext (read length from the secrets file, grep for the value via a script that never prints it — assert absent). Repo grep for the value (same discipline — assert absent). Manifest fields gone from the addon-settings UI (screenshot). Legacy fallback: with the secrets build ABSENT (the no-secrets exe), stored settings still work.
- [ ] **Step 3:** full suite; commit — `feat: ScreenScraper dev credentials embedded (obfuscated, gitignored secrets; fields removed from UI)`

---

### Task 4: Close-out — exit criteria measured + final review prep

- [ ] Exit-criteria audit against the spec: zero-request menu walk (measured), addon-change + TTL refresh observed, scraper works with no user-entered dev creds, no plaintext in repo/binary, startup unchanged (tables in the report).
- [ ] Full suite + perfbaseline + a themed-surface regression spot-check (home/browse/detail unaffected).
- [ ] Spec Status → `Complete: prefetch live (zero-request menus), ScreenScraper dev creds embedded. B2 resumes.` Commit — `feat: close out prefetch + creds track`.
