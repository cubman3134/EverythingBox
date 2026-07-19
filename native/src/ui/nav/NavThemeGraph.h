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

// Register the themed surface's zones + declared edges on a fresh NavGraph. `itemCount` is the item column's
// starting count (buildView: items.size(); the probe: a fixed test count). categories/buttons/actions start
// hidden (count 0) — their live counts arrive later via setZoneCount, keeping their edges inert until then.
inline void buildThemedNavGraph(NavGraph& g, int itemCount)
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
}
