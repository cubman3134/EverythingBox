// WHICH FILES THE BOOK READER OPENS, as one predicate (issue #144). Header-only and QtCore-only, so the
// routing question can be asked from MainWindow, from a probe, and from anywhere else, and always answered
// the same way.
//
// WHY THIS EXISTS. The reader's format list is spelled out in several places — the Open dialog's filter, the
// split-pane branch, the full-screen branch — and each of them used to carry its own `lower.endsWith(".epub")`
// chain. Adding a format meant finding every chain, and missing one meant a format that opened on one route
// and fell through to the video player on another. One list, named once.
//
// WHAT IS DELIBERATELY NOT IN IT:
//
//   * .pdf. It is a book the reader opens, but its routing is a TWO-STEP: EbookView first (a text PDF
//     reflows), then PdfView (a scanned one does not). Folding it in here would make the first branch claim
//     it and the fallback unreachable. The PDF branch stays exactly where it is.
//   * .cbz / .cbr / .cb7 / .cbt. Comics are ComicView's, and ComicView::isComicFile is their list.
//   * a bare .zip. "A zip in a books folder is a book" is a guess with no marker behind it — the same
//     refusal BookLibrary.h states. `.fb2.zip` is claimed because its NAME says FB2 in so many words.
#pragma once
#include <QLatin1String>
#include <QString>

namespace EbookFormats
{
    // Every reflowable book format EbookView can open: EPUB, FictionBook (plain and zipped), the Kindle
    // family, and plain text / Markdown. Matched on the whole name, not on QFileInfo::suffix(), because
    // ".fb2.zip" has the suffix "zip".
    inline bool opensInBookReader(const QString& path)
    {
        const QString p = path.toLower();
        for (const char* ext : { ".epub",
                                 ".fb2", ".fb2.zip", ".fbz",
                                 ".azw3", ".azw", ".mobi",
                                 ".txt", ".text", ".md", ".markdown", ".mdown", ".mkd" })
            if (p.endsWith(QLatin1String(ext))) return true;
        return false;
    }
}
