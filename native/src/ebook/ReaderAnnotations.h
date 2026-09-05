// ReaderAnnotations — the ONE list the reader's annotation panel draws (issue #136): this book's bookmarks and
// its highlights, interleaved in DOCUMENT order. Pure and header-only, so the ordering the panel shows is
// pinned by a probe rather than by opening a book and looking at it.
//
// Why merge them here rather than in the panel: the panel is drawn twice (a themed QML list and, in the
// classic layout, the reader's own overlay), and an order that lives in either one of them is an order the
// other can disagree with. The bookmarks half already sorted by ReaderAnchor::inReadingOrder; a highlight is
// the same anchor with its range end filled in, so the same comparator orders both without a second rule —
// which is the whole reason ReaderAnchor::inReadingOrder is a TOTAL order including endOffset.
//
// The excerpt is what the issue asks the panel to show: a bookmark lists its label (a chapter title / page
// read-out), a highlight lists the words it covers. A highlight with no stored text falls back to its label
// form, so a row is never blank.
#pragma once
#include <QString>
#include <QVector>

#include <algorithm>

#include "ReaderAnchor.h"
#include "../core/BookmarkStore.h"
#include "../core/HighlightStore.h"

namespace ReaderAnnotations
{

struct Entry
{
    enum Kind { Bookmark = 0, Highlight = 1 };

    Kind         kind = Bookmark;
    QString      id;        // the store id — whichever store this row came from
    ReaderAnchor anchor;    // where in the book (a highlight's carries the range end)
    QString      excerpt;   // what the panel shows: a bookmark's label, a highlight's words
    int          color = -1; // highlight palette index; -1 for a bookmark (it has no colour)

    bool isHighlight() const { return kind == Highlight; }
};

// Both stores' rows for one book, in document order. Bookmarks and highlights that share an offset order
// bookmark-first (a point anchor's endOffset is -1, which inReadingOrder puts before any range end) — a stable,
// stated rule rather than whichever store happened to be read first.
inline QVector<Entry> merged(const QVector<BookmarkStore::Bookmark>& bookmarks,
                             const QVector<HighlightStore::Highlight>& highlights)
{
    QVector<Entry> out;
    out.reserve(bookmarks.size() + highlights.size());

    int n = 0;
    for (const BookmarkStore::Bookmark& b : bookmarks)
    {
        Entry e;
        e.kind    = Entry::Bookmark;
        e.id      = b.id;
        e.anchor  = b.anchor;
        e.excerpt = b.label.isEmpty() ? QStringLiteral("Bookmark %1").arg(++n) : b.label;
        out.push_back(e);
    }
    for (const HighlightStore::Highlight& h : highlights)
    {
        Entry e;
        e.kind    = Entry::Highlight;
        e.id      = h.id;
        e.anchor  = h.anchor;
        e.color   = h.color;
        e.excerpt = h.text.isEmpty() ? QStringLiteral("Highlight") : h.text;
        out.push_back(e);
    }

    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return ReaderAnchor::inReadingOrder(a.anchor, b.anchor);
    });
    return out;
}

// The row a panel draws. A highlight is marked so the two kinds are told apart at a glance without the panel
// inventing its own vocabulary; the marks are text, so they survive a theme with no icon font.
inline QString rowLabel(const Entry& e)
{
    return (e.isHighlight() ? QStringLiteral("▍ ") : QStringLiteral("🔖 ")) + e.excerpt;
}

} // namespace ReaderAnnotations
