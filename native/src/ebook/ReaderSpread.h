#pragma once
#include <QtGlobal>

// Dual-page landscape for BOOKS (issue #147), as pure geometry: a viewport and a preference go in, a column
// count and column rectangles come out. No widgets and no document, so probe_readergestures can pin the
// wide-viewport predicate — and, above all, its BOUNDARY — without laying out a single line of text.
//
// It is a pagination-geometry change and nothing more. The text flow, the reading anchor (a document
// character offset), the page numbering and the saved place are all untouched: a page simply holds two
// columns' worth of whole lines instead of one, which is why turning the preference on and off mid-book
// keeps the reader on the same words.
//
// COMICS ARE NOT HERE. A comic's two-up spread is issue #154's, it lays out images rather than lines, and it
// already handles issue #152's right-to-left direction; this increment does not touch it.
namespace ReaderSpread
{

// The narrowest viewport that gets a spread. A second column is only an improvement when each column is
// still wide enough to hold a readable line, so the predicate is landscape AND genuinely wide — not
// landscape alone, which every desktop window and every phone held sideways would satisfy.
inline int minWideWidthPx() { return 900; }

// The wide-viewport predicate. Strictly wider than it is tall (a square viewport is not landscape) and at
// least minWideWidthPx across, so the boundary is: minWideWidthPx itself spreads, one pixel less does not.
inline bool active(bool enabled, int viewportW, int viewportH)
{
    return enabled && viewportW > viewportH && viewportW >= minWideWidthPx();
}

inline int columns(bool enabled, int viewportW, int viewportH)
{
    return active(enabled, viewportW, viewportH) ? 2 : 1;
}

// The gutter between the two columns — the inner margin of an open book. Proportional with a floor and a
// ceiling, for the same reason the top band is: fixed pixels are a chasm on a phone and a hairline on a
// television, and the reader runs on both.
inline double gutterFor(double contentW)
{
    return qBound(24.0, contentW * 0.06, 96.0);
}

// One column's width inside the content box. Never returns 0 or less, so a caller can hand it straight to
// QTextDocument::setTextWidth on a viewport that has not been laid out yet.
inline double columnWidth(double contentW, int columns, double gutter)
{
    if (columns < 2) return qMax(1.0, contentW);
    return qMax(1.0, (contentW - gutter * double(columns - 1)) / double(columns));
}

// Where column `i` (0 = the one the text enters first) is drawn.
//
// WHICH SIDE THE FIRST COLUMN IS ON is the whole of what reading direction changes about a spread — exactly
// the rule ComicView's two-up follows for issue #152, stated once here so the book side cannot drift from
// it: right to left puts the EARLIER column on the RIGHT, because that is the one the eye reaches first.
// Nothing about the text order, the numbering or the reading position moves; only the two rectangles.
//
// Books pass rtl=false today: no book format's declared page-progression-direction is read yet (#152's value
// is per-series and lives on comics), so this is the seam that reading direction plugs into, not a claim
// that a book already has one.
inline double columnLeft(double contentLeft, double columnW, double gutter, int i, int columns, bool rtl)
{
    const int slot = rtl ? (columns - 1 - i) : i;
    return contentLeft + double(slot) * (columnW + gutter);
}

} // namespace ReaderSpread
