// Headless test for the synthetic browse-catalog builders (Recent / Downloaded / Favorites):
// kind+system filtering, the pcgame-in-games rule, and missing-file hiding. Prints BROWSE-OK.
#include <QCoreApplication>
#include "../src/browse/SyntheticCatalogs.h"
#include "../src/core/PlaylistStore.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QList<RecentItem> recents;
    { RecentItem r; r.path = "C:/v/movie.mkv"; r.title = "Movie"; r.kind = "video"; recents << r; }
    { RecentItem r; r.path = "C:/g/mario.nes"; r.title = "Mario"; r.kind = "game"; r.system = "nes"; recents << r; }
    { RecentItem r; r.path = "C:/g/doom.exe";  r.title = "Doom";  r.kind = "pcgame"; r.system = "pc"; recents << r; }

    auto vids = browse::recentsCatalog(recents, "video");
    CHECK(vids.items.size() == 1 && vids.items[0].title == "Movie", "recents: kind filter");
    auto games = browse::recentsCatalog(recents, "game");
    CHECK(games.items.size() == 2, "recents: pcgame counts as game");
    auto nes = browse::recentsCatalog(recents, "game|nes");
    CHECK(nes.items.size() == 1 && nes.items[0].title == "Mario", "recents: per-console scope");

    QList<DownloadedItem> dls;
    { DownloadedItem d; d.path = "C:/dl/here.mkv"; d.title = "Here"; d.kind = "video"; dls << d; }
    { DownloadedItem d; d.path = "C:/dl/gone.mkv"; d.title = "Gone"; d.kind = "video"; dls << d; }
    auto have = browse::downloadsCatalog(dls, "video|",
        [](const QString& p) { return p.contains("here"); }); // fake existence check
    CHECK(have.items.size() == 1 && have.items[0].title == "Here",
          "downloads: deleted-outside-the-app entries hidden");

    QList<FavoriteItem> favs;
    { FavoriteItem f; f.path = "C:/g/zelda.sfc"; f.title = "Zelda"; f.system = "snes"; favs << f; }
    { FavoriteItem f; f.path = "";               f.title = "NoPath"; favs << f; } // streamed fav: no console home
    auto snes = browse::favoritesCatalog(favs, "snes");
    CHECK(snes.items.size() == 1 && snes.items[0].title == "Zelda", "favorites: system scope + path-only");

    // ---- Playlists level: this catalogue's playlists + the trailing synthetic New-playlist row -------------
    QList<Playlist> pls;
    { Playlist p; p.id = "id-a"; p.name = "Alpha"; PlaylistEntry e; p.items << e << e; pls << p; } // 2 items
    { Playlist p; p.id = "id-b"; p.name = "Beta"; pls << p; }                                        // 0 items
    auto plCat = browse::playlistsCatalog(pls, "native|cat|movie");
    CHECK(plCat.items.size() == 3, "playlists: 2 playlists + New-playlist row");
    CHECK(plCat.items[0].id == "pl:id-a" && plCat.items[0].type == "_playlist"
          && plCat.items[0].title == "Alpha" && plCat.items[0].expandable
          && plCat.items[0].mime == "playlist:id-a", "playlists: playlist row mapped");
    CHECK(plCat.items[2].id == "_newplaylist" && plCat.items[2].type == "_newplaylist"
          && plCat.items[2].mime == "newplaylist:native|cat|movie",
          "playlists: New-playlist marker row (id/type/mime)");

    // ---- Playlist items level: addon / steam / local-path entry variants -----------------------------------
    Playlist items;
    { PlaylistEntry e; e.itemId = "addon-1"; e.type = "movie"; e.title = "Film"; e.subtitle = "2020";
      e.thumbnailUrl = "http://x/p.jpg"; e.expandable = true; items.items << e; }        // ordinary addon entry
    { PlaylistEntry e; e.itemId = "steam:440"; e.type = "game"; e.title = "TF2"; items.items << e; } // steam
    { PlaylistEntry e; e.itemId = "local-1"; e.type = "game"; e.title = "Mario"; e.path = "C:/g/m.nes";
      e.kind = "game"; items.items << e; }                                              // local-file entry
    auto plItems = browse::playlistItemsCatalog(items);
    CHECK(plItems.items.size() == 3, "playlistItems: all three entries mapped");
    CHECK(plItems.items[0].id == "addon-1" && plItems.items[0].type == "movie"
          && plItems.items[0].title == "Film" && plItems.items[0].subtitle == "2020"
          && plItems.items[0].thumbnailUrl == "http://x/p.jpg" && plItems.items[0].expandable
          && plItems.items[0].mime.isEmpty() && plItems.items[0].url.isEmpty(),
          "playlistItems: ordinary addon entry (no special-case mime/url)");
    CHECK(plItems.items[1].id == "steam:440" && plItems.items[1].mime == "steamgame"
          && plItems.items[1].url.isEmpty(), "playlistItems: steam: entry -> steamgame");
    CHECK(plItems.items[2].id == "local-1" && plItems.items[2].url == "C:/g/m.nes"
          && plItems.items[2].mime == "localgame:game",
          "playlistItems: local-path entry -> url + localgame:<kind>");

    // ---- Steam games level: SteamGame -> MediaItem mapping + the in-console query filter -------------------
    QList<SteamGame> steam;
    { SteamGame g; g.appid = "440"; g.name = "Team Fortress 2"; steam << g; }
    { SteamGame g; g.appid = "570"; g.name = "Dota 2";          steam << g; }
    auto poster = [](const SteamGame& g) { return QStringLiteral("poster:") + g.appid; }; // inject: no I/O
    auto allSteam = browse::steamGamesCatalog(steam, QString(), poster);
    CHECK(allSteam.items.size() == 2, "steam: empty query -> all installed");
    auto tf2 = browse::steamGamesCatalog(steam, "fortress", poster);
    CHECK(tf2.items.size() == 1 && tf2.items[0].id == "steam:440"
          && tf2.items[0].mime == "steamgame" && tf2.items[0].type == "game"
          && tf2.items[0].title == "Team Fortress 2" && tf2.items[0].thumbnailUrl == "poster:440"
          && tf2.items[0].url.isEmpty(),
          "steam: query filter -> one game with exact id/mime/poster/type mapping");

    if (fails == 0) printf("BROWSE-OK\n");
    return fails == 0 ? 0 : 1;
}
