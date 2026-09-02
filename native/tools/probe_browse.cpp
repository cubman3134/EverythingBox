// Headless test for the synthetic browse-catalog builders (Recent / Downloaded / Favorites / Trakt
// "Airing soon"): kind+system filtering, the pcgame-in-games rule, missing-file hiding, and the Trakt
// calendar's air-time ordering + past-exclusion boundary + unplayable-row rule. Prints BROWSE-OK.
#include <QCoreApplication>
#include <QDateTime>
#include "../src/browse/SyntheticCatalogs.h"
#include "../src/core/ReadingForm.h"   // the reading-form oracle the Recent/Downloaded scope reads
#include "../src/core/PhotoLibrary.h"   // photosCatalog / photosFolderCatalog fixtures (#102)
#include "../src/browse/SearchAggregator.h"
#include "../src/core/PcGameRemap.h"   // the drift assertion: catalog item id == remap destination
#include "../src/core/PlaylistStore.h"
#include "../src/browse/JellyfinCatalogs.h"   // #83: the Jellyfin browse levels, beside the synthetic ones

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

    // ---- Reading scope: Books / Comics / Manga are three catalogues sharing ONE routing kind -------------
    //
    // Every reading row is kind "document" (an EPUB, a PDF, a comic issue and a manga chapter are all opened
    // by the reader stack), so the per-catalogue Recent folder filtered on kind alone showed all three
    // catalogues the same list: opening Comics > Recent listed your novels. The scope is the marker's second
    // slot, exactly as a console is for games, and it reads RecentItem::form.
    QList<RecentItem> reads;
    { RecentItem r; r.path = "C:/b/novel.epub";  r.title = "Novel";  r.kind = "document"; r.form = "book";  reads << r; }
    { RecentItem r; r.path = "C:/c/issue1.cbz";  r.title = "Issue";  r.kind = "document"; r.form = "comic"; reads << r; }
    { RecentItem r; r.path = "C:/m/chap1.cbz";   r.title = "Chapter";r.kind = "document"; r.form = "manga"; reads << r; }

    auto allReading = browse::recentsCatalog(reads, "document");
    CHECK(allReading.items.size() == 3, "recents: unscoped reading marker keeps every form");
    auto comics = browse::recentsCatalog(reads, "document|comic");
    CHECK(comics.items.size() == 1 && comics.items[0].title == "Issue", "recents: comics scope excludes books");
    auto books = browse::recentsCatalog(reads, "document|book");
    CHECK(books.items.size() == 1 && books.items[0].title == "Novel", "recents: books scope excludes comics");
    auto manga = browse::recentsCatalog(reads, "document|manga");
    CHECK(manga.items.size() == 1 && manga.items[0].title == "Chapter", "recents: manga scope is its own");

    // A row written before `form` existed. It is classified by the FILE and only as far as the file honestly
    // says: comic-shaped matches BOTH comic and manga (the manga provider serves chapters as CBZ, so the
    // extension cannot tell them apart and must not pretend to), book-shaped matches Books, and a path that
    // says nothing stays visible in all three rather than vanishing out of every catalogue.
    QList<RecentItem> legacy;
    { RecentItem r; r.path = "C:/old/backissue.cbz"; r.title = "OldCbz";  r.kind = "document"; legacy << r; }
    { RecentItem r; r.path = "C:/old/oldnovel.epub"; r.title = "OldEpub"; r.kind = "document"; legacy << r; }
    { RecentItem r; r.path = "https://host/a1b2c3";  r.title = "OldHash"; r.kind = "document"; legacy << r; }
    auto lComic = browse::recentsCatalog(legacy, "document|comic");
    CHECK(lComic.items.size() == 2, "recents: legacy cbz + unknowable show under Comics");
    auto lManga = browse::recentsCatalog(legacy, "document|manga");
    CHECK(lManga.items.size() == 2, "recents: a legacy cbz is comic-shaped, so Manga keeps it too");
    auto lBook = browse::recentsCatalog(legacy, "document|book");
    CHECK(lBook.items.size() == 2 && lBook.items[0].title == "OldEpub",
          "recents: legacy epub + unknowable show under Books, the cbz does not");

    // The Downloaded folder carries the identical filter and had the identical blur.
    QList<DownloadedItem> readDls;
    { DownloadedItem d; d.path = "C:/dl/novel.epub"; d.title = "DNovel"; d.kind = "document"; d.form = "book";  readDls << d; }
    { DownloadedItem d; d.path = "C:/dl/issue.cbz";  d.title = "DIssue"; d.kind = "document"; d.form = "comic"; readDls << d; }
    auto always = [](const QString&) { return true; };
    auto dComics = browse::downloadsCatalog(readDls, "document|comic", always);
    CHECK(dComics.items.size() == 1 && dComics.items[0].title == "DIssue",
          "downloads: comics scope excludes books");
    auto dBooks = browse::downloadsCatalog(readDls, "document|book", always);
    CHECK(dBooks.items.size() == 1 && dBooks.items[0].title == "DNovel",
          "downloads: books scope excludes comics");
    auto dAll = browse::downloadsCatalog(readDls, "document|", always);
    CHECK(dAll.items.size() == 2, "downloads: unscoped reading marker keeps every form");

    // The oracle itself, at the two boundaries the catalogues actually cross.
    CHECK(core::readingForm("comic_issue") == "comic" && core::readingForm("manga_chapter") == "manga"
          && core::readingForm("ebook") == "book" && core::readingForm("movie").isEmpty(),
          "readingForm: the three reading forms, and nothing else");
    CHECK(core::matchesReadingScope("", "x.cbz", "manga") && core::matchesReadingScope("", "x.cbz", "comic")
          && !core::matchesReadingScope("", "x.cbz", "book"),
          "matchesReadingScope: comic-shaped is comic OR manga, never book");


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

    // ---- The gate a MIGRATED PC favourite has to satisfy ---------------------------------------------------
    // A merged PC game has no path on purpose (which copy runs is decided at activation), so its id gets it
    // past the path test — but `system` is a SECOND key, and this folder is scoped on it. That is why
    // pcgame::applyRemap stamps a migrated record rather than only rewriting its id: the legacy per-launcher
    // star carried no system, and an id-only migration produces the record below that this folder DROPS,
    // which the user reads as their star having been deleted. probe_pcgames pins the stamp; this pins the
    // rule the stamp exists for, so neither half can be "fixed" by weakening the other.
    {
        QList<FavoriteItem> pcf;
        { FavoriteItem f; f.itemId = "pcgame:hades"; f.title = "Hades";
          f.system = "pc"; f.kind = "pcgame"; pcf << f; }              // migrated AND stamped
        { FavoriteItem f; f.itemId = "pcgame:hades ii"; f.title = "Hades II"; pcf << f; } // id rewritten ONLY
        const MediaCatalog folder = browse::favoritesCatalog(pcf, "pc");
        CHECK(folder.items.size() == 1 && folder.items[0].id == "pcgame:hades"
              && folder.items[0].url.isEmpty() && folder.items[0].mime == "pcgame",
              "favorites: a stamped path-less merged PC favourite IS listed in the pc folder");
        CHECK(folder.items.size() == 1,
              "favorites: an id-only migrated favourite (no system) is INVISIBLE here — hence the stamp");
    }

    // ---- Favorites write side: starring a local game must stamp the console (else the per-console ----------
    // ---- ★ Favorites folder never matches it). Hint (from the Recent/Downloads store) wins; a ROM ----------
    // ---- extension is the fallback; PC games are always "pc" (.exe would else collide with psx). ----------
    {
        MediaItem g; g.url = "C:/g/mario.nes"; g.id = "key1"; g.title = "Mario";
        g.mime = "game"; g.thumbnailUrl = "http://x/m.jpg";
        FavoriteItem f = browse::localGameFavorite(g, QString());
        CHECK(f.system == "nes" && f.path == "C:/g/mario.nes" && f.kind == "game"
              && f.itemId == "key1" && f.title == "Mario" && f.type == "game"
              && f.thumbnailUrl == "http://x/m.jpg",
              "favWrite: system derived from the ROM extension");
        CHECK(browse::localGameFavorite(g, "snes").system == "snes",
              "favWrite: store hint wins over the extension");
        MediaItem pc; pc.url = "C:/pc/doom.exe"; pc.title = "Doom"; pc.mime = "pcgame";
        FavoriteItem fp = browse::localGameFavorite(pc, QString());
        CHECK(fp.system == "pc" && fp.kind == "pcgame", "favWrite: pcgame maps to pc, not psx(.exe)");
        MediaItem nk = g; nk.id.clear();
        CHECK(browse::localGameFavorite(nk, QString()).itemId == "C:/g/mario.nes",
              "favWrite: itemId falls back to the path (gameFavId rule)");
    }

    // ---- Favorites backfill: favourites saved before `system` was stamped get it derived from their path ---
    {
        QVector<FavoriteItem> old;
        { FavoriteItem f; f.itemId = "a"; f.path = "C:/g/zelda.sfc"; f.kind = "game"; old << f; }
        { FavoriteItem f; f.itemId = "b"; f.path = "C:/pc/doom.exe"; f.kind = "pcgame"; old << f; }
        { FavoriteItem f; f.itemId = "c"; f.path = "C:/g/mario.nes"; f.kind = "game"; f.system = "nes"; old << f; }
        { FavoriteItem f; f.itemId = "d"; old << f; } // streamed favourite: no path, stays untouched
        CHECK(FavoritesStore::backfillSystems(old), "backfill: reports a change");
        CHECK(old[0].system == "snes" && old[1].system == "pc"
              && old[2].system == "nes" && old[3].system.isEmpty(),
              "backfill: derives only the missing local-game systems");
        CHECK(!FavoritesStore::backfillSystems(old), "backfill: idempotent once stamped");

        // A store hint (the Recent/Downloads entry for the same game) outranks the extension: e.g. an
        // Atari ST ".st" disk that the extension table would read as snes (Sufami Turbo ".st").
        QVector<FavoriteItem> amb;
        { FavoriteItem f; f.itemId = "e"; f.path = "C:/g/creatures.st"; f.kind = "game"; amb << f; }
        CHECK(FavoritesStore::backfillSystems(amb, [](const FavoriteItem& f) {
                  return f.path.endsWith(QStringLiteral(".st")) ? QStringLiteral("atarist") : QString();
              }) && amb[0].system == "atarist",
              "backfill: store hint outranks the extension");
    }

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

    // ---- PC games level: SteamGame -> MediaItem mapping + the in-folder query filter -----------------------
    // (This was browse::steamGamesCatalog until the four per-launcher folders became one. The mapping it
    // pinned still matters — a Steam library entry has to reach the grid as a playable tile — it is just
    // pinned through the builder that now performs it.)
    QList<SteamGame> steam;
    { SteamGame g; g.appid = "440"; g.name = "Team Fortress 2"; steam << g; }
    { SteamGame g; g.appid = "570"; g.name = "Dota 2";          steam << g; }
    auto poster = [](const QVector<pcgame::PcGameSource>& v) {
        return v.isEmpty() ? QString() : QStringLiteral("poster:") + v.first().launchId; // inject: no I/O
    };
    auto allSteam = browse::pcGamesCatalog(steam, {}, {}, {}, {}, QString(), QString(), poster);
    CHECK(allSteam.items.size() == 2, "steam: empty query -> all installed");
    auto tf2 = browse::pcGamesCatalog(steam, {}, {}, {}, {}, "fortress", QString(), poster);
    CHECK(tf2.items.size() == 1 && tf2.items[0].id == "pcgame:team fortress 2"
          && tf2.items[0].mime == "pcgame" && tf2.items[0].type == "game"
          && tf2.items[0].title == "Team Fortress 2" && tf2.items[0].thumbnailUrl == "poster:440"
          && tf2.items[0].url.isEmpty() && tf2.items[0].pcSources.size() == 1
          && tf2.items[0].pcSources[0].launchId == "440",
          "steam: query filter -> one game with exact id/mime/poster/type mapping");

    // ---- The merged PC Games folder: ONE entry per game, every launcher a SOURCE --------------------------
    // The property this builder exists for: a game present in two launchers plus a downloaded copy is ONE
    // item with THREE sources, while "Hades" and "Hades II" across two launchers stay TWO items. Merging
    // those would remove a game from the user's library, which is strictly worse than showing it twice.
    {
        QList<SteamGame> st;
        { SteamGame g; g.appid = "1145360"; g.name = "Hades";    st << g; }
        { SteamGame g; g.appid = "2074920"; g.name = "Hades II"; st << g; }
        { SteamGame g; g.appid = "620";     g.name = "Portal 2"; st << g; }
        QList<EpicGame> ep;
        { EpicGame g; g.appName = "Pewter"; g.name = "Hades II"; ep << g; }  // the SAME sequel, second launcher
        { EpicGame g; g.appName = "Fort";   g.name = "Fortnite"; ep << g; }
        QList<GogGame> gg;
        { GogGame g; g.id = "1207658930"; g.name = "Hades"; g.exe = "C:/gog/hades.exe"; gg << g; }
        { GogGame g; g.id = "42";         g.name = "Portal 2";                          gg << g; } // no exe
        QList<BattleNetGame> bn;
        { BattleNetGame g; g.code = "wow"; g.name = "World of Warcraft";      bn << g; } // protocol launch
        { BattleNetGame g; g.name = "Hearthstone"; g.exe = "C:/bn/hs.exe";    bn << g; } // code-less, has exe
        { BattleNetGame g; g.name = "Ghost";                                  bn << g; } // code-less, NO exe
        QVector<pcgame::PcGameSource> dl;
        { pcgame::PcGameSource s; s.kind = pcgame::PcGameSource::Downloaded;
          s.addonItemId = "dl-1"; s.exePath = "C:/dl/Hades.exe"; s.ready = true;
          s.label = "HADES (2020)"; dl << s; }                    // a release name: loses the title contest
        // Injected like steamGamesCatalog's poster, so the probe stays I/O-free (the default reads the local
        // Steam librarycache).
        auto art = [](const QVector<pcgame::PcGameSource>&) { return QStringLiteral("art:x"); };

        const MediaCatalog pc = browse::pcGamesCatalog(st, ep, gg, bn, dl, QString(), QString(), art);
        auto find = [&pc](const char* t) {
            for (const MediaItem& i : pc.items) if (i.title == QString::fromLatin1(t)) return i;
            return MediaItem();
        };
        auto count = [&pc](const char* t) {
            int n = 0;
            for (const MediaItem& i : pc.items) if (i.title == QString::fromLatin1(t)) ++n;
            return n;
        };
        const MediaItem hades  = find("Hades");
        const MediaItem hades2 = find("Hades II");
        const MediaItem portal = find("Portal 2");

        // 3 Steam + 2 Epic + 2 GOG + 3 Battle.net + 1 downloaded = eleven rows in; Hades' three copies and
        // Hades II's two collapse, so seven games out.
        CHECK(pc.items.size() == 7, "pcgames: seven distinct games out of eleven library rows");
        // THE assertion: Steam + GOG + a downloaded copy collapse to one entry carrying all three.
        CHECK(count("Hades") == 1 && hades.pcSources.size() == 3,
              "pcgames: Steam + GOG + downloaded -> exactly ONE item with THREE sources");
        // A game in only one launcher is still exactly one item, with exactly one source.
        CHECK(count("Fortnite") == 1 && find("Fortnite").pcSources.size() == 1,
              "pcgames: a game in one launcher only -> one item, one source");
        // The regression that would LOSE a game: sequel numerals must not merge, even across launchers.
        CHECK(count("Hades") == 1 && count("Hades II") == 1 && !hades.id.isEmpty()
              && hades.id != hades2.id,
              "pcgames: Hades and Hades II stay TWO items with different ids");
        CHECK(hades2.pcSources.size() == 2, "pcgames: Hades II merges its Steam and Epic copies");

        // Item shape: the merged id, the ONE routing kind, and an EMPTY url (the picker resolves the launch).
        CHECK(hades.id == "pcgame:hades" && hades.mime == "pcgame" && hades.type == "game"
              && hades.url.isEmpty() && hades.systemHint == "pc" && hades.thumbnailUrl == "art:x",
              "pcgames: id/mime/type/systemHint/art set and url left EMPTY");

        // Source order: ready before not-ready, then by launcher name. Hades' three are all ready, so they
        // order by launcher — the downloaded copy has an empty launcher and leads.
        CHECK(hades.pcSources.size() == 3 && hades.pcSources[0].launcher.isEmpty()
              && hades.pcSources[1].launcher == "gog" && hades.pcSources[2].launcher == "steam",
              "pcgames: ready sources ordered by launcher name (empty launcher first)");
        // Portal 2's GOG copy has no exe, so it is NOT ready and must sort AFTER the Steam one. This is the
        // half a picker gets wrong: an unlaunchable row must never be the first thing offered.
        CHECK(portal.pcSources.size() == 2 && portal.pcSources[0].launcher == "steam"
              && portal.pcSources[0].ready && portal.pcSources[1].launcher == "gog"
              && !portal.pcSources[1].ready,
              "pcgames: a ready source sorts before a not-ready one");
        // And what Play would do with it: exactly one ready source, so no menu — and it is the ready one.
        CHECK(pcgame::pickAutoSource(portal.pcSources) == 0,
              "pcgames: pickAutoSource takes Portal 2's single READY source");

        // Launch payloads. Steam/Epic/Battle.net carry a protocol url; GOG carries an exe.
        CHECK(hades.pcSources.size() == 3 && hades.pcSources[2].launchUrl == "steam://rungameid/1145360"
              && hades.pcSources[2].launchId == "1145360"
              && hades.pcSources[1].exePath == "C:/gog/hades.exe"
              && hades.pcSources[1].launchUrl.isEmpty(),
              "pcgames: steam:// url on the Steam source, an exe on the GOG one");
        CHECK(hades2.pcSources.size() == 2
              && hades2.pcSources[0].launchUrl
                     == "com.epicgames.launcher://apps/Pewter?action=launch&silent=true",
              "pcgames: the Epic source carries the launcher URI");
        CHECK(find("World of Warcraft").pcSources.size() == 1
              && find("World of Warcraft").pcSources[0].launchUrl == "battlenet://wow"
              && find("World of Warcraft").pcSources[0].label == "Battle.net"
              && find("World of Warcraft").pcSources[0].ready,
              "pcgames: a coded Battle.net title launches by protocol, labelled plainly");

        // A code-less Battle.net title is a GUESS at an exe. It must not be presented at parity with a real
        // protocol launch, and with no exe at all it must not be READY either.
        const MediaItem hs = find("Hearthstone"), ghost = find("Ghost");
        CHECK(hs.pcSources.size() == 1 && hs.pcSources[0].ready
              && hs.pcSources[0].exePath == "C:/bn/hs.exe" && hs.pcSources[0].launchUrl.isEmpty()
              && hs.pcSources[0].label != "Battle.net"
              && hs.pcSources[0].label.contains("exe"),
              "pcgames: a code-less Battle.net title is labelled a best-effort exe, not \"Battle.net\"");
        CHECK(ghost.pcSources.size() == 1 && !ghost.pcSources[0].ready
              && ghost.pcSources[0].label != hs.pcSources[0].label
              && pcgame::pickAutoSource(ghost.pcSources) == -1,
              "pcgames: a code-less Battle.net title with no exe is NOT ready (Play must ask, not no-op)");

        // Display title: a launcher's own name beats the downloaded release name (scene tokens and all),
        // while the release name is still kept verbatim on its own picker row.
        CHECK(count("HADES (2020)") == 0 && hades.title == "Hades",
              "pcgames: the launcher's name wins the display title over the release name");
        CHECK(hades.pcSources.size() == 3 && hades.pcSources[0].label == "HADES (2020)",
              "pcgames: the downloaded source keeps its own label for the picker row");

        // launcherFilter narrows WHICH GAMES appear, not which sources they carry — "what I own on Steam"
        // still launches by whichever copy is ready.
        const MediaCatalog only = browse::pcGamesCatalog(st, ep, gg, bn, dl, QString(), "steam", art);
        bool everyHasSteam = !only.items.isEmpty();
        for (const MediaItem& i : only.items)
        {
            bool s = false;
            for (const pcgame::PcGameSource& x : i.pcSources) if (x.launcher == "steam") s = true;
            if (!s) everyHasSteam = false;
        }
        CHECK(only.items.size() == 3 && everyHasSteam,
              "pcgames: launcherFilter=steam keeps only games WITH a Steam source");
        CHECK(only.items.size() == 3 && only.items[0].pcSources.size() == 3,
              "pcgames: the filter narrows games, not the sources on them");

        // query filters on the NORMALISED title, and matches any contributing title, not just the shown one.
        CHECK(browse::pcGamesCatalog(st, ep, gg, bn, dl, "hades", QString(), art).items.size() == 2,
              "pcgames: query matches on the normalised title");
        CHECK(browse::pcGamesCatalog(st, ep, gg, bn, dl, "Portal 2 - Game of the Year Edition",
                                     QString(), art).items.size() == 1,
              "pcgames: a query carrying edition noise still finds the game");
        // A query that normalises to NOTHING would be a substring of every title; it must not match all.
        CHECK(browse::pcGamesCatalog(st, ep, gg, bn, dl, "!!!", QString(), art).items.isEmpty(),
              "pcgames: a query that normalises to empty does not match the whole library");

        // Determinism: the same library in a different scan order yields the same folder and the same rows.
        QList<SteamGame> rev; for (int i = st.size() - 1; i >= 0; --i) rev << st[i];
        const MediaCatalog again = browse::pcGamesCatalog(rev, ep, gg, bn, dl, QString(), QString(), art);
        bool sameOrder = again.items.size() == pc.items.size();
        for (int i = 0; sameOrder && i < again.items.size(); ++i)
            if (again.items[i].id != pc.items[i].id) sameOrder = false;
        CHECK(sameOrder, "pcgames: item order does not depend on the order the launchers were scanned in");

        const MediaCatalog none = browse::pcGamesCatalog({}, {}, {}, {}, {}, QString(), QString(), art);
        CHECK(none.items.isEmpty() && !none.title.isEmpty() && !none.hasMore,
              "pcgames: empty input -> empty catalog with a valid title");

        // ---- THE DRIFT ASSERTION: the catalog's item id IS the remap's destination -----------------
        // Two different files decide one thing, and the user's entire per-item history rides on them
        // agreeing: the catalog decides which key a tile's favourite / marks / play time are READ under,
        // and PcGameRemap decides which key those records are MOVED to. Disagree by one character and
        // every migrated record lands where nothing will ever look for it — silently, and strictly worse
        // than not migrating at all. This is the one probe that can build BOTH sides, so the property is
        // pinned here rather than left to a reviewer noticing.
        {
            QVector<QPair<QString, QString>> lib;
            for (const SteamGame& g : st)     lib << qMakePair(QStringLiteral("steam:") + g.appid, g.name);
            for (const EpicGame& g : ep)      lib << qMakePair(QStringLiteral("epic:") + g.appName, g.name);
            for (const GogGame& g : gg)       lib << qMakePair(QStringLiteral("gog:") + g.id, g.name);
            for (const BattleNetGame& g : bn) lib << qMakePair(QStringLiteral("bnet:") + g.name, g.name);
            for (const pcgame::PcGameSource& s : dl)
                lib << qMakePair(QStringLiteral("dl:") + s.addonItemId, s.label);
            const QHash<QString, QString> t = pcgame::remapTable(lib);

            // Every destination the remap would move a record TO is an id the catalog actually builds.
            bool everyDestReachable = !t.isEmpty();
            for (auto it = t.cbegin(); it != t.cend(); ++it)
            {
                bool built = false;
                for (const MediaItem& i : pc.items) if (i.id == it.value()) { built = true; break; }
                if (!built) everyDestReachable = false;
            }
            CHECK(everyDestReachable,
                  "pcgames: every remap destination is an id the catalog actually builds");

            // ...and the mirror: every tile the catalog builds is a destination the remap would reach.
            // Without this half a remap that mapped everything onto ONE valid id would still pass above.
            bool everyTileReached = !pc.items.isEmpty();
            for (const MediaItem& i : pc.items)
            {
                bool reached = false;
                for (auto it = t.cbegin(); it != t.cend(); ++it)
                    if (it.value() == i.id) { reached = true; break; }
                if (!reached) everyTileReached = false;
            }
            CHECK(everyTileReached,
                  "pcgames: every catalog tile id is a destination the remap would move records to");
        }
    }

    // ---- Owned, not installed: its own filter / group (issue #62 part 2) ---------------------------------
    // The owned-but-not-installed games already flow into the folder as LauncherOwned sources; part 2 is
    // surfacing them as their OWN group, via the sentinel launcherFilter kPcFilterOwnedNotInstalled. It keeps
    // ONLY games whose EVERY source is a LauncherOwned one, and the menu offers the row only when the group
    // would be non-empty.
    {
        QList<SteamGame> st;
        { SteamGame g; g.appid = "620";     g.name = "Portal 2"; st << g; }
        { SteamGame g; g.appid = "1145360"; g.name = "Hades";    st << g; }
        QList<SteamGame> owned;
        { SteamGame g; g.appid = "620";    g.name = "Portal 2"; owned << g; } // installed too -> not owned-not-installed
        { SteamGame g; g.appid = "504230"; g.name = "Celeste";  owned << g; } // owned, NOT installed
        auto art = [](const QVector<pcgame::PcGameSource>&) { return QString(); };
        const QString ownedFilter = QString::fromLatin1(browse::kPcFilterOwnedNotInstalled);

        // The full folder carries all three: Portal 2 and Hades installed, Celeste owned-not-installed.
        const MediaCatalog all = browse::pcGamesCatalog(st, {}, {}, {}, {}, QString(), QString(), art, owned);
        CHECK(all.items.size() == 3,
              "owned: full folder has both installed games and the owned-not-installed one");

        // The filter keeps exactly the game whose only source is LauncherOwned, and excludes the installed ones.
        const MediaCatalog owf =
            browse::pcGamesCatalog(st, {}, {}, {}, {}, QString(), ownedFilter, art, owned);
        bool onlyOwned = owf.items.size() == 1 && owf.items[0].title == "Celeste";
        for (const MediaItem& i : owf.items)
            for (const pcgame::PcGameSource& s : i.pcSources)
                if (s.kind != pcgame::PcGameSource::LauncherOwned) onlyOwned = false;
        CHECK(onlyOwned, "owned: the filter selects exactly the LauncherOwned-only games");
        bool hasInstalled = false;
        for (const MediaItem& i : owf.items) if (i.title == "Portal 2" || i.title == "Hades") hasInstalled = true;
        CHECK(!hasInstalled,
              "owned: a game installed anywhere is excluded — even Portal 2, which is BOTH installed and owned");

        // The menu offers the group only when an owned game is not installed.
        CHECK(browse::pcLaunchersPresent(st, {}, {}, {}, owned).contains(ownedFilter),
              "owned: the menu offers the group when an owned game is uninstalled");
        QList<SteamGame> allInstalledOwned;
        { SteamGame g; g.appid = "620"; g.name = "Portal 2"; allInstalledOwned << g; } // every owned one installed
        CHECK(!browse::pcLaunchersPresent(st, {}, {}, {}, allInstalledOwned).contains(ownedFilter),
              "owned: the group is NOT offered when every owned game is already installed");
        CHECK(browse::pcLauncherLabel(ownedFilter) == "Owned, not installed",
              "owned: the sentinel labels as the owned-not-installed group");
    }

    // ---- Persisted-scan availability (issue #62 part 1, the catalog half) --------------------------------
    // A game shown from the last-good cache because its store was UNREADABLE this scan carries available=false
    // on its launcher struct. The catalog must keep showing it (it did not vanish), badge it "Unavailable?",
    // force its source NOT ready, and never auto-launch it.
    {
        QList<SteamGame> st;
        { SteamGame g; g.appid = "1"; g.name = "Live";   g.available = true;  st << g; }
        { SteamGame g; g.appid = "2"; g.name = "Cached"; g.available = false; st << g; }
        auto art = [](const QVector<pcgame::PcGameSource>&) { return QString(); };
        const MediaCatalog pc = browse::pcGamesCatalog(st, {}, {}, {}, {}, QString(), QString(), art);
        auto find = [&pc](const char* t) {
            for (const MediaItem& i : pc.items) if (i.title == QString::fromLatin1(t)) return i;
            return MediaItem();
        };
        const MediaItem live = find("Live"), cached = find("Cached");
        CHECK(pc.items.size() == 2 && live.subtitle.isEmpty() && live.pcSources.size() == 1
              && live.pcSources[0].ready && live.pcSources[0].available,
              "avail: an available installed game is ready with no badge");
        CHECK(cached.pcSources.size() == 1 && !cached.pcSources[0].ready
              && !cached.pcSources[0].available && cached.subtitle == "Unavailable?",
              "avail: an unavailable installed game still shows, is badged Unavailable?, and is not ready");
        CHECK(pcgame::pickAutoSource(cached.pcSources) == -1,
              "avail: pickAutoSource never auto-launches an unavailable copy");
    }

    // ---- THE LAUNCH ID: what a launch banks under must be what the remap migrates FROM --------------------
    // launchPcSource routes a merged tile through the per-launcher launch path, so this session's play time,
    // marks and resume land under the PRE-MERGE id — and the next refresh migrates them only if that id is a
    // key of the table populatePcGames builds. Three of the four launchers key on an id the source carries.
    // The fourth does not: a code-less Battle.net title has no product code, so its id is a NAME — and the
    // launch site used to mint it from the MERGED DISPLAY TITLE, which is the best-ranked name across every
    // launcher. For a code-less title that loses that contest the two names differ, the records accrue under
    // an id the remap never visits, and they are stranded permanently and silently. pcgame::legacyLaunchId is
    // now the one construction both sides use; this pins that it lands inside the table.
    {
        QList<SteamGame> st;
        // Same game, same normalised key, DIFFERENT raw name — and Steam outranks Battle.net, so this is the
        // name the tile shows. That divergence is the whole failure mode; without it the bug is invisible.
        { SteamGame g; g.appid = "1234"; g.name = "HEARTHSTONE™"; st << g; }
        QList<BattleNetGame> bn;
        { BattleNetGame g; g.name = "Hearthstone"; g.exe = "C:/bn/hs.exe"; bn << g; }   // no code
        auto art = [](const QVector<pcgame::PcGameSource>&) { return QString(); };
        const MediaCatalog pc = browse::pcGamesCatalog(st, {}, {}, bn, {}, QString(), QString(), art);

        MediaItem tile;
        for (const MediaItem& i : pc.items) if (i.id == "pcgame:hearthstone") tile = i;
        pcgame::PcGameSource bnetSrc;
        for (const pcgame::PcGameSource& s : tile.pcSources)
            if (s.launcher == "battlenet") bnetSrc = s;

        CHECK(pc.items.size() == 1 && tile.pcSources.size() == 2 && bnetSrc.launchId.isEmpty()
              && tile.title != "Hearthstone",
              "launchid: the premise — one merged tile whose title is NOT Battle.net's own name");

        // The candidate table, built exactly the way populatePcGames builds it.
        QVector<QPair<QString, QString>> lib;
        for (const SteamGame& g : st)     lib << qMakePair(QStringLiteral("steam:") + g.appid, g.name);
        for (const BattleNetGame& g : bn)
            lib << qMakePair(QStringLiteral("bnet:") + (g.code.isEmpty() ? g.name : g.code), g.name);
        const QHash<QString, QString> t = pcgame::remapTable(lib);

        CHECK(pcgame::legacyLaunchId(bnetSrc) == "bnet:Hearthstone",
              "launchid: a code-less Battle.net source keys on BATTLE.NET's name, not the merged title");
        CHECK(t.contains(pcgame::legacyLaunchId(bnetSrc))
              && t.value(pcgame::legacyLaunchId(bnetSrc)) == tile.id,
              "launchid: that id IS a table key, and the remap moves it onto this very tile");
        // The mirror, and the actual regression guard: the id the launch site used to mint is NOT migratable.
        CHECK(!t.contains(QStringLiteral("bnet:") + tile.title),
              "launchid: an id built from the MERGED title is in no table — the stranding this pins against");

        // Not a Battle.net special case: EVERY launcher source's launch id has to be a table key, or that
        // launcher is the next one to strand a session's play time.
        bool allMigratable = !tile.pcSources.isEmpty();
        for (const pcgame::PcGameSource& s : tile.pcSources)
        {
            if (s.launcher.isEmpty()) continue;                 // a downloaded copy keys on addonItemId
            const QString id = pcgame::legacyLaunchId(s);
            if (id.isEmpty() || t.value(id) != tile.id) allMigratable = false;
        }
        CHECK(allMigratable,
              "launchid: every launcher source's launch id maps to this tile in the remap table");
        // A source with no launcher (downloaded / addon) claims no pre-merge id at all, rather than
        // inventing one — rule 1: an id with no destination must be absent, never an empty key.
        pcgame::PcGameSource dlSrc;
        dlSrc.kind = pcgame::PcGameSource::Downloaded; dlSrc.addonItemId = "dl-1"; dlSrc.sourceName = "Hades";
        CHECK(pcgame::legacyLaunchId(dlSrc).isEmpty(),
              "launchid: a launcher-less source has NO pre-merge id (it is keyed by addonItemId)");
    }

    // ---- A TITLELESS DOWNLOAD MUST NOT BE STRANDED OFF ITS OWN TILE --------------------------------------
    // Same failure class as the Battle.net one above, on the other kind of source. DownloadedItem::title is
    // OPTIONAL, so the catalog falls back to the file's base name to have something to group on. The remap's
    // candidate was built from the raw title instead — so for a record with an EMPTY title the two sides
    // disagreed completely: the catalog built a tile keyed on the base name, while the remap's destination
    // was pcgame::itemId("") = empty, i.e. the entry was absent from the table (rule 1) and NOTHING was ever
    // migrated onto that tile. The user's marks and play time accrue under the launch id forever, on a tile
    // that shows none of them, with nothing logged.
    //
    // Both sides now call pcgame::downloadedTitle, and this pins that they land on ONE id. Reverting either
    // side to its own fallback fails this check.
    {
        // The Downloads records as the store hands them over. Three shapes: a titled one, a TITLELESS one
        // (the stranded case), and one with neither a title NOR a key — DownloadsStore documents `key` as
        // "empty -> use path", and the path is the id such a copy's launches actually bank under, so it is
        // a candidate too rather than being skipped.
        QList<DownloadedItem> dls;
        { DownloadedItem d; d.path = "C:/dl/hades/Hades.exe"; d.title = "Hades Repack";
          d.kind = "pcgame"; d.key = "dl-hades";   dls << d; }
        { DownloadedItem d; d.path = "C:/dl/Celeste.Deluxe.exe"; d.title = "";
          d.kind = "pcgame"; d.key = "dl-celeste"; dls << d; }
        { DownloadedItem d; d.path = "C:/dl/Tunic.exe"; d.title = "";
          d.kind = "pcgame"; d.key = "";           dls << d; }

        // HomeView::pcLibraryCatalog's downloaded-source construction, verbatim.
        QVector<pcgame::PcGameSource> dl;
        for (const DownloadedItem& d : dls)
        {
            pcgame::PcGameSource s;
            s.kind        = pcgame::PcGameSource::Downloaded;
            s.addonItemId = d.key.isEmpty() ? d.path : d.key;
            s.exePath     = d.path;
            s.label       = pcgame::downloadedTitle(d.title, d.path);
            s.ready       = true;
            dl << s;
        }
        auto art = [](const QVector<pcgame::PcGameSource>&) { return QString(); };
        const MediaCatalog pc = browse::pcGamesCatalog({}, {}, {}, {}, dl, QString(), QString(), art);

        // HomeView::populatePcGames' candidate list, verbatim.
        QVector<QPair<QString, QString>> lib;
        for (const DownloadedItem& d : dls)
        {
            const QString oldId = d.key.isEmpty() ? d.path : d.key;
            if (oldId.isEmpty()) continue;
            lib << qMakePair(oldId, pcgame::downloadedTitle(d.title, d.path));
        }
        const QHash<QString, QString> t = pcgame::remapTable(lib);

        CHECK(pc.items.size() == 3 && t.size() == 3,
              "dltitle: the premise — three downloaded copies, three tiles, three remap candidates");

        // THE ASSERTION. For every downloaded record: the tile that CARRIES this copy as a source, and the
        // id the remap would move this copy's records TO, are the same string. Both halves matter — a check
        // that only compared ids would pass a build that put the source on some other tile.
        bool everyCopyLandsOnItsOwnTile = !pc.items.isEmpty();
        for (const DownloadedItem& d : dls)
        {
            const QString oldId = d.key.isEmpty() ? d.path : d.key;
            const QString dest  = t.value(oldId);
            bool onTile = false;
            for (const MediaItem& i : pc.items)
                for (const pcgame::PcGameSource& s : i.pcSources)
                    if (s.addonItemId == oldId && !dest.isEmpty() && i.id == dest) onTile = true;
            if (!onTile) everyCopyLandsOnItsOwnTile = false;
        }
        CHECK(everyCopyLandsOnItsOwnTile,
              "dltitle: every downloaded copy's tile id IS the remap destination for that same copy");

        // The titleless case spelled out, because it is the one the two fallbacks got wrong and a blanket
        // loop would not say which record failed.
        {
            const QString dest = t.value(QStringLiteral("dl-celeste"));
            QString tileId;
            for (const MediaItem& i : pc.items)
                for (const pcgame::PcGameSource& s : i.pcSources)
                    if (s.addonItemId == "dl-celeste") tileId = i.id;
            CHECK(!dest.isEmpty() && !tileId.isEmpty() && dest == tileId,
                  "dltitle: an EMPTY-title download has a tile id AND a remap destination, and they are equal");
            CHECK(dest == pcgame::itemId(QStringLiteral("Celeste.Deluxe")),
                  "dltitle: ...and that id is the one derived from the file's base name, not from nothing");
        }
        // A record with no key either is still migratable — it keys on the path, exactly as its launch does.
        {
            const QString dest = t.value(QStringLiteral("C:/dl/Tunic.exe"));
            CHECK(dest == pcgame::itemId(QStringLiteral("Tunic")),
                  "dltitle: a download with NO key keys on its path, which is what its launches bank under");
        }
    }

    // ---- pcGamesCatalog: two copies from the SAME launcher must not read as identical picker rows ---------
    // The year strip merges a remake with its original ("Prey (2006)" / "Prey (2017)" both normalise to
    // "prey"). That merge is documented and deliberate. What is NOT acceptable is the consequence: both
    // sources are Steam, both ready, so pickAutoSource asks — and the menu would offer two rows with the
    // same text, over a tile titled just "Prey", leaving the user no way to reach the remake on purpose.
    {
        QList<SteamGame> st;
        { SteamGame g; g.appid = "3970";   g.name = "Prey (2006)"; st << g; }
        { SteamGame g; g.appid = "480490"; g.name = "Prey (2017)"; st << g; }
        { SteamGame g; g.appid = "620";    g.name = "Portal 2";    st << g; }
        QList<GogGame> gg;
        { GogGame g; g.id = "42"; g.name = "Portal 2"; g.exe = "C:/gog/p2.exe"; gg << g; }
        auto art = [](const QVector<pcgame::PcGameSource>&) { return QString(); };
        const MediaCatalog pc = browse::pcGamesCatalog(st, {}, gg, {}, {}, QString(), QString(), art);
        MediaItem prey, portal;
        for (const MediaItem& i : pc.items)
        {
            if (i.id == "pcgame:prey")  prey   = i;
            if (i.title == "Portal 2")  portal = i;
        }
        // Both launches survive the merge, and Play must ask rather than guess between two ready copies.
        CHECK(prey.pcSources.size() == 2 && prey.pcSources[0].launchId == "3970"
              && prey.pcSources[1].launchId == "480490"
              && pcgame::pickAutoSource(prey.pcSources) == -1,
              "pcgames: two same-launcher copies survive the merge as two ready sources");
        // THE assertion: the two rows must be distinguishable, and each must name the copy it launches.
        CHECK(prey.pcSources.size() == 2 && prey.pcSources[0].label != prey.pcSources[1].label
              && prey.pcSources[0].label.contains("Prey (2006)")
              && prey.pcSources[1].label.contains("Prey (2017)"),
              "pcgames: two Steam sources in one group get DISTINCT labels naming each copy");
        // One source per launcher is the common case and must NOT gain the suffix. Sorted by launcher name,
        // so GOG leads Steam.
        CHECK(portal.pcSources.size() == 2 && portal.pcSources[0].label == "GOG"
              && portal.pcSources[1].label == "Steam",
              "pcgames: one source per launcher keeps its plain label");
    }

    // ---- pcGamesCatalog: the title contest and launcherFilter key on KIND, not on the `launcher` string --
    // A Downloaded source may legitimately record where its copy came from. If the rules read `launcher`
    // alone, that one field would let a scene release name win the tile AND make the game answer "what I own
    // on Steam" — two different wrong claims from the same slip.
    {
        QVector<pcgame::PcGameSource> dl;
        { pcgame::PcGameSource s; s.kind = pcgame::PcGameSource::Downloaded;
          s.launcher = "steam";                       // the launcher this copy came FROM — not a Steam entry
          s.addonItemId = "dl-9"; s.exePath = "C:/dl/Control.exe"; s.ready = true;
          s.label = "CONTROL (2019)"; dl << s; }      // normalises to "control", so it merges with the GOG row
        QList<GogGame> gg;
        { GogGame g; g.id = "7"; g.name = "Control"; g.exe = "C:/gog/control.exe"; gg << g; }
        auto art = [](const QVector<pcgame::PcGameSource>&) { return QString(); };

        const MediaCatalog pc = browse::pcGamesCatalog({}, {}, gg, {}, dl, QString(), QString(), art);
        CHECK(pc.items.size() == 1 && pc.items[0].pcSources.size() == 2 && pc.items[0].title == "Control",
              "pcgames: a Downloaded source carrying launcher=\"steam\" still loses the display title");
        // The same field must not answer the ownership question either.
        CHECK(browse::pcGamesCatalog({}, {}, gg, {}, dl, QString(), "steam", art).items.isEmpty(),
              "pcgames: launcherFilter=steam ignores a Downloaded source that carries launcher=\"steam\"");
        // The real Steam entry still matches, so the filter narrowed on kind and not by ignoring launcher.
        QList<SteamGame> st;
        { SteamGame g; g.appid = "870780"; g.name = "Control"; st << g; }
        CHECK(browse::pcGamesCatalog(st, {}, gg, {}, dl, QString(), "steam", art).items.size() == 1,
              "pcgames: launcherFilter=steam still matches a real LauncherInstalled Steam source");
    }

    // ---- pcGamesCatalog degenerate titles: the id must not double-prefix, and nameless rows must not fuse --
    {
        QList<SteamGame> st;
        { SteamGame g; g.appid = "1"; g.name = "GOTY";             st << g; } // ALL edition noise -> normalises
        { SteamGame g; g.appid = "2"; g.name = "Enhanced Edition"; st << g; } // to nothing, both of them
        { SteamGame g; g.appid = "3"; g.name = "";                 st << g; } // nothing to group on at all
        const MediaCatalog pc = browse::pcGamesCatalog(st, {}, {}, {}, {}, QString(), QString(),
                                                       [](const QVector<pcgame::PcGameSource>&) {
                                                           return QString();
                                                       });
        // mergeKey ALREADY namespaces its raw-title fallback, so pcgame::itemId prefixing unconditionally
        // would emit "pcgame:pcgame:rawtitle/goty" — a different id from the one every per-item store keys on.
        CHECK(pc.items.size() == 2, "pcgames: two empty-normalising titles stay two items; the nameless one is dropped");
        CHECK(pc.items.size() == 2 && pc.items[1].id == "pcgame:rawtitle/goty"
              && !pc.items[1].id.startsWith("pcgame:pcgame:"),
              "pcgames: an empty-normalising title keeps mergeKey's own namespaced id, not a doubled prefix");
        CHECK(pc.items.size() == 2 && pc.items[0].id != pc.items[1].id,
              "pcgames: two all-noise titles do NOT collapse into one bucket");
    }

    // ---- pcgames-filter: the launcher filter's CONTROL (issue #44) ---------------------------------------
    // pcGamesCatalog's launcherFilter shipped working and probe-tested with every call site passing "" and no
    // surface offering it, so "show me what I own on Steam" — the thing the design used the filter to justify
    // deleting the four per-launcher folders for — had no replacement. These are the pure halves of the
    // control that now reaches it: which launchers may be offered, what the menu says, and the folder row.
    {
        QList<SteamGame> st; { SteamGame g; g.appid = "1"; g.name = "Hades"; st << g; }
        QList<GogGame>   gg; { GogGame g;   g.id = "2";    g.name = "Tunic"; gg << g; }

        // Only launchers this machine HAS. Offering all four always would put rows in the menu whose only
        // possible effect is to empty the folder — the common case is one store installed.
        CHECK(browse::pcLaunchersPresent(st, {}, gg, {}) == QStringList({ "steam", "gog" }),
              "pcfilter: only launchers the library actually has, in the folder's fixed order");
        CHECK(browse::pcLaunchersPresent({}, {}, {}, {}).isEmpty(),
              "pcfilter: an empty library offers no launcher rows");
        // Owned-but-not-installed Steam entries COUNT — they are Steam library entries, and "what I own on
        // Steam" is the exact phrase this feature answers. Without this the filter is missing on a machine
        // whose Steam games are all uninstalled, which is the library most in need of it. As of issue #62
        // such a library ALSO offers the dedicated "Owned, not installed" group row (`st` here is an owned
        // game with no installed copy, so the group would be non-empty).
        CHECK(browse::pcLaunchersPresent({}, {}, {}, {}, st)
                  == QStringList({ "steam", QString::fromLatin1(browse::kPcFilterOwnedNotInstalled) }),
              "pcfilter: an owned-but-not-installed Steam library offers both the Steam row and the owned group");
        // The order is FIXED, not the order the scans came back in.
        QList<EpicGame> ep; { EpicGame g; g.appName = "e"; g.name = "Fortnite"; ep << g; }
        QList<BattleNetGame> bn; { BattleNetGame g; g.code = "wow"; g.name = "WoW"; bn << g; }
        CHECK(browse::pcLaunchersPresent(st, ep, gg, bn) == QStringList({ "steam", "epic", "gog", "battlenet" }),
              "pcfilter: the launcher order does not depend on which scan returned first");

        // The menu. "All launchers" is always row 0 with an EMPTY value, so a filter that emptied the folder
        // can still be cleared; the value and the label ride one pair, so the row pressed and the filter it
        // means cannot drift apart.
        {
            const QVector<QPair<QString, QString>> all = browse::pcLauncherFilterChoices({ "steam", "gog" }, QString());
            CHECK(all.size() == 3 && all[0].first.isEmpty(),
                  "pcfilter: the menu is All + one row per available launcher, All first with an empty value");
            // Guarded on the size, not just asserted after it: CHECK records a failure and CONTINUES, so a
            // build that returns a shorter list would index past the end and take the probe down with an
            // access violation instead of printing which assertion failed. (Measured — the "drop the All
            // row" mutation did exactly that.)
            CHECK(all.size() == 3 && all[1].first == "steam" && all[2].first == "gog",
                  "pcfilter: each row's VALUE is the launcher id pcGamesCatalog takes");
            CHECK(all.size() == 3 && all[1].second.contains("Steam") && all[2].second.contains("GOG"),
                  "pcfilter: ...and each row's LABEL is the launcher's own name");
            // Exactly ONE tick, and it is on the current choice. A menu that ticked everything, or nothing,
            // would pass a size check and tell the user nothing about the state they are in.
            int ticks = 0; for (const auto& c : all) if (c.second.contains(QChar(0x2713))) ++ticks;
            CHECK(ticks == 1 && all.size() == 3 && all[0].second.contains(QChar(0x2713)),
                  "pcfilter: no filter set -> the tick is on All launchers, and only there");
        }
        {
            const QVector<QPair<QString, QString>> onGog = browse::pcLauncherFilterChoices({ "steam", "gog" }, "gog");
            int ticks = 0; for (const auto& c : onGog) if (c.second.contains(QChar(0x2713))) ++ticks;
            CHECK(ticks == 1 && onGog.size() == 3 && onGog[2].second.contains(QChar(0x2713)),
                  "pcfilter: the tick follows the current filter, and does not stay on All");
            CHECK(onGog.size() == 3 && onGog[0].first.isEmpty(),
                  "pcfilter: All launchers is still offered while a filter is active (the way back)");
        }
        {
            // The stale filter: set to a launcher whose games have since gone. The row must still be there,
            // ticked, or the folder is empty while the menu claims "All launchers" — two surfaces disagreeing
            // about one state, with no way to see which is true.
            const QVector<QPair<QString, QString>> stale = browse::pcLauncherFilterChoices({ "steam" }, "gog");
            bool hasGog = false; for (const auto& c : stale) if (c.first == "gog") hasGog = true;
            CHECK(hasGog, "pcfilter: a filter on a launcher that has gone is still shown, not silently dropped");
            int ticks = 0; for (const auto& c : stale) if (c.second.contains(QChar(0x2713))) ++ticks;
            CHECK(ticks == 1, "pcfilter: ...and it is the one ticked row");
        }
        // An unknown launcher id has no name, so it is not offered — a nameless row can only confuse.
        CHECK(browse::pcLauncherLabel("itch").isEmpty(), "pcfilter: an unknown launcher id has no label");
        {
            const QVector<QPair<QString, QString>> odd = browse::pcLauncherFilterChoices({ "itch" }, QString());
            CHECK(odd.size() == 1, "pcfilter: a launcher with no name is not offered as a row");
        }

        // The folder row. Its type is what HomeView routes on, and it must NOT look like a media item — a
        // '_' type keeps it out of the library-management verbs, which have no marks key for it.
        {
            const MediaItem all = browse::pcLauncherFilterRow(QString());
            CHECK(all.type == "_pcfilter" && all.id == "_pcfilter" && all.mime == "pcfilter:",
                  "pcfilter: the control row carries the routing type/id/mime HomeView dispatches on");
            CHECK(all.url.isEmpty(), "pcfilter: the control row has no url (a url would make it open as a file)");
            CHECK(all.subtitle.isEmpty(), "pcfilter: with no filter set the row says nothing extra");
            const MediaItem one = browse::pcLauncherFilterRow("steam");
            CHECK(one.title.contains("Steam") && one.title != all.title,
                  "pcfilter: the row NAMES the active launcher, so a filtered folder is never unexplained");
            CHECK(!one.subtitle.isEmpty(),
                  "pcfilter: ...and says the folder is narrowed — games 'missing' with no cause is the failure");
            CHECK(one.mime == "pcfilter:steam", "pcfilter: the row carries the active filter in its mime");
        }
    }

    // ---- pcgames-override: the user's "these are / aren't the same game" verdict reaches the FOLDER -------
    // pcgame::setOverride and pcgame::sameGame shipped probe-tested with zero callers (issue #44); the
    // grouping key was pcgame::itemId alone, so the escape hatch the design named as the thing that makes a
    // fuzzy heuristic shippable could not change anything the user sees. The folder now groups on
    // pcgame::effectiveItemId, and this is where that is pinned END TO END — a verdict written through the
    // store the UI writes through, then a catalog built from a real library.
    //
    // No test seam is involved: every probe_* target compiles with EB_ISOLATED_DATA_DIR, so this process's
    // ini starts empty and is its own. Each block clears its verdict afterwards, because the store PERSISTS
    // and a later section reading an earlier one's leftovers is a documented past failure in this area.
    //
    // THE TRAP, named because it is the easy mistake here: "the game appears once" passes just as well on a
    // build that fuses the WHOLE library into a single tile, and "it appears twice" passes on one that
    // splits everything. Every block below therefore also asserts on the games that must NOT move —
    // "Hades" vs "Hades II" above all, since fusing those deletes a game from the library.
    {
        QList<SteamGame> st;
        { SteamGame g; g.appid = "3970";    g.name = "Prey";       st << g; }  // 2006
        { SteamGame g; g.appid = "480490";  g.name = "Prey (2017)"; st << g; } // the remake: same merge key
        { SteamGame g; g.appid = "1145360"; g.name = "Hades";      st << g; }
        { SteamGame g; g.appid = "2074920"; g.name = "Hades II";   st << g; }
        auto art = [](const QVector<pcgame::PcGameSource>&) { return QString(); };
        auto idsOf = [](const MediaCatalog& c) {
            QSet<QString> s; for (const MediaItem& i : c.items) s.insert(i.id); return s;
        };
        auto titled = [](const MediaCatalog& c, const char* t) {
            int n = 0; for (const MediaItem& i : c.items) if (i.title == QString::fromLatin1(t)) ++n; return n;
        };

        // The PREMISE. Without this the whole section could be passing because the two Prey copies never
        // merged in the first place, and the separate verdict below would be proving nothing.
        const MediaCatalog before = browse::pcGamesCatalog(st, {}, {}, {}, {}, QString(), QString(), art);
        CHECK(before.items.size() == 3, "pcgames-override: the premise — the two Prey copies fuse into one tile");
        CHECK(titled(before, "Prey") == 1 && titled(before, "Hades") == 1 && titled(before, "Hades II") == 1,
              "pcgames-override: ...and Hades / Hades II are already two separate tiles");
        QString preyId, hadesId, hades2Id;
        for (const MediaItem& i : before.items)
        {
            if (i.title == "Prey")     { preyId = i.id;  CHECK(i.pcSources.size() == 2,
                  "pcgames-override: the fused Prey tile carries BOTH copies as sources"); }
            if (i.title == "Hades")    hadesId  = i.id;
            if (i.title == "Hades II") hades2Id = i.id;
        }

        // ---- SEPARATE ------------------------------------------------------------------------------
        pcgame::setOverride(QStringLiteral("Prey"), QStringLiteral("Prey (2017)"), false);
        const MediaCatalog sep = browse::pcGamesCatalog(st, {}, {}, {}, {}, QString(), QString(), art);
        CHECK(sep.items.size() == 4, "pcgames-override: separating the key yields one tile per copy");
        CHECK(titled(sep, "Prey") == 1 && titled(sep, "Prey (2017)") == 1,
              "pcgames-override: each separated copy is named by its OWN title, not the merged one");
        // Identity, not just count: two tiles with DIFFERENT non-empty ids, each carrying exactly its own
        // copy. A build that emitted the same id twice, or an empty one, would pass a size check alone.
        {
            // Looked up UNCONDITIONALLY rather than asserted inside a loop over whatever tiles exist: a
            // build that never separated at all has no "Prey (2017)" row, the loop body never runs, and an
            // in-loop CHECK silently passes. (Measured — that is exactly what the "ignore the separate
            // verdict" mutation did to this block before it was written this way.)
            auto byTitle = [&sep](const char* t) {
                for (const MediaItem& i : sep.items) if (i.title == QString::fromLatin1(t)) return i;
                return MediaItem();
            };
            const MediaItem a2006 = byTitle("Prey"), a2017 = byTitle("Prey (2017)");
            CHECK(a2006.pcSources.size() == 1 && a2006.pcSources[0].launchId == "3970",
                  "pcgames-override: the separated 2006 tile exists and carries only the 2006 copy");
            CHECK(a2017.pcSources.size() == 1 && a2017.pcSources[0].launchId == "480490",
                  "pcgames-override: the separated remake tile exists and carries only the remake copy");
            CHECK(!a2006.id.isEmpty() && !a2017.id.isEmpty() && a2006.id != a2017.id,
                  "pcgames-override: the two separated tiles have distinct, real ids");
            CHECK(a2006.id.startsWith("pcgame:") && a2017.id.startsWith("pcgame:"),
                  "pcgames-override: a separated id is still a pcgame: id (favourites/marks key on the prefix)");
        }
        // THE NEGATIVE HALF. Everything the user did not point at is untouched, by id — this is what fails
        // on a build that separates the whole library instead of one key.
        CHECK(idsOf(sep).contains(hadesId) && idsOf(sep).contains(hades2Id),
              "pcgames-override: separating one key leaves every other tile's id EXACTLY as it was");
        CHECK(titled(sep, "Hades") == 1 && titled(sep, "Hades II") == 1 && hadesId != hades2Id,
              "pcgames-override: Hades and Hades II are still two games after a separate verdict");

        // The remap sends each copy's records to the tile that copy is actually on. Cheap to get wrong and
        // invisible when it is: the records land on an id no tile carries and the user's hours vanish.
        {
            QVector<QPair<QString, QString>> lib;
            for (const SteamGame& g : st) lib << qMakePair(QStringLiteral("steam:") + g.appid, g.name);
            const QHash<QString, QString> t = pcgame::remapTable(lib);
            for (const MediaItem& i : sep.items)
                for (const pcgame::PcGameSource& s : i.pcSources)
                    CHECK(t.value(pcgame::legacyLaunchId(s)) == i.id,
                          "pcgames-override: every separated copy's remap destination is the tile it is on");
            CHECK(t.value("steam:3970") != t.value("steam:480490"),
                  "pcgames-override: ...and the two copies do NOT share one destination");
        }

        pcgame::clearOverride(QStringLiteral("Prey"), QStringLiteral("Prey (2017)"));
        const MediaCatalog undone = browse::pcGamesCatalog(st, {}, {}, {}, {}, QString(), QString(), art);
        CHECK(undone.items.size() == 3 && idsOf(undone).contains(preyId),
              "pcgames-override: clearing the verdict restores the ORIGINAL tile, id and all");

        // ---- FUSE ----------------------------------------------------------------------------------
        // Two spellings the title heuristic cannot join. The premise is checked first, again so the block
        // cannot pass by them never having been apart.
        QList<SteamGame> ff = st;
        { SteamGame g; g.appid = "1462040"; g.name = "Final Fantasy VII Remake"; ff << g; }
        QList<EpicGame> ep;
        { EpicGame g; g.appName = "ff7r"; g.name = "FF7 Remake"; ep << g; }
        const MediaCatalog apart = browse::pcGamesCatalog(ff, ep, {}, {}, {}, QString(), QString(), art);
        CHECK(apart.items.size() == 5, "pcgames-override: the premise — two spellings are two tiles");

        pcgame::setOverride(QStringLiteral("Final Fantasy VII Remake"), QStringLiteral("FF7 Remake"), true);
        const MediaCatalog fused = browse::pcGamesCatalog(ff, ep, {}, {}, {}, QString(), QString(), art);
        CHECK(fused.items.size() == 4, "pcgames-override: fusing two keys yields ONE tile for the game");
        {
            bool found = false;
            for (const MediaItem& i : fused.items)
                if (i.pcSources.size() == 2)
                {
                    QSet<QString> launchers;
                    for (const pcgame::PcGameSource& s : i.pcSources) launchers.insert(s.launcher);
                    if (launchers.contains("steam") && launchers.contains("epic")) found = true;
                }
            CHECK(found, "pcgames-override: the fused tile carries BOTH spellings' copies as sources");
        }
        // The negative half again, and it is the one that matters most: fusing must not be contagious.
        CHECK(idsOf(fused).contains(hadesId) && idsOf(fused).contains(hades2Id),
              "pcgames-override: fusing two keys leaves every other tile's id EXACTLY as it was");
        CHECK(titled(fused, "Hades") == 1 && titled(fused, "Hades II") == 1,
              "pcgames-override: Hades and Hades II are still two games after a fuse verdict");
        CHECK(titled(fused, "Prey") == 1,
              "pcgames-override: an unrelated fused key does not re-separate the Prey tile");
        {
            QVector<QPair<QString, QString>> lib;
            for (const SteamGame& g : ff) lib << qMakePair(QStringLiteral("steam:") + g.appid, g.name);
            for (const EpicGame& g : ep)  lib << qMakePair(QStringLiteral("epic:") + g.appName, g.name);
            const QHash<QString, QString> t = pcgame::remapTable(lib);
            CHECK(t.value("steam:1462040") == t.value("epic:ff7r") && !t.value("epic:ff7r").isEmpty(),
                  "pcgames-override: both fused copies' records move to the SAME tile");
            CHECK(t.value("steam:1145360") != t.value("epic:ff7r"),
                  "pcgames-override: ...and an untouched game keeps its own destination");
        }
        pcgame::clearOverride(QStringLiteral("Final Fantasy VII Remake"), QStringLiteral("FF7 Remake"));
        CHECK(browse::pcGamesCatalog(ff, ep, {}, {}, {}, QString(), QString(), art).items.size() == 5,
              "pcgames-override: clearing the fuse verdict returns the two tiles");
    }

    // ---- SearchAggregator dedup/skip rule: the merge path's pure helper (see SearchAggregator::onCatalogReady).
    {
        QSet<QString> seen;
        MediaItem a; a.title = "Halo"; a.type = "game";
        CHECK(SearchAggregator::acceptResult(a, seen), "search: first result accepted");
        MediaItem dup; dup.title = "HALO"; dup.type = "GAME"; // same title|type, different case
        CHECK(!SearchAggregator::acceptResult(dup, seen),
              "search: duplicate title|type rejected case-insensitively");
        MediaItem diffType; diffType.title = "Halo"; diffType.type = "movie"; // same title, different type
        CHECK(SearchAggregator::acceptResult(diffType, seen), "search: same title different type accepted");
        MediaItem info; info.title = "A header"; info.type = "info";
        CHECK(!SearchAggregator::acceptResult(info, seen), "search: info synthetic row skipped");
        MediaItem rechdr; rechdr.title = "Recently played"; rechdr.type = "rechdr";
        CHECK(!SearchAggregator::acceptResult(rechdr, seen), "search: rechdr synthetic row skipped");
        MediaItem open; open.title = "Open a file…"; open.type = "_open";
        CHECK(!SearchAggregator::acceptResult(open, seen), "search: _open synthetic row skipped");
        MediaItem noTitle; noTitle.title = ""; noTitle.type = "game";
        CHECK(!SearchAggregator::acceptResult(noTitle, seen), "search: empty-title row skipped");
    }

    // ---- Trakt "Airing soon" (#23): ordering, the past/boundary exclusion, and the unplayable row ----------
    // Every time here is UTC, like CalendarEntry::airsAtUtc, and `nowUtc` is injected so the boundary is a
    // pinned tick rather than whatever the clock said when the suite ran.
    {
        const QDateTime now = QDateTime::fromString(QStringLiteral("2026-07-20T12:00:00Z"), Qt::ISODate);
        auto entry = [](const char* airs, const char* show, const char* imdb, int s, int e,
                        const char* poster = "") {
            CalendarEntry c;
            c.airsAtUtc = QDateTime::fromString(QString::fromLatin1(airs), Qt::ISODate);
            c.showTitle = QString::fromLatin1(show);
            c.showIds.imdb = QString::fromLatin1(imdb);
            c.season = s; c.episode = e;
            c.posterUrl = QString::fromLatin1(poster);
            return c;
        };
        // Deliberately NOT in air order, and deliberately straddling the boundary in both directions.
        // Alpha airs at 01:30 UTC ON PURPOSE: that is the shape of a US prime-time slot (Mon 20:30 CDT),
        // so anywhere west of UTC its LOCAL day is the day BEFORE its UTC day — see the day assertions.
        QVector<CalendarEntry> cal;
        cal << entry("2026-07-23T20:00:00Z", "Zeta",     "tt300", 2, 5);   // latest -> last; NO poster
        cal << entry("2026-07-19T12:00:00Z", "Past",     "tt200", 1, 1, "https://img/past.jpg");
        cal << entry("2026-07-21T01:30:00Z", "Alpha",    "tt100", 1, 4, "https://img/alpha.jpg"); // -> first
        cal << entry("2026-07-20T12:00:00Z", "Boundary", "tt400", 1, 1, "https://img/bound.jpg"); // == nowUtc
        cal << entry("2026-07-22T09:00:00Z", "NoIds",    "",      3, 10, "https://img/noids.jpg"); // unplayable
        { CalendarEntry c; c.showTitle = QStringLiteral("Undated"); c.showIds.imdb = QStringLiteral("tt500");
          c.season = 1; c.episode = 1; cal << c; }                          // invalid air time -> dropped

        const MediaCatalog cat = browse::traktCalendarCatalog(cal, now);

        CHECK(cat.items.size() == 3, "traktcal: three of six entries survive the window");
        CHECK(cat.items.size() == 3 && cat.items[0].title == "Alpha"
              && cat.items[1].title == "NoIds" && cat.items[2].title == "Zeta",
              "traktcal: sorted by air time ascending regardless of input order");
        auto has = [&cat](const char* t) {
            for (const MediaItem& i : cat.items) if (i.title == QString::fromLatin1(t)) return true;
            return false;
        };
        CHECK(!has("Past"), "traktcal: an episode that already aired is excluded");
        // The boundary is CLOSED on the past side: airsAtUtc <= nowUtc is #25's job, not this shelf's.
        CHECK(!has("Boundary"), "traktcal: airsAtUtc exactly equal to nowUtc is excluded");
        CHECK(has("Alpha") && has("Zeta"), "traktcal: future episodes are kept");
        CHECK(!has("Undated"), "traktcal: an entry with no valid air time is dropped");

        CHECK(cat.items.size() == 3 && cat.items[0].imdbStreamId == "tt100:1:4",
              "traktcal: a show with an imdb id yields ttShow:season:episode");
        // The contract that is easiest to get wrong: "" means SHOW IT, unplayable — never skip it.
        CHECK(has("NoIds"), "traktcal: a show with no imdb id is still PRESENT in cat.items");
        CHECK(cat.items.size() == 3 && cat.items[1].imdbStreamId.isEmpty(),
              "traktcal: a show with no imdb id yields an empty imdbStreamId");
        CHECK(cat.items.size() == 3 && !cat.items[1].id.isEmpty()
              && cat.items[1].id == "trakt:NoIds:3:10",
              "traktcal: an unplayable row still has a stable synthetic identity");
        CHECK(cat.items.size() == 3 && cat.items[1].subtitle.contains(QStringLiteral("No source")),
              "traktcal: an unplayable row says so in its subtitle");
        CHECK(cat.items.size() == 3 && cat.items[0].subtitle.startsWith(QStringLiteral("S01E04"))
              && cat.items[2].subtitle.startsWith(QStringLiteral("S02E05")),
              "traktcal: subtitle leads with the zero-padded SxxEyy code");

        // ---- the DAY half of the subtitle ------------------------------------------------------------
        // Machine-independent by CONSTRUCTION, not by being left unasserted: the expectation is the LOCAL
        // conversion of the same pinned instant, computed here, so it is exact on every runner while still
        // pinning (a) the "ddd d MMM" format and (b) that the conversion happens at all. Selection stays UTC
        // — only what the user READS is local (a Tuesday-night US episode is Wednesday in UTC).
        const QDateTime alphaUtc = QDateTime::fromString(QStringLiteral("2026-07-21T01:30:00Z"), Qt::ISODate);
        const QString alphaLocalDay = alphaUtc.toLocalTime().toString(QStringLiteral("ddd d MMM"));
        CHECK(cat.items.size() == 3
              && cat.items[0].subtitle == QStringLiteral("S01E04 · ") + alphaLocalDay,
              "traktcal: subtitle day is the LOCAL calendar day of the air instant, formatted ddd d MMM");
        // And on any runner whose local day for that instant DIFFERS from its UTC day (every zone west of
        // UTC, i.e. the whole US), assert the regression directly: the UTC day must not appear. Skipped —
        // not silently weakened — on a UTC runner, where the two renderings are the same string and there
        // is nothing to tell apart; the assertion above still pins the format there.
        const QString alphaUtcDay = alphaUtc.toString(QStringLiteral("ddd d MMM"));
        if (alphaLocalDay != alphaUtcDay)
            CHECK(cat.items.size() == 3 && !cat.items[0].subtitle.contains(alphaUtcDay),
                  "traktcal: the UTC day is NOT what gets printed when it differs from the local one");

        // ---- artwork + the routing marker ------------------------------------------------------------
        // thumbnailUrl is the entry's poster VERBATIM: no MetaCache lookup (these rows are not on disk), no
        // placeholder substituted for an empty one. Checked per row so a sort that shuffled art off its
        // title would fail here rather than pass on a count.
        CHECK(cat.items.size() == 3 && cat.items[0].thumbnailUrl == "https://img/alpha.jpg"
              && cat.items[1].thumbnailUrl == "https://img/noids.jpg",
              "traktcal: thumbnailUrl is the entry's posterUrl, matched to its own row");
        CHECK(cat.items.size() == 3 && cat.items[2].thumbnailUrl.isEmpty(),
              "traktcal: an entry with no poster yields an EMPTY thumbnailUrl (no placeholder)");
        // The load-bearing string. "trakt:cal" is the ONLY thing routing activation for these rows on both
        // surfaces (HomeView::activateItem keys on it; the Home shelf has no level context to key off), and
        // it crosses a file boundary — rename it here and both surfaces fall through to an addon-less detail
        // level with the whole suite still green. Hence the literal, spelled out.
        bool allTraktMime = true;
        for (const MediaItem& i : cat.items) if (i.mime != QStringLiteral("trakt:cal")) allTraktMime = false;
        CHECK(allTraktMime, "traktcal: every row's mime is EXACTLY \"trakt:cal\" (HomeView routes on it)");

        bool allUrlless = true, allEpisodes = true;
        for (const MediaItem& i : cat.items)
        { if (!i.url.isEmpty()) allUrlless = false; if (i.type != "episode") allEpisodes = false; }
        CHECK(allUrlless, "traktcal: every item has an empty url (the resolver fills it at play time)");
        CHECK(allEpisodes, "traktcal: every item is typed episode");

        // Empty in -> a well-formed empty catalog, not a malformed one. The SURFACE gates on this being
        // empty (no shelf, no folder), so a title-less or hasMore=true result would be a real defect.
        const MediaCatalog none = browse::traktCalendarCatalog({}, now);
        CHECK(none.items.isEmpty() && !none.title.isEmpty() && !none.hasMore,
              "traktcal: empty input -> empty catalog with a valid title");
    }

    // ---- the Trakt watchlist / collection folder (#23) -------------------------------------------
    {
        auto entry = [](const char* type, const char* title, int year, const char* imdb, qint64 added) {
            TraktListEntry e;
            e.type = QString::fromLatin1(type); e.title = QString::fromLatin1(title);
            e.year = year; e.ids.imdb = QString::fromLatin1(imdb); e.addedAt = added;
            return e;
        };
        QVector<TraktListEntry> in;
        in << entry("movie", "Older Movie",  1999, "tt10001", 100)
           << entry("show",  "A Show",       2021, "tt10002", 300)
           << entry("movie", "Newest Movie", 2026, "tt10003", 500)
           << entry("movie", "No Id Movie",  2010, "",        400)
           << entry("season", "A Season",    2021, "tt10004", 900)   // no tile shape -> dropped
           << entry("movie", "",             0,    "",        900)   // no title, no id -> dropped
           << entry("movie", "",             0,    "tt10005", 200);  // no title but IDENTIFIABLE -> kept

        const MediaCatalog cat = browse::traktListCatalog(in, QStringLiteral("Trakt Watchlist"));
        CHECK(cat.title == QStringLiteral("Trakt Watchlist"),
              "traktlist: the folder is named by its caller, so one builder serves both lists");
        CHECK(cat.items.size() == 5, "traktlist: the season row and the anonymous row are dropped");

        auto idxOf = [&cat](const QString& t) {
            for (int i = 0; i < cat.items.size(); ++i) if (cat.items[i].title == t) return i;
            return -1;
        };
        // ORDER: most recently added first. Asserted as the full sequence rather than as "the first row
        // is X", so a comparator that got only the top of the list right still fails.
        CHECK(cat.items[0].title == QStringLiteral("Newest Movie")   // 500
              && cat.items[1].title == QStringLiteral("No Id Movie") // 400
              && cat.items[2].title == QStringLiteral("A Show")      // 300
              && cat.items[3].title.isEmpty()                        // 200, the titleless tt10005
              && cat.items[4].title == QStringLiteral("Older Movie"),// 100
              "traktlist: rows are ordered most-recently-added first");
        // The titleless-but-identifiable row survives. This is the half that discriminates the
        // admissibility rule: dropping a row missing EITHER a title or an id (rather than BOTH) loses a
        // watchlist entry the app can actually resolve, which is the expensive direction.
        CHECK(cat.items[3].imdbStreamId == QStringLiteral("tt10005"),
              "traktlist: a row with no title but a usable id is KEPT and stays playable");

        // A MOVIE carries the stream id...
        CHECK(cat.items[idxOf(QStringLiteral("Newest Movie"))].imdbStreamId == QStringLiteral("tt10003"),
              "traktlist: a movie row carries its IMDB id as the stream id");
        // ...a SHOW carries NONE, even though tt10002 is a perfectly good show id. Carrying it would
        // build a row that looks playable and can only dead-end, because the stream bridge resolves
        // "tt123" and "ttShow:S:E" and nothing in between. The show row is routed by its mime instead.
        CHECK(cat.items[idxOf(QStringLiteral("A Show"))].imdbStreamId.isEmpty(),
              "traktlist: a SHOW row carries no stream id even when Trakt gave a good show id");
        CHECK(cat.items[idxOf(QStringLiteral("A Show"))].type == QStringLiteral("series")
              && cat.items[idxOf(QStringLiteral("Newest Movie"))].type == QStringLiteral("movie"),
              "traktlist: a show is typed series and a movie is typed movie");

        // The load-bearing strings: the two mime markers ARE the routing contract, and they must
        // DIFFER, because the two kinds of row do different things when pressed.
        CHECK(cat.items[idxOf(QStringLiteral("A Show"))].mime
                  == QLatin1String(browse::kTraktListShowMime)
              && cat.items[idxOf(QStringLiteral("Newest Movie"))].mime
                  == QLatin1String(browse::kTraktListMovieMime),
              "traktlist: each row's mime is exactly the marker for its kind");
        CHECK(QLatin1String(browse::kTraktListShowMime) != QLatin1String(browse::kTraktListMovieMime),
              "traktlist: the show and movie markers are DIFFERENT strings (they route differently)");

        // Subtitles say the year, which kind of row it is, and — for a movie with no id — that it
        // cannot be played, rather than leaving a row that silently does nothing.
        CHECK(cat.items[idxOf(QStringLiteral("Older Movie"))].subtitle
                  == QStringLiteral("1999 · Movie"),
              "traktlist: a playable movie's subtitle is year + kind, and says nothing more");
        CHECK(cat.items[idxOf(QStringLiteral("No Id Movie"))].subtitle
                  == QStringLiteral("2010 · Movie · No source"),
              "traktlist: a movie with no usable id says so");
        CHECK(cat.items[idxOf(QStringLiteral("A Show"))].subtitle == QStringLiteral("2021 · Show"),
              "traktlist: a show's subtitle never claims 'No source' — it searches, it does not play");

        bool allUrlless = true;
        for (const MediaItem& i : cat.items) if (!i.url.isEmpty()) allUrlless = false;
        CHECK(allUrlless, "traktlist: every row has an empty url (else the generic branch claims it)");

        // Identity: a row with no stream id still gets a stable, non-empty key it can be focused and
        // marked under, and two rows never collide on it.
        CHECK(!cat.items[idxOf(QStringLiteral("No Id Movie"))].id.isEmpty()
              && !cat.items[idxOf(QStringLiteral("A Show"))].id.isEmpty(),
              "traktlist: an unplayable row still has an identity");
        {
            QSet<QString> ids;
            for (const MediaItem& i : cat.items) ids.insert(i.id);
            CHECK(ids.size() == cat.items.size(), "traktlist: row ids are unique");
        }
        // The collision that makes the TYPE part of a synthetic id load-bearing: a film and a series
        // really can share a title, and an id-less pair of them would otherwise land on ONE key — so
        // focusing, marking or hiding either would silently do it to both.
        {
            QVector<TraktListEntry> clash;
            clash << entry("movie", "Fargo", 1996, "", 10)
                  << entry("show",  "Fargo", 2014, "", 10);
            const MediaCatalog c = browse::traktListCatalog(clash, QStringLiteral("T"));
            CHECK(c.items.size() == 2, "traktlist: a same-titled film and series are two rows");
            CHECK(c.items[0].id != c.items[1].id,
                  "traktlist: ...with DIFFERENT ids, because the kind is part of the key");
        }

        // A TOTAL order: two rows added in the same second (common — no endpoint stamps sub-second
        // times, and /sync/collection sometimes stamps none at all) must not reshuffle between runs.
        {
            QVector<TraktListEntry> tie;
            tie << entry("movie", "Bravo", 2000, "tt2", 0)
                << entry("movie", "Alpha", 2000, "tt1", 0)
                << entry("movie", "Alpha", 2000, "tt3", 0);
            const MediaCatalog a = browse::traktListCatalog(tie, QStringLiteral("T"));
            std::reverse(tie.begin(), tie.end());
            const MediaCatalog b = browse::traktListCatalog(tie, QStringLiteral("T"));
            CHECK(a.items.size() == 3 && a.items[0].imdbStreamId == QStringLiteral("tt1")
                  && a.items[1].imdbStreamId == QStringLiteral("tt3")
                  && a.items[2].imdbStreamId == QStringLiteral("tt2"),
                  "traktlist: equal timestamps break on title then id, not on input order");
            bool same = a.items.size() == b.items.size();
            for (int i = 0; same && i < a.items.size(); ++i) same = a.items[i].id == b.items[i].id;
            CHECK(same, "traktlist: reversing the input yields the identical folder");
        }

        // Empty in -> a well-formed empty catalog. The SURFACE gates the folder on this being empty.
        const MediaCatalog nolist = browse::traktListCatalog({}, QStringLiteral("Trakt Collection"));
        CHECK(nolist.items.isEmpty() && nolist.title == QStringLiteral("Trakt Collection")
              && !nolist.hasMore,
              "traktlist: empty input -> empty catalog with the caller's title");

        // THE EQUIVALENCE. The video root asks only "is there a folder to draw", on every navigation,
        // and used to answer it by building and fully sorting the whole catalog — thousands of rows for
        // a real watchlist — to compare a size against zero. traktListHasRows answers it directly, and
        // the ONLY thing that makes that safe is that it applies the same admissibility rule: a
        // predicate that said yes where the builder produces nothing gives a folder that opens onto an
        // empty list, and one that said no where it produces rows hides the folder entirely.
        //
        // Asserted as a PROPERTY over a table containing both answers, rather than as two facts, so a
        // change to either side that does not change the other is a failure.
        const QVector<QVector<TraktListEntry>> kCases = {
            {},                                                            // nothing at all
            in,                                                            // the mixed batch above: 5 rows
            { entry("season", "A Season", 2021, "tt10004", 900) },         // only undrawable rows
            { entry("movie", "", 0, "", 900) },                            // no title AND no id
            { entry("person", "Someone", 0, "tt1", 1) },                   // a type with no tile shape
            { entry("movie", "", 0, "tt10005", 200) },                     // no title but identifiable
            { entry("show", "A Show", 2021, "", 300) },                    // a show with no id: drawable
            { entry("season", "S", 0, "tt1", 1), entry("movie", "M", 0, "tt2", 2) }, // one of each
        };
        for (const QVector<TraktListEntry>& c : kCases)
            CHECK(browse::traktListHasRows(c)
                      == !browse::traktListCatalog(c, QStringLiteral("T")).items.isEmpty(),
                  "traktlist: hasRows agrees with the builder on every case");
        // ...and the property is not vacuous: the table really does contain both answers.
        CHECK(browse::traktListHasRows(in), "traktlist: hasRows says yes for a list with rows");
        CHECK(!browse::traktListHasRows({}), "traktlist: hasRows says no for an empty list");
        CHECK(!browse::traktListHasRows({ entry("season", "A Season", 2021, "tt10004", 900) }),
              "traktlist: hasRows says no for a list of rows the builder drops");
    }

    // ---- Trakt "You missed" (#25): the tile, the shelf cap, and the marker the press reads back ----------
    // The SELECTION rule is probe_trakt's (it is a join over two callbacks and belongs nowhere near a
    // catalog). What is asserted here is everything the rule hands to the surface and everything the
    // surface hands back — plus one property only this file can state, because it is the only place both
    // Trakt calendar builders exist: that the two of them PARTITION the calendar.
    {
        auto row = [](const char* showKey, const char* title, int s, int e, const char* airedIso,
                      const char* latestIso, int count, const char* poster = "") {
            trakt::MissedRow r;
            r.showKey  = QString::fromLatin1(showKey);
            r.showTitle = QString::fromLatin1(title);
            r.showIds.imdb = r.showKey;
            r.season = s; r.episode = e;
            r.streamId = QStringLiteral("%1:%2:%3").arg(r.showKey).arg(s).arg(e);
            r.posterUrl = QString::fromLatin1(poster);
            r.airedAtUtc     = QDateTime::fromString(QString::fromLatin1(airedIso), Qt::ISODate);
            r.latestAiredUtc = QDateTime::fromString(QString::fromLatin1(latestIso), Qt::ISODate);
            r.count = count;
            return r;
        };
        // Alpha stands for three episodes; Beta for one. Alpha's oldest aired at 01:30 UTC ON PURPOSE —
        // the US prime-time shape traktCalendarCatalog's day assertions use, so a UTC-formatted day would
        // print the wrong one west of UTC.
        const QVector<trakt::MissedRow> rows = {
            row("tt100", "Alpha", 1, 4, "2026-07-21T01:30:00Z", "2026-07-23T20:00:00Z", 3, "https://img/a.jpg"),
            row("tt200", "Beta",  2, 11, "2026-07-19T18:00:00Z", "2026-07-19T18:00:00Z", 1),
        };

        const MediaCatalog cat = browse::traktMissedCatalog(rows, 0);
        CHECK(cat.items.size() == 2, "missed: one tile per row, uncapped at maxRows <= 0");
        CHECK(!cat.title.isEmpty(), "missed: the catalog is named");
        CHECK(cat.items.size() == 2 && cat.items[0].title == "Alpha" && cat.items[1].title == "Beta",
              "missed: the rule's order is preserved, never re-sorted here");

        // The row plays the OLDEST episode, and its identity IS that episode's stream id — which is the
        // key ItemMarks and the Trakt backfill both write under, so marking it watched clears this row
        // with no clearing code of its own.
        CHECK(cat.items.size() == 2 && cat.items[0].imdbStreamId == "tt100:1:4",
              "missed: the tile plays the row's oldest unwatched episode");
        CHECK(cat.items.size() == 2 && cat.items[0].id == cat.items[0].imdbStreamId,
              "missed: the tile's marks identity is that same episode");
        bool allUrlless = true, allEpisodes = true, allPlayable = true;
        for (const MediaItem& i : cat.items)
        {
            if (!i.url.isEmpty()) allUrlless = false;
            if (i.type != QStringLiteral("episode")) allEpisodes = false;
            if (i.imdbStreamId.isEmpty()) allPlayable = false;
        }
        CHECK(allUrlless, "missed: every row has an empty url (else the generic branch claims it)");
        CHECK(allEpisodes, "missed: every row is typed episode");
        // Unlike the calendar and the watchlist there is NO unplayable row here: the rule dropped anything
        // it could not key, precisely so this surface never accuses the user of missing something it
        // cannot then play.
        CHECK(allPlayable, "missed: every row is playable — the rule dropped the unkeyable ones");
        CHECK(cat.items.size() == 2 && cat.items[0].thumbnailUrl == "https://img/a.jpg",
              "missed: the row's poster rides the tile");

        // The subtitle: the zero-padded code, the LOCAL day of the episode being offered, and — only when
        // the row stands for more than one — how many are behind it.
        CHECK(cat.items.size() == 2 && cat.items[0].subtitle.startsWith(QStringLiteral("S01E04")),
              "missed: subtitle leads with the zero-padded SxxEyy code");
        CHECK(cat.items.size() == 2 && cat.items[1].subtitle.startsWith(QStringLiteral("S02E11")),
              "missed: ...for a two-digit episode too");
        {
            const QString expectDay = QDateTime::fromString(QStringLiteral("2026-07-21T01:30:00Z"), Qt::ISODate)
                                          .toLocalTime().toString(QStringLiteral("ddd d MMM"));
            CHECK(cat.items.size() == 2 && cat.items[0].subtitle.contains(expectDay),
                  "missed: the day printed is the episode's LOCAL day, not its UTC one");
        }
        // "3 more waiting" would be wrong: three episodes are missed, one of which is the one named on the
        // row, so two are "more". A row that stands for one says nothing at all.
        CHECK(cat.items.size() == 2 && cat.items[0].subtitle.contains(QStringLiteral("2")),
              "missed: a grouped row says how many OTHER episodes are behind it");
        CHECK(cat.items.size() == 2 && !cat.items[0].subtitle.contains(QStringLiteral("3 ")),
              "missed: ...counting the episodes BEHIND the one it offers, not including it");
        CHECK(cat.items.size() == 2
              && cat.items[1].subtitle.count(QLatin1Char(0x00B7)) == 1,
              "missed: a row standing for one episode has no 'more waiting' clause");

        // The shelf cap takes the FIRST n, i.e. the most recent, because the rule already ordered them.
        CHECK(browse::traktMissedCatalog(rows, 1).items.size() == 1, "missed: maxRows caps the shelf");
        CHECK(browse::traktMissedCatalog(rows, 1).items.value(0).title == "Alpha",
              "missed: the cap keeps the most recent rows, never an arbitrary subset");
        CHECK(browse::traktMissedCatalog(rows, 9).items.size() == 2,
              "missed: a cap above the row count is not a floor");
        CHECK(browse::traktMissedCatalog({}, 0).items.isEmpty(), "missed: no rows, no catalog");
        CHECK(browse::traktMissedCatalog({}, 8).items.isEmpty(), "missed: ...capped or not");

        // The marker. The press has to recover WHICH show and THROUGH WHAT TIME, and the second is the one
        // that cannot be re-derived at the press: the tile shows its OLDEST episode while the dismissal
        // must cover its NEWEST, so a surface that reused the visible date would dismiss part of the group
        // and hand the rest straight back on the next rebuild.
        {
            const QString m = cat.items.value(0).mime;
            CHECK(browse::isTraktMissedMime(m), "missed: the row carries the missed marker");
            CHECK(browse::traktMissedShowKeyOf(m) == QStringLiteral("tt100"),
                  "missed: the marker round-trips the show key");
            CHECK(browse::traktMissedThroughOf(m)
                      == QDateTime::fromString(QStringLiteral("2026-07-23T20:00:00Z"), Qt::ISODate).toSecsSinceEpoch(),
                  "missed: the marker carries the NEWEST air time, not the one the tile shows");
            CHECK(browse::traktMissedThroughOf(m)
                      != QDateTime::fromString(QStringLiteral("2026-07-21T01:30:00Z"), Qt::ISODate).toSecsSinceEpoch(),
                  "missed: ...and those two really are different in this fixture");
            // Nothing else answers to it, including the app's other Trakt markers — a mime test that also
            // matched the calendar's rows would route them into a dismissal they have no key for.
            for (const char* other : { "trakt:cal", "trakt:list:movie", "trakt:list:show", "video/mp4", "" })
            {
                CHECK(!browse::isTraktMissedMime(QString::fromLatin1(other)),
                      "missed: a non-missed mime is not claimed by the marker test");
                CHECK(browse::traktMissedShowKeyOf(QString::fromLatin1(other)).isEmpty(),
                      "missed: ...and yields no show key");
                CHECK(browse::traktMissedThroughOf(QString::fromLatin1(other)) == 0,
                      "missed: ...and no stamp");
            }
            // A marker whose stamp field is not a number fails CLOSED — 0 is "never dismissed", so the
            // press does nothing, rather than filing a dismissal through the epoch.
            CHECK(browse::traktMissedThroughOf(QStringLiteral("trakt:missed:tt100:bogus")) == 0,
                  "missed: an unparseable stamp reads as 0, not as a dismissal through the epoch");
            CHECK(browse::traktMissedShowKeyOf(QStringLiteral("trakt:missed:tt100:bogus")) == "tt100",
                  "missed: ...and the show key is still recovered from it");
        }

        // THE PARTITION. The two Trakt calendar surfaces must between them claim every entry exactly once:
        // the same episode on both is a duplicate the user has to reason about, and on neither is an
        // episode that silently vanishes. Only this file can assert it — probe_trakt has no catalog builder
        // and no other probe has both — and the boundary tick is where it would break, so the fixture puts
        // an episode exactly there.
        {
            const QDateTime now = QDateTime::fromString(QStringLiteral("2026-07-20T12:00:00Z"), Qt::ISODate);
            auto ce = [](const char* airs, const char* show, const char* imdb, int s, int e) {
                CalendarEntry c;
                c.airsAtUtc = QDateTime::fromString(QString::fromLatin1(airs), Qt::ISODate);
                c.showTitle = QString::fromLatin1(show);
                c.showIds.imdb = QString::fromLatin1(imdb);
                c.season = s; c.episode = e;
                return c;
            };
            QVector<CalendarEntry> all;
            all << ce("2026-07-23T20:00:00Z", "Future",   "tt900", 1, 1);   // ahead  -> Airing Soon
            all << ce("2026-07-20T12:00:00Z", "OnTheTick","tt901", 1, 1);   // exactly now -> You Missed
            all << ce("2026-07-19T12:00:00Z", "Behind",   "tt902", 1, 1);   // aired  -> You Missed
            const MediaCatalog soon = browse::traktCalendarCatalog(all, now);
            const QVector<trakt::MissedRow> missedRows =
                trakt::planMissed(all, now, trakt::kMissedLookbackDays,
                                  [](const QString&) { return trakt::LocalState::Unmarked; },
                                  [](const QString&) { return qint64(0); });
            const MediaCatalog missed = browse::traktMissedCatalog(missedRows, 0);
            CHECK(soon.items.size() + missed.items.size() == all.size(),
                  "missed/traktcal: every calendar entry lands on exactly one of the two surfaces");
            auto titles = [](const MediaCatalog& c) {
                QStringList t; for (const MediaItem& i : c.items) t << i.title; t.sort(); return t;
            };
            CHECK(titles(soon) == QStringList{ QStringLiteral("Future") },
                  "missed/traktcal: only the future entry is 'Airing Soon'");
            CHECK(titles(missed) == (QStringList{ QStringLiteral("Behind"), QStringLiteral("OnTheTick") }),
                  "missed/traktcal: the aired ones — INCLUDING the one on the exact tick — are 'You Missed'");
        }
    }

    // ---- Photos browse (issue #102): the browse half of the photo feature -----------------------------------
    // Fixtures are hand-built PhotoEntry lists — folder/path set by hand, independent of any scan — so the
    // expectations below are computed by inspection, never by running the function under test.
    {
        // A library spanning two folders. Within each folder the FIRST entry is the intended cover.
        QVector<PhotoLibrary::PhotoEntry> two;
        { PhotoLibrary::PhotoEntry e; e.folder = "/lib/album"; e.path = "/lib/album/a1.jpg"; two << e; }
        { PhotoLibrary::PhotoEntry e; e.folder = "/lib/album"; e.path = "/lib/album/a2.jpg"; two << e; }
        { PhotoLibrary::PhotoEntry e; e.folder = "/lib/trip";  e.path = "/lib/trip/b1.png";  two << e; }

        const MediaCatalog cat = browse::photosCatalog(two);
        CHECK(cat.title == QStringLiteral("Photos"), "photos: top-level title");
        // Two folders => a row per folder (NOT a flat grid), in folder-path order (album before trip).
        CHECK(cat.items.size() == 2, "photos: two folders -> two folder rows");
        CHECK(cat.items.size() == 2 && cat.items[0].type == QStringLiteral("_photofolder")
              && cat.items[1].type == QStringLiteral("_photofolder"), "photos: multi-folder rows are _photofolder");
        CHECK(cat.items.size() == 2 && cat.items[0].mime == QStringLiteral("photofolder:/lib/album")
              && cat.items[1].mime == QStringLiteral("photofolder:/lib/trip"),
              "photos: folder rows sorted by path, mime carries the folder");
        CHECK(cat.items.size() == 2 && cat.items[0].expandable && cat.items[0].url.isEmpty(),
              "photos: a folder row is expandable and has no url (it drills, not opens)");
        CHECK(cat.items.size() == 2 && cat.items[0].title == QStringLiteral("album")
              && cat.items[1].title == QStringLiteral("trip"), "photos: folder row title = folder name");
        // Cover = the folder's FIRST image (a1, not a2); count is the folder's image count.
        CHECK(cat.items.size() == 2 && cat.items[0].thumbnailUrl == QStringLiteral("/lib/album/a1.jpg"),
              "photos: folder cover = its first image");
        CHECK(cat.items.size() == 2 && cat.items[0].subtitle.startsWith(QStringLiteral("2"))
              && cat.items[1].subtitle.startsWith(QStringLiteral("1")), "photos: folder row count");

        // A flat library (one folder) => the grid directly, no intermediate folder row.
        QVector<PhotoLibrary::PhotoEntry> flat;
        { PhotoLibrary::PhotoEntry e; e.folder = "/lib/solo"; e.path = "/lib/solo/s1.jpg"; flat << e; }
        { PhotoLibrary::PhotoEntry e; e.folder = "/lib/solo"; e.path = "/lib/solo/s2.jpg"; flat << e; }
        { PhotoLibrary::PhotoEntry e; e.folder = "/lib/solo"; e.path = "/lib/solo/s3.png"; flat << e; }
        const MediaCatalog grid = browse::photosCatalog(flat);
        CHECK(grid.items.size() == 3, "photos: flat tree -> a single grid of images");
        CHECK(grid.items.size() == 3 && grid.items[0].type == QStringLiteral("photo"),
              "photos: flat grid items are image tiles, not folder rows");
        // Each image carries its path as url (routes to the viewer), id, and thumbnail, and its filename title.
        CHECK(grid.items.size() == 3 && grid.items[0].url == QStringLiteral("/lib/solo/s1.jpg")
              && grid.items[0].id == QStringLiteral("/lib/solo/s1.jpg")
              && grid.items[0].thumbnailUrl == QStringLiteral("/lib/solo/s1.jpg"),
              "photos: an image item carries its path as url + id + thumbnail");
        CHECK(grid.items.size() == 3 && grid.items[0].title == QStringLiteral("s1.jpg"),
              "photos: an image item's title is its filename");

        // photosFolderCatalog: only the named folder's images, in order; titled with the folder name.
        const MediaCatalog folder = browse::photosFolderCatalog(two, QStringLiteral("/lib/album"));
        CHECK(folder.title == QStringLiteral("album"), "photos: folder grid title = folder name");
        CHECK(folder.items.size() == 2, "photos: folder grid holds only that folder's images");
        CHECK(folder.items.size() == 2 && folder.items[0].url == QStringLiteral("/lib/album/a1.jpg")
              && folder.items[1].url == QStringLiteral("/lib/album/a2.jpg")
              && folder.items[0].type == QStringLiteral("photo"),
              "photos: folder grid images carry their path + photo type");

        // Empty scan => an empty (titled, itemless) catalog, never a crash, on both builders.
        CHECK(browse::photosCatalog({}).items.isEmpty(), "photos: empty scan -> empty catalog");
        CHECK(browse::photosFolderCatalog({}, QStringLiteral("/lib/album")).items.isEmpty(),
              "photos: empty folder grid");
    }

    // ---- OPDS browse builders (issue #146): an OPDS feed / saved-catalog list -> browse rows --------------
    // FIXTURE INDEPENDENCE: the OpdsFeed/OpdsEntry/OpdsLink structs below are hand-BUILT, NOT run through
    // parseOpds — so opdsCatalog's classification-to-rows and its preferred-acquisition pick are measured
    // against link vectors set here by hand, with parseOpds (pinned separately in probe_opds) nowhere in the
    // loop. (OpdsFeed / OpdsCatalog reach here through SyntheticCatalogs.h's own includes.)
    {
        auto acq = [](const QString& type, const QString& href) {
            OpdsLink l; l.rel = QStringLiteral("http://opds-spec.org/acquisition");
            l.type = type; l.href = href; return l;
        };
        auto nav = [](const QString& href) {
            OpdsLink l; l.rel = QStringLiteral("subsection");
            l.type = QStringLiteral("application/atom+xml;profile=opds-catalog;kind=acquisition");
            l.href = href; return l;
        };

        OpdsFeed feed;
        feed.title = QStringLiteral("My Library");
        // entry 0: a NAVIGATION shelf — a navigation link, no acquisition, a cover.
        { OpdsEntry e; e.title = QStringLiteral("Science Fiction"); e.id = QStringLiteral("urn:shelf:sf");
          e.summary = QStringLiteral("The SF shelf"); e.coverHref = QStringLiteral("http://books.lan/covers/sf.png");
          e.navigation << nav(QStringLiteral("http://books.lan/opds/shelf/sf.xml")); feed.entries << e; }
        // entry 1: a BOOK offering PDF *then* EPUB — the reader wants the EPUB, so the pick must NOT be "first".
        { OpdsEntry e; e.title = QStringLiteral("Dune"); e.author = QStringLiteral("Frank Herbert");
          e.id = QStringLiteral("urn:book:dune"); e.coverHref = QStringLiteral("http://books.lan/covers/dune.png");
          e.acquisition << acq(QStringLiteral("application/pdf"),      QStringLiteral("http://books.lan/opds/dl/dune.pdf"))
                        << acq(QStringLiteral("application/epub+zip"), QStringLiteral("http://books.lan/opds/dl/dune.epub"));
          feed.entries << e; }
        // entry 2: an entry offering NEITHER acquisition nor navigation — nothing to act on -> skipped.
        { OpdsEntry e; e.title = QStringLiteral("Just text"); feed.entries << e; }

        const MediaCatalog cat = browse::opdsCatalog(feed);
        CHECK(cat.title == QStringLiteral("My Library"), "opds: catalog title = feed title");
        CHECK(cat.items.size() == 2, "opds: an entry with neither acquisition nor navigation is skipped");

        // Navigation shelf -> an expandable drill row carrying the sub-feed url, and NO url (so it drills in
        // rather than opening as a file).
        const MediaItem shelf = cat.items[0];
        CHECK(shelf.type == QStringLiteral("_opdsfeed") && shelf.title == QStringLiteral("Science Fiction")
              && shelf.expandable && shelf.url.isEmpty()
              && shelf.mime == QStringLiteral("opdsfeed:http://books.lan/opds/shelf/sf.xml")
              && shelf.thumbnailUrl == QStringLiteral("http://books.lan/covers/sf.png"),
              "opds: a navigation entry -> a drill row carrying the sub-feed url, no url");

        // Acquisition book -> a book item carrying the PREFERRED (epub) href + its content-type + cover/author/id.
        const MediaItem book = cat.items[1];
        CHECK(book.type == QStringLiteral("opdsbook") && book.title == QStringLiteral("Dune")
              && book.subtitle == QStringLiteral("Frank Herbert") && book.id == QStringLiteral("urn:book:dune")
              && book.thumbnailUrl == QStringLiteral("http://books.lan/covers/dune.png"),
              "opds: an acquisition entry -> a book item with title/author/cover/id");
        CHECK(book.url == QStringLiteral("http://books.lan/opds/dl/dune.epub")
              && book.mime == QStringLiteral("application/epub+zip"),
              "opds: the EPUB is preferred over the PDF even when the PDF is offered first");

        // Preferred-format ranking, each rung its own mutation target.
        auto pick = [&acq](const QString& t1, const QString& h1, const QString& t2, const QString& h2) {
            OpdsFeed f; OpdsEntry e; e.title = QStringLiteral("B");
            e.acquisition << acq(t1, h1) << acq(t2, h2); f.entries << e;
            return browse::opdsCatalog(f).items[0].url;
        };
        CHECK(pick(QStringLiteral("application/x-cbz"), QStringLiteral("http://x/h.cbz"),
                   QStringLiteral("application/epub+zip"), QStringLiteral("http://x/h.epub"))
              == QStringLiteral("http://x/h.epub"), "opds: EPUB outranks CBZ");
        CHECK(pick(QStringLiteral("application/pdf"), QStringLiteral("http://x/h.pdf"),
                   QStringLiteral("application/x-cbz"), QStringLiteral("http://x/h.cbz"))
              == QStringLiteral("http://x/h.cbz"), "opds: CBZ outranks PDF");
        CHECK(pick(QStringLiteral("application/x-mobipocket-ebook"), QStringLiteral("http://x/h1.mobi"),
                   QStringLiteral("application/x-mobipocket-ebook"), QStringLiteral("http://x/h2.mobi"))
              == QStringLiteral("http://x/h1.mobi"),
              "opds: with no reader-preferred format offered, the FIRST is carried");

        // An entry offering BOTH a navigation link and an acquisition -> the acquisition (a book) wins.
        { OpdsFeed f; OpdsEntry e; e.title = QStringLiteral("Both");
          e.navigation << nav(QStringLiteral("http://x/sub.xml"));
          e.acquisition << acq(QStringLiteral("application/epub+zip"), QStringLiteral("http://x/b.epub"));
          f.entries << e;
          const MediaCatalog c = browse::opdsCatalog(f);
          CHECK(c.items.size() == 1 && c.items[0].type == QStringLiteral("opdsbook")
                && c.items[0].url == QStringLiteral("http://x/b.epub"),
                "opds: an entry with BOTH nav and acquisition is treated as a book"); }

        // An empty feed -> an empty catalog, never a crash.
        CHECK(browse::opdsCatalog(OpdsFeed()).items.isEmpty(), "opds: an empty feed -> an empty catalog");

        // opdsCatalogsList: one row per saved catalog + a trailing add row; an empty list -> JUST the add row.
        QList<OpdsCatalog> cats;
        { OpdsCatalog c; c.id = QStringLiteral("id1"); c.name = QStringLiteral("My Calibre");
          c.url = QStringLiteral("http://books.lan/opds");
          c.username = QStringLiteral("reader"); c.password = QStringLiteral("s3cr3t"); cats << c; }
        const MediaCatalog list = browse::opdsCatalogsList(cats);
        CHECK(list.items.size() == 2, "opds: the saved-catalogs shelf = one row per catalog + an add row");
        const MediaItem row = list.items[0];
        CHECK(row.type == QStringLiteral("_opdscatalog") && row.expandable
              && row.title == QStringLiteral("My Calibre")
              && row.subtitle == QStringLiteral("http://books.lan/opds")
              && row.mime == QStringLiteral("opdscatalog:id1") && row.id == QStringLiteral("opdscat:id1"),
              "opds: a saved-catalog row carries the catalog id in its mime + id");
        // SAFETY tripwire (deliberate absence-of-behaviour): the device-local password must never ride a row.
        // No opdsCatalogsList mutation kills this — it guards the leak, not a computed value — so it is labelled
        // one rather than left reading as coverage of a behaviour.
        const bool leaked = row.title.contains(QStringLiteral("s3cr3t"))
                            || row.subtitle.contains(QStringLiteral("s3cr3t"))
                            || row.mime.contains(QStringLiteral("s3cr3t"))
                            || row.id.contains(QStringLiteral("s3cr3t"))
                            || row.url.contains(QStringLiteral("s3cr3t"));
        CHECK(!leaked, "opds: a saved-catalog row never carries the device-local password");
        CHECK(list.items[1].type == QStringLiteral("_newopds") && list.items[1].mime == QStringLiteral("newopds"),
              "opds: the trailing add-catalog row");
        CHECK(browse::opdsCatalogsList({}).items.size() == 1
              && browse::opdsCatalogsList({}).items[0].type == QStringLiteral("_newopds"),
              "opds: an empty catalog list still offers the add row");
    }

    // =====================================================================================================
    // JELLYFIN BROWSE LEVELS (#83) - the rows, the tagging rule, and the one thing none of them carries
    // =====================================================================================================
    {
        const QString srvA = QStringLiteral("0123456789abcdef0123456789abcdef");
        const QString srvB = QStringLiteral("fedcba9876543210fedcba9876543210");
        auto item = [](const QString& srv, const QString& srvName, const QString& id, const QString& title,
                       const QString& type) {
            Jellyfin::UnionItem u;
            u.id = Jellyfin::qualify(srv, id);
            u.title = title; u.type = type; u.serverId = srv; u.serverName = srvName;
            return u;
        };

        // ---- The leaf row. THE ONE SHAPE every level shares. ---------------------------------------------
        {
            Jellyfin::UnionItem m = item(srvA, QStringLiteral("Attic"), QStringLiteral("m1"),
                                         QStringLiteral("Alien"), QStringLiteral("Movie"));
            m.year = 1979;
            const MediaItem row = browse::jellyfinLeafRow(m, /*tagServer*/ false);
            // NO URL. This is the whole credential design: the stream link carries the token in its query,
            // so it is minted at play time and never sits on a row that gets copied into a playlist, a
            // favourite or a recents entry.
            CHECK(row.url.isEmpty(), "jellyfin: a playable row carries NO url");
            // ...and the KEYED mime the local-leaf table claims, carrying the qualified id.
            CHECK(row.mime == QString::fromLatin1(browse::kJellyfinItemPrefix) + m.id,
                  "jellyfin: the leaf mime is the prefix + the qualified id");
            CHECK(browse::jellyfinKeyOf(row.mime, browse::kJellyfinItemPrefix) == m.id,
                  "jellyfin: jellyfinKeyOf reads the whole qualified id back, colons and all");
            CHECK(row.id == m.id, "jellyfin: the row is filed under the qualified id");
            CHECK(row.type == QStringLiteral("movie"), "jellyfin: a Movie types as the app's own 'movie'");
            CHECK(row.subtitle == QStringLiteral("1979"),
                  "jellyfin: one server, so the second line is the year and not the server's name");
        }

        // ---- The tagging rule, BOTH ARMS. A tag that is always on and a tag that is never on are the two
        // ways this is wrong, and they look identical from one screenshot.
        {
            Jellyfin::UnionItem m = item(srvA, QStringLiteral("Attic"), QStringLiteral("m1"),
                                         QStringLiteral("Alien"), QStringLiteral("Movie"));
            m.year = 1979;
            CHECK(!browse::jellyfinLeafRow(m, false).subtitle.contains(QStringLiteral("Attic")),
                  "jellyfin: untagged, the server name is absent");
            CHECK(browse::jellyfinLeafRow(m, true).subtitle.contains(QStringLiteral("Attic")),
                  "jellyfin: tagged, the server name is on the row");
        }

        // ---- Level 1: the libraries. -------------------------------------------------------------------
        {
            QVector<Jellyfin::LibraryRef> libs;
            libs.push_back({ Jellyfin::qualify(srvA, QStringLiteral("l1")), QStringLiteral("Films"),
                             QStringLiteral("movies"), srvA, QStringLiteral("Attic") });
            libs.push_back({ Jellyfin::qualify(srvA, QStringLiteral("l2")), QStringLiteral("Records"),
                             QStringLiteral("music"), srvA, QStringLiteral("Attic") });
            libs.push_back({ Jellyfin::qualify(srvA, QStringLiteral("l3")), QStringLiteral("Boxes"),
                             QStringLiteral("boxsets"), srvA, QStringLiteral("Attic") });
            const MediaCatalog one = browse::jellyfinLibrariesCatalog(libs, {});
            // A library this increment cannot BROWSE is not listed: a folder whose every row is unopenable
            // is worse than no folder. Music is named by the mapping and is #194's surface, not this one.
            CHECK(one.items.size() == 1 && one.items[0].title == QStringLiteral("Films"),
                  "jellyfin: only a video library is listed");
            CHECK(one.items[0].expandable && one.items[0].type
                      == QString::fromLatin1(browse::kJellyfinLibType),
                  "jellyfin: a library row is an expandable container");
            CHECK(one.items[0].subtitle.isEmpty(),
                  "jellyfin: with one server contributing, a library row is not tagged");

            libs.push_back({ Jellyfin::qualify(srvB, QStringLiteral("l1")), QStringLiteral("Films"),
                             QStringLiteral("movies"), srvB, QStringLiteral("Basement") });
            const MediaCatalog two = browse::jellyfinLibrariesCatalog(libs, {});
            CHECK(two.items.size() == 2, "jellyfin: two servers' film libraries, both listed (no dedupe)");
            CHECK(two.items[0].subtitle == QStringLiteral("Attic")
                      && two.items[1].subtitle == QStringLiteral("Basement"),
                  "jellyfin: with two servers, every row says which one it is from");
            // ...and the two rows are DIFFERENT keys, though both servers call the library "l1". This is
            // #160's whole point, seen from the browse surface.
            CHECK(two.items[0].mime != two.items[1].mime,
                  "jellyfin: the same raw library id on two servers is two different rows");

            // A server that did not contribute says so ON THE SCREEN, not only in a log.
            const MediaCatalog noted = browse::jellyfinLibrariesCatalog(
                libs, { QStringLiteral("jellyfin: \"Cellar\" did not answer in time; its items are not in "
                                       "this view") });
            CHECK(noted.items.size() == 3 && noted.items[2].type == QStringLiteral("info"),
                  "jellyfin: an unavailable server's note is a non-actionable row");
            // An EMPTY level explains itself rather than being blank...
            CHECK(browse::jellyfinLibrariesCatalog({}, {}).items.size() == 1,
                  "jellyfin: an empty library list still says something");
            // ...but a level whose only content is notes has explained itself already.
            const MediaCatalog onlyNotes = browse::jellyfinLibrariesCatalog(
                {}, { QStringLiteral("jellyfin: \"Cellar\" is switched off; its items are hidden") });
            CHECK(onlyNotes.items.size() == 1,
                  "jellyfin: notes alone are the explanation; no second 'nothing here' line");
        }

        // ---- Level 2: a library's titles. A Movie is a leaf, a Series is a container. --------------------
        {
            QVector<Jellyfin::UnionItem> items;
            items << item(srvA, QStringLiteral("Attic"), QStringLiteral("m1"), QStringLiteral("Alien"),
                          QStringLiteral("Movie"));
            items << item(srvA, QStringLiteral("Attic"), QStringLiteral("s1"), QStringLiteral("Trek"),
                          QStringLiteral("Series"));
            const MediaCatalog c = browse::jellyfinLibraryCatalog(QStringLiteral("Films"), items, false, {});
            CHECK(c.items.size() == 2, "jellyfin: a library lists its titles");
            CHECK(!c.items[0].expandable, "jellyfin: a film is a leaf");
            CHECK(c.items[1].expandable
                      && c.items[1].type == QString::fromLatin1(browse::kJellyfinSeriesType),
                  "jellyfin: a series is a container that drills into its seasons");
            CHECK(browse::jellyfinKeyOf(c.items[1].mime, browse::kJellyfinSeriesPrefix)
                      == items[1].id,
                  "jellyfin: the series row carries its qualified id");
        }

        // ---- Level 3: seasons. The marker carries BOTH ids. ----------------------------------------------
        {
            const QString seriesRef = Jellyfin::qualify(srvA, QStringLiteral("s1"));
            QVector<Jellyfin::UnionItem> seasons;
            seasons << item(srvA, QStringLiteral("Attic"), QStringLiteral("se1"),
                            QStringLiteral("Season 1"), QStringLiteral("Season"));
            const MediaCatalog c = browse::jellyfinSeasonsCatalog(QStringLiteral("Trek"), seriesRef, seasons);
            CHECK(c.items.size() == 1 && c.items[0].expandable, "jellyfin: a season is a container");
            const QString marker = browse::jellyfinKeyOf(c.items[0].mime, browse::kJellyfinSeasonPrefix);
            const int nl = marker.indexOf(QLatin1Char('\n'));
            // /Shows/<seriesId>/Episodes is addressed by the SERIES and filtered by the season, so a marker
            // carrying only the season would have to guess at the series on the way back in.
            CHECK(nl > 0 && marker.left(nl) == seriesRef && marker.mid(nl + 1) == seasons[0].id,
                  "jellyfin: a season row carries its series AND its season, qualified");
        }

        // ---- Level 4: episodes, numbered ONLY when the server gave numbers. ------------------------------
        {
            QVector<Jellyfin::UnionItem> eps;
            Jellyfin::UnionItem e = item(srvA, QStringLiteral("Attic"), QStringLiteral("e1"),
                                         QStringLiteral("The Cage"), QStringLiteral("Episode"));
            e.indexNumber = 4; e.parentIndexNumber = 1; e.seriesName = QStringLiteral("Trek");
            eps << e;
            Jellyfin::UnionItem x = item(srvA, QStringLiteral("Attic"), QStringLiteral("e2"),
                                         QStringLiteral("Behind the scenes"), QStringLiteral("Episode"));
            eps << x;   // a special / an extra: no number at all
            const MediaCatalog c = browse::jellyfinEpisodesCatalog(QStringLiteral("Season 1"), eps);
            CHECK(c.items.size() == 2, "jellyfin: a season lists its episodes");
            CHECK(c.items[0].title.startsWith(QStringLiteral("S1E4")),
                  "jellyfin: a numbered episode leads with its number");
            // "S0E0 · " in front of an extra would be this app inventing a number the server never gave.
            CHECK(c.items[1].title == QStringLiteral("Behind the scenes"),
                  "jellyfin: an episode with no number is titled by its name alone");
            CHECK(c.items[0].url.isEmpty() && c.items[1].url.isEmpty(),
                  "jellyfin: an episode row carries no url either");
        }

        // ---- Continue Watching: only what the user is actually part-way through. -------------------------
        {
            QVector<Jellyfin::UnionItem> items;
            Jellyfin::UnionItem half = item(srvA, QStringLiteral("Attic"), QStringLiteral("m1"),
                                            QStringLiteral("Alien"), QStringLiteral("Movie"));
            half.positionTicks = 6000000000LL;
            Jellyfin::UnionItem none = item(srvA, QStringLiteral("Attic"), QStringLiteral("m2"),
                                            QStringLiteral("Solaris"), QStringLiteral("Movie"));
            items << half << none;
            const QVector<MediaItem> rows = browse::jellyfinContinueRows(items, false);
            // A row at zero would be a film the user has never started, sitting at the top of their home
            // screen claiming otherwise.
            CHECK(rows.size() == 1 && rows[0].title == QStringLiteral("Alien"),
                  "jellyfin: Continue Watching holds only part-watched items");
            CHECK(rows[0].mime.startsWith(QString::fromLatin1(browse::kJellyfinItemPrefix))
                      && rows[0].url.isEmpty(),
                  "jellyfin: a Continue Watching row is the same leaf shape a browse level builds");
        }
    }

    if (fails == 0) printf("BROWSE-OK\n");
    return fails == 0 ? 0 : 1;
}
