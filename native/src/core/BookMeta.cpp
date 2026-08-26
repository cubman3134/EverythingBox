#include "BookMeta.h"
#include "../comic/ComicPageOrder.h"   // the reader's own page order + image-member rule (#205 / #134)
#include "../ebook/EpubMeta.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
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
        const QImage img = doc.render(0, QSize(w, qMax(1, h)));
        if (img.isNull()) return QByteArray();
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
    const QString e = ext(path);
    if (e == QStringLiteral("epub")) return readEpub(path);
    if (e == QStringLiteral("pdf"))  return readPdf(path);
    if (e == QStringLiteral("cbz"))  return readComic(path);
    return Info();
}

QByteArray coverBytes(const QString& path)
{
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
    return QByteArray();
}

} // namespace BookMeta
