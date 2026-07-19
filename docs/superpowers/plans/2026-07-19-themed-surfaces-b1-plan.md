# Themed Surfaces B1 Implementation Plan (Phase 3, Subsystem B, Plan 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The themed detail view replaces the classic info page, and all four readers get themed chrome on the Nav Contract — books, PDFs, comics as chrome over their engines; audiobooks as a fully themed now-playing page.

**Architecture:** Detail rides the EXISTING theme-view switching (`currentView` → the `detail` view already declared in theme.json), with detail zones added to the themed NavGraph. Reader chrome strategy is settled by an opening composition spike (strip-overlay QQuickWidgets over the raster readers; full transparency only if the spike proves it); audiobooks avoid compositing entirely (mpv renders no video for audio — the themed page IS the surface). Spec: `docs/superpowers/specs/2026-07-19-themed-surfaces-design.md`.

**Tech Stack:** Qt 6 C++/QML on the established stack (NavGraph contract, NavThemeGraph builder pattern, probe_navqml, theme.json element schema, ThemedChoice/ThemedTextField).

## Global Constraints

- Migration rule (spec, binding): a surface is themed only when it (1) registers via a shared graph builder, (2) passes its probe shape-test (validate() + directed move-BFS), (3) uses ThemedTextField/ThemedChoice for all input. QML self-registers leaf zones only; graph shapes are C++ builders.
- Reader engines untouched (render/persist/resume machinery); chrome replacement only. Classic widget chrome stays for classic-home mode; hosted mode suppresses it.
- Latency gate: `nav.select` median ≤2 ms drift vs 16 ms baseline (perfbaseline medians, 3 runs) at close-out.
- probe_nav + probe_navqml + full suite green every commit; new shape-tests use the shared-builder discipline (NavThemeGraph.h pattern — production and probe call the same function; drift structurally impossible).
- Watchdog stays armed in all live testing (new QML surfaces are where stale-frame bugs would appear); any detection is a finding, not noise.
- First commit of this plan: `git rm` the accidentally committed `.superpowers/polish-audit/p3t3-*.png` (11 files, ~4 MB) and add `.superpowers/` to `.gitignore` if absent (do NOT rewrite history).
- Machine facts (as all tracks): build `cmake --build build --config Release [--target <t>]`; probes need Qt bin (+ /c/mpv-dev) on PATH + offscreen QPA; deploy to C:\MyMediaVault-app keeping the exe name; MMV_UITEST=1 + uitest.py only; protect user data; `rtk` proxy normal.
- Scout anchors (verified 2026-07-19): theme.json `views.detail` exists (Default/theme.json:23-34); view switching = `currentView` prop + `theme.views[currentView]` (ThemeView.qml:14,22; Key_I→detail + Esc→detailReturn qml:306-309); element loading qml:332-339/380-406; `dataCtx.selected` aliases from `MediaArt::writeInto` (AddonModels.h:109; roles h:83-110); themed graph stored per-widget as `mmvNavGraph` (ThemeEngine.cpp:231,317-320); nav arbitration `updateNavForPage` (MainWindow.cpp:1007-1046); the retiring bridge `infoPageRequested`/`themedReturnAfterDetail` (HomeView.h:99; MainWindow.cpp:445-460); detail data `MediaDetail` (AddonModels.h:161-178) + `showMeta`/`requestMeta` (HomeView.cpp:3065,3327) + action row (HomeView.cpp:559-627, `resolvePlay` h:228); readers: EbookView chrome `menu_`/`tocList_` (EbookView.cpp:438-488, API openBook/persist + homeRequested/backRequested signals, own keyPressEvent, WA_OpaquePaintEvent cpp:65, position = topTextPosition char offset); PdfView bar (PdfView.cpp:40-78, QPdfView viewport); ComicView bar (ComicView.cpp:59-90, QScrollArea, two-up spread, NO stream-issue affordance); audio chrome = playlist_/mediaControls_/playerButtons_ (MainWindow.cpp:295-301,486-546) driving PlaybackSession (media/PlaybackSession.h) + MpvWidget controls (video/MpvWidget.h:21-45, QOpenGLWidget — GL surface, hostile to overlays); themed QQuickWidget is forced-opaque software backend (ThemeEngine.cpp:197-204 clearColor) — NO transparency precedent in the tree.

---

### Task 1: Composition spike — settle the reader-chrome architecture (+ PNG cleanup commit)

**Files:**
- Create: `native/tools/spike_readerchrome.cpp` (a THROWAWAY probe-style harness — deleted at the end of this task; its FINDINGS ship, not its code)
- Modify: `.gitignore`, delete `.superpowers/polish-audit/p3t3-*.png` (the housekeeping commit, FIRST, separate)
- Create: `docs/superpowers/specs/2026-07-19-themed-surfaces-design.md` gains a `## Composition decision (Task 1 outcome)` section

**Interfaces:** Produces THE DECISION Tasks 3-4 build on. Candidate architectures to test, in order:
- **A (preferred if it works): strip overlays** — two small OPAQUE QQuickWidgets (top bar, bottom bar) as children over the reader widget; no transparency anywhere; auto-hide = hide()/show() with the fade inside the strip's own QML. Test over EbookView (WA_OpaquePaintEvent custom painter) and a QScrollArea like ComicView: keys still reach the reader beneath when strips are visible? strips render above without flicker on page turns? resize behavior?
- **B: full transparent layer** — one full-size QQuickWidget child with `WA_TranslucentBackground` + clearColor alpha 0 + no backdrop rect: does the software Quick backend composite it over the raster readers at all? (Do NOT test over MpvWidget/GL — audiobooks don't need it, per the plan architecture.)
- **C (fallback if A and B both fail): framed hosting** — the reader widget shrinks into a content rect; themed chrome renders in the same-level sibling regions (no overlay at all). Ugliest to lay out; always works.

- [ ] **Step 1: Housekeeping commit** — `git rm .superpowers/polish-audit/p3t3-*.png`; ensure `.superpowers/` is gitignored. Commit: `chore: remove accidentally committed audit screenshots; ignore .superpowers scratch`.
- [ ] **Step 2: Build the spike harness** — a windowed (NOT offscreen; compositing is the question) mini-app: a stub page with a custom-painted opaque widget (mimicking BookPageWidget) + variant A strips + variant B layer, keyboard toggles between variants, on-screen readout. Drive it via screenshots (`QWidget::grab` + on-disk dumps at each state; grab() is software-composited, so ALSO verify visually via a normal windowed run screenshot through uitest-style capture of the real screen if grab() and screen disagree — note the difference).
- [ ] **Step 3: Record the decision** — the spec section states: which variant per reader class (raster readers; note audiobooks = no compositing by design), the evidence (screenshot paths in .superpowers/), and the constraints discovered (input routing, flicker, resize). Delete the spike source. Commit: `docs: reader-chrome composition decision (spike outcome)`.
- [ ] **Step 4: Report.** If NO variant is workable for some reader, STOP — the controller re-plans that reader with the user.

---

### Task 2: Themed detail view on the contract (+ classic info-page retirement)

**Files:**
- Modify: `native/src/ui/nav/NavThemeGraph.h` (the shared builder grows detail-view zones/edges — one function, both callers)
- Modify: `native/src/theme2/qml/ThemeView.qml` (detail view key handling → nav.*; action-row element hookup)
- Create: `native/src/theme2/qml/elements/ActionRow.qml` (themed action row element: Play/Download/Favorite/Add-to-playlist buttons as a contract zone; registered like the xmb actions zone)
- Modify: `native/themes-sample/...`/default `theme.json` (detail view gains the actionrow element + facts element if missing)
- Modify: `native/src/ui/MainWindow.cpp` (route themed info-requests to `currentView="detail"`; DELETE the `infoPageRequested`→classic swap at :445-460 and `themedReturnAfterDetail_`/`themedDetailFrom_`)
- Modify: `native/src/ui/HomeView.cpp` (detail data → the themed dataCtx: on meta-ready in themed mode, push `MediaDetail` fields + `art.writeInto` into the selected map / a `detailCtx` property; expose action invocations — reuse the existing `actionRequested(QString)` ThemeBridge signal with verbs "play"/"download"/"favorite"/"playlist")
- Modify: `native/tools/probe_navqml.cpp` (detail-view shape test via the shared builder: validate() + directed BFS with the detail zones active)

**Interfaces:**
- Consumes: `MediaDetail` (AddonModels.h:161-178), `MediaArt::writeInto` aliases, ThemeBridge `actionRequested`, NavGraph API, ThemedChoice (if any picker appears — playlist pick stays the NavMenu overlay for now, which is already a contract level).
- Produces: `buildThemedNavGraph(g, itemCount, DetailState st)` where `struct DetailState { bool active = false; int actionCount = 0; int childCount = 0; }` — detail zones (`"detailActions"` Horizontal, `"detailChildren"` Vertical for the series/season in-page list IF childCount>0, `"detailBody"` scroll zone) registered active-count-gated exactly like the `actions` overlay pattern; declared edges detailActions↔detailBody↔detailChildren. Zone names verbatim — Tasks 3-5 and the probe use them.
- Behavior contract: opening info on a themed surface switches `currentView` to `detail` (existing mechanism), populates detail data, activates the detail zones; Back = one level pop → previous view (the existing `detailReturn` + a `pushLevel("detail", ...)` so the Back router owns it). Movies/games/books = flat detail; series/seasons keep GRID drilling for children (scout: drilling is levels today) — the in-page `detailChildren` zone renders the CURRENT container's children as a quick-open list, replacing the classic info-page-then-drill dance for expandables only where the classic flow showed an info page.
- Classic mode untouched (HomeView's own meta_ panel remains for classic home).

- [ ] Steps: probe shape-test first (RED via the new builder signature) → builder + QML + wiring → GREEN → live: movie info (backend permitting; else a local game and a book), game info, series drill, every action verb round-trips (play launches, favorite toggles + persists, download queues, playlist opens the NavMenu); Back pops to the exact prior view/selection; screenshots. Retirement grep: `themedReturnAfterDetail|themedDetailFrom_` → zero hits. Suite green. Commit: `themed: detail view on the contract — classic info page retired from themed flows`.

---

### Task 3: ReaderChrome for books (EbookView hosted mode)

**Files:**
- Create: `native/src/theme2/qml/elements/ReaderChrome.qml` (parameterized: `readerType: "book"|"pdf"|"comic"`; zones per type; built per the Task-1 composition decision)
- Create: `native/src/theme2/ReaderChromeHost.{h,cpp}` (the C++ host: owns the chrome QQuickWidget(s) per the spike architecture, the reader's NavGraph via `buildReaderNavGraph(g, ReaderKind kind)` — ADD to NavThemeGraph.h — and the bridge object `readerBridge` exposed as context property: page info, commands)
- Modify: `native/src/ebook/EbookView.{h,cpp}` (hosted mode: `void setHostedChrome(bool)` — suppresses `menu_`/`tocList_`/`streamIssueBtn_` and disables its own overlay reveal; expose for the bridge: `int currentPage() const`, `int pageCount() const`, `void nextPage()`, `void prevPage()`, `void fontDelta(int)`, `QStringList tocTitles() const`, `void gotoTocIndex(int)` — thin public wrappers over what the buttons already call, NO pagination logic changes; keys keep working via its own keyPressEvent for arrows — chrome zones activate only when chrome is VISIBLE)
- Modify: `native/src/ui/MainWindow.cpp` (themed mode: opening a book wraps EbookView in the host; `updateNavForPage` arbitration for the hosted page; chrome-visible toggling — reveal on Up/menu key, hide on idle/Back per the spec's Back rule: chrome visible → hide, hidden → level pop)
- Modify: `native/tools/probe_navqml.cpp` (reader graph shape test per kind — shared builder discipline; chrome zone reachability; Back semantics asserts: visible→hide is NOT a level pop)

**Interfaces:**
- Consumes: Task 1's composition decision; Task 2's builder-signature pattern. Produces: `ReaderChromeHost` + `buildReaderNavGraph(NavGraph*, ReaderKind)` with zone names `"readerNav"` (Horizontal: prev/next/progress), `"readerSettings"` (ThemedChoice rows: font size for books), `"readerToc"` (Vertical list; count = toc size) — Tasks 4-5 reuse the names and host.
- Resume/persist untouched: MainWindow's existing `book_->persist()` call sites unchanged.

- [ ] Steps: probe RED (new builder) → host + hosted mode + QML → GREEN → live: open a real EPUB themed (seed a Recent if needed, byte-identical ini restore), chrome reveals/hides, page nav via chrome + via raw arrows, font size via ThemedChoice round-trips, ToC jump works, Back rule per spec, resume still works after exit/reopen; classic mode unaffected (open the same book with themedHome disabled — full old chrome). Screenshots both modes. Suite green. Commit: `themed: book reader chrome on the contract (EbookView hosted mode)`.

---

### Task 4: ReaderChrome for PDFs and comics

**Files:**
- Modify: `native/src/pdf/PdfView.{h,cpp}` + `native/src/comic/ComicView.{h,cpp}` (hosted mode mirroring Task 3's EbookView contract: suppress bars; expose `currentPage/pageCount/nextPage/prevPage` + `zoomDelta(int)`, `fitWidth()`, and for comics `setTwoUp(bool)`/`twoUp()`; ComicView also gains the `setStreamIssueVisible(bool)` no-op stub for chrome uniformity — scout: it lacks one)
- Modify: `native/src/theme2/qml/elements/ReaderChrome.qml` (pdf/comic parameterization: zoom/fit controls via ThemedChoice/buttons in `readerSettings`; comics add the two-up toggle)
- Modify: `native/src/theme2/ReaderChromeHost.cpp` (kind wiring), `native/src/ui/MainWindow.cpp` (route themed PDF/comic opens through the host), `native/tools/probe_navqml.cpp` (pdf/comic shape params)

**Interfaces:** Consumes Task 3's host/builder/zones verbatim. CRITICAL input constraint (scout): the chrome must NOT eat scroll/drag over the page viewport — per the Task-1 architecture (strips = no interference by construction; full-layer = input-transparent center region required, prove it live).

- [ ] Steps: probe params RED→GREEN → hosted modes → live: a PDF (zoom/fit/page nav/scroll-in-viewport all working, chrome + raw input), a CBZ comic (incl. two-up toggle + spread rendering), Back rule, resume both, classic mode unaffected. Screenshots. Suite green. Commit: `themed: PDF + comic reader chrome on the contract`.

---

### Task 5: Themed audiobook now-playing page

**Files:**
- Create: `native/src/theme2/qml/elements/NowPlayingAudio.qml` (full themed page: cover art from `selected` aliases, title/author/chapter, progress, transport zone, queue list zone; NO compositing — this page replaces the player page for themed audio)
- Modify: `native/src/theme2/ThemeEngine.cpp/.h` (a `nowplaying` view or hosted page per the detail-view pattern — prefer a theme.json VIEW named `nowplayingAudio` following Task 2's mechanism; seed it into the default theme.json)
- Modify: `native/src/ui/nav/NavThemeGraph.h` (new shared builder `buildAudioPageNavGraph(NavGraph*)` — zones `"transport"` Horizontal wraps=false, `"queue"` Vertical, edges transport↔queue; same both-callers discipline)
- Modify: `native/src/ui/MainWindow.cpp` (themed audio opens route to the themed page; mpv keeps playing — the player PAGE isn't shown; transport verbs bridge to `session_`/`player_`: play/pause=`player_->togglePause()`, seek±=`seekRelative`, next/prev chapter=`nextChapter/prevChapter`, next/prev track=`session_->next()/prev()`, speed=`setSpeed` via ThemedChoice; position/duration flow from the existing onPosition/onDuration into the QML props)
- Modify: `native/tools/probe_navqml.cpp` (audio-page shape test)

**Interfaces:** Consumes PlaybackSession API (media/PlaybackSession.h — setQueue/playIndex/next/prev/signals) and MpvWidget controls (video/MpvWidget.h:21-45) EXACTLY as the classic transport does — no new session/player API. Queue list activation = `session_->playIndex(row)`. Classic audio page untouched for classic mode and for VIDEO (video keeps the mpv page — this task is audio-only).

- [ ] Steps: probe RED→GREEN → page + wiring → live via lavfi audio Recent seed (the proven technique; restore ini): themed page shows with transport, play/pause/seek/speed/chapter verbs all drive playback (verify via uitest state + perf spans open.audio), queue navigation works, Back pops home while audio continues or stops per today's behavior (VERIFY what today's behavior is first — leaving the audio page today keeps playing; preserve exactly), resume round-trip. Screenshots. Suite green. Commit: `themed: audiobook now-playing page on the contract`.

---

### Task 6: B1 close-out

- [ ] Retirement verification: in themed mode, grep + live-walk — no classic detail page, no classic reader chrome, no classic audio transport reachable (drive every B1 surface; screenshot each themed). Classic mode: all four still fully classic (drive spot-checks).
- [ ] Full suite; perfbaseline 3 runs (`nav.select` ≤2 ms drift; table); polishsweep re-run (no home regressions); watchdog log review across all B1 live testing (zero unexplained detections — any detection gets investigated before merge).
- [ ] Spec Status → `B1 complete: detail + 4 readers themed on the contract; classic retired from themed flows. B2 (settings + hardening) next.` Commit: `themed: close out B1 — content surfaces live on the contract`.
