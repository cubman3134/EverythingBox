// The natural page-order collation shared by the non-ZIP comic archive readers (CB7, CBT). A comic's pages
// are usually named page1, page2, …, page10 — a plain lexical sort puts page10 before page2, so the pages
// must compare numeric-aware and case-insensitively. The CBZ/ZIP path in ComicView.cpp keeps its own inline
// QCollator (byte-for-byte unchanged); this header exists so the new readers reuse the identical rule and so
// the rule can be unit-tested (probe_tar) without pulling in the widget.
#pragma once
#include <QCollator>
#include <QString>

namespace ComicPages
{
    // A numeric-aware, case-insensitive collator. Built once per open and passed to the comparator below.
    inline QCollator collator()
    {
        QCollator c;
        c.setNumericMode(true);              // "page2" < "page10" — the whole point
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }

    // Order two page names under the given collator. Kept tiny and explicit so a mutation to the comparison
    // (or to the collator's numeric mode) is caught by a single assertion.
    inline bool lessThan(const QCollator& c, const QString& a, const QString& b)
    {
        return c.compare(a, b) < 0;
    }
}
