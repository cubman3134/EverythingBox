#include "JellyfinCatalogs.h"

#include <QCoreApplication>
#include <QLatin1String>

namespace browse
{
namespace {

// A non-actionable line. type "info" is refused by activateItem and drilled (never chooser'd) by
// themedEnterFor, so it is inert on both layouts — which is what makes it safe to use for prose.
MediaItem infoRow(const QString& text)
{
    MediaItem it;
    it.type  = QStringLiteral("info");
    it.title = text;
    return it;
}

// Every level ends the same way: the notes from servers that contributed nothing, then — only if the level
// would otherwise be empty — one line saying so. Both are "info" rows, so neither is activatable.
void appendNotesAndEmptiness(MediaCatalog& c, const QStringList& notes, const QString& emptyText)
{
    const bool hadRows = !c.items.isEmpty();
    for (const QString& n : notes)
        if (!n.isEmpty()) c.items.push_back(infoRow(n));
    // THE EMPTINESS LINE IS ABOUT ROWS, NOT ABOUT THE CATALOG. A level whose only content is three "that
    // server did not answer" notes has explained itself already; adding "nothing here" under them would be
    // a second, less true, sentence.
    if (!hadRows && c.items.isEmpty() && !emptyText.isEmpty()) c.items.push_back(infoRow(emptyText));
}

// The second line of a row: the pieces that are known, joined. Built in one place so a tagged row and an
// untagged one differ by exactly one element rather than by two separate format strings.
QString subtitleOf(const QStringList& parts)
{
    QStringList kept;
    for (const QString& p : parts)
        if (!p.trimmed().isEmpty()) kept << p.trimmed();
    return kept.join(QStringLiteral(" · "));   // " · "
}

// The server's own type token, lowered onto the vocabulary the rest of the app routes on. "Movie" ->
// "movie" and "Episode" -> "episode" both land in core::mediaCategory's video bucket, which is what puts a
// Jellyfin row in the same place as every other film.
QString leafTypeOf(const QString& serverType)
{
    const QString t = serverType.toLower();
    if (t == QLatin1String("episode")) return QStringLiteral("episode");
    if (t == QLatin1String("series"))  return QStringLiteral("series");
    return QStringLiteral("movie");
}

} // namespace

MediaItem jellyfinLeafRow(const Jellyfin::UnionItem& it, bool tagServer)
{
    MediaItem row;
    row.id    = it.id;                      // the QUALIFIED id: what everything downstream is filed under
    row.title = it.title;
    row.type  = leafTypeOf(it.type);
    // THE KEYED MIME, AND AN EMPTY URL. See the header: the link is minted at play time and never sits on
    // a row. LeafRoute claims this prefix and hands the key (the qualified id) to the open route.
    row.mime  = QString::fromLatin1(kJellyfinItemPrefix) + it.id;
    row.url.clear();
    QStringList parts;
    if (!it.seriesName.isEmpty() && row.type == QLatin1String("episode")) parts << it.seriesName;
    if (it.year > 0) parts << QString::number(it.year);
    if (tagServer && !it.serverName.isEmpty()) parts << it.serverName;
    row.subtitle = subtitleOf(parts);
    return row;
}

MediaCatalog jellyfinLibrariesCatalog(const QVector<Jellyfin::LibraryRef>& libraries,
                                      const QStringList& notes)
{
    MediaCatalog c;
    c.title = QCoreApplication::translate("browse", "Jellyfin");
    // MORE THAN ONE SERVER CONTRIBUTED => TAG. Computed here rather than taken as a parameter because at
    // THIS level the answer is a property of the list itself, and a caller free to get it wrong is a
    // caller that will.
    QStringList servers;
    for (const Jellyfin::LibraryRef& l : libraries)
        if (!l.serverName.isEmpty() && !servers.contains(l.serverName)) servers << l.serverName;
    const bool tag = servers.size() > 1;

    for (const Jellyfin::LibraryRef& l : libraries)
    {
        // A library this increment cannot browse is NOT LISTED. Jellyfin's view list carries collections,
        // playlists and live tv; a folder whose every row would be unopenable is worse than no folder.
        if (!Jellyfin::isVideoCollection(l.collectionType)) continue;
        if (l.ref.isEmpty()) continue;      // unqualifiable: dropped, never emitted bare (section 1)
        MediaItem row;
        row.id         = l.ref;
        row.title      = l.name;
        row.type       = QString::fromLatin1(kJellyfinLibType);
        row.mime       = QString::fromLatin1(kJellyfinLibPrefix) + l.ref;
        row.expandable = true;
        if (tag) row.subtitle = l.serverName;
        c.items.push_back(row);
    }
    appendNotesAndEmptiness(c, notes,
        QCoreApplication::translate("browse",
            "No libraries to show. Check the servers under Settings, or that this account can see them."));
    return c;
}

MediaCatalog jellyfinLibraryCatalog(const QString& title,
                                            const QVector<Jellyfin::UnionItem>& items,
                                            bool tagServers, const QStringList& notes)
{
    MediaCatalog c;
    c.title = title;
    for (const Jellyfin::UnionItem& it : items)
    {
        if (it.id.isEmpty()) continue;
        if (it.type.compare(QLatin1String("Series"), Qt::CaseInsensitive) == 0)
        {
            // A CONTAINER. Its seasons are fetched when it is opened, never up front — see
            // Jellyfin::libraryItemsQuery for why a library does not enumerate its episodes.
            MediaItem row;
            row.id         = it.id;
            row.title      = it.title;
            row.type       = QString::fromLatin1(kJellyfinSeriesType);
            row.mime       = QString::fromLatin1(kJellyfinSeriesPrefix) + it.id;
            row.expandable = true;
            QStringList parts;
            if (it.year > 0) parts << QString::number(it.year);
            if (tagServers && !it.serverName.isEmpty()) parts << it.serverName;
            row.subtitle = subtitleOf(parts);
            c.items.push_back(row);
            continue;
        }
        c.items.push_back(jellyfinLeafRow(it, tagServers));
    }
    appendNotesAndEmptiness(c, notes,
        QCoreApplication::translate("browse", "Nothing in this library yet."));
    return c;
}

MediaCatalog jellyfinSeasonsCatalog(const QString& seriesTitle, const QString& seriesRef,
                                            const QVector<Jellyfin::UnionItem>& seasons)
{
    MediaCatalog c;
    c.title = seriesTitle;
    for (const Jellyfin::UnionItem& s : seasons)
    {
        if (s.id.isEmpty()) continue;
        MediaItem row;
        row.id         = s.id;
        row.title      = s.title;
        row.type       = QString::fromLatin1(kJellyfinSeasonType);
        // BOTH IDS. /Shows/<seriesId>/Episodes is addressed by the series and filtered by the season, and
        // a level that carried only the season would have to guess at the series on the way back in.
        row.mime       = QString::fromLatin1(kJellyfinSeasonPrefix) + seriesRef
                       + QLatin1Char('\n') + s.id;
        row.expandable = true;
        c.items.push_back(row);
    }
    appendNotesAndEmptiness(c, {},
        QCoreApplication::translate("browse", "This show has no seasons on the server."));
    return c;
}

MediaCatalog jellyfinEpisodesCatalog(const QString& seasonTitle,
                                             const QVector<Jellyfin::UnionItem>& episodes)
{
    MediaCatalog c;
    c.title = seasonTitle;
    for (const Jellyfin::UnionItem& e : episodes)
    {
        if (e.id.isEmpty()) continue;
        MediaItem row = jellyfinLeafRow(e, /*tagServer*/ false);
        // NUMBERED ONLY WHEN THE SERVER GAVE A NUMBER. A special, an extra or a mis-scanned file has none,
        // and "S0E0 · " in front of its title would be this app inventing one. Season 0 is a real season on
        // Jellyfin (it is where specials live), so the test is on the EPISODE number.
        if (e.indexNumber > 0)
            row.title = QStringLiteral("S%1E%2 · %3")
                            .arg(e.parentIndexNumber).arg(e.indexNumber).arg(e.title);
        c.items.push_back(row);
    }
    appendNotesAndEmptiness(c, {},
        QCoreApplication::translate("browse", "No episodes in this season on the server."));
    return c;
}

QVector<MediaItem> jellyfinContinueRows(const QVector<Jellyfin::UnionItem>& items, bool tagServers)
{
    QVector<MediaItem> out;
    for (const Jellyfin::UnionItem& it : items)
    {
        if (it.id.isEmpty()) continue;
        // AN ITEM WITH NO POSITION IS NOT "CONTINUE WATCHING". Jellyfin's own Resume endpoint answers with
        // part-watched items, but a row at zero would be a film the user has never started sitting at the
        // top of their home screen claiming otherwise.
        if (it.positionTicks <= 0) continue;
        out.push_back(jellyfinLeafRow(it, tagServers));
    }
    return out;
}

} // namespace browse
