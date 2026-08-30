// Headless checks for the game-importers Task 1 (Steam gap-closure) logic — the pure, I/O-free cores of the
// Recents round-trip, the owned-games extension, and the marks integration. No network, no registry, no Steam
// install needed: every assert runs against in-memory fixtures. Prints IMPORTERS-OK on success; a failure
// prints IMPORTERS-FAIL <cond> (line) and exits non-zero.
//
// Covered:
//   * SteamLibrary::parseOwnedGames — the GetOwnedGames JSON parse over valid / invalid / empty fixtures
//     (numeric appid, a nameless game keeping its appid as the label, name-sort);
//   * SteamLibrary::ownedCacheFresh — the TTL window semantics (fresh inside, stale past, zero/future = not fresh);
//   * SteamLibrary::ownedFetchDecision — the async owned-games state machine's pure core (unconfigured / cache
//     hit for the same creds inside TTL / fetch when cold, stale, or the key/id changed);
//   * SteamLibrary::launchUrl / installUrl — the run vs install handoff URLs;
//   * browse::pcGamesCatalog with a Steam owned list — installed entries unchanged (no subtitle), owned-not-
//     installed added as a LauncherOwned SOURCE (badge "Not installed", launchUrl steam://install/<appid>, and
//     NEVER ready, so Play cannot start a download), already-installed owned skipped, the in-folder query
//     scoping both sets, and a pure injected poster so it stays I/O-free;
//   * RecentStore::relaunchFor — the Recent-kind dispatch table the app's openRecent switch mirrors;
//   * browse::iconTypeForKind — a "steamgame" Recent draws the game placeholder icon;
//   * browse::pcGamesCatalog's per-launcher source mapping — Epic's launcher URI, GOG's exe-on-the-source, and
//     the Battle.net two-route split (a coded title keys on its code and carries the battlenet:// URI; a
//     code-less one carries its exe ⇒ launchPcExe), plus the in-folder query scoping and the empty case.
//     (These were four per-launcher builders until the four folders became one; the mappings they pinned are
//     the same, restated on the builder that now performs them.)
//
// Links only QtCore-friendly units (SteamLibrary/SyntheticCatalogs/MetaCache/RecentStore/AddonModels + the
// AppPaths/ProfileStore closure RecentStore pulls). relaunchFor/parse/TTL touch no store, so nothing here writes
// a real ini.
#include "SteamLibrary.h"
#include "EpicLibrary.h"
#include "GogLibrary.h"
#include "BattleNetLibrary.h"
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

    // ---- 2b. Owned-fetch decision (the async state machine's pure core) -----------------------------------
    {
        using OF = SteamLibrary::OwnedFetch;
        const int ttl = 1800;
        const qint64 base = 1'000'000;
        const QString K = QStringLiteral("KEY"), I = QStringLiteral("ID");

        // Not configured: an empty key OR empty id -> never touch the network (no callback).
        CHECK(SteamLibrary::ownedFetchDecision(QString(), QString(), 0, QString(), I, base, ttl) == OF::NotConfigured);
        CHECK(SteamLibrary::ownedFetchDecision(K, I, base, QString(), I, base, ttl) == OF::NotConfigured); // no key
        CHECK(SteamLibrary::ownedFetchDecision(K, I, base, K, QString(), base, ttl) == OF::NotConfigured); // no id

        // Cold cache (never fetched) with creds -> Fetch.
        CHECK(SteamLibrary::ownedFetchDecision(QString(), QString(), 0, K, I, base, ttl) == OF::Fetch);

        // Same key+id, still inside the TTL window -> CacheHit (no re-fetch; this is what stops the re-present loop).
        CHECK(SteamLibrary::ownedFetchDecision(K, I, base, K, I, base, ttl) == OF::CacheHit);
        CHECK(SteamLibrary::ownedFetchDecision(K, I, base, K, I, base + ttl - 1, ttl) == OF::CacheHit);

        // Same creds but the cache went stale -> Fetch again.
        CHECK(SteamLibrary::ownedFetchDecision(K, I, base, K, I, base + ttl, ttl) == OF::Fetch);

        // A fresh cache but for DIFFERENT creds (key or id changed) -> Fetch (the cached list isn't ours).
        CHECK(SteamLibrary::ownedFetchDecision(K, I, base, QStringLiteral("K2"), I, base, ttl) == OF::Fetch);
        CHECK(SteamLibrary::ownedFetchDecision(K, I, base, K, QStringLiteral("I2"), base, ttl) == OF::Fetch);
    }

    // ---- 3. Launch vs install handoff URLs ----------------------------------------------------------------
    CHECK(SteamLibrary::launchUrl(QStringLiteral("570")) == QStringLiteral("steam://rungameid/570"));
    CHECK(SteamLibrary::installUrl(QStringLiteral("570")) == QStringLiteral("steam://install/570"));

    // ---- 4. The owned-not-installed Steam library, in the merged PC Games folder ---------------------------
    // This used to pin browse::steamGamesCatalog's owned-tile append. That builder is gone with the Steam
    // folder, but the FEATURE is not: an owned-but-not-installed game still has to appear, still has to say
    // it isn't installed, and must still hand its install to the Steam client — now as a LauncherOwned SOURCE
    // on the merged item rather than a tile of its own. The properties below are the same ones, restated on
    // the shape that replaced it, plus the one the merge adds: an owned source must never be READY, because
    // pickAutoSource would otherwise let a single Play keypress start a multi-gigabyte download.
    {
        // A pure poster keeps the builder I/O-free (SteamLibrary::posterUrl would touch the local librarycache).
        auto poster = [](const QVector<pcgame::PcGameSource>& v) {
            return v.isEmpty() ? QString() : QStringLiteral("cap://") + v.first().launchId;
        };
        QList<SteamGame> installed{ { QStringLiteral("100"), QStringLiteral("Alpha") } };
        QList<SteamGame> owned{
            { QStringLiteral("100"), QStringLiteral("Alpha") },   // already installed -> must be skipped in owned pass
            { QStringLiteral("200"), QStringLiteral("Bravo") },   // owned, not installed
            { QStringLiteral("300"), QStringLiteral("Charlie") }, // owned, not installed
        };
        const MediaCatalog cat = browse::pcGamesCatalog(installed, {}, {}, {}, {}, QString(), QString(),
                                                        poster, owned);
        CHECK(cat.items.size() == 3); // Alpha (installed) + Bravo + Charlie (owned-not-installed); no dup Alpha

        const MediaItem* alpha = find(cat, QStringLiteral("pcgame:alpha"));
        CHECK(alpha && alpha->mime == QStringLiteral("pcgame"));
        CHECK(alpha && alpha->url.isEmpty());              // the picker decides the launch, not the tile
        CHECK(alpha && alpha->subtitle.isEmpty());         // no "Not installed" badge on an installed game
        // The owned duplicate of an INSTALLED game is dropped, not carried as a second source: the installed
        // copy is strictly better, and two Steam rows in the picker would be a choice with no difference.
        CHECK(alpha && alpha->pcSources.size() == 1
              && alpha->pcSources[0].kind == pcgame::PcGameSource::LauncherInstalled);

        const MediaItem* bravo = find(cat, QStringLiteral("pcgame:bravo"));
        CHECK(bravo && bravo->mime == QStringLiteral("pcgame"));
        CHECK(bravo && bravo->pcSources.size() == 1);
        CHECK(bravo && bravo->pcSources[0].kind == pcgame::PcGameSource::LauncherOwned);
        CHECK(bravo && bravo->pcSources[0].launchUrl == QStringLiteral("steam://install/200")); // hands off to Steam
        CHECK(bravo && !bravo->pcSources[0].ready);        // Play must never start this by itself
        CHECK(bravo && pcgame::pickAutoSource(bravo->pcSources) == -1); // ...and pickAutoSource agrees
        CHECK(bravo && !bravo->subtitle.isEmpty());        // badged "Not installed"
        CHECK(bravo && bravo->thumbnailUrl == QStringLiteral("cap://200"));  // poster still resolved

        int installSources = 0;
        for (const MediaItem& it : cat.items)
            for (const pcgame::PcGameSource& s : it.pcSources)
                if (s.launchUrl.startsWith(QStringLiteral("steam://install/"))) ++installSources;
        CHECK(installSources == 2);
    }

    // ---- 4b. Query scopes BOTH installed and owned; no owned list == installed-only -----------------------
    {
        auto poster = [](const QVector<pcgame::PcGameSource>&) { return QString(); };
        QList<SteamGame> installed{ { QStringLiteral("100"), QStringLiteral("Alpha") },
                                    { QStringLiteral("101"), QStringLiteral("Beta") } };
        QList<SteamGame> owned{ { QStringLiteral("200"), QStringLiteral("Alfredo") },
                                { QStringLiteral("300"), QStringLiteral("Charlie") } };
        const MediaCatalog scoped = browse::pcGamesCatalog(installed, {}, {}, {}, {}, QStringLiteral("al"),
                                                           QString(), poster, owned);
        // "al" matches Alpha (installed) + Alfredo (owned), not Beta/Charlie.
        CHECK(scoped.items.size() == 2);
        CHECK(find(scoped, QStringLiteral("pcgame:alpha")));    // Alpha
        CHECK(find(scoped, QStringLiteral("pcgame:alfredo")));  // Alfredo (owned-not-installed)

        // No owned list -> installed-only (unchanged pre-feature behavior).
        const MediaCatalog none = browse::pcGamesCatalog(installed, {}, {}, {}, {}, QString(), QString(), poster);
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
    CHECK(RecentStore::relaunchFor(QStringLiteral("battlenetgame")) == RL::BattleNetGame);

    // ---- #224: a Recents row carries the recipe to re-mint its link -------------------------------------
    //
    // The four fields are ids, never links: an addon manifest id, an item id, and two enum-ish strings. None
    // may ever hold a url with a query — that is #200's invariant and probe_cloudmerge §38 is what holds it
    // across the sync boundary. Here we only pin that they round-trip.
    {
        RecentStore::clear();
        RecentItem in;
        in.path  = QStringLiteral("https://store-034.example/dld/6f1e/movie.mkv");
        in.title = QStringLiteral("A Film");
        in.kind  = QStringLiteral("video");
        in.key   = QStringLiteral("eyJ0IjoiQSBGaWxtIiwiaCI6ImRlYWRiZWVm");
        in.sourceAddonId = QStringLiteral("com.example.allarr");
        in.sourceItemId  = QStringLiteral("eyJ0IjoiQSBGaWxtIiwiaCI6ImRlYWRiZWVm");
        in.sourceRoute   = QStringLiteral("direct");
        in.sourceType    = QStringLiteral("movie");
        RecentStore::add(in);

        const QVector<RecentItem> got = RecentStore::list();
        CHECK(got.size() == 1);
        CHECK(got[0].sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(got[0].sourceItemId  == QStringLiteral("eyJ0IjoiQSBGaWxtIiwiaCI6ImRlYWRiZWVm"));
        CHECK(got[0].sourceRoute   == QStringLiteral("direct"));
        CHECK(got[0].sourceType    == QStringLiteral("movie"));

        // find() by either identity. openRecent has the path and the resume key and nothing else, so this is
        // the lookup that lets it reach the recipe without widening HomeView's openRecent signal.
        CHECK(RecentStore::find(in.key).sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(RecentStore::find(in.path).sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(RecentStore::find(QStringLiteral("nothing-here")).path.isEmpty());

        // A LEGACY ROW — written before this change — reads back with the four fields empty and is not
        // corrupted by their absence. This is the assertion that stops the fix from eating existing recents.
        RecentStore::clear();
        RecentItem legacy;
        legacy.path = QStringLiteral("C:\\Users\\me\\Videos\\old.mkv");
        legacy.kind = QStringLiteral("video");
        RecentStore::add(legacy);
        const QVector<RecentItem> old = RecentStore::list();
        CHECK(old.size() == 1);
        CHECK(old[0].sourceAddonId.isEmpty());
        CHECK(old[0].sourceRoute.isEmpty());
        CHECK(old[0].path == QStringLiteral("C:\\Users\\me\\Videos\\old.mkv"));
        RecentStore::clear();
    }

    // ---- 6. Marks-sanity foundation: game Recents draw the game icon (keyFor keys are <store>:<id>) --------
    CHECK(browse::iconTypeForKind(QStringLiteral("steamgame")) == QStringLiteral("game"));
    CHECK(browse::iconTypeForKind(QStringLiteral("epicgame"))  == QStringLiteral("game"));
    CHECK(browse::iconTypeForKind(QStringLiteral("goggame"))   == QStringLiteral("game"));
    CHECK(browse::iconTypeForKind(QStringLiteral("battlenetgame")) == QStringLiteral("game"));

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
        const MediaCatalog cat = browse::pcGamesCatalog({}, installed, {}, {}, {}, QString(), QString());
        CHECK(cat.items.size() == 2);
        const MediaItem* ace = find(cat, QStringLiteral("pcgame:ace game"));
        CHECK(ace && ace->mime == QStringLiteral("pcgame"));
        CHECK(ace && ace->url.isEmpty());   // no url -> the picker decides the launch
        CHECK(ace && ace->title == QStringLiteral("Ace Game"));
        CHECK(ace && ace->pcSources.size() == 1 && ace->pcSources[0].launcher == QStringLiteral("epic")
              && ace->pcSources[0].launchId == QStringLiteral("Ace") && ace->pcSources[0].ready);
        // Query scopes by name.
        const MediaCatalog scoped = browse::pcGamesCatalog({}, installed, {}, {}, {}, QStringLiteral("zed"),
                                                           QString());
        CHECK(scoped.items.size() == 1 && find(scoped, QStringLiteral("pcgame:zed game")));
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

    // ---- 8b. GOG in the PC Games folder: the exe rides the SOURCE (the launchPcExe target) ----------------
    {
        QList<GogGame> installed{
            { QStringLiteral("100"), QStringLiteral("Alpha"), QStringLiteral("C:/G/Alpha/a.exe"), QStringLiteral("C:/G/Alpha") },
            { QStringLiteral("200"), QStringLiteral("Bravo"), QStringLiteral("C:/G/Bravo/b.exe"), QStringLiteral("C:/G/Bravo") } };
        const MediaCatalog cat = browse::pcGamesCatalog({}, {}, installed, {}, {}, QString(), QString());
        CHECK(cat.items.size() == 2);
        const MediaItem* alpha = find(cat, QStringLiteral("pcgame:alpha"));
        CHECK(alpha && alpha->mime == QStringLiteral("pcgame"));
        CHECK(alpha && alpha->url.isEmpty());              // never on the tile — the picker resolves it
        CHECK(alpha && alpha->pcSources.size() == 1
              && alpha->pcSources[0].launcher == QStringLiteral("gog")
              && alpha->pcSources[0].exePath == QStringLiteral("C:/G/Alpha/a.exe") // the exe rides the source
              && alpha->pcSources[0].launchUrl.isEmpty()   // DRM-free: an exe, not a protocol handoff
              && alpha->pcSources[0].ready);
        const MediaCatalog scoped = browse::pcGamesCatalog({}, {}, installed, {}, {}, QStringLiteral("brav"),
                                                            QString());
        CHECK(scoped.items.size() == 1 && find(scoped, QStringLiteral("pcgame:bravo")));
    }

    // ---- Battle.net: pure entry parse + INI-fixture registry scan --------------------------------
    {
        using BattleNetLibrary::parseUninstallEntry;
        // A Blizzard entry is kept, its code resolved from the title.
        const BattleNetGame wow = parseUninstallEntry(QStringLiteral("World of Warcraft"),
            QStringLiteral("Blizzard Entertainment"), QStringLiteral("C:\\Games\\World of Warcraft"));
        CHECK(wow.name == QStringLiteral("World of Warcraft"));
        CHECK(wow.code == QStringLiteral("wow"));
        CHECK(wow.installDir == QStringLiteral("C:/Games/World of Warcraft"));   // separators normalized
        // A non-Blizzard publisher is filtered (empty name ⇒ callers drop it).
        CHECK(parseUninstallEntry(QStringLiteral("Some App"), QStringLiteral("Acme Inc"),
                                  QStringLiteral("C:\\Acme")).name.isEmpty());
        // A Blizzard title with no known code still parses — code empty ⇒ exe-launch fallback.
        const BattleNetGame unk = parseUninstallEntry(QStringLiteral("Blizzard Arcade Collection"),
            QStringLiteral("Blizzard Entertainment"), QStringLiteral("C:\\Games\\Arcade"));
        CHECK(!unk.name.isEmpty());
        CHECK(unk.code.isEmpty());
        // An entry with NO InstallLocation is incomplete and dropped — this is the Battle.net CLIENT's own
        // uninstall row (Blizzard publisher, real DisplayName, nothing to list or launch).
        CHECK(parseUninstallEntry(QStringLiteral("Battle.net"), QStringLiteral("Blizzard Entertainment"),
                                  QString()).name.isEmpty());
        // …and the client is rejected BY TITLE even when its row DOES carry an InstallLocation — the depth-2
        // exe scan can otherwise reach Battle.net/<build>/BlizzardBrowser.exe, clearing the launch-route gate
        // and shipping a tile that opens an embedded browser. Same rule catches the update agent.
        CHECK(parseUninstallEntry(QStringLiteral("Battle.net"), QStringLiteral("Blizzard Entertainment"),
                                  QStringLiteral("C:\\Program Files (x86)\\Battle.net")).name.isEmpty());
        CHECK(parseUninstallEntry(QStringLiteral("Blizzard Battle.net Update Agent"),
                                  QStringLiteral("Blizzard Entertainment"),
                                  QStringLiteral("C:\\ProgramData\\Battle.net\\Agent")).name.isEmpty());
        // The publisher gate is a startsWith, so "Blizzard Entertainment, Inc." passes …
        CHECK(!parseUninstallEntry(QStringLiteral("Hearthstone"),
                                   QStringLiteral("Blizzard Entertainment, Inc."),
                                   QStringLiteral("C:\\Games\\HS")).name.isEmpty());
        // … while an unrelated publisher that merely mentions Blizzard does not.
        CHECK(parseUninstallEntry(QStringLiteral("Blizzard Fan Tool"), QStringLiteral("Acme (for Blizzard)"),
                                  QStringLiteral("C:\\Acme")).name.isEmpty());
        // Case/spacing-insensitive title→code.
        CHECK(BattleNetLibrary::codeForTitle(QStringLiteral("  diablo   III  ")) == QStringLiteral("d3"));
        CHECK(BattleNetLibrary::codeForTitle(QStringLiteral("Totally Not A Blizzard Game")).isEmpty());
        CHECK(BattleNetLibrary::launchUri(QStringLiteral("wow")) == QStringLiteral("battlenet://wow"));
        // The code table is LONGEST-PREFIX-FIRST: reordering "starcraft ii" after "starcraft" would send
        // every SC2 title to s1. Pin the one order-dependent pair, plus the prefix match and a dropped row.
        CHECK(BattleNetLibrary::codeForTitle(QStringLiteral("StarCraft II: Wings of Liberty")) == QStringLiteral("s2"));
        CHECK(BattleNetLibrary::codeForTitle(QStringLiteral("StarCraft: Remastered")) == QStringLiteral("s1"));
        CHECK(BattleNetLibrary::codeForTitle(QStringLiteral("World of Warcraft Classic")) == QStringLiteral("wow"));
        CHECK(BattleNetLibrary::codeForTitle(QStringLiteral("Diablo IV")).isEmpty());   // dropped row ⇒ exe fallback

        // Fake-registry INI: groups = Uninstall subkeys, keys mirror the registry value names.
        // The SUBKEY order deliberately contradicts the DISPLAY-NAME order (AAA = "World of Warcraft",
        // ZZZ = "Overwatch") so the sort-by-name assertion below cannot pass vacuously — childGroups()
        // hands them back in subkey order.
        QTemporaryDir tmp; CHECK(tmp.isValid());
        const QString ini = tmp.path() + QStringLiteral("/bnet.ini");
        {
            QSettings s(ini, QSettings::IniFormat);
            s.setValue(QStringLiteral("AAA/DisplayName"), QStringLiteral("World of Warcraft"));
            s.setValue(QStringLiteral("AAA/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s.setValue(QStringLiteral("AAA/InstallLocation"), QStringLiteral("C:\\Games\\WoW"));
            s.setValue(QStringLiteral("ZZZ/DisplayName"), QStringLiteral("Overwatch"));
            s.setValue(QStringLiteral("ZZZ/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s.setValue(QStringLiteral("ZZZ/InstallLocation"), QStringLiteral("C:\\Games\\OW"));
            // Same title under a DIFFERENT subkey — the 64-bit and WOW6432Node views both carrying it.
            s.setValue(QStringLiteral("MMM_OtherView/DisplayName"), QStringLiteral("World of Warcraft"));
            s.setValue(QStringLiteral("MMM_OtherView/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s.setValue(QStringLiteral("MMM_OtherView/InstallLocation"), QStringLiteral("C:\\Games\\WoW"));
            // Blizzard publisher but a BLANK DisplayName — incomplete, dropped.
            s.setValue(QStringLiteral("BlankName/DisplayName"), QString());
            s.setValue(QStringLiteral("BlankName/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s.setValue(QStringLiteral("BlankName/InstallLocation"), QStringLiteral("C:\\Games\\Blank"));
            // Blizzard publisher, real DisplayName, NO InstallLocation — the client's own row, dropped.
            s.setValue(QStringLiteral("NoInstallDir/DisplayName"), QStringLiteral("Battle.net"));
            s.setValue(QStringLiteral("NoInstallDir/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s.setValue(QStringLiteral("Notepadpp/DisplayName"), QStringLiteral("Notepad++"));
            s.setValue(QStringLiteral("Notepadpp/Publisher"), QStringLiteral("Don Ho"));
            s.setValue(QStringLiteral("Notepadpp/InstallLocation"), QStringLiteral("C:\\npp"));
            s.sync();
        }
        const QVector<BattleNetGame> games = BattleNetLibrary::installedGames(ini);
        // 6 groups in, 2 out: non-Blizzard filtered, blank-name dropped, no-InstallLocation dropped,
        // duplicate title deduped.
        CHECK(games.size() == 2);
        CHECK(games[0].name == QStringLiteral("Overwatch"));         // sorted by NAME, not by subkey
        CHECK(games[1].name == QStringLiteral("World of Warcraft"));
        CHECK(games[1].code == QStringLiteral("wow"));
        CHECK(BattleNetLibrary::isAvailable(ini));
        CHECK(!BattleNetLibrary::isAvailable(tmp.path() + QStringLiteral("/missing.ini")));  // dormant

        // The exe fallback must find a NESTED binary: Blizzard titles routinely ship it under _retail_/ or
        // x64/, and a code-less title with only a top-level scan would list but never launch. Bounded to
        // root+2 levels, skipping asset dirs, preferring the largest real (non-plumbing) exe.
        {
            const QString game = tmp.path() + QStringLiteral("/ArcadeInstall");
            QDir().mkpath(game + QStringLiteral("/_retail_"));
            QDir().mkpath(game + QStringLiteral("/Data"));          // asset dir: must NOT be descended
            const auto put = [](const QString& p, int bytes) {
                QFile f(p); f.open(QIODevice::WriteOnly); f.write(QByteArray(bytes, 'x')); f.close();
            };
            put(game + QStringLiteral("/Battle.net Launcher.exe"), 4000);   // plumbing: skipped by name
            put(game + QStringLiteral("/Uninstall.exe"), 3000);             // plumbing: skipped by name
            put(game + QStringLiteral("/_retail_/Arcade.exe"), 9000);       // the real (nested) binary
            put(game + QStringLiteral("/Data/huge.exe"), 99000);            // in an asset dir: never chosen

            QSettings s2(ini, QSettings::IniFormat);
            s2.setValue(QStringLiteral("Arcade/DisplayName"), QStringLiteral("Blizzard Arcade Collection"));
            s2.setValue(QStringLiteral("Arcade/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s2.setValue(QStringLiteral("Arcade/InstallLocation"), game);
            s2.sync();

            const QVector<BattleNetGame> withNested = BattleNetLibrary::installedGames(ini);
            CHECK(withNested.size() == 3);        // the two kept above, plus the code-less Arcade entry
            const BattleNetGame* arcade = nullptr;
            for (const BattleNetGame& g : withNested)
                if (g.name == QStringLiteral("Blizzard Arcade Collection")) arcade = &g;
            CHECK(arcade != nullptr);
            if (arcade)
            {
                CHECK(arcade->code.isEmpty());                                   // no curated code ⇒ exe launch
                CHECK(arcade->exe.endsWith(QStringLiteral("_retail_/Arcade.exe")));  // nested binary found
            }

            // The launch-route gate: a codeless Blizzard title whose install dir holds NO usable exe must not
            // ship as a dead tile. (Only plumbing binaries here, all skipped by name.)
            const QString dead = tmp.path() + QStringLiteral("/DeadInstall");
            QDir().mkpath(dead);
            put(dead + QStringLiteral("/Uninstall.exe"), 2000);
            QSettings s3(ini, QSettings::IniFormat);
            s3.setValue(QStringLiteral("Dead/DisplayName"), QStringLiteral("Blizzard Something Uncurated"));
            s3.setValue(QStringLiteral("Dead/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s3.setValue(QStringLiteral("Dead/InstallLocation"), dead);
            s3.sync();
            const QVector<BattleNetGame> afterDead = BattleNetLibrary::installedGames(ini);
            for (const BattleNetGame& g : afterDead)
                CHECK(g.name != QStringLiteral("Blizzard Something Uncurated"));   // no code + no exe ⇒ dropped

            // The exe scan is BOUNDED at root+2: a depth-2 binary is found, a depth-3 one is not (else a
            // multi-GB install would be walked on every Games-root render).
            const QString deep = tmp.path() + QStringLiteral("/DeepInstall");
            QDir().mkpath(deep + QStringLiteral("/Game/bin"));
            QDir().mkpath(deep + QStringLiteral("/a/b/c"));
            put(deep + QStringLiteral("/Game/bin/Deep.exe"), 5000);   // depth 2 ⇒ found
            put(deep + QStringLiteral("/a/b/c/TooDeep.exe"), 90000);  // depth 3 ⇒ never seen, despite being bigger
            QSettings s4(ini, QSettings::IniFormat);
            s4.setValue(QStringLiteral("Deep/DisplayName"), QStringLiteral("Blizzard Deep Title"));
            s4.setValue(QStringLiteral("Deep/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s4.setValue(QStringLiteral("Deep/InstallLocation"), deep);
            s4.sync();
            const QVector<BattleNetGame> afterDeep = BattleNetLibrary::installedGames(ini);
            const BattleNetGame* deepG = nullptr;
            for (const BattleNetGame& g : afterDeep)
                if (g.name == QStringLiteral("Blizzard Deep Title")) deepG = &g;
            CHECK(deepG != nullptr);
            if (deepG) CHECK(deepG->exe.endsWith(QStringLiteral("Game/bin/Deep.exe")));
        }
    }

    // ---- Battle.net in the PC Games folder: the TWO-ROUTE split (the whole point of the mime) -------------
    // A coded title's SOURCE carries battlenet://<code> and no exe — MainWindow launches the client by URI. A
    // code-less one carries its exe and rides the monitored launchPcExe path, exactly like a GOG source.
    // Swapping either half silently breaks one of the two launch routes.
    {
        QList<BattleNetGame> installed;
        BattleNetGame a; a.name = QStringLiteral("World of Warcraft"); a.code = QStringLiteral("wow");
        a.installDir = QStringLiteral("C:/Games/WoW"); a.exe = QStringLiteral("C:/Games/WoW/Wow.exe");
        BattleNetGame b; b.name = QStringLiteral("Arcade");            // no code ⇒ exe route
        b.installDir = QStringLiteral("C:/Games/Arcade"); b.exe = QStringLiteral("C:/Games/Arcade/arcade.exe");
        installed << a << b;

        const MediaCatalog c = browse::pcGamesCatalog({}, {}, {}, installed, {}, QString(), QString());
        CHECK(c.items.size() == 2);
        const MediaItem* wow = find(c, QStringLiteral("pcgame:world of warcraft"));
        CHECK(wow && wow->mime == QStringLiteral("pcgame"));
        CHECK(wow && wow->type == QStringLiteral("game"));
        CHECK(wow && wow->title == QStringLiteral("World of Warcraft"));
        CHECK(wow && wow->systemHint == QStringLiteral("pc"));
        CHECK(wow && wow->pcSources.size() == 1);
        CHECK(wow && wow->pcSources[0].launchId == QStringLiteral("wow"));  // keyed by CODE, not name
        CHECK(wow && wow->pcSources[0].launchUrl == QStringLiteral("battlenet://wow")); // coded ⇒ URI launch
        CHECK(wow && wow->pcSources[0].exePath.isEmpty());
        const MediaItem* arc = find(c, QStringLiteral("pcgame:arcade"));
        CHECK(arc && arc->pcSources.size() == 1);
        CHECK(arc && arc->pcSources[0].launchUrl.isEmpty());                // code-less ⇒ no protocol launch
        CHECK(arc && arc->pcSources[0].exePath == QStringLiteral("C:/Games/Arcade/arcade.exe"));

        // Query scopes by name (the in-folder search path).
        const MediaCatalog scoped = browse::pcGamesCatalog({}, {}, {}, installed, {}, QStringLiteral("arca"),
                                                            QString());
        CHECK(scoped.items.size() == 1 && find(scoped, QStringLiteral("pcgame:arcade")));
        CHECK(browse::pcGamesCatalog({}, {}, {}, QList<BattleNetGame>(), {}, QString(), QString())
                  .items.isEmpty()); // dormant
    }

    // ---- A Battle.net Recent groups under the games catalogue's Recent (like steam/epic/gog) --------------
    {
        QList<RecentItem> all;
        RecentItem r; r.path = QStringLiteral("battlenet://wow"); r.title = QStringLiteral("WoW");
        r.kind = QStringLiteral("battlenetgame"); r.key = QStringLiteral("bnet:wow");
        all << r;
        const MediaCatalog cat = browse::recentsCatalog(all, QStringLiteral("game"));
        CHECK(cat.items.size() == 1);
        CHECK(cat.items[0].mime == QStringLiteral("battlenetgame"));
    }

    // ==== PLAYLIST STORE-GAME BRANCHES (Task 2 ride-along) ================================================

    // ---- 9. playlistItemsCatalog: a store game added to a playlist becomes a LAUNCHABLE tile ---------------
    // The builder branch table: steam:/epic:/gog: entries mirror their console tiles (dead before this task);
    // a path-only entry is a local game; an ordinary addon entry stays a plain drill item.
    {
        Playlist p;
        p.name = QStringLiteral("Mixed");
        auto add = [&](const QString& itemId, const QString& title, const QString& path, const QString& kind) {
            PlaylistEntry e; e.itemId = itemId; e.title = title; e.type = QStringLiteral("game");
            e.path = path; e.kind = kind; p.items.push_back(e);
        };
        add(QStringLiteral("steam:730"), QStringLiteral("CS"),      QString(),                     QString());
        add(QStringLiteral("epic:Fortnite"), QStringLiteral("FN"),  QString(),                     QString());
        add(QStringLiteral("gog:100"), QStringLiteral("Witcher"),   QStringLiteral("C:/G/w.exe"),  QString());
        add(QStringLiteral("local-1"), QStringLiteral("Doom"),      QStringLiteral("C:/G/doom.exe"), QStringLiteral("pcgame"));
        // Both Battle.net routes ride the ONE gog-shaped branch: a coded entry was added with an empty url so its
        // path is empty (launch builds battlenet://), a code-less one persisted its exe.
        add(QStringLiteral("bnet:wow"), QStringLiteral("WoW"),      QString(),                     QString());
        add(QStringLiteral("bnet:Hearthstone"), QStringLiteral("HS"), QStringLiteral("C:/G/hs.exe"), QString());
        // A plain addon entry (no store prefix, no path) — stays a bare drill item, no launch mime.
        { PlaylistEntry e; e.itemId = QStringLiteral("addon:movie1"); e.title = QStringLiteral("Movie");
          e.type = QStringLiteral("movie"); p.items.push_back(e); }

        const MediaCatalog cat = browse::playlistItemsCatalog(p);
        CHECK(cat.items.size() == 7);

        const MediaItem* steam = find(cat, QStringLiteral("steam:730"));
        CHECK(steam && steam->mime == QStringLiteral("steamgame"));
        CHECK(steam && steam->url.isEmpty());                    // launches by id (steam://), no url needed

        const MediaItem* epic = find(cat, QStringLiteral("epic:Fortnite"));
        CHECK(epic && epic->mime == QStringLiteral("epicgame"));
        CHECK(epic && epic->url.isEmpty());                      // launches by id (com.epicgames.launcher://)

        const MediaItem* gog = find(cat, QStringLiteral("gog:100"));
        CHECK(gog && gog->mime == QStringLiteral("goggame"));
        CHECK(gog && gog->url == QStringLiteral("C:/G/w.exe"));  // the persisted exe rides back onto the tile

        const MediaItem* bnetCoded = find(cat, QStringLiteral("bnet:wow"));
        CHECK(bnetCoded && bnetCoded->mime == QStringLiteral("battlenetgame"));
        CHECK(bnetCoded && bnetCoded->url.isEmpty());            // coded: launches by battlenet:// URI, no url

        const MediaItem* bnetExe = find(cat, QStringLiteral("bnet:Hearthstone"));
        CHECK(bnetExe && bnetExe->mime == QStringLiteral("battlenetgame"));
        CHECK(bnetExe && bnetExe->url == QStringLiteral("C:/G/hs.exe")); // code-less: the persisted exe rides back

        const MediaItem* local = find(cat, QStringLiteral("local-1"));
        CHECK(local && local->mime == QStringLiteral("localgame:pcgame"));
        CHECK(local && local->url == QStringLiteral("C:/G/doom.exe"));

        const MediaItem* addonItem = find(cat, QStringLiteral("addon:movie1"));
        CHECK(addonItem && addonItem->mime.isEmpty() && addonItem->url.isEmpty()); // plain drill item, no launch
    }

    // ---- pcGamesCatalog's inline protocol URIs must equal the canonical ones --------------------------
    // browse::pcGamesCatalog builds the Epic and Battle.net launch URIs INLINE, because probe_browse /
    // probe_locallib / probe_perf compile SyntheticCatalogs.cpp without EpicLibrary.cpp or
    // BattleNetLibrary.cpp and a call there would be a CI-only link break. This target DOES link both, so
    // it is the one place the two copies can be held against their originals — otherwise a change to
    // EpicLibrary::launchUrl would silently leave the merged PC folder launching nothing.
    {
        QList<EpicGame> ep;      { EpicGame g; g.appName = QStringLiteral("Pewter");
                                   g.name = QStringLiteral("Hades II"); ep << g; }
        QList<BattleNetGame> bn; { BattleNetGame g; g.code = QStringLiteral("wow");
                                   g.name = QStringLiteral("World of Warcraft"); bn << g; }
        const MediaCatalog pc = browse::pcGamesCatalog({}, ep, {}, bn, {}, QString(), QString(),
                                                       [](const QVector<pcgame::PcGameSource>&) {
                                                           return QString();
                                                       });
        const MediaItem* epicItem = find(pc, QStringLiteral("pcgame:hades ii"));
        CHECK(epicItem && epicItem->pcSources.size() == 1);
        CHECK(epicItem && epicItem->pcSources.size() == 1
              && epicItem->pcSources.at(0).launchUrl == EpicLibrary::launchUrl(QStringLiteral("Pewter")));
        const MediaItem* bnetItem = find(pc, QStringLiteral("pcgame:world of warcraft"));
        CHECK(bnetItem && bnetItem->pcSources.size() == 1
              && bnetItem->pcSources.at(0).launchUrl == BattleNetLibrary::launchUri(QStringLiteral("wow")));
    }

    if (failures == 0) { std::puts("IMPORTERS-OK"); return 0; }
    std::fprintf(stderr, "IMPORTERS: %d check(s) failed\n", failures);
    return 1;
}
