#include "BookMeta.h"
#include "../comic/ComicPageOrder.h"   // the reader's own page order + image-member rule (#205 / #134)
#include "../comic/RarComic.h"         // .cbr — the same unarr reader ComicView opens one with (#144)
#include "../ebook/EpubMeta.h"
#include "../ebook/Fb2Meta.h"          // .fb2 / .fb2.zip — the same <description> walk Fb2Book reads (#144)
#include "../ebook/MarkdownHtml.h"     // .md — top-level headings, without rendering the document (#144)
#include "../ebook/MobiHeader.h"       // .mobi / .azw / .azw3 — the container walk MobiBook reads (#144)
#include "../ebook/TextBook.h"         // .txt / .md — the encoding ladder, shared with the reader (#144)

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QVector>
#include <algorithm>
#include <cstring>

#if !defined(Q_OS_ANDROID)
#include <QPdfDocument>
#endif

#include "miniz.h"

namespace BookMeta
{
namespace
{
    QString ext(const QString& path) { return QFileInfo(path).suffix().toLower(); }

    // ---- CBZ ------------------------------------------------------------------------------------------
    // The page members of a comic archive, in the reader's order. Both callers below want exactly this, and
    // both want the ORDER to be the reader's rather than the zip's: a shelf that shows page one and a reader
    // that opens on a different page one would be the same bug NaturalOrder.h was written about, arriving
    // through a second door.
    QVector<QPair<QString, mz_uint>> comicPages(mz_zip_archive* zip)
    {
        QVector<QPair<QString, mz_uint>> imgs;
        const mz_uint count = mz_zip_reader_get_num_files(zip);
        for (mz_uint i = 0; i < count; ++i)
        {
            if (mz_zip_reader_is_file_a_directory(zip, i)) continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(zip, i, &st)) continue;
            const QString name = QString::fromUtf8(st.m_filename);
            if (ComicPages::isImageName(name)) imgs.append({ name, i });
        }
        const QCollator coll = ComicPages::collator();
        std::sort(imgs.begin(), imgs.end(),
                  [&coll](const QPair<QString, mz_uint>& a, const QPair<QString, mz_uint>& b) {
                      return ComicPages::lessThan(coll, a.first, b.first);
                  });
        return imgs;
    }

    Info readComic(const QString& path)
    {
        Info i;
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_file(&zip, path.toUtf8().constData(), 0)) return i;
        const QVector<QPair<QString, mz_uint>> pages = comicPages(&zip);
        mz_zip_reader_end(&zip);
        i.pageCount = int(pages.size());
        i.hasCover  = !pages.isEmpty();
        // No title, no author, no series: a CBZ carries none of them, and the FILENAME is the only source —
        // which is ComicName's judgement to make, folder by folder, and not this file's to guess at.
        return i;
    }

    QByteArray comicCover(const QString& path)
    {
        QByteArray out;
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_file(&zip, path.toUtf8().constData(), 0)) return out;
        const QVector<QPair<QString, mz_uint>> pages = comicPages(&zip);
        if (!pages.isEmpty())
        {
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap(&zip, pages.first().second, &sz, 0);
            if (p) { out = QByteArray(static_cast<const char*>(p), int(sz)); mz_free(p); }
        }
        mz_zip_reader_end(&zip);
        return out;
    }

    // ---- CBR ------------------------------------------------------------------------------------------
    // The RAR twin of readComic() above, and the same two facts: how many pages, and whether page one can be
    // got at. It costs a HEADER WALK and no decompression (RarComic.h says why that is what lets a .cbr be
    // scanned when a .cb7 and a .cbt still cannot be).
    Info readCbr(const QString& path)
    {
        Info i;
        RarComic::Status st = RarComic::Status::Ok;
        const QStringList pages = RarComic::imageNames(path, &st);
        if (st != RarComic::Status::Ok) return i;   // RAR5, damaged, encrypted: the shelf shows the filename
        i.pageCount = int(pages.size());
        i.hasCover  = !pages.isEmpty();
        return i;   // no title/author/series: a comic archive carries none — see readComic()
    }

    // ---- FB2 ------------------------------------------------------------------------------------------
    Info readFb2(const QString& path)
    {
        Info i;
        Fb2Meta::Metadata m;
        if (!Fb2Meta::readFile(path, &m)) return i;
        i.title       = m.title;
        i.author      = m.author;
        i.series      = m.series;
        i.seriesIndex = m.seriesIndex;
        i.language    = m.language;
        i.year        = m.year;
        i.pageCount   = m.sectionCount;    // top-level <section>s == the chapters the reader will show
        i.hasCover    = !m.coverId.isEmpty();
        return i;
    }

    // ---- MOBI / AZW / AZW3 -----------------------------------------------------------------------------
    // WHY THESE ARE SCANNED NOW AND WERE NOT BEFORE. BookLibrary.h refused .mobi because reading its title
    // meant decompressing every text record of the book — a whole-file inflate per tile. That was true of the
    // code as it stood and is not true of MobiHeader, which answers title/author/cover from the headers and
    // the EXTH block and decompresses NOTHING. The cost the refusal was about is gone, so the refusal is too.
    //
    // A DRM'd file reads as an empty Info rather than as a failure: it is still a book on the shelf, under
    // its own file name, and it says what it is when you open it (MobiHeader.h on why that must be loud).
    Info readMobi(const QString& path)
    {
        Info i;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return i;
        const QByteArray data = f.readAll();
        f.close();

        MobiHeader::Info info;
        if (MobiHeader::read(data, &info) != MobiHeader::Result::Ok) return i;
        i.title    = info.title.trimmed();
        i.author   = info.author.trimmed();
        i.hasCover = info.hasCover;
        // No page count: a MOBI is ONE stream of HTML with no chapter division the reader honours, so any
        // number here would be invented. 0 == "the container did not say", which is the truth.
        return i;
    }

    // ---- Plain text / Markdown -------------------------------------------------------------------------
    // A .txt says NOTHING about itself: no title, no author, no cover, no chapter count. It scans to an empty
    // Info and appears under its own filename, exactly as an untagged EPUB does. A .md is the one difference:
    // an author's own top-level headings ARE its chapters, and the first of them is as close to a stated
    // title as the format has.
    Info readTextBook(const QString& path)
    {
        Info i;
        if (!TextBook::isMarkdownPath(path)) return i;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return i;
        const QByteArray bytes = f.readAll();
        f.close();
        const QStringList heads = MarkdownHtml::topLevelHeadings(TextBook::decode(bytes));
        if (heads.isEmpty()) return i;
        i.title     = heads.first().trimmed();
        i.pageCount = int(heads.size());
        return i;
    }

    // ---- EPUB -----------------------------------------------------------------------------------------
    Info readEpub(const QString& path)
    {
        Info i;
        EpubMeta::Metadata m;
        if (!EpubMeta::readEpubFile(path, &m)) return i;
        i.title       = m.title;
        i.author      = m.author;
        i.series      = m.series;
        i.seriesIndex = m.seriesIndex;
        i.language    = m.language;
        i.year        = m.year;
        i.pageCount   = m.spineCount;
        i.hasCover    = !m.coverHref.isEmpty();
        return i;
    }

    // ---- PDF ------------------------------------------------------------------------------------------
#if !defined(Q_OS_ANDROID)
    Info readPdf(const QString& path)
    {
        Info i;
        QPdfDocument doc;
        if (doc.load(path) != QPdfDocument::Error::None) return i;
        i.pageCount = qMax(0, doc.pageCount());
        i.hasCover  = i.pageCount > 0;
        i.title     = doc.metaData(QPdfDocument::MetaDataField::Title).toString().trimmed();
        i.author    = doc.metaData(QPdfDocument::MetaDataField::Author).toString().trimmed();
        // No series: the PDF information dictionary has no such field, and the two places people put one —
        // the Subject and Keywords strings — hold free text that would have to be pattern-matched out. That
        // is the normaliser this feature refuses to write (ComicName.h says why at length).
        return i;
    }

    QByteArray pdfCover(const QString& path)
    {
        QPdfDocument doc;
        if (doc.load(path) != QPdfDocument::Error::None) return QByteArray();
        if (doc.pageCount() <= 0) return QByteArray();
        // Keep the page's own aspect. A PDF whose page size PDFium will not report degrades to a plain
        // portrait guess rather than to a squashed cover.
        const QSizeF pt = doc.pagePointSize(0);
        const int w = kPdfCoverWidth;
        const int h = (pt.width() > 0.0 && pt.height() > 0.0)
                          ? int(double(w) * pt.height() / pt.width())
                          : int(double(w) * 11.0 / 8.5);
        const QImage page = doc.render(0, QSize(w, qMax(1, h)));
        if (page.isNull()) return QByteArray();

        // COMPOSITE ONTO WHITE, and this is not a nicety. PDFium paints only what the page DRAWS: a text
        // page arrives as black glyphs on a fully TRANSPARENT background, because a PDF page has no paint of
        // its own where nothing was put. The cache writer then re-encodes it as JPEG, which has no alpha
        // channel, and every transparent pixel becomes BLACK - so a shelf of PDFs came out as a wall of
        // black rectangles that decoded perfectly and told the user nothing. (Found live on the #134 drive;
        // the probe had asserted only that the bytes decoded, which they did.) A page IS white, so say so.
        QImage img(page.size(), QImage::Format_RGB32);
        img.fill(Qt::white);
        {
            QPainter p(&img);
            p.drawImage(0, 0, page);
        }
        QByteArray out;
        QBuffer buf(&out);
        buf.open(QIODevice::WriteOnly);
        // PNG, and re-encoded to JPEG a moment later by the cache writer. One extra encode ONCE per book in
        // its whole life, in exchange for one representation crossing this boundary instead of two.
        if (!img.save(&buf, "PNG")) return QByteArray();
        return out;
    }
#else
    // Android ships no QtPdf in the open-source packages (see native/CMakeLists.txt), so a PDF there is a
    // file with a name and nothing else — which is still a book on the shelf, opening through whatever the
    // platform reader is. It is NOT dropped from the scan.
    Info       readPdf(const QString&) { return Info(); }
    QByteArray pdfCover(const QString&) { return QByteArray(); }
#endif
}

Info read(const QString& path)
{
    // FB2 IS ASKED FIRST, and by whole-name rather than by suffix: the zipped wire form is called
    // "book.fb2.zip", whose QFileInfo::suffix() is "zip" — which this dispatch does not claim and must not
    // start claiming, because "a zip in a books folder is a comic" is the guess BookLibrary.h refuses.
    if (Fb2Meta::isFb2Path(path)) return readFb2(path);
    const QString e = ext(path);
    if (e == QStringLiteral("epub")) return readEpub(path);
    if (e == QStringLiteral("pdf"))  return readPdf(path);
    if (e == QStringLiteral("cbz"))  return readComic(path);
    if (e == QStringLiteral("cbr"))  return readCbr(path);
    if (e == QStringLiteral("azw3") || e == QStringLiteral("azw") || e == QStringLiteral("mobi"))
        return readMobi(path);
    if (TextBook::isTextBookPath(path)) return readTextBook(path);
    return Info();
}

QByteArray coverBytes(const QString& path)
{
    if (Fb2Meta::isFb2Path(path))
    {
        Fb2Meta::Metadata m;
        QByteArray cover;
        if (!Fb2Meta::readFile(path, &m, &cover)) return QByteArray();
        return cover;   // the <binary> the <coverpage> declares — never one that merely looks like a cover
    }
    const QString e = ext(path);
    if (e == QStringLiteral("epub"))
    {
        EpubMeta::Metadata m;
        QByteArray cover;
        if (!EpubMeta::readEpubFile(path, &m, &cover)) return QByteArray();
        return cover;
    }
    if (e == QStringLiteral("pdf")) return pdfCover(path);
    if (e == QStringLiteral("cbz")) return comicCover(path);
    if (e == QStringLiteral("cbr")) return RarComic::coverBytes(path);
    if (e == QStringLiteral("azw3") || e == QStringLiteral("azw") || e == QStringLiteral("mobi"))
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return MobiHeader::coverBytes(f.readAll());
    }
    // .txt / .md have no cover to carry. hasCover stayed false for them, so this is never reached for one —
    // and if it were, an empty answer is the same "no picture" a coverless EPUB gives.
    return QByteArray();
}

} // namespace BookMeta
