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
    g.addEdge(QStringLiteral("detailActions"), Qt::Key_Escape, QStringLiteral("items"));
}
