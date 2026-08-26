// The natural page-order collation shared by the non-ZIP comic archive readers (CB7, CBT). A comic's pages
// are usually named page1, page2, …, page10 — a plain lexical sort puts page10 before page2, so the pages
// must compare numeric-aware and case-insensitively. The CBZ/ZIP path in ComicView.cpp keeps its own inline
// QCollator (byte-for-byte unchanged); this header exists so the new readers reuse the identical rule and so
// the rule can be unit-tested (probe_tar) without pulling in the widget.
#pragma once
#include "../core/NaturalOrder.h"

#include <QCollator>
#include <QFileInfo>
#include <QLatin1String>
#include <QString>

namespace ComicPages
{
    // "Is this archive member a page?" — the same question the readers ask, moved here when the LIBRARY
    // scan (issue #134) became a second asker: a comic's cover is its first page, which means the scan has
    // to pick page one out of a CBZ using exactly the rule the reader will use when the same file is opened.
    // Two copies of it would let a shelf show one picture and the reader open on another.
    //
    // The two exclusions are not decoration. `__MACOSX` is the resource-fork directory every archive built
    // on a Mac carries, and it holds a byte-identical shadow of every real page; a dotfile is the same story
    // one level down. Both sort BEFORE the real pages, so without this the cover of half the comics in the
    // world would be an 82-byte AppleDouble stub that does not decode.
    inline bool isImageName(const QString& name)
    {
        const QString lower = name.toLower();
        const QString base = QFileInfo(name).fileName();
        if (name.contains(QStringLiteral("__MACOSX")) || base.startsWith(QLatin1Char('.'))) return false;
        for (const char* ext : { ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".avif" })
            if (lower.endsWith(QLatin1String(ext))) return true;
        return false;
    }

    // A numeric-aware, case-insensitive collator. Built once per open and passed to the comparator below.
    // Built through NaturalOrder because a plain `QCollator c; c.setNumericMode(true);` is INERT under the
    // C locale — it accepts numeric mode, reports it as set, and orders page10 before page2 anyway. See
    // NaturalOrder.h; that silence is issue #205.
    inline QCollator collator()
    {
        return NaturalOrder::collator();
    }

    // Order two page names under the given collator. Kept tiny and explicit so a mutation to the comparison
    // (or to the collator's numeric mode) is caught by a single assertion.
    inline bool lessThan(const QCollator& c, const QString& a, const QString& b)
    {
        return c.compare(a, b) < 0;
    }
}
