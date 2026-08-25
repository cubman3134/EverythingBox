#include "AudiobookCatalogs.h"

#include <QObject>
#include <QString>

namespace browse
{
namespace {

// "9h 14m" / "42m". Books are HOURS long, so the music library's "m:ss" would read as a stopwatch; nothing
// at all for a duration of 0, which is what a container that cannot give one cheaply reports — printing
// "0m" beside a real book reads as a broken file rather than as a missing number.
QString fmtBookDuration(int secs)
{
    if (secs <= 0) return QString();
    const int h = secs / 3600, m = (secs % 3600) / 60;
    if (h > 0) return m > 0 ? QObject::tr("%1h %2m").arg(h).arg(m) : QObject::tr("%1h").arg(h);
    return QObject::tr("%1m").arg(qMax(1, m));   // a 40-second file is "1m", never "0m"
}

// "m:ss" / "h:mm:ss" — for one PART, where the exact length is the useful fact and the parts are minutes
// rather than hours.
QString fmtPartDuration(int secs)
{
    if (secs <= 0) return QString();
    const int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString joinDot(const QStringList& parts)
{
    QStringList kept;
    for (const QString& p : parts) if (!p.isEmpty()) kept << p;
    return kept.join(QStringLiteral(" · "));
}

QString coverFor(const AudiobookLibrary::Book& b, const AudiobookCoverFn& fn)
{
    return fn ? fn(b) : QString();   // no default: this unit touches no filesystem (see the header)
}

// What a book row says under its title. `credit` is the ONE fact that differs by where the row is being
// rendered — an author's shelf names the narrator, a narrator's shelf names the author — and it is passed
// in rather than chosen here so there is one builder and no way for the two to disagree about the rest.
QString bookSubtitle(const AudiobookLibrary::Book& b, const QString& credit)
{
    return joinDot({ credit,
                     // Only when the files actually carried a chapter list. A book with none says nothing
                     // rather than "0 chapters", which would read as a defect in the file.
                     b.chapterCount > 0 ? QObject::tr("%n chapter(s)", "", b.chapterCount) : QString(),
                     // ...and the part count only when there is more than one, because "1 part" is noise on
                     // every single-file m4b in the collection.
                     b.files.size() > 1 ? QObject::tr("%n part(s)", "", int(b.files.size())) : QString(),
                     fmtBookDuration(b.durationSec) });
}

MediaItem bookRow(const AudiobookLibrary::Book& b, const QString& credit, const AudiobookCoverFn& cover)
{
    MediaItem it;
    it.id           = QString::fromLatin1(kAudiobookBookPrefix) + b.key;
    it.type         = QString::fromLatin1(kAudiobookBookType);
    it.mime         = QString::fromLatin1(kAudiobookBookPrefix) + b.key;   // -> audiobookBookCatalog
    it.expandable   = true;
    it.title        = AudiobookLibrary::displayBook(b);
    it.thumbnailUrl = coverFor(b, cover);
    it.subtitle     = bookSubtitle(b, credit);
    return it;
}

// A container / action row with a prefixed key. One builder for every synthetic row in this file, so each
// one's id and mime carry the same string and every reader is audiobookKeyOf.
MediaItem syntheticRow(const char* type, const char* prefix, const QString& key, bool expandable,
                       const QString& title, const QString& subtitle, const QString& art)
{
    MediaItem it;
    it.id           = QString::fromLatin1(prefix) + key;
    it.type         = QString::fromLatin1(type);
    it.mime         = QString::fromLatin1(prefix) + key;
    it.expandable   = expandable;
    it.title        = title;
    it.subtitle     = subtitle;
    it.thumbnailUrl = art;
    return it;
}

// The first book in a bucket that has any art, so a bucket row is not a blank card when there is a perfectly
// good picture one level down. Same reason an artist row borrows its first album's cover.
QString bucketArt(const AudiobookLibrary::Author& bucket, const AudiobookCoverFn& cover)
{
    for (const AudiobookLibrary::Book& b : bucket.books)
    {
        const QString art = coverFor(b, cover);
        if (!art.isEmpty()) return art;
    }
    return QString();
}

// One bucket's books, rendered. THE shared level: an author's shelf, a narrator's shelf and a series' shelf
// are the same rows read from three sides, and a builder apiece would drift on the day one of them learned
// something. `credit` decides only what a row names underneath itself.
enum class Credit { Narrator, Author };

MediaCatalog booksCatalog(const AudiobookLibrary::Author* bucket, const QString& fallbackTitle,
                          Credit credit, const AudiobookCoverFn& cover)
{
    MediaCatalog cat;
    cat.hasMore = false;
    cat.title   = bucket && !bucket->name.trimmed().isEmpty() ? bucket->name.trimmed() : fallbackTitle;
    if (!bucket) return cat;   // a stale route: empty and titled, never a crash
    for (const AudiobookLibrary::Book& b : bucket->books)
    {
        QString line;
        if (credit == Credit::Narrator)
            line = b.narrator.trimmed().isEmpty() ? QString()
                                                  : QObject::tr("Read by %1").arg(b.narrator.trimmed());
        else
            line = b.author.trimmed().isEmpty() ? QString() : b.author.trimmed();
        // Inside a SERIES the book's place in it leads the line, because that is what somebody standing in a
        // series wants to know first and the shelf is already ordered by it.
        if (b.seriesIndex > 0 && credit == Credit::Author && !b.series.trimmed().isEmpty())
            line = joinDot({ QObject::tr("#%1").arg(b.seriesIndex), line });
        cat.items.push_back(bookRow(b, line, cover));
    }
    return cat;
}

// One dimension's buckets — Narrators or Series — as rows. Same shape for both, for the same reason the
// level above is shared.
MediaCatalog bucketsCatalog(const QVector<AudiobookLibrary::Author>& buckets, const QString& title,
                            const char* type, const char* prefix, const AudiobookCoverFn& cover)
{
    MediaCatalog cat; cat.title = title;
    cat.hasMore = false;
    for (const AudiobookLibrary::Author& bucket : buckets)
        cat.items.push_back(syntheticRow(type, prefix, bucket.key, /*expandable*/ true,
                                         bucket.name.trimmed(),
                                         joinDot({ QObject::tr("%n book(s)", "", int(bucket.books.size())),
                                                   fmtBookDuration(bucket.durationSec) }),
                                         bucketArt(bucket, cover)));
    return cat;
}

} // namespace

MediaCatalog audiobookRootCatalog(const AudiobookLibrary::Index& idx, const AudiobookEmptyNote& note,
                                  const AudiobookCoverFn& cover)
{
    MediaCatalog cat; cat.title = QObject::tr("Audiobooks");
    cat.hasMore = false;

    if (idx.authors.isEmpty())
    {
        // An empty shelf with no explanation is the failure this parameter exists to prevent: the user has
        // just pointed the app at a folder and wants to know what happened to it.
        if (!note.isEmpty())
        {
            MediaItem info;
            info.type     = QStringLiteral("info");   // the surface's own non-actionable guidance row
            info.id       = QStringLiteral("_abempty");
            info.title    = note.text;
            info.subtitle = note.detail;
            cat.items.push_back(info);
        }
        return cat;
    }

    // The two DIMENSION doors, each only when its dimension exists. See the header: a collection with no
    // narrator and no series tag anywhere gets a plain list of authors, which is what it had before.
    if (!idx.narrators.isEmpty())
    {
        int books = 0;
        for (const AudiobookLibrary::Narrator& n : idx.narrators) books += int(n.books.size());
        cat.items.push_back(syntheticRow(kAudiobookNarratorsType, kAudiobookNarratorsPrefix, QString(),
                                         /*expandable*/ true, QObject::tr("Narrators"),
                                         joinDot({ QObject::tr("%n narrator(s)", "", int(idx.narrators.size())),
                                                   QObject::tr("%n book(s)", "", books) }),
                                         bucketArt(idx.narrators.first(), cover)));
    }
    if (!idx.series.isEmpty())
    {
        int books = 0;
        for (const AudiobookLibrary::Series& s : idx.series) books += int(s.books.size());
        cat.items.push_back(syntheticRow(kAudiobookSeriesListType, kAudiobookSeriesListPrefix, QString(),
                                         /*expandable*/ true, QObject::tr("Series"),
                                         joinDot({ QObject::tr("%n series", "", int(idx.series.size())),
                                                   QObject::tr("%n book(s)", "", books) }),
                                         bucketArt(idx.series.first(), cover)));
    }

    for (const AudiobookLibrary::Author& a : idx.authors)
        cat.items.push_back(syntheticRow(kAudiobookAuthorType, kAudiobookAuthorPrefix, a.key,
                                         /*expandable*/ true, AudiobookLibrary::displayAuthor(a),
                                         joinDot({ QObject::tr("%n book(s)", "", int(a.books.size())),
                                                   fmtBookDuration(a.durationSec) }),
                                         bucketArt(a, cover)));
    return cat;
}

MediaCatalog audiobookAuthorCatalog(const AudiobookLibrary::Index& idx, const QString& authorKey,
                                    const AudiobookCoverFn& cover)
{
    const AudiobookLibrary::Author* a = idx.author(authorKey);
    MediaCatalog cat = booksCatalog(a, QObject::tr("Audiobooks"), Credit::Narrator, cover);
    // displayAuthor rather than the raw name, so the UNKNOWN bucket has a title instead of a blank bar.
    if (a) cat.title = AudiobookLibrary::displayAuthor(*a);
    return cat;
}

MediaCatalog audiobookNarratorCatalog(const AudiobookLibrary::Index& idx, const QString& narratorKey,
                                      const AudiobookCoverFn& cover)
{
    // Credit::Author: standing inside a narrator, the fact a row is missing is who WROTE it.
    return booksCatalog(idx.narrator(narratorKey), QObject::tr("Narrators"), Credit::Author, cover);
}

MediaCatalog audiobookSeriesCatalog(const AudiobookLibrary::Index& idx, const QString& seriesKey,
                                    const AudiobookCoverFn& cover)
{
    return booksCatalog(idx.seriesFor(seriesKey), QObject::tr("Series"), Credit::Author, cover);
}

MediaCatalog audiobookNarratorsCatalog(const AudiobookLibrary::Index& idx, const AudiobookCoverFn& cover)
{
    return bucketsCatalog(idx.narrators, QObject::tr("Narrators"),
                          kAudiobookNarratorType, kAudiobookNarratorPrefix, cover);
}

MediaCatalog audiobookSeriesListCatalog(const AudiobookLibrary::Index& idx, const AudiobookCoverFn& cover)
{
    return bucketsCatalog(idx.series, QObject::tr("Series"),
                          kAudiobookSeriesType, kAudiobookSeriesPrefix, cover);
}

MediaCatalog audiobookBookCatalog(const AudiobookLibrary::Index& idx, const QString& bookKey,
                                  const AudiobookCoverFn& cover)
{
    MediaCatalog cat; cat.hasMore = false;
    const AudiobookLibrary::Book* b = idx.book(bookKey);
    if (!b)
    {
        // A stale route — the book was rescanned out from under the row. Empty and titled, and with NO play
        // row: a level that is not there must not offer to play something that is not there either.
        cat.title = QObject::tr("Audiobooks");
        return cat;
    }
    cat.title = AudiobookLibrary::displayBook(*b);

    const QString art = coverFor(*b, cover);
    cat.items.push_back(syntheticRow(kAudiobookPlayType, kAudiobookPlayPrefix, b->key, /*expandable*/ false,
                                     QObject::tr("▶  Play book"),
                                     bookSubtitle(*b, b->narrator.trimmed().isEmpty()
                                                          ? b->author.trimmed()
                                                          : QObject::tr("Read by %1").arg(b->narrator.trimmed())),
                                     art));

    const bool numbered = b->files.size() > 1;   // "1." on a single-file book is noise
    int n = 0;
    for (const AudiobookLibrary::BookFile& f : b->files)
    {
        ++n;
        MediaItem it;
        // THE PLAYABLE HANDLE. Never opened as a url from here: the surface intercepts kAudiobookFilePrefix
        // ahead of the generic "this item has a url" route (browse::localLeafRoute) and plays the BOOK
        // starting at this file — so what this string has to be is the thing the queue will hold and match
        // on, which is exactly what it is.
        it.url          = f.path;
        it.id           = f.path;
        it.type         = QString::fromLatin1(kAudiobookFileType);
        it.mime         = QString::fromLatin1(kAudiobookFilePrefix) + b->key;   // WHICH book to queue
        it.thumbnailUrl = art;
        it.title        = numbered ? QStringLiteral("%1. %2").arg(n).arg(f.title) : f.title;
        it.subtitle     = joinDot({ f.chapterCount > 0 ? QObject::tr("%n chapter(s)", "", f.chapterCount)
                                                       : QString(),
                                    fmtPartDuration(f.durationSec) });
        cat.items.push_back(it);
    }
    return cat;
}

} // namespace browse
