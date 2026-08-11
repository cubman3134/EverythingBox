// ReaderAnchor — the ONE repagination-stable position model shared by every reader kind (issue #136,
// "Anchors first ... build it once"). It is the format annotations and cross-device reading positions key
// on: a bookmark is an anchor with a label, and a highlight (deferred) is the SAME anchor with its reserved
// end-offset filled in. Building it once here is what lets bookmarks, highlights, notes and positions all
// reuse one identity instead of each store inventing its own.
//
// The kinds and what each carries (the #135 reflow work established the book seam — a character OFFSET into a
// spine item, stable across resize/font change, unlike a page index):
//   * Book  — spine item (chapter) index + a character `offset` within that chapter. `endOffset` is RESERVED
//             for a future highlight RANGE end (-1 == a point anchor, which is all a bookmark ever uses);
//             bookmarks set `offset` only.
//   * Pdf   — a `page` (0-based) and an OPTIONAL region on it (regionX/Y/W/H, per-mille of the page, -1 == none).
//   * Comic — a `page` only (bookmarks yes; highlights n/a — there is no text to anchor into).
//
// Pure, header-only, QtCore-only (QJsonObject) so it links into headless probes with nothing else. toJson /
// fromJson round-trip EXACTLY (fromJson(toJson(a)) == a for every field, so a re-serialised anchor is byte-
// identical), and inReadingOrder is a deterministic total order — spine-then-offset for a book, page(-then-
// region) for pdf/comic — so a book's bookmark list sorts into document order the same way on every device.
// probe_bookmarks pins all of this.
#pragma once
#include <QJsonObject>
#include <QJsonValue>

struct ReaderAnchor
{
    enum Kind { Book = 0, Pdf = 1, Comic = 2 };

    Kind kind = Book;

    // Book: the spine item (chapter) index and the character offset of the anchored spot within that chapter.
    int spine  = 0;
    int offset = 0;
    // Book: the END offset of a highlight RANGE. RESERVED for the deferred highlights work — a bookmark is a
    // POINT anchor and leaves it -1. Carried through toJson/fromJson so the format never has to change to gain
    // ranges: a highlight is simply an anchor with endOffset >= 0.
    int endOffset = -1;

    // Pdf / Comic: the page (0-based). A book leaves this 0.
    int page = 0;
    // Pdf ONLY: an optional region on the page, as per-mille (0..1000) of the page width/height. -1 in any
    // field means "no region" (a whole-page anchor). Comics never carry one; a book leaves them -1.
    int regionX = -1;
    int regionY = -1;
    int regionW = -1;
    int regionH = -1;

    bool operator==(const ReaderAnchor& o) const
    {
        return kind == o.kind && spine == o.spine && offset == o.offset && endOffset == o.endOffset
            && page == o.page && regionX == o.regionX && regionY == o.regionY
            && regionW == o.regionW && regionH == o.regionH;
    }
    bool operator!=(const ReaderAnchor& o) const { return !(*this == o); }

    // Every field is written, so fromJson(toJson(a)) == a holds unconditionally (no field defaults to a value
    // that could hide a difference). Keys are stable and short-but-legible.
    QJsonObject toJson() const
    {
        QJsonObject o;
        o.insert(QStringLiteral("kind"), int(kind));
        o.insert(QStringLiteral("spine"), spine);
        o.insert(QStringLiteral("offset"), offset);
        o.insert(QStringLiteral("endOffset"), endOffset);
        o.insert(QStringLiteral("page"), page);
        o.insert(QStringLiteral("regionX"), regionX);
        o.insert(QStringLiteral("regionY"), regionY);
        o.insert(QStringLiteral("regionW"), regionW);
        o.insert(QStringLiteral("regionH"), regionH);
        return o;
    }

    static ReaderAnchor fromJson(const QJsonObject& o)
    {
        ReaderAnchor a;
        a.kind      = normalizeKind(o.value(QStringLiteral("kind")).toInt(int(Book)));
        a.spine     = o.value(QStringLiteral("spine")).toInt(0);
        a.offset    = o.value(QStringLiteral("offset")).toInt(0);
        a.endOffset = o.value(QStringLiteral("endOffset")).toInt(-1);
        a.page      = o.value(QStringLiteral("page")).toInt(0);
        a.regionX   = o.value(QStringLiteral("regionX")).toInt(-1);
        a.regionY   = o.value(QStringLiteral("regionY")).toInt(-1);
        a.regionW   = o.value(QStringLiteral("regionW")).toInt(-1);
        a.regionH   = o.value(QStringLiteral("regionH")).toInt(-1);
        return a;
    }

    // A book anchor is a point when it carries no range end. Highlights (deferred) are the endOffset >= 0 case.
    bool isRange() const { return kind == Book && endOffset >= 0; }

    // Reading order — a deterministic TOTAL order so a bookmark list sorts identically everywhere. Kind is the
    // outermost key (a list is one kind in practice, but a mixed input must still order deterministically);
    // then a book orders by spine, then offset (then endOffset as a tiebreak so two anchors that differ only in
    // a range end are still strictly ordered); a pdf/comic orders by page, then the pdf region (comics leave it
    // -1, so page alone decides). Never depends on iteration or insertion order.
    static bool inReadingOrder(const ReaderAnchor& a, const ReaderAnchor& b)
    {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.kind == Book)
        {
            if (a.spine  != b.spine)  return a.spine  < b.spine;
            if (a.offset != b.offset) return a.offset < b.offset;
            return a.endOffset < b.endOffset;
        }
        // Pdf / Comic: page-ordered; the pdf region tie-breaks within a page (comics have none).
        if (a.page    != b.page)    return a.page    < b.page;
        if (a.regionY != b.regionY) return a.regionY < b.regionY;
        if (a.regionX != b.regionX) return a.regionX < b.regionX;
        if (a.regionW != b.regionW) return a.regionW < b.regionW;
        return a.regionH < b.regionH;
    }

    bool lessThan(const ReaderAnchor& o) const { return inReadingOrder(*this, o); }

private:
    // A JSON int outside the enum (a corrupt/forged document) reads back as Book rather than an out-of-range
    // Kind — so a malformed anchor is a defined book anchor, never undefined behaviour in the comparator.
    static Kind normalizeKind(int k)
    {
        return (k == Pdf || k == Comic) ? Kind(k) : Book;
    }
};
