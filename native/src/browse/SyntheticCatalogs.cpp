#include "SyntheticCatalogs.h"
#include "../core/MetaCache.h"
#include <QFileInfo>
#include <QCoreApplication>

namespace browse
{

QString iconTypeForKind(const QString& kind)
{
    if (kind == QStringLiteral("video"))    return QStringLiteral("movie");
    if (kind == QStringLiteral("audio"))    return QStringLiteral("album");
    if (kind == QStringLiteral("document")) return QStringLiteral("book");
    if (kind == QStringLiteral("game") || kind == QStringLiteral("pcgame")) return QStringLiteral("game");
    return QString();
}

MediaCatalog recentsCatalog(const QList<RecentItem>& all, const QString& marker)
{
    // marker = "<kind>" or "<kind>|<system>": the optional system scopes a games console (its SystemCatalog id,
    // or "pc"); empty system = all of that kind (the catalogue-root Recent).
    const QString kind = marker.section(QLatin1Char('|'), 0, 0);
    const QString system = marker.section(QLatin1Char('|'), 1, 1);
    MediaCatalog cat; cat.title = QObject::tr("Recent");
    for (const RecentItem& r : all)
    {
        // PC games belong to the game catalogue's Recent view alongside emulated ones.
        const bool match = r.kind == kind
                           || (kind == QStringLiteral("game") && r.kind == QStringLiteral("pcgame"));
        if (!kind.isEmpty() && !match) continue;
        if (!system.isEmpty() && r.system != system) continue; // per-console scope
        MediaItem it;
        it.url = r.path;                                       // re-open target
        it.id = r.key;                                         // stable resume key (streamed items)
        it.mime = r.kind;                                      // routing kind
        it.type = iconTypeForKind(r.kind);                    // drives the placeholder icon + resume bar
        // Offline-first artwork: a downloaded item's locally cached poster wins over the remote url.
        it.thumbnailUrl = MetaCache::displayImage(it.id.isEmpty() ? it.url : it.id, r.thumb);
        it.title = r.title.isEmpty() ? QFileInfo(r.path).completeBaseName() : r.title;
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog downloadsCatalog(const QList<DownloadedItem>& all, const QString& marker,
                               const std::function<bool(const QString&)>& fileExists)
{
    // marker = "<kind>|<system>": kind filters the catalogue; system (a SystemCatalog id, or "pc") scopes a
    // games console. An empty system matches any (non-game catalogues).
    const QString kind = marker.section(QLatin1Char('|'), 0, 0);
    const QString system = marker.section(QLatin1Char('|'), 1, 1);
    MediaCatalog cat; cat.title = QObject::tr("Downloaded");
    for (const DownloadedItem& d : all)
    {
        if (!kind.isEmpty() && d.kind != kind) continue;
        if (!system.isEmpty() && d.system != system) continue;
        const bool exists = fileExists ? fileExists(d.path) : QFileInfo::exists(d.path);
        if (!exists) continue; // hide entries whose file was deleted outside the app
        MediaItem it;
        it.url = d.path;
        it.id = d.key;
        it.mime = d.kind;                       // routing kind (openRecent dispatches on it)
        it.type = iconTypeForKind(d.kind);
        // Offline-first artwork: the locally cached poster wins over the remote url.
        it.thumbnailUrl = MetaCache::displayImage(it.id.isEmpty() ? it.url : it.id, d.thumb);
        it.title = d.title.isEmpty() ? QFileInfo(d.path).completeBaseName() : d.title;
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog favoritesCatalog(const QList<FavoriteItem>& all, const QString& system)
{
    MediaCatalog cat; cat.title = QObject::tr("Favorites");
    for (const FavoriteItem& f : all)
    {
        if (f.path.isEmpty()) continue;                 // only local games have a per-console home
        if (!system.isEmpty() && f.system != system) continue;
        MediaItem it;
        it.url = f.path;
        it.id = f.itemId;
        it.mime = f.kind.isEmpty() ? QStringLiteral("game") : f.kind; // routing kind -> the game action menu
        it.type = iconTypeForKind(it.mime);
        // Offline-first artwork: the locally cached poster wins over the remote url.
        it.thumbnailUrl = MetaCache::displayImage(it.id.isEmpty() ? it.url : it.id, f.thumbnailUrl);
        it.title = f.title.isEmpty() ? QFileInfo(f.path).completeBaseName() : f.title;
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog playlistsCatalog(const QList<Playlist>& all, const QString& catalogKey)
{
    MediaCatalog cat; cat.title = QObject::tr("Playlists");
    for (const Playlist& p : all)
    {
        MediaItem it;
        it.id = QStringLiteral("pl:") + p.id;
        it.type = QStringLiteral("_playlist");
        it.title = p.name;
        it.subtitle = QObject::tr("%n item(s)", "", int(p.items.size()));
        it.expandable = true;
        it.mime = QStringLiteral("playlist:") + p.id;
        cat.items.push_back(it);
    }
    MediaItem add; // a New-playlist entry at the bottom (activates -> name prompt)
    add.id = QStringLiteral("_newplaylist");
    add.type = QStringLiteral("_newplaylist");
    add.title = QObject::tr("➕  New playlist…");
    add.mime = QStringLiteral("newplaylist:") + catalogKey;
    cat.items.push_back(add);
    cat.hasMore = false;
    return cat;
}

MediaCatalog playlistItemsCatalog(const Playlist& p)
{
    MediaCatalog cat; cat.title = p.name;
    for (const PlaylistEntry& e : p.items)
    {
        MediaItem it;
        it.id = e.itemId; it.type = e.type; it.title = e.title; it.subtitle = e.subtitle;
        it.thumbnailUrl = e.thumbnailUrl; it.expandable = e.expandable;
        if (e.itemId.startsWith(QStringLiteral("steam:"))) it.mime = QStringLiteral("steamgame"); // launch natively
        else if (!e.path.isEmpty()) { it.url = e.path; it.mime = QStringLiteral("localgame:") + e.kind; } // local game -> re-open by path
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

} // namespace browse
