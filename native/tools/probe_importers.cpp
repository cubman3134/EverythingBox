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
#include "RecentStore.h"
#include "../src/browse/SyntheticCatalogs.h"

#include <QCoreApplication>
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
    CHECK(RecentStore::relaunchFor(QStringLiteral("pcgame"))    == RL::PcGame);
    CHECK(RecentStore::relaunchFor(QStringLiteral("video"))     == RL::Video);
    CHECK(RecentStore::relaunchFor(QStringLiteral("audio"))     == RL::Audio);
    CHECK(RecentStore::relaunchFor(QStringLiteral("document"))  == RL::Document);
    CHECK(RecentStore::relaunchFor(QStringLiteral("game"))      == RL::Game);
    CHECK(RecentStore::relaunchFor(QStringLiteral("bogus"))     == RL::Unknown);
    CHECK(RecentStore::relaunchFor(QString())                   == RL::Unknown);

    // ---- 6. Marks-sanity foundation: a steamgame Recent draws the game icon (keyFor keys are steam:<appid>) -
    CHECK(browse::iconTypeForKind(QStringLiteral("steamgame")) == QStringLiteral("game"));

    if (failures == 0) { std::puts("IMPORTERS-OK"); return 0; }
    std::fprintf(stderr, "IMPORTERS: %d check(s) failed\n", failures);
    return 1;
}
