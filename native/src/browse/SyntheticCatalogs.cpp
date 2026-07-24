#include "SyntheticCatalogs.h"
#include "../core/MetaCache.h"
#include <QFileInfo>
#include <QCoreApplication>
#include <QSet>

namespace browse
{

QString iconTypeForKind(const QString& kind)
{
    if (kind == QStringLiteral("video"))    return QStringLiteral("movie");
    if (kind == QStringLiteral("audio"))    return QStringLiteral("album");
    if (kind == QStringLiteral("document")) return QStringLiteral("book");
    if (kind == QStringLiteral("game") || kind == QStringLiteral("pcgame")
        || kind == QStringLiteral("steamgame") || kind == QStringLiteral("epicgame")
        || kind == QStringLiteral("goggame")) return QStringLiteral("game");
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
        // PC + Steam games belong to the game catalogue's Recent view alongside emulated ones.
        const bool match = r.kind == kind
                           || (kind == QStringLiteral("game")
                               && (r.kind == QStringLiteral("pcgame") || r.kind == QStringLiteral("steamgame")
                                   || r.kind == QStringLiteral("epicgame") || r.kind == QStringLiteral("goggame")));
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

MediaCatalog localLibraryCatalog(const QVector<LocalLibrary::VideoEntry>& entries)
{
    MediaCatalog cat; cat.title = QObject::tr("Local Library");
    for (const LocalLibrary::VideoEntry& e : entries)
    {
        MediaItem it;
        it.url = e.path;
        it.mime = QStringLiteral("local:video");
        it.type = QStringLiteral("movie");                 // both movies and episodes render as video tiles
        it.id = e.imdbId.isEmpty() ? (QStringLiteral("local:") + e.path) : e.imdbId;
        it.title = LocalLibrary::displayTitle(e);
        it.subtitle = e.plot;
        // Offline-first: a local NFO <thumb> is a file path; MetaCache::displayImage serves it if present.
        it.thumbnailUrl = MetaCache::displayImage(it.id, e.thumbPath);
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

FavoriteItem localGameFavorite(const MediaItem& it, const QString& systemHint)
{
    FavoriteItem f;
    f.itemId = it.id.isEmpty() ? it.url : it.id; // gameFavId rule: stable key, else path
    f.title = it.title;
    f.type = QStringLiteral("game");
    f.thumbnailUrl = it.thumbnailUrl;
    f.path = it.url;   // re-open by path (openFavorite recovers the console from the stores)
    f.kind = it.mime;  // "game" | "pcgame" (openRecent routing kind)
    f.system = systemHint.isEmpty() ? FavoritesStore::deriveSystem(f.path, f.kind) : systemHint;
    return f;
}

MediaCatalog playlistsCatalog(const QList<Playlist>& all, const QString& categoryKey)
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
    MediaItem add; // a New-playlist entry at the bottom (activates -> name prompt, creating in this category)
    add.id = QStringLiteral("_newplaylist");
    add.type = QStringLiteral("_newplaylist");
    add.title = QObject::tr("➕  New playlist…");
    add.mime = QStringLiteral("newplaylist:") + categoryKey;
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
        it.sourceAddonId = e.addonId; // per-entry addon resolution (mixed-source category playlists)
        // Store-game entries mirror their console tiles so a game added to a playlist still LAUNCHES (dead before):
        //   steam: / epic: — no url; the leaf-open builds the launcher URI from the id (steam://, com.epicgames…)
        //   gog:          — a DRM-free exe; the resolved exe was persisted into the entry's path at add-time and
        //                   rides back onto the tile's url, so the MONITORED launchPcExe path can run it.
        if (e.itemId.startsWith(QStringLiteral("steam:")))     it.mime = QStringLiteral("steamgame"); // launch natively
        else if (e.itemId.startsWith(QStringLiteral("epic:"))) it.mime = QStringLiteral("epicgame");  // launch via URI
        else if (e.itemId.startsWith(QStringLiteral("gog:")))  { it.mime = QStringLiteral("goggame"); it.url = e.path; } // exe rides in path
        else if (!e.path.isEmpty()) { it.url = e.path; it.mime = QStringLiteral("localgame:") + e.kind; } // local game -> re-open by path
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog steamGamesCatalog(const QList<SteamGame>& installed, const QString& query,
                               const std::function<QString(const SteamGame&)>& poster,
                               const QList<SteamGame>& owned)
{
    // A search while in the Steam console scopes to the library: keep only games whose name matches.
    const QString q = query.trimmed();
    MediaCatalog cat;
    cat.title = q.isEmpty() ? QObject::tr("Steam") : QObject::tr("Steam · %1").arg(q);
    auto posterFor = [&poster](const SteamGame& g) {
        return poster ? poster(g) : SteamLibrary::posterUrl(g.appid);
    };
    QSet<QString> haveIds;
    for (const SteamGame& g : installed)
    {
        haveIds.insert(g.appid);
        if (!q.isEmpty() && !g.name.contains(q, Qt::CaseInsensitive)) continue;
        MediaItem it;
        it.id = QStringLiteral("steam:") + g.appid;
        it.type = QStringLiteral("game");
        it.title = g.name;
        it.mime = QStringLiteral("steamgame"); // no url -> clicking opens the info page; Play launches it
        it.thumbnailUrl = posterFor(g);
        cat.items.push_back(it);
    }
    // Owned-but-not-installed (creds-gated): appended after the installed games, badged "Not installed", and
    // carrying a steam://install/<appid> url so activation hands the install off to the Steam client (the same
    // openUrl path a run uses). Skips anything already installed. Empty when no key/SteamID is configured.
    for (const SteamGame& g : owned)
    {
        if (haveIds.contains(g.appid)) continue;                 // installed entries stay unchanged
        if (!q.isEmpty() && !g.name.contains(q, Qt::CaseInsensitive)) continue;
        MediaItem it;
        it.id = QStringLiteral("steam:") + g.appid;
        it.type = QStringLiteral("game");
        it.title = g.name;
        it.subtitle = QObject::tr("Not installed");             // the badge
        it.mime = QStringLiteral("steamgame");
        it.url = SteamLibrary::installUrl(g.appid);              // activation -> steam://install/<appid>
        it.thumbnailUrl = posterFor(g);
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog epicGamesCatalog(const QList<EpicGame>& installed, const QString& query,
                              const std::function<QString(const EpicGame&)>& poster)
{
    const QString q = query.trimmed();
    MediaCatalog cat;
    cat.title = q.isEmpty() ? QObject::tr("Epic Games") : QObject::tr("Epic Games · %1").arg(q);
    for (const EpicGame& g : installed)
    {
        if (!q.isEmpty() && !g.name.contains(q, Qt::CaseInsensitive)) continue;
        MediaItem it;
        it.id = QStringLiteral("epic:") + g.appName;
        it.type = QStringLiteral("game");
        it.title = g.name;
        it.mime = QStringLiteral("epicgame"); // no url -> info page; Play launches via the launcher URI
        if (poster) it.thumbnailUrl = poster(g); // Epic has no local capsule; empty -> scrapers fill it later
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog gogGamesCatalog(const QList<GogGame>& installed, const QString& query,
                             const std::function<QString(const GogGame&)>& poster)
{
    const QString q = query.trimmed();
    MediaCatalog cat;
    cat.title = q.isEmpty() ? QObject::tr("GOG") : QObject::tr("GOG · %1").arg(q);
    for (const GogGame& g : installed)
    {
        if (!q.isEmpty() && !g.name.contains(q, Qt::CaseInsensitive)) continue;
        MediaItem it;
        it.id = QStringLiteral("gog:") + g.id;
        it.type = QStringLiteral("game");
        it.title = g.name;
        it.mime = QStringLiteral("goggame"); // launched through the monitored launchPcExe path
        it.url = g.exe;                       // the resolved exe rides the tile (MainWindow runs it)
        if (poster) it.thumbnailUrl = poster(g);
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

} // namespace browse
