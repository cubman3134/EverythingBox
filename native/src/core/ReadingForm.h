// The reading FORM of a stored row: which of the three reading catalogues (Books / Comics / Manga) it
// belongs in. The second axis of the Recent/Downloaded filter for `reading`, exactly as RecentItem::system
// is for `game` — and it exists for the same reason.
//
// WHY A SECOND AXIS AT ALL. RecentStore/DownloadsStore file a row under a ROUTING kind ("video" | "audio" |
// "document" | "game" | …): the thing MainWindow::openRecent switches on to decide which viewer opens it.
// Every reading item is "document", because an EPUB, a PDF, a comic issue and a manga chapter are all opened
// by the reader stack. But Books, Comics and Manga are three SEPARATE catalogues (AIO Catalog declares them
// as types book/comic/manga; core::mediaCategory files all three under `reading`), and the per-catalogue
// "Recent"/"Downloaded" folders filtered on the routing kind alone — so opening Comics ▸ Recent listed the
// novels you had been reading too. Games never had this because they carry SystemCatalog's console id.
//
// A row's form is RECORDED from the catalog item's own type when it is written, so it is a fact rather than
// a guess. The guessing only happens for a row written before this field existed, and it is deliberately
// CONSERVATIVE — see matchesReadingScope.
#pragma once
#include <QString>

namespace core
{
    // The form a catalog/media type reads as: "book" | "comic" | "manga", or empty for a type that is not a
    // reading type at all. VERBATIM the three tokens core::mediaCategory files under `reading`, so a stored
    // form and a catalogue's scope are always the same spelling and can be compared directly.
    inline QString readingForm(const QString& type)
    {
        const QString t = type.toLower();
        if (t == QLatin1String("book") || t == QLatin1String("ebook") || t == QLatin1String("novel")
            || t == QLatin1String("pdf"))                                 return QStringLiteral("book");
        if (t == QLatin1String("comic") || t == QLatin1String("comic_issue")) return QStringLiteral("comic");
        if (t == QLatin1String("manga") || t == QLatin1String("manga_chapter")) return QStringLiteral("manga");
        return QString();
    }

    // Does a row whose stored form is `form` and whose file is `path` belong in the catalogue scoped to
    // `scope`? The three rules, in order:
    //
    //   1. NO SCOPE — every reading row. This is the pre-existing behaviour and the answer for any caller
    //      that has not asked a narrower question.
    //   2. A STORED FORM — an exact match, and nothing else. The row was written by a build that knew what it
    //      was, so there is nothing to infer.
    //   3. NO STORED FORM (a row written before this field existed) — infer from the FILE, and only as far as
    //      the file honestly says. A .cbz is comic-SHAPED, which is true of both a comic issue and a manga
    //      chapter (the manga provider serves chapters as CBZ), so it matches BOTH of those scopes rather
    //      than picking one; .epub/.pdf/… are book-shaped and match Books. An extension that says nothing —
    //      a remote document cached under a hashed name, a bare URL — matches EVERY scope, because dropping
    //      an old row out of all three catalogues would be a worse answer than showing it in all three.
    inline bool matchesReadingScope(const QString& form, const QString& path, const QString& scope)
    {
        if (scope.isEmpty()) return true;
        if (!form.isEmpty()) return form == scope;
        const QString p = path.toLower();
        auto ends = [&p](const char* ext) { return p.endsWith(QLatin1String(ext)); };
        if (ends(".cbz") || ends(".cbr") || ends(".cb7") || ends(".cbt") || ends(".cba"))
            return scope == QLatin1String("comic") || scope == QLatin1String("manga");
        if (ends(".epub") || ends(".pdf") || ends(".mobi") || ends(".azw") || ends(".azw3")
            || ends(".fb2") || ends(".djvu"))
            return scope == QLatin1String("book");
        return true; // says nothing about itself -> visible everywhere, never silently dropped
    }
}
