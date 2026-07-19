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
