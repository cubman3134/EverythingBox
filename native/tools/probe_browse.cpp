// Headless test for the synthetic browse-catalog builders (Recent / Downloaded / Favorites / Trakt
// "Airing soon"): kind+system filtering, the pcgame-in-games rule, missing-file hiding, and the Trakt
// calendar's air-time ordering + past-exclusion boundary + unplayable-row rule. Prints BROWSE-OK.
#include <QCoreApplication>
#include <QDateTime>
#include "../src/browse/SyntheticCatalogs.h"
#include "../src/browse/SearchAggregator.h"
#include "../src/core/PcGameRemap.h"   // the drift assertion: catalog item id == remap destination
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

    if (fails == 0) printf("BROWSE-OK\n");
    return fails == 0 ? 0 : 1;
}
