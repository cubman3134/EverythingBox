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

// How a theme TRAVERSES the `categories` zone. The zone itself is registered unconditionally either way (its
// live count is fed by the QML) — what differs is its AXIS and the declared edges that reach it:
//
//   Cross   — the XMB's horizontal category axis, CO-LOCATED with the item column: Left/Right from the column
//             switch to + step it (the fused step), Up/Down from it switch to + step the column. This is the
//             shape the engine has always had, and the default: every theme that does not declare a `sidebar`
//             element gets exactly these registrations, byte for byte.
//   Sidebar — a VERTICAL list of categories beside a grid (the Playnite shape). Up/Down step the LIST itself,
//             so those must NOT be declared as edges: a declared edge is consulted before axis stepping and
//             would freeze the list (the reader/queue lesson above). Left from the grid enters the sidebar and
//             Right leaves it; the QML gates WHEN Left crosses (only at the grid's leftmost column — exactly
//             how it gates the items->buttons bottom-row edge), so 2-D grid stepping is untouched. Both legs
//             are CROSS-axis for their source zone, so neither freezes anything, and because Left/Right are
//             cross-axis for the Vertical TARGET too there is no fused step — each crossing enters at the
//             target's REMEMBERED index, which is what makes the grid cursor survive a sidebar round trip.
enum class CategoriesNav { Cross, Sidebar };

// Register the themed surface's zones + declared edges on a fresh NavGraph. `itemCount` is the item column's
// starting count (buildView: items.size(); the probe: a fixed test count). categories/buttons/actions start
// hidden (count 0) — their live counts arrive later via setZoneCount, keeping their edges inert until then.
// `detail` seeds the detail-view zones' counts (the app builds with a default {inactive} and the QML feeds
// live counts when the detail view opens; the probe supplies fixed counts to shape-test the detail surface).
// `cats` picks the categories-zone shape (see CategoriesNav); the default is the historical XMB cross.
inline void buildThemedNavGraph(NavGraph& g, int itemCount, DetailState detail = {},
                                CategoriesNav cats = CategoriesNav::Cross)
{
    // Zone layout: `items` (the XMB column / the grid, Vertical) and `categories` (the XMB horizontal axis)
    // are CO-LOCATED at (0,0) — the two always-visible cursors of ONE surface, which pure spatial crossing
    // cannot express, so their transitions are DECLARED edges with the fused co-located step. `actions` (the
    // inline chooser overlay) is co-located too; entered by activation, its declared Esc edge documents the
    // dismissal so validate() sees it connected. `buttons` (the bottom button bar) is spatially real at row 1.
    g.registerZone(QStringLiteral("items"), itemCount, 0, 0, Qt::Vertical);
    // Sidebar mode WRAPS the list. Not decoration — it is the containment: the sidebar's Up/Down must stay
    // along-axis (declaring them would freeze the list), so a boundary Down would otherwise fall through to
    // GEOMETRY and land in the `buttons` bar a row below — a one-way trapdoor, since buttons--Up-->items goes
    // to the grid, not back to the sidebar. Wrapping keeps a boundary along-axis arrow inside the strip, which
    // is exactly how detailActions and the audio transport contain themselves (see their notes below).
    g.registerZone(QStringLiteral("categories"), 0, 0, 0,
                   cats == CategoriesNav::Sidebar ? Qt::Vertical : Qt::Horizontal,
                   /*wraps=*/cats == CategoriesNav::Sidebar);
    g.registerZone(QStringLiteral("buttons"), 0, 1, 0, Qt::Horizontal);
    g.registerZone(QStringLiteral("actions"), 0, 0, 0, Qt::Vertical, /*wraps=*/true);
    g.setDefaultZone(QStringLiteral("items"));
    if (cats == CategoriesNav::Sidebar)
    {
        // Sidebar surface: a vertical category list beside the grid. ONLY the two cross-axis legs are declared
        // (see CategoriesNav) — the sidebar's own Up/Down stay along-axis so the list scrolls, and the grid's
        // Left/Right stay axis-resolved so its 2-D stepping is untouched (the QML only asks for the Left
        // crossing at the grid's leftmost column). `categories`--Left--> itself is the containment pin:
        // nothing sits left of the sidebar, so a stray Left is a visible no-op, not a geometric hop.
        g.addEdge(QStringLiteral("items"), Qt::Key_Left, QStringLiteral("categories"));
        g.addEdge(QStringLiteral("categories"), Qt::Key_Right, QStringLiteral("items"));
        g.addEdge(QStringLiteral("categories"), Qt::Key_Left, QStringLiteral("categories"));
    }
    else
    {
        // Two-cursor XMB surface: Left/Right switch to + step the category axis; Up/Down from the category axis
        // switch to + step the item column (fused step = old stepCat/step parity, no eaten press).
        g.addEdge(QStringLiteral("items"), Qt::Key_Left,  QStringLiteral("categories"));
        g.addEdge(QStringLiteral("items"), Qt::Key_Right, QStringLiteral("categories"));
        g.addEdge(QStringLiteral("categories"), Qt::Key_Down, QStringLiteral("items"));
        g.addEdge(QStringLiteral("categories"), Qt::Key_Up,   QStringLiteral("items"));
    }
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
//   * chrome (row 19, col 0) — the page's Back affordance, above the strip. One entry; Up from the strip
//     reaches it, Down returns.
//   * transport (row 20, col 0, Horizontal, wraps) — the transport strip: prev-track / prev-chapter /
//     seek-back / play-pause / seek-fwd / next-chapter / next-track / speed. The default/entered zone.
//   * queue (row 21, col 0, Vertical) — the session queue titles; activating a row is session_->playIndex(row).
//   * lyrics (row 21, col 1, Vertical) — the karaoke lines, when the track has SYNCED ones (issue #142);
//     activating a row seeks to that line's timestamp. Counted to 0 for an unsynced sheet, which has no
//     timestamps to seek to, so the zone simply does not exist on that track.
//
// Declared edges: transport <-> queue (Down enters the queue, Up returns to the transport strip) and
// queue <-> lyrics (Right crosses into the lines, Left comes back). Containment
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
    // Row 19: above the transport strip, which is where it is drawn.
    g.registerZone(QStringLiteral("chrome"), 0, 19, 0, Qt::Horizontal);
    g.registerZone(QStringLiteral("transport"), 0, 20, 0, Qt::Horizontal, /*wraps=*/true);
    g.registerZone(QStringLiteral("queue"), 0, 21, 0, Qt::Vertical);
    // Row 21, COLUMN 1 — beside the queue, which is where the panel is drawn (issue #142). The lyric lines are
    // a nav zone because selecting one SEEKS there, and a seek you can only reach with a mouse is not a feature
    // on this app's primary surface. Count-gated like the rest of the page, and gated twice over: the host
    // counts it from audioLyricCount, which is 0 unless the lyrics are SYNCED — an unsynced sheet has no
    // timestamps, so there is nothing to seek to and the zone is never enterable.
    g.registerZone(QStringLiteral("lyrics"), 0, 21, 1, Qt::Vertical);
    g.addEdge(QStringLiteral("transport"), Qt::Key_Down, QStringLiteral("queue"));
    g.addEdge(QStringLiteral("queue"), Qt::Key_Up, QStringLiteral("transport"));
    // Containment pins (SELF edges = consume, no geometric escape onto the live home zones underneath).
    // Up leaves the strip for the page's chrome — the Back affordance top-left. This was a SELF edge pinning
    // Up, correct while the strip was the topmost thing on the page; adding a button above it without
    // revisiting this left that button drawable, clickable, and unreachable by anything but a mouse.
    g.addEdge(QStringLiteral("transport"), Qt::Key_Up, QStringLiteral("chrome"));
    g.addEdge(QStringLiteral("chrome"), Qt::Key_Down, QStringLiteral("transport"));
    // Containment for the new zone, same discipline as the rest of this modal page: every arrow that is not a
    // declared crossing is consumed here rather than escaping onto the live home zones underneath.
    g.addEdge(QStringLiteral("chrome"), Qt::Key_Up,    QStringLiteral("chrome"));
    g.addEdge(QStringLiteral("chrome"), Qt::Key_Left,  QStringLiteral("chrome"));
    g.addEdge(QStringLiteral("chrome"), Qt::Key_Right, QStringLiteral("chrome"));
    g.addEdge(QStringLiteral("queue"), Qt::Key_Left,  QStringLiteral("queue"));        // cross-axis on a V list
    // Right crosses INTO the lyric list, and falls back to the containment pin when there is no lyric zone to
    // cross into. Both edges are declared, in this order, and that ordering is the whole trick: move() walks a
    // zone's edges and SKIPS one whose target is hidden (count 0), so with lyrics the first edge fires and
    // without them the second consumes the key — instead of falling through to geometric crossing and escaping
    // onto the live home zones this modal page covers.
    g.addEdge(QStringLiteral("queue"), Qt::Key_Right, QStringLiteral("lyrics"));
    // TRIPWIRE, and deliberately unkillable today — say so, because a mutation run reports it as a survivor
    // and a survivor with no explanation gets deleted. Removing this line changes NOTHING right now: with the
    // lyric zone hidden, geometric crossing to the right of the queue finds `lyrics` FIRST (column 1, against
    // detailActions' column 8), sees count 0 and refuses, so the arrow is contained anyway. It is here for the
    // day something else is registered in a column right of the queue, when the absence of this line would
    // silently become an escape onto a live home zone from a modal page. Same reasoning as every other SELF
    // pin above; those are killable only because something IS reachable in their direction.
    g.addEdge(QStringLiteral("queue"), Qt::Key_Right, QStringLiteral("queue"));
    // The lyric list's own containment. Left returns to the queue (the reverse of the crossing above); Right is
    // the pin — nothing sits right of the panel. Up and Down are its Vertical along-axis and are deliberately
    // NOT declared, for the reason spelled out for the queue below: a declared edge is consulted BEFORE axis
    // stepping, so pinning either would freeze the list and there would be no way to pick a line at all. At the
    // top row Up finds no earlier line and crosses geometrically to the transport strip (row 20, the nearest
    // zone above); at the bottom row Down finds nothing below row 21 and is a contained no-op.
    g.addEdge(QStringLiteral("lyrics"), Qt::Key_Left,  QStringLiteral("queue"));
    g.addEdge(QStringLiteral("lyrics"), Qt::Key_Right, QStringLiteral("lyrics"));
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
//   * readerNav (row 2, col 0, Horizontal, wraps) — the bottom strip: prev / progress / next. ALWAYS visible
//     (count 3); the default zone the chrome reveals onto.
//   * readerSettings (row 1, col 0, Horizontal, does NOT wrap) — the top strip's control ROW, left to right:
//     Exit, then the kind's own controls (Book: font smaller/larger, reading theme, typeface; Pdf/Comic: zoom
//     out/in, fit, and a comic's two-up). Count-gated (0 until the chrome feeds live counts), exactly like
//     `categories`/`actions` on the themed home. Horizontal because it IS a row: its along-axis Left/Right step
//     between the controls, which is the movement the strip's layout promises. It deliberately does not wrap,
//     so Right off the LAST control still crosses geometrically to the bookmark list (see below) — wrapping
//     would send it back to Exit and cut that reach off on a Pdf/Comic entirely.
//   * readerToc (row 0, col 0, Vertical) — the chapter list, count = toc size. Count-gated (fed from tocTitles()).
//   * readerBookmarks (row 0, col 1, Vertical) — the per-book bookmark list, BESIDE the ToC (issue #136).
//     Count-gated (fed from the ReaderBridge's bookmarkCount); a book/pdf/comic with no bookmarks holds it at
//     0, so it is never a crossing target and focus can never strand on an empty list (the empty-state IS the
//     hidden zone). Reachable from the ToC by Left/Right (the two panels sit side by side) AND from the settings
//     row by geometry Right — the latter is what makes it reachable on a Pdf/Comic, where readerToc is gated
//     off so the ToC can't be the bridge to it.
//
// Declared edges: just readerNav --Up--> readerSettings (chosen so none blocks a zone's ALONG-axis internal
// stepping — a declared edge is consulted before axis stepping, so declaring Up/Down on a Vertical list zone
// would freeze its scrolling). Everything else is GEOMETRIC: readerToc/readerSettings/readerNav are stacked in
// col 0 (crossed by Up/Down at a list's edge) and readerBookmarks sits at col 1, so the ToC↔bookmark-list
// switch and the settings-row→bookmark-list reach are the nearest-zone crossing in the cross-axis direction —
// no declared edge needed (and a declared same-index cross would report "no move", swallowing the step's feel).
// Containment SELF edges pin the OUTWARD arrows that would otherwise run off the surface into nothing (mirrors
// the detail view's SELF-edge pins).
inline void buildReaderNavGraph(NavGraph& g, ReaderKind kind)
{
    g.registerZone(QStringLiteral("readerNav"), 3, 2, 0, Qt::Horizontal, /*wraps=*/true); // prev/progress/next
    g.registerZone(QStringLiteral("readerSettings"), 0, 1, 0, Qt::Horizontal);            // the control row (gated)
    g.registerZone(QStringLiteral("readerToc"), 0, 0, 0, Qt::Vertical);                   // chapter list (gated)
    g.registerZone(QStringLiteral("readerBookmarks"), 0, 0, 1, Qt::Vertical);             // bookmark list (gated)
    g.setDefaultZone(QStringLiteral("readerNav"));

    // The chrome chain (declared where geometry can't be trusted / to keep the shape explicit, like the home's
    // items<->categories edges). readerNav's Up/Down are cross-axis (it is Horizontal) so declaring them is
    // safe. readerSettings is Horizontal too, so ONLY its cross-axis (Up/Down) could be declared — and neither
    // is, because the col-0 stack already resolves them geometrically (up to the ToC, down to the bottom bar).
    // Declaring its Left/Right would be the real mistake: those are its ALONG axis now, and a declared edge is
    // consulted BEFORE axis stepping, so it would freeze the row and the controls would be unreachable.
    g.addEdge(QStringLiteral("readerNav"), Qt::Key_Up, QStringLiteral("readerSettings"));

    // The ToC ↔ bookmark-list switch (issue #136) is GEOMETRIC: readerToc at col 0, readerBookmarks at col 1,
    // so Right off the ToC crosses to the bookmark list and Left crosses back — the nearest-zone resolution in
    // the cross axis, which never freezes either Vertical list's own Up/Down. A hidden target is not a crossing
    // target, so an empty bookmark list (or a Pdf/Comic's gated ToC) makes the crossing a contained no-op with
    // no declared edge required.

    // Containment (SELF edges = consume, no geometric escape). readerNav wraps Left/Right in-strip and pins its
    // Down (nothing below the bottom bar). readerToc/readerBookmarks are Vertical lists: pin the OUTWARD
    // cross-axis arrows that face off the surface (toc-Left, bookmarks-Right) so a stray horizontal arrow can't
    // fall through; their along-axis Up/Down keep stepping the list, crossing to the neighbour zone by geometry
    // only at the list's edge.
    //
    // readerSettings gets NO horizontal pin, and must not: Left/Right are its along axis. Left at the first
    // control and Right at the last fall through to geometry — nothing sits left of col 0, so Left is a
    // contained no-op on its own, while Right resolves to readerBookmarks (col 1). That crossing is the ONLY
    // path to the bookmark list on a Pdf/Comic (their ToC is gated, so the toc↔bookmarks bridge is inert
    // there); with an empty list it too is gated and the step is simply a no-op.
    g.addEdge(QStringLiteral("readerNav"), Qt::Key_Down, QStringLiteral("readerNav"));
    g.addEdge(QStringLiteral("readerToc"), Qt::Key_Left,  QStringLiteral("readerToc"));
    g.addEdge(QStringLiteral("readerBookmarks"), Qt::Key_Right, QStringLiteral("readerBookmarks"));

    // All three kinds share this exact zone STRUCTURE + edge set; only the live counts differ and are fed
    // externally by the host/probe via setZoneCount (Book: readerSettings=5 controls, readerToc=chapters,
    // readerBookmarks=bookmarks; Pdf: readerSettings=4 (exit + zoom/fit), readerToc=0; Comic: readerSettings=5
    // (+two-up), readerToc=0; both may carry bookmarks). So kind is not consulted here — keeping the shape
    // identical is exactly what lets ONE builder back all three.
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
