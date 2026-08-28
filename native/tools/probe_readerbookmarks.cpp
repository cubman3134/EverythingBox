// probe_readerbookmarks — the half of the #136 bookmark feature that lives in the READERS: the natural key a
// pdf/comic reports for itself (HostedReader::itemKey) and the page jump that restores a bookmark's anchor
// (HostedReader::gotoPage). probe_bookmarks pins the anchor model and the store; this pins the two ends the
// themed chrome plugs them into, over the REAL PdfView and ComicView.
//
// THE BUG IT PINS. HostedReader::itemKey() defaults to an empty string, and ReaderChromeHost's add/goto/remove
// all return early on an empty key. Only EbookView overrode it, so in a PDF or a comic the Bookmarks panel was
// permanently empty and "add bookmark" did nothing — silently, with nothing to see and no message saying why.
// gotoPage() had the same shape: a base-class no-op nobody overrode, so even a bookmark that existed could not
// be jumped to. Both ends are asserted here; §1 is the tripwire that says WHY an empty key is fatal.
//
// THE READERS ARE REAL, AND SO ARE THE FILES. A stub HostedReader would prove only that a stub returns what it
// was written to return — it is exactly the shape that was green while the feature was dead. So this probe
// constructs an actual PdfView and an actual ComicView and opens an actual multi-page PDF and an actual CBZ of
// real PNGs (written here with miniz, as probe_books does), then asks the reader what it just opened.
//
// ORACLE IS INDEPENDENT OF THE CODE UNDER TEST: the fixture paths, the page counts and every expected page
// number are written out by hand, never read back from the reader that is being asserted.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so BookmarkStore and both
// readers' resume keys open an everythingbox.ini that starts empty and is removed at exit.
//
// Prints READERBM-OK on success; any failure prints READERBM-FAIL <cond> (line) and exits non-zero.
#include "PdfView.h"
#include "ComicView.h"
#include "BookmarkStore.h"
#include "ReaderAnchor.h"
#include "ProfileStore.h"

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QVector>
#include <cstdio>
#include <cstring>

#include "miniz.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "READERBM-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---------------------------------------------------------------------------------------------------------
// Fixture builders — a real CBZ of real PNGs and a real multi-page PDF (the same approach probe_books takes:
// every bug this feature can have lives between a file on disk and what the reader reports about it).
// ---------------------------------------------------------------------------------------------------------

static QByteArray pngBytes(const QColor& c, int w = 40, int h = 60)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(c);
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

// One .cbz of `pages` flat-colour PNGs, named page1..pageN.
static bool writeCbz(const QString& path, int pages)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path.toUtf8().constData(), 0)) return false;
    bool ok = true;
    for (int i = 1; i <= pages; ++i)
    {
        const QByteArray name = QStringLiteral("page%1.png").arg(i).toUtf8();
        const QByteArray png  = pngBytes(QColor(20 * i % 255, 90, 160));
        if (!mz_zip_writer_add_mem(&zip, name.constData(), png.constData(), size_t(png.size()), MZ_BEST_SPEED))
            ok = false;
    }
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    return ok;
}

// The smallest complete PDF with `pages` pages: catalog, page tree, one page object + content stream each,
// one font, and a real cross-reference table — PDFium refuses one with wrong offsets, so the offsets are
// recorded as the objects are emitted rather than guessed.
static bool writeMultiPagePdf(const QString& path, int pages)
{
    QVector<QByteArray> objs;
    // Object numbering: 1 = catalog, 2 = pages, 3 = font, then per page (obj, content stream) pairs.
    const int firstPageObj = 4;
    QByteArray kids;
    for (int i = 0; i < pages; ++i)
    {
        if (i) kids += ' ';
        kids += QByteArray::number(firstPageObj + i * 2) + QByteArrayLiteral(" 0 R");
    }
    objs.append(QByteArrayLiteral("<< /Type /Catalog /Pages 2 0 R >>"));
    objs.append(QByteArrayLiteral("<< /Type /Pages /Kids [") + kids + QByteArrayLiteral("] /Count ")
                + QByteArray::number(pages) + QByteArrayLiteral(" >>"));
    objs.append(QByteArrayLiteral("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));
    for (int i = 0; i < pages; ++i)
    {
        const int contentObj = firstPageObj + i * 2 + 1;
        objs.append(QByteArrayLiteral("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents ")
                    + QByteArray::number(contentObj)
                    + QByteArrayLiteral(" 0 R /Resources << /Font << /F1 3 0 R >> >> >>"));
        const QByteArray stream = QByteArrayLiteral("BT /F1 24 Tf 72 700 Td (Page ")
                                + QByteArray::number(i + 1) + QByteArrayLiteral(") Tj ET");
        objs.append(QByteArrayLiteral("<< /Length ") + QByteArray::number(stream.size())
                    + QByteArrayLiteral(" >>\nstream\n") + stream + QByteArrayLiteral("\nendstream"));
    }

    QByteArray out = QByteArrayLiteral("%PDF-1.4\n");
    QVector<int> offsets;
    for (int i = 0; i < objs.size(); ++i)
    {
        offsets.append(out.size());
        out += QByteArray::number(i + 1) + QByteArrayLiteral(" 0 obj\n") + objs.at(i)
             + QByteArrayLiteral("\nendobj\n");
    }
    const int xrefAt = out.size();
    out += QByteArrayLiteral("xref\n0 ") + QByteArray::number(objs.size() + 1) + '\n';
    out += QByteArrayLiteral("0000000000 65535 f \n");
    for (int off : offsets)
    {
        QByteArray n = QByteArray::number(off);
        while (n.size() < 10) n.prepend('0');
        out += n + QByteArrayLiteral(" 00000 n \n");
    }
    out += QByteArrayLiteral("trailer\n<< /Size ") + QByteArray::number(objs.size() + 1)
         + QByteArrayLiteral(" /Root 1 0 R >>\nstartxref\n") + QByteArray::number(xrefAt)
         + QByteArrayLiteral("\n%%EOF\n");

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(out) == out.size();
}

// The anchors are built BY HAND (the fixture — never produced by the reader under test).
static ReaderAnchor pdfAnchor(int page)   { ReaderAnchor a; a.kind = ReaderAnchor::Pdf;   a.page = page; return a; }
static ReaderAnchor comicAnchor(int page) { ReaderAnchor a; a.kind = ReaderAnchor::Comic; a.page = page; return a; }

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    ProfileStore::setCurrent(QStringLiteral("readerbmtest"));

    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "READERBM-FAIL could not create a temp dir\n"); return 1; }

    const QString pdfPath   = tmp.filePath(QStringLiteral("Manual.pdf"));
    const QString cbzPath   = tmp.filePath(QStringLiteral("Saga 001.cbz"));
    const QString photoDir  = tmp.filePath(QStringLiteral("Holiday"));
    const int kPdfPages     = 5;   // hand-declared; the fixture is written to match
    const int kComicPages   = 6;

    CHECK(writeMultiPagePdf(pdfPath, kPdfPages));
    CHECK(writeCbz(cbzPath, kComicPages));
    QDir().mkpath(photoDir);
    for (int i = 1; i <= 3; ++i)
    {
        QFile f(photoDir + QStringLiteral("/shot%1.png").arg(i));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(pngBytes(QColor(200, 40 * i, 30)));
    }

    // ---- 1. An empty key is fatal to the whole feature -------------------------------------------------
    // ABSENCE-OF-BEHAVIOUR TRIPWIRE, and the reason the rest of this file exists. The store no-ops on an
    // empty bookKey and the chrome's add/goto/remove return early on one, so a reader that reports no key
    // has a Bookmarks panel that is permanently empty and an add that does nothing. Nothing else in the
    // suite says so, which is how a reader with no itemKey() override shipped looking like a working
    // feature. No mutation kills these two lines — they assert what the store does NOT do.
    CHECK(BookmarkStore::add(QString(), pdfAnchor(2), QStringLiteral("nowhere")).id.isEmpty());
    CHECK(BookmarkStore::list(QString()).isEmpty());

    // A reader with no file open reports no key either — which is correct, and is the ONE case where the
    // chrome's inert bookmark controls are the right behaviour.
    {
        PdfView   freshPdf;
        ComicView freshComic;
        CHECK(freshPdf.itemKey().isEmpty());
        CHECK(freshComic.itemKey().isEmpty());
    }

    // ---- 2. PDF: the reader names the file it opened, and jumps to a bookmarked page --------------------
    {
        PdfView pdf;
        QString err;
        CHECK(pdf.openPdf(pdfPath, &err));
        CHECK(err.isEmpty());
        CHECK(pdf.pageCount() == kPdfPages);          // hand-declared count, not one read back from the writer
        CHECK(pdf.currentPage() == 1);                // a fresh ini has no resume position

        // The key IS the path — the same basis pdfKey() hashes for the resume position, so one identity
        // names both. (Not "some non-empty string": a key that is not the path splits a file's bookmarks
        // from its resume position and silently loses them when the other end changes.)
        CHECK(pdf.itemKey() == pdfPath);

        // Add at page 4 (0-based 3) — the chrome captures currentPage()-1, so drive the reader there first
        // and take the anchor the way ReaderChromeHost does.
        pdf.gotoPage(3);
        CHECK(pdf.currentPage() == 4);                                  // the jump landed
        const ReaderAnchor taken = pdfAnchor(pdf.currentPage() - 1);
        CHECK(taken.page == 3);
        const BookmarkStore::Bookmark b = BookmarkStore::add(pdf.itemKey(), taken, QStringLiteral("Page 4 / 5"));
        CHECK(!b.id.isEmpty());                                         // a real key stores a real row
        CHECK(b.bookKey == pdfPath);

        // Walk away, then restore from the STORE (the round trip the panel makes).
        pdf.gotoPage(0);
        CHECK(pdf.currentPage() == 1);
        const QVector<BookmarkStore::Bookmark> list = BookmarkStore::list(pdf.itemKey());
        CHECK(list.size() == 1);
        if (!list.isEmpty())
        {
            CHECK(list.at(0).anchor.page == 3);
            CHECK(list.at(0).label == QStringLiteral("Page 4 / 5"));
            pdf.gotoPage(list.at(0).anchor.page);
        }
        CHECK(pdf.currentPage() == 4);                                  // back on the bookmarked page

        // A bookmark that outlived its file cannot land the reader off the end of the document.
        pdf.gotoPage(999);
        CHECK(pdf.currentPage() == kPdfPages);
        pdf.gotoPage(-5);
        CHECK(pdf.currentPage() == 1);
    }

    // ---- 3. PDF: the bookmark survives a NEW reader over the same file ----------------------------------
    // The panel's real job is a position that outlives the session; a key that were per-instance (a pointer,
    // a counter) would pass §2 and fail here.
    {
        PdfView reopened;
        CHECK(reopened.openPdf(pdfPath));
        CHECK(reopened.itemKey() == pdfPath);
        const QVector<BookmarkStore::Bookmark> list = BookmarkStore::list(reopened.itemKey());
        CHECK(list.size() == 1);
        if (!list.isEmpty()) reopened.gotoPage(list.at(0).anchor.page);
        CHECK(reopened.currentPage() == 4);
    }

    // ---- 4. Comic: the same round trip over a real CBZ --------------------------------------------------
    {
        ComicView comic;
        QString err;
        CHECK(comic.openComic(cbzPath, &err));
        CHECK(err.isEmpty());
        CHECK(comic.pageCount() == kComicPages);
        CHECK(comic.currentPage() == 1);
        CHECK(comic.itemKey() == cbzPath);                              // the archive path, as comicKey() hashes

        comic.gotoPage(4);
        CHECK(comic.currentPage() == 5);
        const BookmarkStore::Bookmark b =
            BookmarkStore::add(comic.itemKey(), comicAnchor(comic.currentPage() - 1), QStringLiteral("Page 5 / 6"));
        CHECK(!b.id.isEmpty());
        CHECK(b.bookKey == cbzPath);
        CHECK(b.anchor.page == 4);

        comic.gotoPage(0);
        CHECK(comic.currentPage() == 1);
        const QVector<BookmarkStore::Bookmark> list = BookmarkStore::list(comic.itemKey());
        CHECK(list.size() == 1);
        if (!list.isEmpty()) comic.gotoPage(list.at(0).anchor.page);
        CHECK(comic.currentPage() == 5);

        // Out of range is refused, not clamped-past-the-end or crashed: showPage() ignores it, so the reader
        // stays where it was.
        comic.gotoPage(kComicPages + 10);
        CHECK(comic.currentPage() == 5);
        comic.gotoPage(-1);
        CHECK(comic.currentPage() == 5);

        // The two readers' bookmarks do not bleed into each other — different files, different keys.
        CHECK(BookmarkStore::list(pdfPath).size() == 1);
        CHECK(BookmarkStore::list(cbzPath).size() == 1);
    }

    // ---- 5. Photo mode reports NO key: a folder of pictures is not a book you keep your place in --------
    // Matches what the photo path already does with per-file resume and reading stats (issue #102). The
    // chrome no-ops on the empty key, so the bookmark controls stay inert there.
    {
        ComicView photos;
        QString err;
        CHECK(photos.openFolder(photoDir, QString(), &err));
        CHECK(err.isEmpty());
        CHECK(photos.pageCount() == 3);                                 // it really did open the folder
        CHECK(photos.itemKey().isEmpty());                              // ...and still reports no bookmark key

        // The same widget goes back to reporting a key when it opens a comic again — the emptiness is a
        // property of photo MODE, not a latch on the instance. MainWindow reuses ONE ComicView for both, so
        // this is the ordinary path, not a contrived one: openComic() has to leave photo mode behind, or the
        // comic is paged, decoded and keyed as the photo folder that came before it.
        CHECK(photos.openComic(cbzPath, &err));
        CHECK(photos.itemKey() == cbzPath);
        CHECK(photos.pageCount() == kComicPages);       // the comic's pages, not the folder's three
    }

    if (failures == 0) { std::puts("READERBM-OK"); return 0; }
    std::fprintf(stderr, "READERBM: %d check(s) failed\n", failures);
    return 1;
}
