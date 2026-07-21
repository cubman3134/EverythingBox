#pragma once
// NavThemeGraph — the ONE definition of the themed surface's NavGraph shape (zones + declared edges).
//
// The themed home/browse surface is a two-cursor XMB (an item column co-located with a category axis), plus
// a spatially-real bottom button bar and a transient inline action-chooser overlay. That exact zone layout
// and its declared edges must be identical in two places: ThemeEngine::buildView (the shipped graph) and
// probe_navqml section 9 (the CI assertion that the shipped graph passes its own validator and reaches every
// zone). Previously each spelled the registerZone/addEdge calls out by hand with a "keep in sync" comment —
// a latent drift hazard (the probe could keep asserting a stale shape the real graph no longer has). This
// header makes them call ONE function, so drift is structurally impossible: the probe tests the literal
// registration the app runs.
//
// Counts are intentionally NOT baked in beyond the caller-supplied item count: buildView registers
// categories/buttons/actions at 0 and lets the QML feed live counts (setZoneCount); the probe supplies its
// own fixed test counts afterwards (setZoneCount). What is shared — and what must never drift — is the zone
// STRUCTURE (ids, row/col, axis, wraps), the default zone, and the declared edge set.
#include "NavGraph.h"

// The themed DETAIL view's live shape, fed to the shared builder so the ONE definition also owns the detail
// surface's zones/edges. `active` gates whether the detail zones carry live counts (they are ALWAYS
// registered — like the inline `actions` overlay — so the QML can count them up/down via setZoneCount when
// the detail view opens/closes; a hidden zone's edges are inert and it is never a move target). `actionCount`
// is the number of buttons in the action row (Play/Download/Favorite/Add-to-playlist, per-item filtered);
// `childCount` is the current container's children (a series/season in-page quick-open list) — 0 for a flat
// movie/game/book, so the `detailChildren` zone stays inert there.
struct DetailState { bool active = false; int actionCount = 0; int childCount = 0; };

// Register the themed surface's zones + declared edges on a fresh NavGraph. `itemCount` is the item column's
// starting count (buildView: items.size(); the probe: a fixed test count). categories/buttons/actions start
// hidden (count 0) — their live counts arrive later via setZoneCount, keeping their edges inert until then.
// `detail` seeds the detail-view zones' counts (the app builds with a default {inactive} and the QML feeds
// live counts when the detail view opens; the probe supplies fixed counts to shape-test the detail surface).
inline void buildThemedNavGraph(NavGraph& g, int itemCount, DetailState detail = {})
{
    // Zone layout: `items` (the XMB column / the grid, Vertical) and `categories` (the XMB horizontal axis)
    // are CO-LOCATED at (0,0) — the two always-visible cursors of ONE surface, which pure spatial crossing
    // cannot express, so their transitions are DECLARED edges with the fused co-located step. `actions` (the
    // inline chooser overlay) is co-located too; entered by activation, its declared Esc edge documents the
    // dismissal so validate() sees it connected. `buttons` (the bottom button bar) is spatially real at row 1.
    g.registerZone(QStringLiteral("items"), itemCount, 0, 0, Qt::Vertical);
    g.registerZone(QStringLiteral("categories"), 0, 0, 0, Qt::Horizontal);
    g.registerZone(QStringLiteral("buttons"), 0, 1, 0, Qt::Horizontal);
    g.registerZone(QStringLiteral("actions"), 0, 0, 0, Qt::Vertical, /*wraps=*/true);
    g.setDefaultZone(QStringLiteral("items"));
    // Two-cursor XMB surface: Left/Right switch to + step the category axis; Up/Down from the category axis
    // switch to + step the item column (fused step = old stepCat/step parity, no eaten press).
    g.addEdge(QStringLiteral("items"), Qt::Key_Left,  QStringLiteral("categories"));
    g.addEdge(QStringLiteral("items"), Qt::Key_Right, QStringLiteral("categories"));
    g.addEdge(QStringLiteral("categories"), Qt::Key_Down, QStringLiteral("items"));
    g.addEdge(QStringLiteral("categories"), Qt::Key_Up,   QStringLiteral("items"));
    // The bottom button bar: entered from the grid's bottom row (the QML gates WHEN — it owns the gridCols
    // geometry AND keeps `buttons` hidden in XMB mode so this edge stays inert there), left back upward with
    // the grid cursor restored from zone memory.
    g.addEdge(QStringLiteral("items"), Qt::Key_Down, QStringLiteral("buttons"));
    g.addEdge(QStringLiteral("buttons"), Qt::Key_Up, QStringLiteral("items"));
    // The chooser's dismissal transition (Esc -> back onto the leaf), executed by syncActionsZone; declared
    // so the connectivity walk sees the overlay zone linked to the surface it covers.
    g.addEdge(QStringLiteral("actions"), Qt::Key_Escape, QStringLiteral("items"));

    // The DETAIL view's zones: an action row (Horizontal, wraps) over a scrollable body, plus an optional
    // in-page children list (a series/season quick-open). ALWAYS registered so the QML can count them up/down
    // (setZoneCount) when the detail view opens/closes — count-gated exactly like `actions`: 0 when the detail
    // view is closed makes their edges inert and keeps them off the home surface's arrow paths (a hidden zone
    // is never a move target).
    //
    // CONTAINMENT: the detail view is MODAL — while it is open, no arrow may escape onto the covered home
    // zones (items/categories/buttons), even on a theme whose button bar is live. The zones sit in their own
    // grid column (col 8) below the home rows, and every arrow that the vertical-stack edges don't route is
    // pinned by a declared SELF edge (the containment no-op — see NavGraph::addEdge): Up on the action row
    // (top of the page), and the cross-axis Left/Right on the body/children. The remaining vectors are
    // contained structurally: the action row's Left/Right wrap in-strip, Down past the children list finds
    // no zone below row 12, and Down from the body with the children hidden is blocked by the hidden zone
    // being the nearest target. Connectivity for validate(): the detailActions--Esc-->items edge (the Back
    // dismissal, executed by the host's "detail" level pop) links the detail stack to the home surface in
    // the undirected union, exactly like the `actions` overlay's Esc edge — no reliance on geometry.
    const int aCount = detail.active ? detail.actionCount : 0;
    const int bCount = detail.active ? 1 : 0;                        // the scroll body is a single focus target
    const int cCount = detail.active ? detail.childCount : 0;
    g.registerZone(QStringLiteral("detailActions"), aCount, 10, 8, Qt::Horizontal, /*wraps=*/true);
    g.registerZone(QStringLiteral("detailBody"), bCount, 11, 8, Qt::Vertical);
    g.registerZone(QStringLiteral("detailChildren"), cCount, 12, 8, Qt::Vertical);
    g.addEdge(QStringLiteral("detailActions"), Qt::Key_Down, QStringLiteral("detailBody"));
    g.addEdge(QStringLiteral("detailBody"), Qt::Key_Up, QStringLiteral("detailActions"));
    g.addEdge(QStringLiteral("detailBody"), Qt::Key_Down, QStringLiteral("detailChildren"));
    g.addEdge(QStringLiteral("detailChildren"), Qt::Key_Up, QStringLiteral("detailBody"));
    // Containment pins (self edges = consume, no geometric escape).
    g.addEdge(QStringLiteral("detailActions"), Qt::Key_Up, QStringLiteral("detailActions"));
    g.addEdge(QStringLiteral("detailBody"), Qt::Key_Left,  QStringLiteral("detailBody"));
    g.addEdge(QStringLiteral("detailBody"), Qt::Key_Right, QStringLiteral("detailBody"));
    g.addEdge(QStringLiteral("detailChildren"), Qt::Key_Left,  QStringLiteral("detailChildren"));
    g.addEdge(QStringLiteral("detailChildren"), Qt::Key_Right, QStringLiteral("detailChildren"));
    // The Back dismissal leg (host-executed via the "detail" level pop) — declared so the connectivity walk
    // sees the modal stack linked to the surface it covers, mirroring the `actions` overlay's Esc edge.
    // NB this edge is never walked by move(): Esc on the detail view is caught by the host's Back router,
    // which pops the "detail" level (onPop restores the home surface) — that pop is the real dismissal leg;
    // the edge exists only so validate()'s undirected walk sees the modal stack connected to `items`.
    g.addEdge(QStringLiteral("detailActions"), Qt::Key_Escape, QStringLiteral("items"));
}

// ---- Audiobook now-playing surface (Plan B1, Task 5): the themed page that REPLACES the player page for audio.
//
// Audio has no video, so there is nothing to composite — the themed page IS the surface and mpv keeps playing
// invisibly behind it (the player page is simply never shown). Following the DETAIL view's mechanism exactly,
// the page is a theme.json VIEW named `nowplayingAudio`; its two nav zones are registered up-front on the SAME
// graph as the themed home (like the detail zones) and count-gated — held at 0 until the page opens (currentView
// flips to "nowplayingAudio"), so their edges stay inert and they are never a move target off the home surface.
//
// Zones:
//   * transport (row 20, col 0, Horizontal, wraps) — the transport strip: prev-track / prev-chapter /
//     seek-back / play-pause / seek-fwd / next-chapter / next-track / speed. The default/entered zone.
//   * queue (row 21, col 0, Vertical) — the session queue titles; activating a row is session_->playIndex(row).
//
// Declared edges: transport <-> queue (Down enters the queue, Up returns to the transport strip). Containment
// (this page is MODAL over the home surface, whose items/categories/buttons stay LIVE underneath): every arrow
// the stack edges don't route is pinned by a declared SELF edge (the no-op — see NavGraph::addEdge), mirroring
// the detail view's containment. The strip WRAPS Left/Right in-strip (detailActions' solution): a boundary
// along-axis arrow wraps instead of falling through to geometric crossing, so the horizontal containment is
// SELF-CONTAINED — no reliance on what sits (hidden or not) in neighbouring grid columns. (SELF edges on the
// strip's Left/Right would NOT work: a declared edge is consulted before axis stepping, so they would freeze
// the strip's own stepping.) Vertical escapes (Up off the strip, Down off the queue) and the queue's cross-axis
// Left/Right are pinned by SELF edges. The transport→items Esc edge is the dismissal leg (host-executed via the
// "nowplaying" level pop) — declared only so validate()'s undirected walk sees the modal stack linked to the
// home surface it covers, exactly like the detail/actions Esc edges.
inline void buildAudioPageNavGraph(NavGraph& g)
{
    g.registerZone(QStringLiteral("transport"), 0, 20, 0, Qt::Horizontal, /*wraps=*/true);
    g.registerZone(QStringLiteral("queue"), 0, 21, 0, Qt::Vertical);
    g.addEdge(QStringLiteral("transport"), Qt::Key_Down, QStringLiteral("queue"));
    g.addEdge(QStringLiteral("queue"), Qt::Key_Up, QStringLiteral("transport"));
    // Containment pins (SELF edges = consume, no geometric escape onto the live home zones underneath).
    g.addEdge(QStringLiteral("transport"), Qt::Key_Up, QStringLiteral("transport"));   // nothing above the strip
    g.addEdge(QStringLiteral("queue"), Qt::Key_Left,  QStringLiteral("queue"));        // cross-axis on a V list
    g.addEdge(QStringLiteral("queue"), Qt::Key_Right, QStringLiteral("queue"));
    // NB the queue's Down (its own Vertical along-axis) is deliberately NOT declared: a declared edge is
    // consulted before axis stepping, so pinning Down would freeze the list. Down steps within the list and, at
    // the last row (wraps=false), the along-axis step finds no next row and geometry finds no zone below row 21
    // — a contained no-op either way. (Same discipline as the reader's readerToc list.)
    // The dismissal leg (host-executed via the "nowplaying" level pop) — declared for the connectivity walk.
    g.addEdge(QStringLiteral("transport"), Qt::Key_Escape, QStringLiteral("items"));
}

// ---- Reader surfaces (Plan B1, Tasks 3-5): the themed chrome over the hosted RASTER readers -----------------
//
// A reader (EbookView / PdfView / ComicView in "hosted" mode) owns its OWN NavGraph — there is no home
// surface co-resident in the same graph, so the whole reader graph IS the modal surface. Its zones sit in one
// grid column and the Back router (ReaderChromeHost) owns the reader LEVEL: with chrome hidden, Back pops that
// level (return to where the reader was opened); with chrome visible, Back just hides the chrome (no pop). See
// the composition decision (docs/superpowers/specs/2026-07-19-themed-surfaces-design.md, VARIANT A) — the
// chrome is opaque strip QQuickWidgets raised over the reader; this graph is the selection model behind them.
enum class ReaderKind { Book, Pdf, Comic };

// Register the reader surface's zones + declared edges on a fresh NavGraph. Shared verbatim between the app
// (ReaderChromeHost) and probe_navqml's reader shape-test — the NavThemeGraph.h discipline, so the CI
// assertion can never drift from the shipped graph.
//
// Zones (Book — the only kind this task builds; Pdf/Comic reuse the NAMES and extend the settings/nav rows):
//   * readerNav (row 2, Horizontal, wraps) — the bottom strip: prev / progress / next. ALWAYS visible
//     (count 3); the default zone the chrome reveals onto.
//   * readerSettings (row 1, Vertical) — a column of `ThemedChoice` rows (Book: just font size). Count-gated
//     (0 until the chrome feeds live counts), exactly like `categories`/`actions` on the themed home.
//   * readerToc (row 0, Vertical) — the chapter list, count = toc size. Count-gated (fed from tocTitles()).
//
// Declared edges: readerNav <-> readerSettings <-> readerToc, chosen so none blocks a zone's ALONG-axis
// internal stepping (a declared edge is consulted before axis stepping, so declaring Up/Down on a Vertical
// list zone would freeze its scrolling). The cross-axis / reverse legs are declared; the Vertical zones' own
// down-into-the-next-zone leg is left to geometry (they are stacked in col 0). Containment SELF edges pin the
// arrows that would otherwise run off the surface into nothing (mirrors the detail view's SELF-edge pins).
inline void buildReaderNavGraph(NavGraph& g, ReaderKind kind)
{
    g.registerZone(QStringLiteral("readerNav"), 3, 2, 0, Qt::Horizontal, /*wraps=*/true); // prev/progress/next
    g.registerZone(QStringLiteral("readerSettings"), 0, 1, 0, Qt::Vertical);              // font-size rows (gated)
    g.registerZone(QStringLiteral("readerToc"), 0, 0, 0, Qt::Vertical);                   // chapter list (gated)
    g.setDefaultZone(QStringLiteral("readerNav"));

    // The chrome chain (declared where geometry can't be trusted / to keep the shape explicit, like the home's
    // items<->categories edges). readerNav's Up/Down are cross-axis (it is Horizontal) so declaring them is
    // safe; readerSettings' Up→readerToc is cross... no — readerSettings is Vertical, so ONLY its cross-axis
    // (Left/Right) may be declared. readerNav↔readerSettings uses readerNav's cross-axis Up plus readerSettings'
    // cross-axis... its Down IS along-axis. So: declare readerNav→readerSettings (Up, cross-axis on readerNav);
    // the reverse + the readerSettings↔readerToc legs are geometric (col-0 stack) and thus never freeze a list.
    g.addEdge(QStringLiteral("readerNav"), Qt::Key_Up, QStringLiteral("readerSettings"));

    // Containment (SELF edges = consume, no geometric escape). readerNav wraps Left/Right in-strip and pins its
    // Down (nothing below the bottom bar). readerSettings/readerToc are Vertical lists: pin their cross-axis
    // Left/Right so a stray horizontal arrow can't fall through; their along-axis Up/Down keep stepping the
    // list, crossing to the neighbour zone by geometry only at the list's edge.
    g.addEdge(QStringLiteral("readerNav"), Qt::Key_Down, QStringLiteral("readerNav"));
    g.addEdge(QStringLiteral("readerSettings"), Qt::Key_Left,  QStringLiteral("readerSettings"));
    g.addEdge(QStringLiteral("readerSettings"), Qt::Key_Right, QStringLiteral("readerSettings"));
    g.addEdge(QStringLiteral("readerToc"), Qt::Key_Left,  QStringLiteral("readerToc"));
    g.addEdge(QStringLiteral("readerToc"), Qt::Key_Right, QStringLiteral("readerToc"));

    // All three kinds share this exact zone STRUCTURE + edge set; only the live counts differ and are fed
    // externally by the host/probe via setZoneCount (Book: readerSettings=1 font row, readerToc=chapters;
    // Pdf: readerSettings=3 zoom/fit rows, readerToc=0; Comic: readerSettings=4 (+two-up), readerToc=0). So
    // kind is not consulted here — keeping the shape identical is exactly what lets ONE builder back all three.
    (void)kind;
}

// ---- Themed settings PANEL surface (Plan B2, Task 1): the showPanel analogue on the Nav Contract -----------
//
// A themed panel (ThemedPanelHost) is its OWN NavGraph — a standalone stack page like the reader, with NO home
// surface co-resident in the same graph, so the whole panel graph IS the surface. The host owns the panel
// LEVEL(s): present() pushes "panel:<title>"; a nested present() stacks another; Back pops one (the host's
// onPop re-renders the parent panel, or leaves the host when the last level goes). Shared verbatim between the
// app (ThemedPanelHost) and probe_navqml's §18 shape-test — the NavThemeGraph.h discipline, so the CI
// assertion can never drift from the shipped graph.
//
// Zones (a two-zone surface, mirroring classic showPanel's header-Back + row list):
//   * panelRows (row 1, col 0, Vertical) — the row list; count = rowCount. The default zone the panel opens on.
//   * panelBack (row 0, col 0, Horizontal, count 1) — the header Back affordance, always present. Enter on it
//     is the host's Back (pop the level); Up-crossing from the first row lands here, like the classic panel.
//
// Declared edges: panelBack --Down--> panelRows (panelBack is Horizontal, so Down is its CROSS axis — safe to
// declare; it enters panelRows at that zone's REMEMBERED index). The reverse (Up off the first row) is left to
// GEOMETRY: a declared Up on the Vertical panelRows would freeze its along-axis stepping (the reader lesson),
// so panelRows' Up steps the list and, at the top row, crosses up to panelBack by grid geometry (both sit in
// col 0, panelBack a row above). Containment SELF pins (the standalone surface has nothing to escape onto, but
// the pins keep a stray arrow a visible no-op instead of a silent geometric hop): panelBack Up/Left/Right (a
// 1-count strip — nothing above or beside it) and panelRows' cross-axis Left/Right. panelRows' Down is left to
// geometry (steps the list; past the last row nothing sits below row 1 — a contained no-op).
inline void buildPanelNavGraph(NavGraph& g, int rowCount)
{
    g.registerZone(QStringLiteral("panelRows"), rowCount, 1, 0, Qt::Vertical);
    g.registerZone(QStringLiteral("panelBack"), 1, 0, 0, Qt::Horizontal);
    g.setDefaultZone(QStringLiteral("panelRows"));
    // The back-zone edge: Down off the header enters the row list at its remembered index (cross-axis, safe).
    g.addEdge(QStringLiteral("panelBack"), Qt::Key_Down, QStringLiteral("panelRows"));
    // Containment pins (SELF = consume, no geometric escape off the standalone panel surface).
    g.addEdge(QStringLiteral("panelBack"), Qt::Key_Up,    QStringLiteral("panelBack"));
    g.addEdge(QStringLiteral("panelBack"), Qt::Key_Left,  QStringLiteral("panelBack"));
    g.addEdge(QStringLiteral("panelBack"), Qt::Key_Right, QStringLiteral("panelBack"));
    g.addEdge(QStringLiteral("panelRows"), Qt::Key_Left,  QStringLiteral("panelRows"));
    g.addEdge(QStringLiteral("panelRows"), Qt::Key_Right, QStringLiteral("panelRows"));
}
