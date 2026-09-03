#include "BookCatalogs.h"
#include "LeafRoute.h"    // kLocalBookMime — the leaf kind both surfaces route a book row by

#include <QObject>
#include <QString>

namespace browse
{
namespace {

// "#2", "#2.5" — the series index as somebody would write it. A whole number never shows a decimal point,
// because "#2.0" reads as a defect in the file; a half-book keeps its half, because that is precisely the
// information the decimal was carrying (EpubMeta.h says why the field is a double at all).
QString fmtIndex(double idx)
{
    if (idx <= 0.0) return QString();
    const double whole = double(qint64(idx));
    return whole == idx ? QObject::tr("#%1").arg(qint64(idx))
                        : QObject::tr("#%1").arg(QString::number(idx, 'g', 6));
}

// THE ISSUE AS THE PUBLISHER WROTE IT, when the file said so (ComicInfo.xml's <Number>, issue #152) —
// "Annual 1" and "½" are real issue numbers and there is no double that holds either. Verbatim, because
// re-spelling somebody's number is the whole class of wrongness this feature exists to stop; the derived
// decimal is still what ORDERS the shelf, and is all that is left for a book whose number came from a
// filename.
QString fmtNumber(const BookLibrary::Book& b)
{
    const QString n = b.number.trimmed();
    if (n.isEmpty()) return fmtIndex(b.seriesIndex);
    return n.startsWith(QLatin1Char('#')) ? n : QObject::tr("#%1").arg(n);
}

QString joinDot(const QStringList& parts)
{
    QStringList kept;
    for (const QString& p : parts) if (!p.isEmpty()) kept << p;
    return kept.join(QStringLiteral(" · "));
}

QString coverFor(const BookLibrary::Book& b, const BookCoverFn& fn)
{
    return fn ? fn(b) : QString();   // no default: this unit touches no filesystem (see the header)
}

// How many pages/chapters, in the noun that fits what the file actually is. A COMIC has pages and an EPUB
// has chapters, and calling either by the other's name is the kind of small wrongness that makes a shelf
// feel like it was written by somebody who had not looked at the files. Nothing at all for 0, which is what
// a container that could not say reports — "0 pages" reads as a broken file.
QString fmtExtent(const BookLibrary::Book& b)
{
    if (b.pageCount <= 0) return QString();
    return b.kind == BookLibrary::Kind::Comic ? QObject::tr("%n page(s)", "", b.pageCount)
                                              : QObject::tr("%n chapter(s)", "", b.pageCount);
}

// THE LEAF. A book row is a real file with a url, which is exactly what it looks like: no synthetic type, no
// key-carrying mime, no level underneath it. `mime` names the local-leaf KIND so browse::localLeafRoute
// claims the row ahead of the addon resolve on BOTH layouts — the asymmetry LeafRoute.h exists to close —
// and the route it returns is OpenFile, so the surface hands the item over as it stands and MainWindow's
// existing extension dispatch opens the .epub in the ebook reader, the .pdf in the PDF reader and the .cbz
// in the comic reader. Not one line of that dispatch had to learn what a library is.
MediaItem bookRow(const BookLibrary::Book& b, const QString& credit, const BookCoverFn& cover)
{
    MediaItem it;
    it.id           = b.key;
    it.url          = b.path;
    it.type         = QString::fromLatin1(b.kind == BookLibrary::Kind::Comic ? kComicLeafType
                                                                             : kBookLeafType);
    it.mime         = QString::fromLatin1(kLocalBookMime);
    it.expandable   = false;
    it.title        = b.title;
    it.thumbnailUrl = coverFor(b, cover);
    it.subtitle     = joinDot({ credit, fmtExtent(b), b.year > 0 ? QString::number(b.year) : QString() });
    return it;
}

// A container row with a prefixed key. One builder for every synthetic row in this file, so each one's id
// and mime carry the same string and every reader is bookKeyOf.
MediaItem syntheticRow(const char* type, const char* prefix, const QString& key, const QString& title,
                       const QString& subtitle, const QString& art)
{
    MediaItem it;
    it.id           = QString::fromLatin1(prefix) + key;
    it.type         = QString::fromLatin1(type);
    it.mime         = QString::fromLatin1(prefix) + key;
    it.expandable   = true;
    it.title        = title;
    it.subtitle     = subtitle;
    it.thumbnailUrl = art;
    return it;
}

// The first book in a bucket that has any art, so a bucket row is not a blank card when there is a perfectly
// good picture one level down. Same reason an artist row borrows its first album's cover.
QString bucketArt(const BookLibrary::Author& bucket, const BookCoverFn& cover)
{
    for (const BookLibrary::Book& b : bucket.books)
    {
        const QString art = coverFor(b, cover);
        if (!art.isEmpty()) return art;
    }
    return QString();
}

// One bucket's books, rendered. THE shared level: an author's shelf and a series' shelf are the same rows
// read from two sides, and a builder apiece would drift on the day one of them learned something. `credit`
// decides only what a row names underneath itself.
enum class Credit { Series, Author };

MediaCatalog booksCatalog(const BookLibrary::Author* bucket, const QString& fallbackTitle,
                          Credit credit, const BookCoverFn& cover)
{
    MediaCatalog cat;
    cat.hasMore = false;
    cat.title   = bucket && !bucket->name.trimmed().isEmpty() ? bucket->name.trimmed() : fallbackTitle;
    if (!bucket) return cat;   // a stale route: empty and titled, never a crash
    for (const BookLibrary::Book& b : bucket->books)
    {
        QString line;
        if (credit == Credit::Series)
        {
            // Standing inside an AUTHOR, the fact a row is missing is which series it belongs to and where
            // in it — "Foundation · #2". A standalone book says nothing rather than an empty separator.
            const QString s = b.series.trimmed();
            line = s.isEmpty() ? QString() : joinDot({ s, fmtNumber(b) });
        }
        else
        {
            // Standing inside a SERIES, the shelf is already in order, so the number leads and the author
            // follows it — the one fact a series shelf cannot show any other way.
            line = joinDot({ fmtNumber(b), b.author.trimmed() });
        }
        cat.items.push_back(bookRow(b, line, cover));
    }
    return cat;
}

} // namespace

MediaCatalog bookRootCatalog(const BookLibrary::Index& idx, const BookEmptyNote& note,
                             const BookCoverFn& cover)
{
    MediaCatalog cat; cat.title = QObject::tr("Books");
    cat.hasMore = false;

    if (idx.authors.isEmpty())
    {
        // An empty shelf with no explanation is the failure this parameter exists to prevent: the user has
        // just pointed the app at a folder and wants to know what happened to it.
        if (!note.isEmpty())
        {
            MediaItem info;
            info.type     = QStringLiteral("info");   // the surface's own non-actionable guidance row
            info.id       = QStringLiteral("_bkempty");
            info.title    = note.text;
            info.subtitle = note.detail;
            cat.items.push_back(info);
        }
        return cat;
    }

    // The DIMENSION door, only when the dimension exists. See the header: a collection of standalone books
    // gets a plain list of authors, which is what it had before.
    if (!idx.series.isEmpty())
    {
        int books = 0;
        for (const BookLibrary::Series& s : idx.series) books += int(s.books.size());
        cat.items.push_back(syntheticRow(kBookSeriesListType, kBookSeriesListPrefix, QString(),
                                         QObject::tr("Series"),
                                         joinDot({ QObject::tr("%n series", "", int(idx.series.size())),
                                                   QObject::tr("%n book(s)", "", books) }),
                                         bucketArt(idx.series.first(), cover)));
    }

    for (const BookLibrary::Author& a : idx.authors)
        cat.items.push_back(syntheticRow(kBookAuthorType, kBookAuthorPrefix, a.key,
                                         BookLibrary::displayAuthor(a),
                                         QObject::tr("%n book(s)", "", int(a.books.size())),
                                         bucketArt(a, cover)));
    return cat;
}

MediaCatalog bookAuthorCatalog(const BookLibrary::Index& idx, const QString& authorKey,
                               const BookCoverFn& cover)
{
    const BookLibrary::Author* a = idx.author(authorKey);
    MediaCatalog cat = booksCatalog(a, QObject::tr("Books"), Credit::Series, cover);
    // displayAuthor rather than the raw name, so the UNKNOWN bucket has a title instead of a blank bar.
    if (a) cat.title = BookLibrary::displayAuthor(*a);
    return cat;
}

MediaCatalog bookSeriesCatalog(const BookLibrary::Index& idx, const QString& seriesKey,
                               const BookCoverFn& cover)
{
    return booksCatalog(idx.seriesFor(seriesKey), QObject::tr("Series"), Credit::Author, cover);
}

MediaCatalog bookSeriesListCatalog(const BookLibrary::Index& idx, const BookCoverFn& cover)
{
    MediaCatalog cat; cat.title = QObject::tr("Series");
    cat.hasMore = false;
    for (const BookLibrary::Series& s : idx.series)
        cat.items.push_back(syntheticRow(kBookSeriesType, kBookSeriesPrefix, s.key, s.name.trimmed(),
                                         QObject::tr("%n book(s)", "", int(s.books.size())),
                                         bucketArt(s, cover)));
    return cat;
}

} // namespace browse
