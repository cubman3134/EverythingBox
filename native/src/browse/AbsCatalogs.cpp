#include "AbsCatalogs.h"

#include <QObject>

#include <algorithm>

namespace browse
{
namespace {

// "9h 14m" / "42m". AudiobookCatalogs' rule, restated rather than shared, because sharing it would drag
// AudiobookLibrary (and TagLib behind it) into this unit's link set for two arithmetic lines — the same
// argument AudiobookCatalogs.h makes about AudiobookEmptyNote. Nothing at all for a duration of 0: a
// server that has not scanned an item yet reports one, and "0m" beside a real book reads as a broken file.
QString fmtBookDuration(int secs)
{
    if (secs <= 0) return QString();
    const int h = secs / 3600, m = (secs % 3600) / 60;
    if (h > 0) return m > 0 ? QObject::tr("%1h %2m").arg(h).arg(m) : QObject::tr("%1h").arg(h);
    return QObject::tr("%1m").arg(qMax(1, m));   // a 40-second file is "1m", never "0m"
}

// "m:ss" / "h:mm:ss" — for one PART or one EPISODE, where the exact length is the useful fact.
QString fmtPartDuration(int secs)
{
    if (secs <= 0) return QString();
    const int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0'))
                                             .arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString joinDot(const QStringList& parts)
{
    QStringList kept;
    for (const QString& p : parts) if (!p.isEmpty()) kept << p;
    return kept.join(QStringLiteral(" · "));
}

QString coverFor(const QString& qualifiedId, const AbsCoverFn& fn)
{
    return fn ? fn(qualifiedId) : QString();   // no default: this unit touches no filesystem
}

// One builder for every synthetic row in this file, so each row's id and mime carry the same string and
// every reader is absKeyOf. AudiobookCatalogs' syntheticRow, which is the shape the surfaces already read.
MediaItem syntheticRow(const char* type, const char* prefix, const QString& key, bool expandable,
                       const QString& title, const QString& subtitle, const QString& art = QString())
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

// A book row, wherever it is rendered. `qualifiedId` rather than the raw item id, because that is what the
// row has to be re-openable by after a restart — see Audiobookshelf.h.
MediaItem bookRow(const QString& qualifiedId, const Abs::Item& b, const AbsCoverFn& cover)
{
    const QString sequence = b.seriesSequence.isEmpty()
                                 ? QString()
                                 : QObject::tr("Book %1").arg(b.seriesSequence);
    return syntheticRow(kAbsBookType, kAbsBookPrefix, qualifiedId, /*expandable*/ true,
                        b.title.isEmpty() ? QObject::tr("Untitled") : b.title,
                        joinDot({ b.author, sequence,
                                  // Only when there is more than one, because "1 part" is noise on every
                                  // single-file m4b a server holds.
                                  b.trackCount > 1 ? QObject::tr("%n part(s)", "", b.trackCount)
                                                   : QString(),
                                  fmtBookDuration(int(b.duration)) }),
                        coverFor(qualifiedId, cover));
}

} // namespace

// ==================================================================================================
// The door, and the server list
// ==================================================================================================
MediaItem absServersRow(int count)
{
    return syntheticRow(kAbsServersType, kAbsServersPrefix, QString(), /*expandable*/ true,
                        QObject::tr("Audiobook Servers"),
                        QObject::tr("%n server(s)", "", count));
}

MediaCatalog absServersCatalog(const QStringList& ids, const QStringList& names, const QStringList& urls,
                               const QVector<bool>& enabled)
{
    MediaCatalog cat; cat.title = QObject::tr("Audiobook Servers");
    cat.hasMore = false;
    for (int i = 0; i < ids.size(); ++i)
    {
        // The ADDRESS, not the sign-in. A row that named the account would put a username on a TV in a
        // living room to no purpose, and there is obviously no rendering of the token at all.
        const QString where = i < urls.size() ? urls.at(i) : QString();
        const bool on = i < enabled.size() ? enabled.at(i) : true;
        cat.items.push_back(syntheticRow(kAbsServerType, kAbsServerPrefix, ids.at(i), /*expandable*/ true,
                                         i < names.size() && !names.at(i).trimmed().isEmpty()
                                             ? names.at(i) : QObject::tr("Audiobook server"),
                                         on ? where : joinDot({ QObject::tr("Off"), where })));
    }
    // Always last, always present: with no servers saved this row is the whole level, which is what makes
    // the first one addable at all.
    cat.items.push_back(syntheticRow(kAbsAddServerType, kAbsAddServerPrefix, QString(), /*expandable*/ false,
                                     QObject::tr("＋ Add an audiobook server…"),
                                     QObject::tr("Audiobookshelf")));
    return cat;
}

// ==================================================================================================
// One server's libraries
// ==================================================================================================
MediaCatalog absLibrariesCatalog(const QString& serverId, const QString& serverName,
                                 const QVector<Abs::Library>& libs)
{
    MediaCatalog cat;
    cat.title = serverName.isEmpty() ? QObject::tr("Audiobook server") : serverName;
    cat.hasMore = false;
    for (const Abs::Library& l : libs)
    {
        // A library id is server-scoped exactly as an item id is, so it is qualified the same way and by
        // the same function. A row whose id could not be qualified is dropped rather than rendered: it
        // could not be re-opened, and a level that opens nothing is worse than one row fewer.
        const QString key = Abs::qualify(serverId, l.id);
        if (key.isEmpty()) continue;
        cat.items.push_back(syntheticRow(kAbsLibraryType, kAbsLibraryPrefix, key, /*expandable*/ true,
                                         l.name.isEmpty() ? QObject::tr("Library") : l.name,
                                         l.isPodcast() ? QObject::tr("Podcasts")
                                                       : QObject::tr("Audiobooks")));
    }
    return cat;
}

// ==================================================================================================
// One library
// ==================================================================================================
MediaCatalog absLibraryCatalog(const QString& qualifiedLibraryId, const QString& libraryName,
                               bool isPodcast, int seriesCount, int authorCount, int bookCount,
                               const QVector<Abs::Item>& podcasts, const AbsCoverFn& cover)
{
    MediaCatalog cat;
    cat.title = libraryName.isEmpty() ? QObject::tr("Library") : libraryName;
    cat.hasMore = false;

    if (isPodcast)
    {
        const QString serverId = Abs::serverOf(qualifiedLibraryId);
        for (const Abs::Item& p : podcasts)
        {
            const QString key = Abs::qualify(serverId, p.id);
            if (key.isEmpty()) continue;
            cat.items.push_back(syntheticRow(kAbsPodcastType, kAbsPodcastPrefix, key, /*expandable*/ true,
                                             p.title.isEmpty() ? QObject::tr("Untitled") : p.title,
                                             joinDot({ p.author,
                                                       p.episodeCount > 0
                                                           ? QObject::tr("%n episode(s)", "", p.episodeCount)
                                                           : QString() }),
                                             coverFor(key, cover)));
        }
        return cat;
    }

    // The two DIMENSION doors, each only when its dimension has anything in it — AudiobookCatalogs' rule,
    // and the compatibility half of this whole feature: a library whose books carry no series tag gets a
    // plain list of books and no idioms it did not ask for.
    if (seriesCount > 0)
        cat.items.push_back(syntheticRow(kAbsSeriesListType, kAbsSeriesListPrefix, qualifiedLibraryId,
                                         /*expandable*/ true, QObject::tr("Series"),
                                         QObject::tr("%n series", "", seriesCount)));
    if (authorCount > 0)
        cat.items.push_back(syntheticRow(kAbsAuthorsType, kAbsAuthorsPrefix, qualifiedLibraryId,
                                         /*expandable*/ true, QObject::tr("Authors"),
                                         QObject::tr("%n author(s)", "", authorCount)));
    cat.items.push_back(syntheticRow(kAbsBooksType, kAbsBooksPrefix, qualifiedLibraryId,
                                     /*expandable*/ true, QObject::tr("All Books"),
                                     QObject::tr("%n book(s)", "", bookCount)));
    return cat;
}

MediaCatalog absSeriesListCatalog(const QString& qualifiedLibraryId, const QString& libraryName,
                                  const QVector<Abs::SeriesRow>& series)
{
    MediaCatalog cat;
    cat.title = libraryName.isEmpty() ? QObject::tr("Series") : libraryName;
    cat.hasMore = false;
    for (const Abs::SeriesRow& s : series)
    {
        // KEYED BY NAME, NOT BY THE SERVER'S SERIES ID — and this is the one place in the feature where a
        // row is not keyed by the server's own id, so it is stated here rather than discovered.
        //
        // The reason is the join. A library LISTING gives each book a `seriesName` and an `authorName`; it
        // does not give the series and author IDS that /series and /authors are keyed by. So the only thing
        // the two replies have in common is the name, and a row keyed by `ser_1` opens a level that filters
        // three books by a string none of them carries — which is an empty shelf, on a server that plainly
        // has the books, with nothing anywhere saying why. (That is exactly what the first live drive of
        // this feature showed, which is why the note is this long.)
        //
        // A name is stable enough for what this is: a browse route that is re-read on Back within a session.
        // Nothing is PERSISTED under it — a book row is keyed by the qualified item id, as everything that
        // outlives a level must be.
        if (s.name.isEmpty()) continue;   // a bucket with no name cannot be joined, so it cannot be opened
        cat.items.push_back(syntheticRow(kAbsSeriesType, kAbsSeriesPrefix,
                                         absJoinKey(qualifiedLibraryId, s.name), /*expandable*/ true,
                                         s.name,
                                         s.bookCount > 0 ? QObject::tr("%n book(s)", "", s.bookCount)
                                                         : QString()));
    }
    return cat;
}

MediaCatalog absAuthorsCatalog(const QString& qualifiedLibraryId, const QString& libraryName,
                               const QVector<Abs::AuthorRow>& authors)
{
    MediaCatalog cat;
    cat.title = libraryName.isEmpty() ? QObject::tr("Authors") : libraryName;
    cat.hasMore = false;
    for (const Abs::AuthorRow& a : authors)
    {
        // Keyed by NAME for the reason absSeriesListCatalog gives at length: the library listing carries an
        // `authorName` and not the author id this endpoint is keyed by, so a row keyed by `aut_1` opens an
        // empty level over a shelf that has the books.
        if (a.name.isEmpty()) continue;
        cat.items.push_back(syntheticRow(kAbsAuthorType, kAbsAuthorPrefix,
                                         absJoinKey(qualifiedLibraryId, a.name), /*expandable*/ true,
                                         a.name,
                                         a.bookCount > 0 ? QObject::tr("%n book(s)", "", a.bookCount)
                                                         : QString()));
    }
    return cat;
}

MediaCatalog absBooksCatalog(const QString& title, const QString& serverId,
                             const QVector<Abs::Item>& items, const AbsCoverFn& cover)
{
    MediaCatalog cat; cat.title = title; cat.hasMore = false;
    for (const Abs::Item& b : items)
    {
        const QString key = Abs::qualify(serverId, b.id);
        if (key.isEmpty()) continue;
        cat.items.push_back(bookRow(key, b, cover));
    }
    return cat;
}

// ==================================================================================================
// One book
// ==================================================================================================
MediaCatalog absBookCatalog(const QString& qualifiedItemId, const Abs::Item& item,
                            const QVector<Abs::Track>& tracks, int chapterCount, const AbsCoverFn& cover)
{
    MediaCatalog cat;
    cat.title = item.title.isEmpty() ? QObject::tr("Audiobook") : item.title;
    cat.hasMore = false;

    cat.items.push_back(syntheticRow(kAbsPlayBookType, kAbsPlayBookPrefix, qualifiedItemId,
                                     /*expandable*/ false, QObject::tr("Play book"),
                                     joinDot({ item.author,
                                               // Only when the server actually holds a chapter list. A book
                                               // with none says nothing rather than "0 chapters", which
                                               // would read as a defect in the file.
                                               chapterCount > 0
                                                   ? QObject::tr("%n chapter(s)", "", chapterCount)
                                                   : QString(),
                                               fmtBookDuration(int(item.duration)) }),
                                     coverFor(qualifiedItemId, cover)));

    for (int i = 0; i < tracks.size(); ++i)
    {
        const Abs::Track& t = tracks.at(i);
        // The key is the book plus the PART'S PLACE IN IT, not the server's file id: the place is what the
        // queue is built by and what a resume mark means, and it survives a server-side re-scan that
        // renumbers the files. Zero-based, so it indexes the queue directly.
        cat.items.push_back(syntheticRow(kAbsPartType, kAbsPartPrefix,
                                         absJoinKey(qualifiedItemId, QString::number(i)),
                                         /*expandable*/ false,
                                         t.title.isEmpty() ? QObject::tr("Part %1").arg(i + 1) : t.title,
                                         joinDot({ QObject::tr("Part %1 of %2").arg(i + 1)
                                                       .arg(tracks.size()),
                                                   fmtPartDuration(int(t.duration)) })));
    }
    return cat;
}

MediaCatalog absEpisodesCatalog(const QString& qualifiedItemId, const Abs::Item& item,
                                const QVector<Abs::Episode>& episodes, const AbsCoverFn& cover)
{
    MediaCatalog cat;
    cat.title = item.title.isEmpty() ? QObject::tr("Podcast") : item.title;
    cat.hasMore = false;

    const Abs::Ref ref = Abs::parse(qualifiedItemId);
    if (!ref.ok) return cat;

    // NEWEST FIRST. Decided here, once, rather than by each surface: the server sends its own order and the
    // two layouts must not be able to disagree about which episode is at the top of a podcast.
    QVector<Abs::Episode> ordered = episodes;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Abs::Episode& a, const Abs::Episode& b) { return a.published > b.published; });

    const QString art = coverFor(qualifiedItemId, cover);
    for (const Abs::Episode& e : ordered)
    {
        const QString key = Abs::qualifyEpisode(ref.serverId, ref.itemId, e.id);
        if (key.isEmpty()) continue;
        cat.items.push_back(syntheticRow(kAbsEpisodeType, kAbsEpisodePrefix, key, /*expandable*/ false,
                                         e.title.isEmpty() ? QObject::tr("Episode") : e.title,
                                         joinDot({ e.published.left(10), e.subtitle,
                                                   fmtPartDuration(int(e.duration)) }),
                                         art));
    }
    return cat;
}

MediaCatalog absNoteCatalog(const QString& title, const QString& text, const QString& detail)
{
    MediaCatalog cat; cat.title = title; cat.hasMore = false;
    if (text.isEmpty()) return cat;
    MediaItem info;
    info.type     = QStringLiteral("info");   // the surface's own non-actionable guidance row
    info.id       = QStringLiteral("_absnote");
    info.title    = text;
    info.subtitle = detail;
    cat.items.push_back(info);
    return cat;
}
} // namespace browse
