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
// Section 7 is the EBOOK reader's half of #136's selection (issue #451): a real EbookView over a real EPUB,
// driven with real key events, asserting what the PAGE shows after every way out of cursor mode.
// probe_highlights pins the pure ReaderSelection model, and could not see that the view left the caret drawn.
//
// Prints READERBM-OK on success; any failure prints READERBM-FAIL <cond> (line) and exits non-zero.
#include "PdfView.h"
#include "EbookView.h"            // section 7: the ebook reader's cursor mode (issue #451)
#include "HighlightStore.h"
#include "ComicView.h"
#include "BookmarkStore.h"
#include "ReaderAnchor.h"
#include "ProfileStore.h"
#include "OverflowBar.h"         // #136 follow-up: the classic bar that never widens the window
#include "nav/NavOverlay.h"       // ...whose "More…" is a NavMenu

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
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QPushButton>
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

// The smallest complete EPUB: an uncompressed `mimetype` first (the OCF rule), container.xml pointing at the
// OPF, and one XHTML chapter of ordinary words - enough text that a caret has words to walk across (#451).
static bool writeEpub(const QString& path, const QString& title)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);
    QByteArray body;
    for (int i = 1; i <= 12; ++i)
        body += "<p>Paragraph " + QByteArray::number(i)
              + ". The quick brown fox jumps over the lazy dog while the reader walks a caret across it.</p>";
    const QByteArray t = title.toUtf8();
    const QByteArray chapter = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>" + t + "</title></head><body><h1>"
        + t + "</h1>" + body + "</body></html>";
    const QByteArray opf = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"id\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + t + "</dc:title>"
        "<dc:identifier id=\"id\">" + t + "-451</dc:identifier><dc:language>en</dc:language></metadata>"
        "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"c1\"/></spine></package>";
    const QByteArray container = "<?xml version=\"1.0\"?>"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
        "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
        "</rootfiles></container>";
    const QByteArray mime = "application/epub+zip";

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path.toUtf8().constData(), 0)) return false;
    bool ok = mz_zip_writer_add_mem(&zip, "mimetype", mime.constData(), size_t(mime.size()), MZ_NO_COMPRESSION);
    ok = mz_zip_writer_add_mem(&zip, "META-INF/container.xml", container.constData(), size_t(container.size()),
                               MZ_BEST_SPEED) && ok;
    ok = mz_zip_writer_add_mem(&zip, "OEBPS/content.opf", opf.constData(), size_t(opf.size()), MZ_BEST_SPEED) && ok;
    ok = mz_zip_writer_add_mem(&zip, "OEBPS/c1.xhtml", chapter.constData(), size_t(chapter.size()), MZ_BEST_SPEED)
         && ok;
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    return ok;
}

// The anchors are built BY HAND (the fixture — never produced by the reader under test).
static ReaderAnchor pdfAnchor(int page)   { ReaderAnchor a; a.kind = ReaderAnchor::Pdf;   a.page = page; return a; }
static ReaderAnchor comicAnchor(int page) { ReaderAnchor a; a.kind = ReaderAnchor::Comic; a.page = page; return a; }

int main(int argc, char** argv)
{
    // A PLATFORM OF OUR OWN, BEFORE QApplication EXISTS. This probe asserts over REAL widgets (a QPdfView
    // and a ComicView), so it needs a QApplication — and a QApplication that cannot open a display calls
    // qFatal INSIDE THIS CONSTRUCTOR and aborts (SIGABRT, rc 134). The suite's runner loop launches every
    // probe bare, with no -platform argument, so the platform is whatever the environment happens to give:
    // on Windows the "windows" plugin loads headlessly and this probe was green from the day it landed,
    // while on CI's DISPLAY-less Linux runner xcb failed and the process died six lines into Qt's plugin
    // diagnostics — before ProfileStore below, before the first CHECK, before anything about bookmarks was
    // asserted at all. main went red for a day on a probe that had never once run a line of the feature it
    // names. probe_books.cpp carries this guard and the same scar; it is a property of the RUNNER, not of
    // any one probe, so every probe that builds widgets needs it. Set only when unset, so an explicit
    // -platform or a deliberate override still wins (the probe_shaderassets rule).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
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

    // ---- 6. The classic bar NEVER forces the window wider (issue #136 follow-up) ---------------------------
    // A QHBoxLayout's minimum is the SUM of its children's, a QPushButton's minimum is its whole caption and a
    // QLabel's minimum is its whole text - and a top-level window whose layout minimum exceeds its width is
    // RESIZED to that minimum when shown. So a bar with fourteen buttons and a long "<file>  —  Pages 1–2 / 6"
    // label made opening a comic grow a 1280 window. Asserted as RELATIONSHIPS - the window keeps the width
    // it was given, the reader asks for no more than that, no button on the bar is squeezed below its own
    // caption - never as pixel counts taken off one machine's font (the runner's font is not this one).
    {
        const QString longCbz = tmp.filePath(
            QStringLiteral("Harbour Lights - The Complete Lighthouse Keeper's Edition, Volume 001.cbz"));
        const QString longPdf = tmp.filePath(
            QStringLiteral("The Lighthouse Keeper's Ledger - A Complete Illustrated Maintenance Manual.pdf"));
        CHECK(writeCbz(longCbz, kComicPages));
        CHECK(writeMultiPagePdf(longPdf, kPdfPages));

        auto settle = [] { for (int i = 0; i < 6; ++i) QCoreApplication::processEvents(); };
        // Every VISIBLE button inside `reader` (its bar) is at least as wide as its own minimum: the bar fits by
        // moving controls out of the way, never by crushing them into unreadable slivers.
        auto noneSqueezed = [](QWidget* reader) {
            for (QPushButton* b : reader->findChildren<QPushButton*>())
                if (b->isVisible() && b->width() < b->minimumSizeHint().width()) return false;
            return true;
        };

        for (const int winW : { 1280, 1024 })
        {
            {
                QWidget window;
                auto* col = new QVBoxLayout(&window);
                col->setContentsMargins(0, 0, 0, 0);
                auto* comic = new ComicView(&window);
                col->addWidget(comic);
                window.resize(winW, 720);
                window.show();
                settle();
                CHECK(comic->openComic(longCbz));
                settle();
                CHECK(window.width() == winW);                              // opening it did not grow the window
                CHECK(comic->minimumSizeHint().width() <= winW);            // ...and never will at this width
                CHECK(noneSqueezed(comic));
                if (window.width() != winW)
                    std::fprintf(stderr, "  comic @%d: window became %d, reader asks for %d\n", winW,
                                 window.width(), comic->minimumSizeHint().width());

                // Nothing is LOST to fit: every control the reader wants is on the bar or in "More…", and "More…"
                // is on the bar exactly when something is in it.
                OverflowBar* ob = comic->findChild<OverflowBar*>();
                CHECK(ob != nullptr);
                if (!ob) continue;
                QPushButton* exportBtn = nullptr;
                for (QPushButton* b : ob->findChildren<QPushButton*>())
                {
                    if (b == ob->more()) continue;
                    if (b->text() == QStringLiteral("Export notes")) exportBtn = b;
                    if (ob->wanted(b)) CHECK(b->isVisible() || ob->overflowed().contains(b));
                }
                CHECK(exportBtn != nullptr);
                CHECK(ob->more()->isVisible() == !ob->overflowed().isEmpty());
                // Lowest priority goes first: if anything had to move, Export notes did.
                if (!ob->overflowed().isEmpty()) CHECK(ob->overflowed().contains(exportBtn));

                // The page label draws what fits and keeps the whole text one hover away.
                ElidedLabel* label = ob->findChild<ElidedLabel*>();
                CHECK(label != nullptr);
                if (label)
                {
                    CHECK(label->fullText().contains(QStringLiteral("Harbour Lights")));
                    CHECK(label->toolTip() == label->fullText());
                    CHECK(label->fontMetrics().horizontalAdvance(label->text()) <= label->contentsRect().width());
                    // ...and its MINIMUM is an ellipsis, not its text: a file name as long as the screen cannot
                    // widen the bar through the label either.
                    CHECK(label->minimumSizeHint().width() <= label->fontMetrics().averageCharWidth() * 4);
                }

                // An owner re-showing its own controls (a reading-mode change relabels and re-shows all seven)
                // does not overflow the bar again: the bar tracks the wish apart from its own fit decision.
                comic->comicActivateControl(0);
                settle();
                CHECK(window.width() == winW);
                CHECK(noneSqueezed(comic));
                CHECK(ob->more()->isVisible() == !ob->overflowed().isEmpty());

                // "More…" is a nav-kit menu listing what moved, in bar order, and choosing a row CLICKS that
                // control - one action, two ways in. Driven with a real Return key on the menu.
                QPushButton* firstOver = nullptr;
                for (QPushButton* b : ob->overflowed()) if (b->isEnabled()) { firstOver = b; break; }
                if (winW == 1024) CHECK(firstOver != nullptr);   // a 1024 bar cannot hold all fourteen
                if (firstOver)
                {
                    int clicks = 0;
                    QObject::connect(firstOver, &QPushButton::clicked, [&clicks] { ++clicks; });
                    ob->openMore();
                    settle();
                    NavMenu* menu = window.findChild<NavMenu*>();
                    CHECK(menu != nullptr);
                    if (menu)
                    {
                        CHECK(menu->describe().endsWith(firstOver->text()));
                        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                        QCoreApplication::sendEvent(menu, &enter);
                        settle();
                    }
                    CHECK(clicks == 1);
                }

                // And with room to spare, everything is back on the bar and "More…" is gone.
                window.resize(4000, 720);
                settle();
                CHECK(ob->overflowed().isEmpty());
                CHECK(!ob->more()->isVisible());
            }
            {
                QWidget window;
                auto* col = new QVBoxLayout(&window);
                col->setContentsMargins(0, 0, 0, 0);
                auto* pdf = new PdfView(&window);
                col->addWidget(pdf);
                window.resize(winW, 720);
                window.show();
                settle();
                CHECK(pdf->openPdf(longPdf));
                settle();
                CHECK(window.width() == winW);
                CHECK(pdf->minimumSizeHint().width() <= winW);
                CHECK(noneSqueezed(pdf));
                if (window.width() != winW)
                    std::fprintf(stderr, "  pdf @%d: window became %d, reader asks for %d\n", winW,
                                 window.width(), pdf->minimumSizeHint().width());
            }
        }
    }

    // ---- 7. Leaving cursor mode takes the caret DOWN with it (issue #451) -------------------------------
    // THE BUG IT PINS. With no selection in flight, Escape/Back/Backspace made ReaderSelection's key() call
    // leave() ITSELF and then report Exited; EbookView answered Exited with endCursorMode(), whose first line
    // is `if (!cursor_.active) return;` - so it returned at once. The mode was over (cursorMode() false, the
    // arrows turned pages again) while the page kept drawing the caret and the themed chrome, never told,
    // went on reading "Selecting". probe_highlights asserted !active after Exited, which was TRUE, and so was
    // green over it. The fix gives leaving ONE owner: the model reports Exited and endCursorMode() does all of
    // it - clears the caret, clears the selection band, emits pageInfoChanged() (the signal the themed bridge
    // re-reads cursorMode off). Every exit below is held to that same state, and each one is a DIFFERENT
    // caller of the one path: the key map (three keys, and the second Escape after a selection), a highlight
    // being stored, a highlight being removed, and another book being opened.
    {
        auto settle = [] {
            for (int i = 0; i < 6; ++i)
            {
                QCoreApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);   // dismissed menus go
            }
        };
        const QString epubPath  = tmp.filePath(QStringLiteral("Caret.epub"));
        const QString otherPath = tmp.filePath(QStringLiteral("Other.epub"));
        CHECK(writeEpub(epubPath, QStringLiteral("Caret")));
        CHECK(writeEpub(otherPath, QStringLiteral("Other")));

        QWidget window;
        auto* col = new QVBoxLayout(&window);
        col->setContentsMargins(0, 0, 0, 0);
        auto* view = new EbookView(&window);
        col->addWidget(view);
        window.resize(1024, 720);
        window.show();
        settle();
        QString err;
        CHECK(view->openBook(epubPath, &err));
        CHECK(err.isEmpty());
        settle();
        BookPageWidget* page = view->findChild<BookPageWidget*>();
        CHECK(page != nullptr);
        if (page)
        {
            int infos = 0;
            QObject::connect(view, &EbookView::pageInfoChanged, [&infos] { ++infos; });
            // A real key event to a real receiver: the reader, or the menu the reader opened.
            auto keyTo = [&](QObject* to, int key) {
                QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::sendEvent(to, &ev);
                settle();
            };
            auto press = [&](int key) { keyTo(view, key); };
            // The one live menu the reader opened (a dismissed one is hidden before it is deleted).
            auto liveMenu = [&window]() -> NavMenu* {
                for (NavMenu* m : window.findChildren<NavMenu*>()) if (m->isVisible()) return m;
                return nullptr;
            };
            // Enter the mode and walk the caret two words, so there IS a caret on the page to take down.
            auto enterAndWalk = [&] {
                view->beginCursorMode();
                press(Qt::Key_Right);
                press(Qt::Key_Right);
                CHECK(view->cursorMode());
                CHECK(page->caretPos() > 0);
            };
            // THE CLEAN EXIT: the same facts for every path out.
            auto leftCleanly = [&](const char* how, int infosBefore) {
                const bool ok = !view->cursorMode() && page->caretPos() == -1 && page->selectionStart() == -1
                             && page->selectionEnd() == -1 && infos > infosBefore;
                if (!ok)
                    std::fprintf(stderr, "  exit via %s: cursorMode=%d caret=%d selection=[%d,%d] "
                                 "pageInfoChanged emitted %d time(s)\n", how, int(view->cursorMode()),
                                 page->caretPos(), page->selectionStart(), page->selectionEnd(),
                                 infos - infosBefore);
                CHECK(!view->cursorMode());
                CHECK(page->caretPos() == -1);                  // the caret is no longer drawn
                CHECK(page->selectionStart() == -1);            // ...nor any selection band
                CHECK(page->selectionEnd() == -1);
                CHECK(infos > infosBefore);                     // ...and the chrome was told to re-read it
            };

            // 7a. The issue's steps: Select, move, Escape - and Back and Backspace, the same key-map row.
            for (int key : { int(Qt::Key_Escape), int(Qt::Key_Back), int(Qt::Key_Backspace) })
            {
                enterAndWalk();
                const int before = infos;
                press(key);
                leftCleanly(key == Qt::Key_Escape ? "Escape" : key == Qt::Key_Back ? "Back" : "Backspace", before);
            }

            // 7b. With a selection in flight the FIRST Escape only drops it (mode and caret stay - you cancel a
            // selection far more often than you mean to leave), and the SECOND leaves, just as cleanly.
            enterAndWalk();
            press(Qt::Key_Return);                               // select-from-here
            press(Qt::Key_Right);
            press(Qt::Key_Right);
            CHECK(page->selectionStart() >= 0);                  // the band is really drawn before we cancel it
            CHECK(page->selectionEnd() > page->selectionStart());
            const int caretBeforeCancel = page->caretPos();
            press(Qt::Key_Escape);
            CHECK(view->cursorMode());                           // unchanged path: still in the mode...
            CHECK(page->caretPos() == caretBeforeCancel);        // ...with the caret where it was...
            CHECK(page->selectionStart() == -1);                 // ...and the selection gone
            {
                const int before = infos;
                press(Qt::Key_Escape);
                leftCleanly("second Escape after a selection", before);
            }

            // 7c. Storing a highlight ends the mode: commit a selection, choose "Highlight...", choose a colour.
            const QString key = view->itemKey();
            CHECK(!key.isEmpty());
            enterAndWalk();
            press(Qt::Key_Return);
            press(Qt::Key_Right);
            press(Qt::Key_Right);
            press(Qt::Key_Return);                               // commit: the action menu opens
            NavMenu* actions = liveMenu();
            CHECK(actions != nullptr);
            if (actions)
            {
                CHECK(actions->describe().contains(QStringLiteral("Highlight")));   // row 0
                keyTo(actions, Qt::Key_Return);                  // "Highlight..." opens the colour menu
                NavMenu* colours = liveMenu();
                CHECK(colours != nullptr);
                CHECK(colours != actions);
                if (colours && colours != actions)
                {
                    const int before = infos;
                    keyTo(colours, Qt::Key_Return);              // the first colour
                    leftCleanly("a stored highlight", before);
                }
            }
            const QVector<HighlightStore::Highlight> stored = HighlightStore::list(key);
            CHECK(stored.size() == 1);

            // 7d. Removing a highlight ends the mode too: land the caret inside it (the panel's jump), Enter on
            // it offers the edit menu, and "Remove highlight" is its last row.
            if (stored.size() == 1)
            {
                view->gotoHighlight(stored.at(0).anchor.spine, stored.at(0).anchor.offset);
                settle();
                CHECK(view->cursorMode());
                CHECK(page->caretPos() == stored.at(0).anchor.offset);
                press(Qt::Key_Return);
                NavMenu* edit = liveMenu();
                CHECK(edit != nullptr);
                if (edit)
                {
                    keyTo(edit, Qt::Key_Down);
                    keyTo(edit, Qt::Key_Down);
                    CHECK(edit->describe().contains(QStringLiteral("Remove highlight")));
                    const int before = infos;
                    keyTo(edit, Qt::Key_Return);
                    leftCleanly("a removed highlight", before);
                }
                CHECK(HighlightStore::list(key).isEmpty());
            }

            // 7e. Opening another book ends the mode: a caret belongs to the book it was placed in.
            enterAndWalk();
            {
                const int before = infos;
                CHECK(view->openBook(otherPath, &err));
                settle();
                leftCleanly("opening another book", before);
            }

            // 7f. And the mode comes back after all of that, and leaves cleanly again - leaving is not a latch.
            enterAndWalk();
            {
                const int before = infos;
                press(Qt::Key_Escape);
                leftCleanly("Escape after re-entering", before);
            }
        }
    }

    if (failures == 0) { std::puts("READERBM-OK"); return 0; }
    std::fprintf(stderr, "READERBM: %d check(s) failed\n", failures);
    return 1;
}
