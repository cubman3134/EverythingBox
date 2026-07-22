// Headless checks for the game-importers Task 1 (Steam gap-closure) logic — the pure, I/O-free cores of the
// Recents round-trip, the owned-games extension, and the marks integration. No network, no registry, no Steam
// install needed: every assert runs against in-memory fixtures. Prints IMPORTERS-OK on success; a failure
// prints IMPORTERS-FAIL <cond> (line) and exits non-zero.
//
// Covered:
//   * SteamLibrary::parseOwnedGames — the GetOwnedGames JSON parse over valid / invalid / empty fixtures
//     (numeric appid, a nameless game keeping its appid as the label, name-sort);
//   * SteamLibrary::ownedCacheFresh — the TTL window semantics (fresh inside, stale past, zero/future = not fresh);
//   * SteamLibrary::launchUrl / installUrl — the run vs install handoff URLs;
//   * browse::steamGamesCatalog with an owned list — installed entries unchanged (no subtitle, no url), owned-not-
//     installed appended (badge "Not installed", url steam://install/<appid>), already-installed owned skipped,
//     the in-console query scoping both sets, and a pure injected poster so it stays I/O-free;
//   * RecentStore::relaunchFor — the Recent-kind dispatch table the app's openRecent switch mirrors;
//   * browse::iconTypeForKind — a "steamgame" Recent draws the game placeholder icon.
//
// Links only QtCore-friendly units (SteamLibrary/SyntheticCatalogs/MetaCache/RecentStore/AddonModels + the
// AppPaths/ProfileStore closure RecentStore pulls). relaunchFor/parse/TTL touch no store, so nothing here writes
// a real ini.
#include "SteamLibrary.h"
#include "EpicLibrary.h"
#include "GogLibrary.h"
#include "RecentStore.h"
#include "../src/browse/SyntheticCatalogs.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "IMPORTERS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A MediaItem lookup by id ("steam:<appid>") in a catalog, or nullptr.
static const MediaItem* find(const MediaCatalog& cat, const QString& id)
{
    for (const MediaItem& it : cat.items) if (it.id == id) return &it;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. GetOwnedGames JSON parse: valid ---------------------------------------------------------------
    {
        const QByteArray valid = R"({"response":{"game_count":3,"games":[
            {"appid":570,"name":"Dota 2"},
            {"appid":440,"name":"Team Fortress 2"},
            {"appid":730,"name":"Counter-Strike"}]}})";
        const QVector<SteamGame> g = SteamLibrary::parseOwnedGames(valid);
        CHECK(g.size() == 3);
        // Sorted by name (case-insensitive): Counter-Strike, Dota 2, Team Fortress 2.
        CHECK(g[0].name == QStringLiteral("Counter-Strike") && g[0].appid == QStringLiteral("730"));
        CHECK(g[1].name == QStringLiteral("Dota 2") && g[1].appid == QStringLiteral("570"));
        CHECK(g[2].appid == QStringLiteral("440"));
    }

    // ---- 1b. Nameless game keeps its appid as the label; numeric appid coerced to string --------------------
    {
        const QByteArray q = R"({"response":{"games":[{"appid":12345}]}})";
        const QVector<SteamGame> g = SteamLibrary::parseOwnedGames(q);
        CHECK(g.size() == 1);
        CHECK(g[0].appid == QStringLiteral("12345"));
        CHECK(g[0].name == QStringLiteral("12345")); // no name -> appid is the label
    }

    // ---- 1c. Invalid + empty fixtures -> empty (silent fallback) -------------------------------------------
    {
        CHECK(SteamLibrary::parseOwnedGames(QByteArray("not json at all {")).isEmpty());
        CHECK(SteamLibrary::parseOwnedGames(QByteArray("[]")).isEmpty());               // array, not object
        CHECK(SteamLibrary::parseOwnedGames(QByteArray(R"({"response":{}})")).isEmpty()); // no games key
        CHECK(SteamLibrary::parseOwnedGames(QByteArray(R"({"response":{"games":[]}})")).isEmpty()); // empty games
        CHECK(SteamLibrary::parseOwnedGames(QByteArray()).isEmpty());                   // empty body
        // A game object with no appid is dropped, not kept as a blank.
        CHECK(SteamLibrary::parseOwnedGames(QByteArray(R"({"response":{"games":[{"name":"X"}]}})")).isEmpty());
    }

    // ---- 2. TTL window semantics --------------------------------------------------------------------------
    {
        const int ttl = 1800; // 30 min
        const qint64 base = 1'000'000;
        CHECK(SteamLibrary::ownedCacheFresh(base, base, ttl));               // just cached: fresh
        CHECK(SteamLibrary::ownedCacheFresh(base, base + ttl - 1, ttl));     // inside the window: fresh
        CHECK(!SteamLibrary::ownedCacheFresh(base, base + ttl, ttl));        // exactly TTL later: stale
        CHECK(!SteamLibrary::ownedCacheFresh(base, base + ttl + 100, ttl));  // past the window: stale
        CHECK(!SteamLibrary::ownedCacheFresh(0, base, ttl));                 // never cached: not fresh
        CHECK(!SteamLibrary::ownedCacheFresh(base + 10, base, ttl));         // future timestamp: not fresh
    }

    // ---- 3. Launch vs install handoff URLs ----------------------------------------------------------------
    CHECK(SteamLibrary::launchUrl(QStringLiteral("570")) == QStringLiteral("steam://rungameid/570"));
    CHECK(SteamLibrary::installUrl(QStringLiteral("570")) == QStringLiteral("steam://install/570"));

    // ---- 4. steamGamesCatalog: owned-not-installed append -------------------------------------------------
    {
        // A pure poster keeps the builder I/O-free (SteamLibrary::posterUrl would touch the local librarycache).
        auto poster = [](const SteamGame& g) { return QStringLiteral("cap://") + g.appid; };
        QList<SteamGame> installed{ { QStringLiteral("100"), QStringLiteral("Alpha") } };
        QList<SteamGame> owned{
            { QStringLiteral("100"), QStringLiteral("Alpha") },   // already installed -> must be skipped in owned pass
            { QStringLiteral("200"), QStringLiteral("Bravo") },   // owned, not installed
            { QStringLiteral("300"), QStringLiteral("Charlie") }, // owned, not installed
        };
        const MediaCatalog cat = browse::steamGamesCatalog(installed, QString(), poster, owned);
        CHECK(cat.items.size() == 3); // Alpha (installed) + Bravo + Charlie (owned-not-installed); no dup Alpha

        const MediaItem* alpha = find(cat, QStringLiteral("steam:100"));
        CHECK(alpha && alpha->mime == QStringLiteral("steamgame"));
        CHECK(alpha && alpha->url.isEmpty());              // installed entry unchanged: no url -> info page + Play
        CHECK(alpha && alpha->subtitle.isEmpty());         // no "Not installed" badge on an installed game

        const MediaItem* bravo = find(cat, QStringLiteral("steam:200"));
        CHECK(bravo && bravo->mime == QStringLiteral("steamgame"));
        CHECK(bravo && bravo->url == QStringLiteral("steam://install/200")); // activation installs
        CHECK(bravo && !bravo->subtitle.isEmpty());        // badged "Not installed"
        CHECK(bravo && bravo->thumbnailUrl == QStringLiteral("cap://200"));  // poster still resolved

        // Exactly one owned-not-installed carries the install url; the installed one does not.
        int installTiles = 0;
        for (const MediaItem& it : cat.items)
            if (it.url.startsWith(QStringLiteral("steam://install/"))) ++installTiles;
        CHECK(installTiles == 2);
    }

    // ---- 4b. Query scopes BOTH installed and owned; no owned list == today's installed-only ---------------
    {
        auto poster = [](const SteamGame& g) { return QString(); };
        QList<SteamGame> installed{ { QStringLiteral("100"), QStringLiteral("Alpha") },
                                    { QStringLiteral("101"), QStringLiteral("Beta") } };
        QList<SteamGame> owned{ { QStringLiteral("200"), QStringLiteral("Alfredo") },
                                { QStringLiteral("300"), QStringLiteral("Charlie") } };
        const MediaCatalog scoped = browse::steamGamesCatalog(installed, QStringLiteral("al"), poster, owned);
        // "al" matches Alpha (installed) + Alfredo (owned), not Beta/Charlie.
        CHECK(scoped.items.size() == 2);
        CHECK(find(scoped, QStringLiteral("steam:100")));  // Alpha
        CHECK(find(scoped, QStringLiteral("steam:200")));  // Alfredo (owned-not-installed)

        // No owned list -> installed-only (unchanged pre-feature behavior).
        const MediaCatalog none = browse::steamGamesCatalog(installed, QString(), poster);
        CHECK(none.items.size() == 2);
        for (const MediaItem& it : none.items) CHECK(it.url.isEmpty() && it.subtitle.isEmpty());
    }

    // ---- 5. Recent-kind dispatch table (openRecent mirrors this) ------------------------------------------
    using RL = RecentStore::Relaunch;
    CHECK(RecentStore::relaunchFor(QStringLiteral("steamgame")) == RL::SteamGame);
    CHECK(RecentStore::relaunchFor(QStringLiteral("epicgame"))  == RL::EpicGame);
    CHECK(RecentStore::relaunchFor(QStringLiteral("goggame"))   == RL::GogGame);
    CHECK(RecentStore::relaunchFor(QStringLiteral("pcgame"))    == RL::PcGame);
    CHECK(RecentStore::relaunchFor(QStringLiteral("video"))     == RL::Video);
    CHECK(RecentStore::relaunchFor(QStringLiteral("audio"))     == RL::Audio);
    CHECK(RecentStore::relaunchFor(QStringLiteral("document"))  == RL::Document);
    CHECK(RecentStore::relaunchFor(QStringLiteral("game"))      == RL::Game);
    CHECK(RecentStore::relaunchFor(QStringLiteral("bogus"))     == RL::Unknown);
    CHECK(RecentStore::relaunchFor(QString())                   == RL::Unknown);

    // ---- 6. Marks-sanity foundation: game Recents draw the game icon (keyFor keys are <store>:<id>) --------
    CHECK(browse::iconTypeForKind(QStringLiteral("steamgame")) == QStringLiteral("game"));
    CHECK(browse::iconTypeForKind(QStringLiteral("epicgame"))  == QStringLiteral("game"));
    CHECK(browse::iconTypeForKind(QStringLiteral("goggame"))   == QStringLiteral("game"));

    // ==== EPIC (Task 2) ====================================================================================

    // ---- 7. Epic manifest parse: a real game is kept -----------------------------------------------------
    {
        const QByteArray game = R"({"AppName":"Fortnite","DisplayName":"Fortnite",
            "InstallLocation":"C:\\Games\\Fortnite","bIsIncompleteInstall":false,
            "MainGameAppName":"","AppCategories":["public","games","applications"]})";
        const EpicGame g = EpicLibrary::parseManifest(game);
        CHECK(g.appName == QStringLiteral("Fortnite"));
        CHECK(g.name == QStringLiteral("Fortnite"));
        CHECK(g.installLocation == QStringLiteral("C:/Games/Fortnite")); // native separators normalized
    }

    // ---- 7b. Epic discriminator: DLC / incomplete / engine-tool / malformed are all filtered -------------
    {
        // DLC: MainGameAppName points at a DIFFERENT parent app.
        const QByteArray dlc = R"({"AppName":"FortniteDLC","DisplayName":"Fortnite Skin Pack",
            "InstallLocation":"C:\\Games\\Fortnite","MainGameAppName":"Fortnite","AppCategories":["games","addons"]})";
        CHECK(EpicLibrary::parseManifest(dlc).appName.isEmpty());
        // Still downloading -> not launchable.
        const QByteArray incomplete = R"({"AppName":"Half","DisplayName":"Half Downloaded",
            "InstallLocation":"C:\\Games\\Half","bIsIncompleteInstall":true,"AppCategories":["games"]})";
        CHECK(EpicLibrary::parseManifest(incomplete).appName.isEmpty());
        // Engine/plugin tool (the real shape on this dev machine): categories carry "engines", not "games".
        const QByteArray engine = R"({"AppName":"UE_5.8","DisplayName":"Unreal Engine",
            "InstallLocation":"C:\\Program Files\\Epic Games\\UE_5.8","AppCategories":["engines/ue5","engines"]})";
        CHECK(EpicLibrary::parseManifest(engine).appName.isEmpty());
        // No "games" category at all -> filtered.
        const QByteArray noCat = R"({"AppName":"Bridge","DisplayName":"Quixel Bridge",
            "InstallLocation":"C:\\Program Files\\Epic Games\\UE_5.8","AppCategories":[]})";
        CHECK(EpicLibrary::parseManifest(noCat).appName.isEmpty());
        // Malformed JSON / missing required fields -> filtered (never throws).
        CHECK(EpicLibrary::parseManifest(QByteArray("not json {")).appName.isEmpty());
        CHECK(EpicLibrary::parseManifest(QByteArray("[]")).appName.isEmpty());
        const QByteArray noInstall = R"({"AppName":"X","DisplayName":"X","AppCategories":["games"]})";
        CHECK(EpicLibrary::parseManifest(noInstall).appName.isEmpty()); // no InstallLocation
    }

    // ---- 7c. Epic installedGames over a fixture manifests dir (a game kept, a DLC + malformed skipped) ----
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        auto writeItem = [&](const QString& fn, const QByteArray& body) {
            QFile f(dir.filePath(fn));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write(body);
        };
        writeItem(QStringLiteral("a.item"), R"({"AppName":"Alpha","DisplayName":"Alpha Game",
            "InstallLocation":"C:\\G\\Alpha","AppCategories":["games"]})");
        writeItem(QStringLiteral("b.item"), R"({"AppName":"Beta","DisplayName":"Beta Game",
            "InstallLocation":"C:\\G\\Beta","AppCategories":["games"]})");
        writeItem(QStringLiteral("dlc.item"), R"({"AppName":"AlphaDLC","DisplayName":"Alpha DLC",
            "InstallLocation":"C:\\G\\Alpha","MainGameAppName":"Alpha","AppCategories":["games"]})");
        writeItem(QStringLiteral("junk.item"), QByteArray("garbage {"));

        CHECK(EpicLibrary::isAvailable(dir.path()));
        const QVector<EpicGame> games = EpicLibrary::installedGames(dir.path());
        CHECK(games.size() == 2);                                  // Alpha + Beta; DLC + junk filtered
        CHECK(games[0].name == QStringLiteral("Alpha Game"));      // name-sorted
        CHECK(games[1].name == QStringLiteral("Beta Game"));

        // An empty dir -> not available, no games.
        QTemporaryDir empty;
        CHECK(!EpicLibrary::isAvailable(empty.path()));
        CHECK(EpicLibrary::installedGames(empty.path()).isEmpty());
    }

    // ---- 7d. Epic launch URI + console builder -----------------------------------------------------------
    CHECK(EpicLibrary::launchUrl(QStringLiteral("Fortnite"))
          == QStringLiteral("com.epicgames.launcher://apps/Fortnite?action=launch&silent=true"));
    {
        QList<EpicGame> installed{ { QStringLiteral("Zed"), QStringLiteral("Zed Game"), QStringLiteral("C:/G/Zed") },
                                   { QStringLiteral("Ace"), QStringLiteral("Ace Game"), QStringLiteral("C:/G/Ace") } };
        const MediaCatalog cat = browse::epicGamesCatalog(installed, QString());
        CHECK(cat.items.size() == 2);
        const MediaItem* ace = find(cat, QStringLiteral("epic:Ace"));
        CHECK(ace && ace->mime == QStringLiteral("epicgame"));
        CHECK(ace && ace->url.isEmpty());   // no url -> info page + Play (URI launch), mirrors steamgame
        CHECK(ace && ace->title == QStringLiteral("Ace Game"));
        // Query scopes by name.
        const MediaCatalog scoped = browse::epicGamesCatalog(installed, QStringLiteral("zed"));
        CHECK(scoped.items.size() == 1 && find(scoped, QStringLiteral("epic:Zed")));
    }

    // ==== GOG (Task 2) ====================================================================================

    // ---- 8. GOG installedGames over a fake-registry INI fixture ------------------------------------------
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString iniPath = dir.filePath(QStringLiteral("gog.ini"));
        {
            QSettings ini(iniPath, QSettings::IniFormat);
            ini.setValue(QStringLiteral("1207658924/gameName"), QStringLiteral("The Witcher"));
            ini.setValue(QStringLiteral("1207658924/path"), QStringLiteral("C:\\GOG Games\\The Witcher"));
            ini.setValue(QStringLiteral("1207658924/exe"), QStringLiteral("C:\\GOG Games\\The Witcher\\witcher.exe"));
            ini.setValue(QStringLiteral("1992450334/gameName"), QStringLiteral("Solitaire Collection"));
            ini.setValue(QStringLiteral("1992450334/path"), QStringLiteral("C:\\GOG Games\\Solitaire"));
            ini.setValue(QStringLiteral("1992450334/exe"), QStringLiteral("C:\\GOG Games\\Solitaire\\sol.exe"));
            // An incomplete key (no exe) is skipped.
            ini.setValue(QStringLiteral("999/gameName"), QStringLiteral("Broken"));
            ini.sync();
        }
        CHECK(GogLibrary::isAvailable(iniPath));
        const QVector<GogGame> games = GogLibrary::installedGames(iniPath);
        CHECK(games.size() == 2);                                  // Broken (no exe) skipped
        CHECK(games[0].name == QStringLiteral("Solitaire Collection")); // name-sorted
        CHECK(games[0].id == QStringLiteral("1992450334"));
        CHECK(games[0].exe == QStringLiteral("C:/GOG Games/Solitaire/sol.exe")); // native separators normalized
        CHECK(games[1].name == QStringLiteral("The Witcher"));

        // An empty ini -> not available.
        const QString emptyIni = dir.filePath(QStringLiteral("empty.ini"));
        { QSettings e(emptyIni, QSettings::IniFormat); e.sync(); }
        CHECK(!GogLibrary::isAvailable(emptyIni));
        CHECK(GogLibrary::installedGames(emptyIni).isEmpty());
    }

    // ---- 8b. GOG console builder: exe rides the tile (launchPcExe target); mime goggame -------------------
    {
        QList<GogGame> installed{
            { QStringLiteral("100"), QStringLiteral("Alpha"), QStringLiteral("C:/G/Alpha/a.exe"), QStringLiteral("C:/G/Alpha") },
            { QStringLiteral("200"), QStringLiteral("Bravo"), QStringLiteral("C:/G/Bravo/b.exe"), QStringLiteral("C:/G/Bravo") } };
        const MediaCatalog cat = browse::gogGamesCatalog(installed, QString());
        CHECK(cat.items.size() == 2);
        const MediaItem* alpha = find(cat, QStringLiteral("gog:100"));
        CHECK(alpha && alpha->mime == QStringLiteral("goggame"));
        CHECK(alpha && alpha->url == QStringLiteral("C:/G/Alpha/a.exe")); // the exe rides on the tile
        const MediaCatalog scoped = browse::gogGamesCatalog(installed, QStringLiteral("brav"));
        CHECK(scoped.items.size() == 1 && find(scoped, QStringLiteral("gog:200")));
    }

    if (failures == 0) { std::puts("IMPORTERS-OK"); return 0; }
    std::fprintf(stderr, "IMPORTERS: %d check(s) failed\n", failures);
    return 1;
}
