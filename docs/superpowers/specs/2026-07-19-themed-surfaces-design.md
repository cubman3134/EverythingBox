# Phase 3, Subsystem B — Themed Content Surfaces Design

**Date:** 2026-07-19
**Status:** Approved design, pending implementation plan (B1)
**Builds on:** `2026-07-18-nav-contract-design.md` (subsystem A complete: NavGraph contract,
declared edges, two-state inputs, one Back router, watchdog)

## Goal (user requirement)

Retire classic menus from the themed experience. Everything the user touches from the
couch — detail pages, the readers (books, audiobooks, manga/comics, PDFs), and finally
settings — renders in the themed look, on the Nav Contract, with no classic-widget
styling visible.

## Decisions (user-set)

- **Readers: themed chrome, same engines.** EPUB/PDF/comic/audio rendering engines are
  untouched; ALL surrounding chrome (menus, progress, chapter lists, controls,
  reader settings) becomes themed QML on the contract.
- **Detail: one themed surface.** A single `Detail.qml` page for movies, series (with
  in-page seasons/episodes drill), games, and books. The classic HomeView detail
  rendering and the `themedReturnAfterDetail` fallback retire from themed flows.
- **Hosting: theme-element pages.** New surfaces are elements in the existing theme
  system (`elements/*.qml`, placed/styled via `theme.json`, loaded by the existing
  Loader machinery) — themes can restyle them like everything else. Rejected:
  standalone QML widgets (unthemeable second pattern); widget restyle (keeps the
  structure being retired).

## The surfaces

### `Detail.qml` + `buildDetailNavGraph()`

Hero/poster art from the MediaArt role schema; title/facts/overview; action row
(Play / Download / Favorite / Add-to-playlist) as a registered zone; child-content
zone for series→seasons→episodes drill inside the page; facts/overview scrollable and
reachable. Declared-edge graph shape via a shared builder (the `NavThemeGraph.h`
discipline: production and probe call the same function). Replaces every themed-flow
use of the classic detail header.

### `ReaderChrome.qml` + `buildReaderNavGraph(readerType)`

One transparent themed layer over the reader widgets, parameterized by reader type:
- Common: progress bar, page/chapter navigation zone, reader-settings zone
  (`ThemedChoice` rows: font size, page fit, etc. per reader), Back per the contract.
- Audiobooks additionally: transport zone (play/pause/seek/speed).
- Chrome auto-hides on idle (video-player idiom); Back with chrome visible hides
  chrome; Back with chrome hidden pops the level (returns where you came from).
- Reader engines gain a "hosted" mode: render-only, widget menus/toolbars suppressed,
  input arriving through the contract. Resume/persistence machinery untouched.

Composition: reader widget below, chrome QQuickWidget above with transparent
background (the inversion of the proven XMB-over-widget layering).

## Migration & retirement

- **Rule (inherited from A, enforced per surface):** a surface is themed only when it
  (1) registers via a shared graph builder, (2) passes its probe shape-test
  (validate() + directed move-BFS), (3) uses `ThemedTextField`/`ThemedChoice` for all
  input. QML self-registers leaf zones only; graph shapes are C++ builders.
- Per surface: build themed → route themed flows to it → retire the classic path FOR
  THEMED MODE. Classic widgets stay compiled for classic-home users until B2 completes;
  then `themedHome/enabled` defaults ON, classic paths get deprecation logging, and a
  later cleanup removes them once the user has lived on the themed default.
- Detail retirement: `infoPageRequested` / `themedReturnAfterDetail` deleted from
  themed flows. Reader retirement: hosted mode suppresses widget chrome. Settings (B2):
  a themed equivalent of the `showPanel` in-window panel system built from the
  two-state components, one settings screen per plan-task, including the Esc menu
  styling and profile picker.

## Phasing

- **Plan B1 (content):** `Detail.qml` (+ retirements) → `ReaderChrome.qml` for books →
  PDF → comics → audiobooks (one reader per task, each independently shippable).
- **Plan B2 (settings + hardening):** themed settings hub + dialogs; the A-seeded
  hardening list (NavGraph `activate()` hidden-zone guard, watchdog `tick()` probe
  coverage, `ThemedChoice` empty-options guard, QML no-direct-selection-writes CI
  grep, a ThemeView-level QML probe that CI-pins the XMB-buttons guard and grid-home
  rootBack); `git rm` of the accidentally committed `.superpowers/polish-audit/p3t3-*`
  evidence PNGs.

## Verification (every plan, inherited pipeline)

Per-task review + fix rounds; probe shape-tests for every new surface; live uitest
walks (open each reader type, drive every chrome zone, Back chain to home; detail
page for each media type incl. series drill); latency gate `nav.select` ≤2 ms vs the
16 ms baseline; polishsweep parity before merge; black-frame watchdog armed
throughout (new QML surfaces are where a stale-frame bug would reappear); final
whole-branch review before each merge.

## Exit criteria

- B1: all four readers + detail run themed-chrome-only in themed mode; classic detail
  unreachable from themed flows; probes green (new shape-tests in probe_navqml);
  watchdog telemetry clean over the B1 soaks.
- B2: no classic-styled surface reachable in themed mode at all; settings fully
  themed; hardening list closed; `themedHome/enabled` defaults ON.

## After this spec

Subsystem D: TV + mobile adaptivity (Android input model, form-factor layouts,
player UI) — its own spec.

## Composition decision (Task 1 outcome)

**Question settled:** how themed reader chrome composes over the RASTER readers, given the
app runs Qt Quick on the forced SOFTWARE backend (`main.cpp`:
`QQuickWindow::setGraphicsApi(Software)`) and `ThemeEngine` forces its `QQuickWidget`
opaque. Settled with a windowed spike (`native/tools/spike_readerchrome.cpp`, since
deleted) that drove every (reader × chrome) state and captured BOTH `QWidget::grab()`
(software composite) and `QScreen::grabWindow()` (real compositor). Evidence dumps:
`.superpowers/spike/` (`02-page-A-strips-*`, `04-scroll-A-strips-*`, `05-page-B-layer-*`,
`06-scroll-B-layer-*`, `findings.txt`).

**Decision — per reader class:**

- **EPUB/MOBI/PDF-text reader (`EbookView` / `BookPageWidget`, `WA_OpaquePaintEvent`
  paginated painter):** **Variant A — opaque `QQuickWidget` strip overlays.** Top and
  bottom themed bars are small OPAQUE child `QQuickWidget`s raised over the reader. This
  is exactly what `EbookView` already does with its raster `QFrame` menu (reserves
  `topMargin_`/`kMenuHeight` up top, auto-hide = `hide()/show()`); we swap the `QFrame`
  for a themed strip. Lowest risk — all-opaque, no new translucency dependency, matches
  ThemeEngine's opaque `clearColor` contract.
- **Comic reader (`ComicView`, `QScrollArea`):** **Variant A — opaque strip overlays.**
  Proven to render above the scroll viewport identically (`04-scroll-A-strips`).
- **Audiobook player (`MpvWidget`/GL):** **No compositing — framed/sibling chrome (C).**
  Unchanged by design; GL surface, not tested, no overlay.

**Variant B is VALIDATED and available as an enhancement (not merely the fallback).**
Contrary to the "ThemeEngine forces opaque, no precedent" expectation, a full-size
TRANSLUCENT child `QQuickWidget` (`clearColor(transparent)` + `WA_TranslucentBackground`)
DOES composite correctly over both raster readers in the software backend: the reader
shows through the transparent regions AND under semi-transparent QML (verified in the real
compositor, not just `grab()` — see `05-page-B-layer-screen.png`, page text visibly bleeds
through the chip). Use B when chrome must float translucently over live reader content
(scrims, rounded floating controls). **B is valid over raster readers ONLY — never over
`MpvWidget`/GL.** Variant C (reader shrinks into a content rect, chrome as siblings) is the
trivial always-works fallback; not needed for any raster reader.

**Constraints Tasks 3–4 must honor:**

1. **Input routing.** Overlay strips/layers MUST be `setFocusPolicy(Qt::NoFocus)`. With
   NoFocus the reader keeps keyboard focus and receives ALL keys while chrome is visible
   (spike: page got 5/5 keys with strips up). A strips keep mouse enabled (their buttons
   need clicks); a full B layer that should not eat clicks also sets
   `WA_TransparentForMouseEvents`. Never give an overlay StrongFocus — it would steal the
   arrow keys that page the reader.
2. **Flicker / z-order.** In the software backend a `QQuickWidget` child composites through
   the widget backing store in normal child z-order. `raise()` the strips ONCE after each
   (re)layout; a page-turn `update()`/`repaint()` storm on the reader does NOT paint over a
   raised strip (spike: 400 repaints, strip stayed on top, no per-frame raise needed). No
   `WA_AlwaysStackOnTop` needed for raster (that flag is only for native/GL children).
3. **Resize.** Strips are geometry-managed in the reader's `resizeEvent` (top bar
   full-width at y=0; bottom bar full-width pinned to bottom); `SizeRootObjectToView`
   drives the QML side; re-`raise()` after setting geometry.
4. **grab() is trustworthy here.** For both A and B, `QWidget::grab()` matched
   `QScreen::grabWindow()` exactly in the software backend — future reader-chrome tests may
   rely on `grab()` (it only diverges for GL/native surfaces).
5. **Idle cost.** An embedded, non-animating `QQuickWidget` strip showed zero measurable
   idle cost (60 idle event-loop spins → 0 ms).
