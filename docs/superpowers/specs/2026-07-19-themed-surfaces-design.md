# Phase 3, Subsystem B — Themed Content Surfaces Design

**Date:** 2026-07-19
**Status:** Subsystem B complete: B1 + B2 — themed everywhere, classic paths deprecation-logged, themed default ON. Subsystem D (TV+mobile) next.
  _(Task 7 no-classic capstone walk: 40+ themed surfaces driven live, zero classic surfaces reachable except the one recorded below — Appearance, which is deprecation-logged. Full probe suite green, perf flat vs the B2T6 perf-track baseline, watchdog clean.)_
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
- A uniform themed row-list surface (ThemedPanelHost) models its rows as ONE Vertical
  panelRows zone with per-row indices — not per-row self-registering
  ThemedChoice/ThemedTextField zones, which suit Repeater/Loader surfaces of
  heterogeneous fields. Row interaction (Toggle flip / Choice cycle / TextField OSK
  edit) is dispatched host-side in onGraphActivated, honoring the externalEdit
  contract — select → activate → edit-via-Osk::getText → commit-exactly-once, cancel
  commits nothing — at the row level. Delegates are pure render.

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

### B1 close-out follow-ups (recorded at Task 6; triaged fine-as-follow-up)

Deferred review minors and observations from the B1 live walks. None block B1; carry into B2
or a dedicated polish pass.

- **Reader exit returns to the classic `HomeView` in themed mode.** `returnFromReader`
  (`MainWindow.cpp`) unconditionally does `stack_->setCurrentWidget(home_)` to preserve the
  content-list position you came from — so backing out of any themed reader lands on the classic
  `home_` widget, not the themed XMB. Pre-existing across all of B1 (unchanged since Task 3); the
  home-surface retirement is B2 scope (`themedHome/enabled` defaults ON + broader classic removal).
  Decide the themed reader-return surface there.
- **Comic spread page-label range.** In two-up mode the bottom strip shows a single page number
  ("1 / 33") for a two-page spread rather than a "1–2 / 33" range.
- **PDF/comic 38px top-strip overlay while chrome visible.** The opaque top strip reserves/overlays
  ~38px of the reader viewport while chrome is up (book reserves via `topChromeReserve`; pdf/comic
  overlay). Cosmetic; revisit the reserve-vs-overlay choice for the fixed-page readers.
- **Font-cycle UX (reader settings zone).** On the settings zone, Left/Right are repurposed for
  bidirectional cycle (font for book, zoom for pdf/comic) by reusing the zone's SELF-pinned cross-axis
  no-ops; reaching the other settings buttons (Fit, Two-Up) is non-obvious. Reconsider the settings-zone
  interaction model.
- **`Image.qml` font fallback.** Confirm the reader-chrome QML's font fallback chain renders on a
  machine without the theme's preferred font.
- **Drag-passthrough live demo.** The Variant-B translucent-layer drag/scroll passthrough is proven in
  the spike but never demoed in a shipped reader; capture a live demo if B enhancement lands.
- **`Xmb.qml`/`Video.qml` "still" binding-loop warning.** Themed XMB logs `QML Video: Binding loop
  detected for property "still"` on the metadata panel; pre-existing (not touched by B1), noisy in logs.

### B2 follow-ups (recorded during the settings-panel conversions; triaged fine-as-follow-up)

Gaps surfaced converting the classic `showPanel` builders onto the `PanelRow` model (`PanelModel.h`) /
`ThemedPanelHost`. None block B2; each is a widening of the row-model vocabulary, carried into a later
panel-polish pass.

- **Missing `Slider` `PanelRow` kind.** `PanelModel.h`'s `Kind` enum has no continuous-value row, so the
  BGM volume control shipped as a `Choice` cycling 10% steps (0/10/…/100%) instead of a real slider. A
  `Slider` kind (min/max/step + Left/Right to adjust in place, mirroring the reader settings zone's
  bidirectional cross-axis cycle) would restore fine-grained control and generalize to brightness/volume
  rows. Until then, `Choice`-as-coarse-slider is the stand-in.
- **Dropped Trakt redirect-URI setup hint.** The classic Trakt/Cloud-Sync panel showed a multi-line setup
  hint (the OAuth redirect URI to paste into the Trakt app registration). The single-line `PanelRow` model
  (one `label` + one right-hand `value`) can't carry that guidance, so it was dropped from the themed panel.
  Needs either a multi-line `Info`/help row kind or a dedicated help affordance before the Trakt setup flow
  is fully reachable in themed mode.

### Task 7 no-classic-walk follow-ups (surfaced by the B2 capstone walk; recorded, not fixed)

The capstone walk drove 40+ themed surfaces live. Every settings panel, both reader-exit origin restores,
the Esc/pause menu, the startup + in-app profile pickers, game launch/exit, and the video player all render
themed. The one classic surface still reachable in themed mode, plus minor observations:

- **Appearance panel is still classic (the one gap).** `openAppearance()` (`MainWindow.cpp`) builds its
  panel unconditionally via classic `showPanel` — a `QWidget` with the themed-home checkbox, a `QListWidget`
  theme list, and a **live themed-home preview** embedded on the right. Reached from the themed Settings hub's
  "Appearance" row (and Ctrl+Shift+A), it shows `page:QWidget pageName:settingsPanel` and logs
  `deprecated-classic: panel:Appearance` — the ONLY `deprecated-classic` line the whole themed walk produced.
  Every other settings panel is on `ThemedPanelHost`. Converting it is a real job (re-home the theme-list +
  the live QML preview on the Nav Contract / `PanelRow` model), NOT a one-line routing fix, so it is recorded
  here rather than fixed in the verification task. This is the last panel blocking B2's literal
  "settings fully themed" goal; give it its own conversion task.
- **`split-pane-pick` classic drop (documented inventory item, not driven).** `enterSplitScreen`'s
  `openHereRequested` lambda still drops to the classic `HomeView` and logs `deprecated-classic:
  split-pane-pick` if split-screen is driven. Not exercised in the walk (no split-screen driven); carry the
  themed split-pane picker as a follow-up.
- **Reader-exit key is inconsistent across readers.** Book and comic readers exit on Back (Backspace); the PDF
  reader consumes Back (page-nav) and only exits on Escape. All three restore the themed origin surface, so
  it's not a classic leak — but the exit affordance should be unified.
- **Profile-icon mojibake.** The stored profile icon (`[profiles] list` in the ini) is a double-encoded emoji
  (`Ã°Å¸ÂÂ¶` for 🐶); the themed home/switcher render the mojibake as-is. A stored-data/encoding issue
  (the classic HomeView decodes it correctly), not a themed-surface bug — worth a one-time migration.
- **Live theme-change from the (classic) Appearance panel wedges the panel host.** Selecting a different theme
  in the classic Appearance theme list live-applies it, but Back/Escape then can't return the `ThemedPanelHost`
  hub to the home surface (`panelDepth` sticks at 0) until a relaunch. Likely a consequence of rebuilding the
  themed home under an open panel; folds into the Appearance-conversion task.
- **`Xmb.qml` `still` binding-loop warning** (already noted under B1) still logs on every hero-metadata bind
  in themed XMB, plus a missing `themes2/XMB/poster` image warning — noisy but cosmetic.

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
