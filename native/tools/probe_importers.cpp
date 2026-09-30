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
//   * RecentStore's #224 re-mint recipe — the four source* fields round-trip through the store, find()
//     resolves a row by key, by path, or by the still-signed spelling of either, a keyless (bare-path)
//     re-open ADOPTS the prior row's recipe rather than blanking it, and a legacy row reads back with all
//     four empty AND without those keys appearing in its stored bytes;
//   * browse::iconTypeForKind — a "steamgame" Recent draws the game placeholder icon;
//   * browse::pcGamesCatalog's per-launcher source mapping — Epic's launcher URI, GOG's exe-on-the-source, and
//     the Battle.net two-route split (a coded title keys on its code and carries the battlenet:// URI; a
//     code-less one carries its exe ⇒ launchPcExe), plus the in-folder query scoping and the empty case.
//     (These were four per-launcher builders until the four folders became one; the mappings they pinned are
//     the same, restated on the builder that now performs them.)
//   * UbisoftLibrary (#60) - the pure registry-snapshot parser (installed / dir missing / DisplayName fallback /
//     duplicate ids / odd ids), the uplay://launch/<id>/0 URL, the fixture JSON and its EB_UITEST-only seam, the
//     Windows-only live reader, and Ubisoft as a SOURCE in the merged PC Games folder plus its Recents kind.
//
// Links only QtCore-friendly units (SteamLibrary/SyntheticCatalogs/MetaCache/RecentStore/AddonModels + the
// AppPaths/ProfileStore closure RecentStore pulls). relaunchFor/parse/TTL touch no store, but the #224 block
// DOES write one: RecentStore::add/list/clear go through QSettings. That is safe because CMake compiles
// every probe target with EB_ISOLATED_DATA_DIR, which points AppPaths::dataDir() at a per-probe scratch
// directory — so the ini this writes is the probe's own, never the running profile's.
#include "SteamLibrary.h"
#include "EpicLibrary.h"
#include "GogLibrary.h"
#include "BattleNetLibrary.h"
#include "UbisoftLibrary.h"
#include "RecentStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Tombstones.h"   // the supersede must NOT date a removal (see the chapter block)
#include "../src/browse/SyntheticCatalogs.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
    // may ever hold a url with a query — that is #200's invariant and probe_cloudmerge §38 holds it across
    // the sync boundary. Here we only pin that they round-trip.
    {
        RecentStore::clear();
        // Deliberately NOT the same string as the key. On a real file-provider row `sitem` and `key` hold the
        // same blob, so an implementation that mistakenly read `sitem` out of the `key` JSON field would have
        // passed every assertion below. Distinct literals are what let these checks fail.
        const QString kItemId = QStringLiteral("meta:eyJoIjoiY2FmZWQwMGQifQ");
        RecentItem in;
        in.path  = QStringLiteral("https://store-034.example/dld/6f1e/movie.mkv");
        in.title = QStringLiteral("A Film");
        in.kind  = QStringLiteral("video");
        in.key   = QStringLiteral("eyJ0IjoiQSBGaWxtIiwiaCI6ImRlYWRiZWVm");
        in.sourceAddonId = QStringLiteral("com.example.allarr");
        in.sourceItemId  = kItemId;
        in.sourceRoute   = QStringLiteral("direct");
        in.sourceType    = QStringLiteral("movie");
        RecentStore::add(in);

        const QVector<RecentItem> got = RecentStore::list();
        CHECK(got.size() == 1);
        CHECK(got[0].sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(got[0].sourceItemId  == kItemId);
        CHECK(got[0].sourceRoute   == QStringLiteral("direct"));
        CHECK(got[0].sourceType    == QStringLiteral("movie"));

        // find() by either identity. openRecent has the path and the resume key and nothing else, so this is
        // the lookup that lets it reach the recipe without widening HomeView's openRecent signal.
        CHECK(RecentStore::find(in.key).sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(RecentStore::find(in.path).sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(RecentStore::find(QStringLiteral("nothing-here")).path.isEmpty());
        // And by the SIGNED spelling a caller may still be holding: the row stores the url with its query
        // taken off (#200), so find() has to match the argument scrubbed too, exactly as remove() does.
        CHECK(RecentStore::find(in.path + QStringLiteral("?token=deadbeef&exp=1")).sourceItemId == kItemId);

        // A BARE-PATH RE-OPEN MUST NOT BLANK THE RECIPE. Opening from the Recents list goes through the
        // keyless route — a path and a kind and nothing else — so if add()'s adoption block did not carry the
        // four fields across, the very first re-open would rewrite the row with its recipe gone and #224's
        // fix would work exactly once per item.
        RecentItem reopen;
        reopen.path = in.path;
        reopen.kind = QStringLiteral("video");
        RecentStore::add(reopen);
        const QVector<RecentItem> after = RecentStore::list();
        CHECK(after.size() == 1);                      // adopted the prior identity, so no twin row
        CHECK(after[0].key   == in.key);
        CHECK(after[0].title == QStringLiteral("A Film"));
        CHECK(after[0].sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(after[0].sourceItemId  == kItemId);
        CHECK(after[0].sourceRoute   == QStringLiteral("direct"));
        CHECK(after[0].sourceType    == QStringLiteral("movie"));

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
        CHECK(old[0].sourceItemId.isEmpty());
        CHECK(old[0].sourceRoute.isEmpty());
        CHECK(old[0].sourceType.isEmpty());
        CHECK(old[0].path == QStringLiteral("C:\\Users\\me\\Videos\\old.mkv"));

        // AND ITS STORED BYTES DO NOT GROW. saveList guards all four writes with !isEmpty() precisely so a
        // legacy record gains no keys, and only the round-trip was pinned — dropping all four guards would
        // have survived this probe. Read the raw setting the way RecentStore writes it; the probe's data dir
        // is isolated at compile time, so this is the probe's own ini.
        {
            QSettings raw(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                          QSettings::IniFormat);
            const QString prof = ProfileStore::currentId().isEmpty() ? QStringLiteral("default")
                                                                    : ProfileStore::currentId();
            const QString json =
                raw.value(QStringLiteral("recent/") + prof + QStringLiteral("/items")).toString();
            CHECK(json.contains(QStringLiteral("old.mkv")));   // we are looking at the record just written
            CHECK(!json.contains(QStringLiteral("saddon")));
            CHECK(!json.contains(QStringLiteral("sitem")));
            CHECK(!json.contains(QStringLiteral("sroute")));
            CHECK(!json.contains(QStringLiteral("stype")));
        }
        RecentStore::clear();
    }

    // ---- A chapter arrival supersedes the run's other rows, WITHOUT tombstoning them --------------------
    //
    // One row per series, not per chapter: a Recents list holds forty rows and an evening with a manga is
    // ten chapters, so recording each one would evict everything else the profile has watched or played.
    // The reader therefore drops the run's other entries as it lands (ChapterRecent::superseded feeds this).
    //
    // AND IT IS NOT remove(). A tombstone is the user saying "forget this"; this is the same reading
    // position moving forward one chapter, which is what add()'s own de-dup removal is — and a tombstone
    // here would suppress a chapter you later went back and re-read on every peer that syncs.
    {
        RecentStore::clear();
        auto chapter = [](const QString& id, const QString& title) {
            RecentItem r;
            r.path = QStringLiteral("C:/cache/manga/") + id + QStringLiteral(".cbz");
            r.title = title;
            r.kind = QStringLiteral("document");
            r.key = id;
            r.sourceAddonId = QStringLiteral("com.example.aio");
            r.sourceType = QStringLiteral("manga_chapter");
            return r;
        };
        RecentStore::add(chapter(QStringLiteral("ch4"), QStringLiteral("Chainsaw Man — Ch. 4")));
        RecentStore::add(chapter(QStringLiteral("ch5"), QStringLiteral("Chainsaw Man — Ch. 5")));
        CHECK(RecentStore::list().size() == 2);           // both rows exist until something supersedes one

        // Reading on: the arrival drops its siblings, then writes itself at the front.
        RecentStore::dropSuperseded({ QStringLiteral("ch4"), QStringLiteral("ch5") });
        RecentStore::add(chapter(QStringLiteral("ch6"), QStringLiteral("Chainsaw Man — Ch. 6")));
        const QVector<RecentItem> one = RecentStore::list();
        CHECK(one.size() == 1);
        CHECK(one[0].key == QStringLiteral("ch6"));

        // NO TOMBSTONE, which is the half a passing size check would not notice: going back to chapter 4
        // must put it back. A dated removal would have that re-open suppressed on every peer whose clock
        // reads it at or before the removal's second.
        const QString tombNs = QStringLiteral("recent/")
            + (ProfileStore::currentId().isEmpty() ? QStringLiteral("default") : ProfileStore::currentId());
        auto tombstoned = [&tombNs](const QString& key) {
            for (const Tombstones::Entry& e : Tombstones::all(tombNs)) if (e.key == key) return true;
            return false;
        };
        CHECK(!tombstoned(QStringLiteral("ch4")));
        RecentStore::add(chapter(QStringLiteral("ch4"), QStringLiteral("Chainsaw Man — Ch. 4")));
        CHECK(RecentStore::list().size() == 2);

        // THE CONTROL, so the assertion above cannot pass merely by naming the wrong tombstone store: an
        // EXPLICIT remove of a row in that same store does date it.
        RecentStore::remove(QStringLiteral("ch6"));
        CHECK(tombstoned(QStringLiteral("ch6")));
        CHECK(RecentStore::list().size() == 1);

        // A path-identified row (the Files lane, and every local document row) is reachable by the same
        // call: dropSuperseded matches path OR key, exactly as remove() does.
        RecentStore::clear();
        RecentItem local;
        local.path = QStringLiteral("C:/Comics/a.cbz");
        local.kind = QStringLiteral("document");
        RecentStore::add(local);
        RecentStore::dropSuperseded({ QStringLiteral("C:/Comics/a.cbz") });
        CHECK(RecentStore::list().isEmpty());

        // An id nothing matches is a no-op, not an error and not a rewrite.
        RecentStore::add(chapter(QStringLiteral("ch9"), QStringLiteral("Chainsaw Man — Ch. 9")));
        RecentStore::dropSuperseded({ QStringLiteral("not-a-row"), QString() });
        CHECK(RecentStore::list().size() == 1);
        RecentStore::dropSuperseded({});
        CHECK(RecentStore::list().size() == 1);

        // A chapter row names its addon and what its id IS, but carries no route and no item id — so the
        // #224 router still replays its cached CBZ rather than trying to re-mint a link for it.
        CHECK(RecentStore::reopenFor(RecentStore::list()[0], true) == RecentStore::Reopen::ReplayPath);
        CHECK(RecentStore::reopenFor(RecentStore::list()[0], false) == RecentStore::Reopen::ReplayPath);
        RecentStore::clear();
    }

    // ---- #224: the re-open routing table --------------------------------------------------------------
    {
        using RO = RecentStore::Reopen;
        RecentItem bare;                                  // a local file / legacy row: no recipe at all
        bare.path = QStringLiteral("C:\\x\\y.mkv");
        CHECK(RecentStore::reopenFor(bare, false) == RO::ReplayPath);
        CHECK(RecentStore::reopenFor(bare, true)  == RO::ReplayPath); // an installed addon is irrelevant here

        RecentItem direct;
        direct.path = QStringLiteral("https://h.example/dld/6f1e/m.mkv");
        direct.sourceAddonId = QStringLiteral("com.example.allarr");
        direct.sourceItemId  = QStringLiteral("eyJ0IjoiQSBGaWxt");
        direct.sourceRoute   = QStringLiteral("direct");
        direct.sourceType    = QStringLiteral("movie");
        CHECK(RecentStore::reopenFor(direct, true)  == RO::ResolveDirect);
        // The addon this row names is not installed on THIS device. #77 (roster sync) is open, so a row that
        // synced from another device can legitimately name one that is absent — a defined degradation with
        // its own message, NOT a silent fall back to replaying a link that cannot work.
        CHECK(RecentStore::reopenFor(direct, false) == RO::SourceMissing);

        RecentItem imdb = direct;
        imdb.sourceRoute  = QStringLiteral("imdb");
        imdb.sourceItemId = QStringLiteral("tt0111161");
        CHECK(RecentStore::reopenFor(imdb, true) == RO::ResolveImdb);
        // The imdb route resolves across every installed stream provider rather than one named addon, so a
        // missing named addon does not disqualify it.
        CHECK(RecentStore::reopenFor(imdb, false) == RO::ResolveImdb);

        // A HALF-WRITTEN RECIPE IS NOT A RECIPE. A row with a route but no item id (a truncated peer blob, a
        // hand-edited ini) must fall back to today's behaviour rather than calling resolve with an empty id,
        // which every provider answers with "no source" — a dead end wearing a different message.
        RecentItem partial = direct;
        partial.sourceItemId.clear();
        CHECK(RecentStore::reopenFor(partial, true) == RO::ReplayPath);
        RecentItem noRoute = direct;
        noRoute.sourceRoute.clear();
        CHECK(RecentStore::reopenFor(noRoute, true) == RO::ReplayPath);
        // AND A ROUTE AND AN ID WITH NO TYPE IS STILL NOT A RECIPE. Both resolve verdicts consume all three
        // fields, not two: resolveStreamByImdb(type, id) puts the type into the provider's /stream/{type}/{id}
        // path and early-outs on an empty id and on nothing else, so an empty type is dispatched rather than
        // refused, and ResolveDirect hands the type on as the MediaItem's own. A row holding two of the three
        // is not hypothetical — add()'s adoption merge copies each recipe field across under its own
        // !isEmpty() test, so a mixed row is constructible without anyone hand-editing an ini.
        RecentItem noType = direct;
        noType.sourceType.clear();
        CHECK(RecentStore::reopenFor(noType, true)  == RO::ReplayPath);
        CHECK(RecentStore::reopenFor(noType, false) == RO::ReplayPath);  // not SourceMissing: no recipe at all
        RecentItem imdbNoType = imdb;
        imdbNoType.sourceType.clear();
        CHECK(RecentStore::reopenFor(imdbNoType, true) == RO::ReplayPath);
        // Both sides of addonAvailable, as for `noType` above. The imdb route ignores the flag entirely, so
        // the assertion costs a comparison and pins that the completeness test runs BEFORE the routing rather
        // than the imdb branch happening to swallow a half-written row on one value of the flag.
        CHECK(RecentStore::reopenFor(imdbNoType, false) == RO::ReplayPath);
        // A DIRECT ROW THAT NAMES NO ADDON. "direct" means "ask the one addon that knows this id space", so a
        // row without one is an incomplete recipe, not a request to an absent source: SourceMissing would tell
        // the user to install an add-on the row never named, and refuse a replay that might have worked.
        // Constructible the same way as the mixed rows above — add()'s adoption merge copies each recipe field
        // under its own !isEmpty() test — so it needs no hand-edited ini.
        RecentItem directNoAddon = direct;
        directNoAddon.sourceAddonId.clear();
        CHECK(RecentStore::reopenFor(directNoAddon, false) == RO::ReplayPath);
        CHECK(RecentStore::reopenFor(directNoAddon, true)  == RO::ReplayPath);
        // An UNKNOWN route string — a row written by a newer build than this one — replays rather than
        // guessing. Forward compatibility costs one comparison here and a wrong guess costs a 403.
        RecentItem future = direct;
        future.sourceRoute = QStringLiteral("torrentstream");
        CHECK(RecentStore::reopenFor(future, true) == RO::ReplayPath);
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

    // ---- appmanifest_<id>.acf: the install folder (issue #61) -----------------------------------------------
    // A Steam launch watch falls back to "a process under the game's install folder" when RunningAppID is
    // absent, so the folder must come out of the manifest exactly. The fixture is the shape Steam writes: the
    // top-level AppState block, "installdir" among the other keys, a nested UserConfig block after it.
    {
        const QString acf = QStringLiteral(
            "\"AppState\"\n"
            "{\n"
            "\t\"appid\"\t\t\"620\"\n"
            "\t\"Universe\"\t\t\"1\"\n"
            "\t\"LauncherPath\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\\\\steam.exe\"\n"
            "\t\"name\"\t\t\"Portal 2\"\n"
            "\t\"StateFlags\"\t\t\"4\"\n"
            "\t\"installdir\"\t\t\"Portal 2\"\n"
            "\t\"LastUpdated\"\t\t\"1717000000\"\n"
            "\t\"SizeOnDisk\"\t\t\"12884901888\"\n"
            "\t\"UserConfig\"\n"
            "\t{\n"
            "\t\t\"language\"\t\t\"english\"\n"
            "\t}\n"
            "}\n");
        CHECK(SteamLibrary::manifestInstallDir(acf) == QStringLiteral("Portal 2"));
        // Key case does not matter (older clients wrote "InstallDir"); no installdir -> empty.
        CHECK(SteamLibrary::manifestInstallDir(QStringLiteral("\"AppState\"{\"appid\" \"10\" \"InstallDir\" \"Counter-Strike\"}"))
              == QStringLiteral("Counter-Strike"));
        CHECK(SteamLibrary::manifestInstallDir(QStringLiteral("\"AppState\"{\"appid\" \"10\" \"name\" \"X\"}")).isEmpty());
        CHECK(SteamLibrary::manifestInstallDir(QString()).isEmpty());
        // The folder lives under <library>/steamapps/common; either part missing -> no path.
        CHECK(SteamLibrary::installPath(QStringLiteral("D:/SteamLibrary"), QStringLiteral("Portal 2"))
              == QStringLiteral("D:/SteamLibrary/steamapps/common/Portal 2"));
        CHECK(SteamLibrary::installPath(QStringLiteral("D:/SteamLibrary/"), QStringLiteral("Portal 2"))
              == QStringLiteral("D:/SteamLibrary/steamapps/common/Portal 2"));
        CHECK(SteamLibrary::installPath(QString(), QStringLiteral("Portal 2")).isEmpty());
        CHECK(SteamLibrary::installPath(QStringLiteral("D:/SteamLibrary"), QString()).isEmpty());
        // A positional {appid, name} still builds, with no install folder.
        const SteamGame owned{ QStringLiteral("620"), QStringLiteral("Portal 2") };
        CHECK(owned.installDir.isEmpty() && owned.available);
    }

    // ---- Ubisoft Connect (issue #60, increment 1): the pure snapshot parser ---------------------------------
    // The registry is fed in as plain records, so every rule is pinned here with no Ubisoft client installed.
    // Install dirs are REAL temp folders because "installed" means the folder exists; the one that must not
    // exist is a path under the temp root that is never created.
    {
        using R = UbisoftRegRecord;
        QTemporaryDir root;
        CHECK(root.isValid());
        const QString base = root.path();
        const QString originsDir = base + QStringLiteral("/games/Assassin's Creed Origins");
        const QString anno       = base + QStringLiteral("/games/Anno 1800");
        const QString fallback   = base + QStringLiteral("/games/Far Cry 5");
        CHECK(QDir().mkpath(originsDir) && QDir().mkpath(anno) && QDir().mkpath(fallback));
        const QString gone = base + QStringLiteral("/games/Uninstalled Long Ago");   // never created

        auto inst = [](const QString& key, const QString& dir) {
            R r; r.source = R::Installs; r.keyName = key; r.installDir = dir; return r; };
        auto unin = [](const QString& key, const QString& name, const QString& loc) {
            R r; r.source = R::Uninstall; r.keyName = key; r.displayName = name; r.installLocation = loc; return r; };

        // Installed: InstallDir exists, DisplayName from the uninstall key. Ubisoft writes FORWARD slashes and a
        // trailing slash; the parser keeps forward slashes and drops the trailing one.
        {
            QVector<R> snap;
            snap << inst(QStringLiteral("3539"), originsDir + QStringLiteral("/"))
                 << unin(QStringLiteral("Uplay Install 3539"), QStringLiteral("Assassin's Creed® Origins"),
                         QString());
            const QVector<UbisoftGame> g = UbisoftLibrary::parseSnapshot(snap);
            CHECK(g.size() == 1);
            CHECK(g.size() == 1 && g[0].id == QStringLiteral("3539"));
            CHECK(g.size() == 1 && g[0].name == QStringLiteral("Assassin's Creed® Origins"));
            CHECK(g.size() == 1 && g[0].installDir == originsDir);
            CHECK(g.size() == 1 && g[0].available);
        }
        // Install dir MISSING: an InstallDir that does not exist on disk is not installed — and neither is an
        // id with no dir at all (an Installs row with an empty InstallDir, an uninstall key with no location).
        {
            QVector<R> snap;
            snap << inst(QStringLiteral("720"), gone)
                 << unin(QStringLiteral("Uplay Install 720"), QStringLiteral("Gone Game"), gone)
                 << inst(QStringLiteral("721"), QString())
                 << unin(QStringLiteral("Uplay Install 722"), QStringLiteral("No Dir Anywhere"), QString());
            CHECK(UbisoftLibrary::parseSnapshot(snap).isEmpty());
        }
        // DisplayName MISSING: the name falls back to the install folder's own name (Playnite's rule).
        {
            QVector<R> snap;
            snap << inst(QStringLiteral("4311"), anno + QStringLiteral("/"));
            const QVector<UbisoftGame> g = UbisoftLibrary::parseSnapshot(snap);
            CHECK(g.size() == 1 && g[0].name == QStringLiteral("Anno 1800"));
            // A whitespace-only DisplayName is no name either.
            snap << unin(QStringLiteral("Uplay Install 4311"), QStringLiteral("   "), QString());
            const QVector<UbisoftGame> g2 = UbisoftLibrary::parseSnapshot(snap);
            CHECK(g2.size() == 1 && g2[0].name == QStringLiteral("Anno 1800"));
        }
        // InstallLocation is the FALLBACK, used only when InstallDir is empty — backslashes normalised.
        {
            QString winStyle = fallback;
            winStyle.replace(QLatin1Char('/'), QLatin1Char('\\'));
            winStyle += QLatin1Char('\\');
            QVector<R> snap;
            snap << inst(QStringLiteral("856"), QString())
                 << unin(QStringLiteral("Uplay Install 856"), QStringLiteral("Far Cry® 5"), winStyle);
            const QVector<UbisoftGame> g = UbisoftLibrary::parseSnapshot(snap);
            CHECK(g.size() == 1 && g[0].installDir == fallback && g[0].name == QStringLiteral("Far Cry® 5"));
            // An uninstall-only id (no Installs row) with a real location is installed too.
            QVector<R> only; only << unin(QStringLiteral("Uplay Install 856"), QStringLiteral("Far Cry® 5"), fallback);
            CHECK(UbisoftLibrary::parseSnapshot(only).size() == 1);
            // …but InstallLocation does NOT override a non-empty InstallDir: a stale InstallDir is not installed.
            QVector<R> stale;
            stale << inst(QStringLiteral("857"), gone)
                  << unin(QStringLiteral("Uplay Install 857"), QStringLiteral("Stale"), fallback);
            CHECK(UbisoftLibrary::parseSnapshot(stale).isEmpty());
        }
        // DUPLICATE ids: both registry views (and both hives) describe one game — ONE entry, first non-empty
        // field wins, and whitespace around a key name does not make a second game.
        {
            QVector<R> snap;
            snap << inst(QStringLiteral("3539"), originsDir)
                 << inst(QStringLiteral(" 3539 "), originsDir)
                 << unin(QStringLiteral("Uplay Install 3539"), QString(), QString())
                 << unin(QStringLiteral("Uplay Install 3539"), QStringLiteral("Assassin's Creed® Origins"), QString())
                 << unin(QStringLiteral("uplay install 3539"), QStringLiteral("Late Duplicate Name"), QString());
            const QVector<UbisoftGame> g = UbisoftLibrary::parseSnapshot(snap);
            CHECK(g.size() == 1);
            CHECK(g.size() == 1 && g[0].name == QStringLiteral("Assassin's Creed® Origins"));
        }
        // ODD ids: an id is pasted into a URL and a record key, so anything but decimal digits is refused —
        // never "cleaned". A non-Ubisoft uninstall key is not ours at all.
        {
            QVector<R> snap;
            const QStringList bad = { QString(), QStringLiteral("   "), QStringLiteral("abc"),
                                      QStringLiteral("12/34"), QStringLiteral("12 34"), QStringLiteral("-5"),
                                      QStringLiteral("5?x=1"), QString::fromUtf8("\xEF\xBC\x91\xEF\xBC\x92"),
                                      QStringLiteral("12345678901") };   // full-width "１２"; eleven digits
            for (const QString& b : bad) snap << inst(b, anno);
            snap << unin(QStringLiteral("Uplay Install"), QStringLiteral("No Id"), anno)
                 << unin(QStringLiteral("Uplay Install x9"), QStringLiteral("Bad Id"), anno)
                 << unin(QStringLiteral("Steam App 4311"), QStringLiteral("Not Ubisoft"), anno)
                 << unin(QStringLiteral("Uplay"), QStringLiteral("Ubisoft Connect"), anno);   // the CLIENT
            CHECK(UbisoftLibrary::parseSnapshot(snap).isEmpty());
            CHECK(!UbisoftLibrary::isValidId(QStringLiteral("abc")));
            CHECK(!UbisoftLibrary::isValidId(QString()));
            CHECK(UbisoftLibrary::isValidId(QStringLiteral("0")));
            CHECK(UbisoftLibrary::isValidId(QStringLiteral("1234567890")));
        }
        // Deterministic order: by name, case-insensitively, whatever order the registry enumerated.
        {
            QVector<R> snap;
            snap << inst(QStringLiteral("4311"), anno) << inst(QStringLiteral("3539"), originsDir)
                 << inst(QStringLiteral("856"), fallback);
            const QVector<UbisoftGame> g = UbisoftLibrary::parseSnapshot(snap);
            CHECK(g.size() == 3 && g[0].name == QStringLiteral("Anno 1800")
                  && g[1].name == QStringLiteral("Assassin's Creed Origins") && g[2].name == QStringLiteral("Far Cry 5"));
        }

        // The LAUNCH URL: uplay://launch/<id>/0, and nothing at all for an id isValidId refuses.
        CHECK(UbisoftLibrary::launchUri(QStringLiteral("3539")) == QStringLiteral("uplay://launch/3539/0"));
        CHECK(UbisoftLibrary::launchUri(QStringLiteral("abc")).isEmpty());
        CHECK(UbisoftLibrary::launchUri(QStringLiteral("12/34")).isEmpty());
        CHECK(UbisoftLibrary::launchUri(QString()).isEmpty());
        // ...and the id a launch / Recent names, read back from its key or its recorded URI.
        CHECK(UbisoftLibrary::idFrom(QStringLiteral("ubi:3539"), QString()) == QStringLiteral("3539"));
        CHECK(UbisoftLibrary::idFrom(QString(), QStringLiteral("uplay://launch/3539/0")) == QStringLiteral("3539"));
        CHECK(UbisoftLibrary::idFrom(QStringLiteral("ubi:3539"), QStringLiteral("uplay://launch/1/0"))
              == QStringLiteral("3539"));                                                  // the key wins
        CHECK(UbisoftLibrary::idFrom(QStringLiteral("ubi:x"), QString()).isEmpty());
        CHECK(UbisoftLibrary::idFrom(QString(), QStringLiteral("uplay://open/game/3539")).isEmpty());
        CHECK(UbisoftLibrary::idFrom(QStringLiteral("epic:Pewter"), QString()).isEmpty());

        // The fixture JSON form (what the EB_UITEST seam reads) round-trips into the same records.
        {
            QJsonObject i1; i1.insert(QStringLiteral("key"), QStringLiteral("4311"));
            i1.insert(QStringLiteral("InstallDir"), anno);
            QJsonObject u1; u1.insert(QStringLiteral("key"), QStringLiteral("Uplay Install 4311"));
            u1.insert(QStringLiteral("DisplayName"), QString::fromUtf8("Anno 1800\xE2\x84\xA2"));
            u1.insert(QStringLiteral("InstallLocation"), QString());
            QJsonObject u2; u2.insert(QStringLiteral("key"), QStringLiteral("Uplay Install 99"));
            u2.insert(QStringLiteral("DisplayName"), QStringLiteral("Ghost"));
            u2.insert(QStringLiteral("InstallLocation"), gone);
            QJsonObject top;
            top.insert(QStringLiteral("installs"), QJsonArray{ i1 });
            top.insert(QStringLiteral("uninstall"), QJsonArray{ u1, u2 });
            const QByteArray json = QJsonDocument(top).toJson();
            const QVector<R> snap = UbisoftLibrary::snapshotFromJson(json);
            CHECK(snap.size() == 3);
            CHECK(snap.size() == 3 && snap[0].source == R::Installs && snap[0].keyName == QStringLiteral("4311")
                  && snap[0].installDir == anno);
            CHECK(snap.size() == 3 && snap[1].source == R::Uninstall
                  && snap[1].keyName == QStringLiteral("Uplay Install 4311"));
            const QVector<UbisoftGame> g = UbisoftLibrary::parseSnapshot(snap);
            const QString annoTm = QString::fromUtf8("Anno 1800\xE2\x84\xA2");
            CHECK(g.size() == 1 && g[0].id == QStringLiteral("4311") && g[0].name == annoTm);
            CHECK(UbisoftLibrary::snapshotFromJson(QByteArrayLiteral("not json")).isEmpty());
            CHECK(UbisoftLibrary::snapshotFromJson(QByteArrayLiteral("[1,2]")).isEmpty());

            // The seam is honoured ONLY on the test channel: the fixture path alone changes nothing.
            QFile fx(base + QStringLiteral("/ubisoft-fixture.json"));
            CHECK(fx.open(QIODevice::WriteOnly) && fx.write(json) == json.size());
            fx.close();
            const QByteArray hadUitest = qgetenv("EB_UITEST");
            const bool uitestWasSet = qEnvironmentVariableIsSet("EB_UITEST");
            qunsetenv("EB_UITEST");
            qputenv("EB_UITEST_UBISOFT_FIXTURE", fx.fileName().toUtf8());
            bool fixtureLeaked = false;
            for (const UbisoftGame& x : UbisoftLibrary::installedGames())
                if (x.id == QStringLiteral("4311") && x.installDir == anno) fixtureLeaked = true;
            CHECK(!fixtureLeaked);
            qputenv("EB_UITEST", "1");
            const QVector<UbisoftGame> seam = UbisoftLibrary::installedGames();
            CHECK(seam.size() == 1 && seam[0].id == QStringLiteral("4311"));
            CHECK(UbisoftLibrary::isAvailable());
            qunsetenv("EB_UITEST_UBISOFT_FIXTURE");
            if (uitestWasSet) qputenv("EB_UITEST", hadUitest); else qunsetenv("EB_UITEST");
        }
    }

    // ---- Ubisoft: the live reader is Windows-only -----------------------------------------------------------
    // Off Windows there is no Ubisoft Connect: the reader is compiled (CI is GCC on Linux) and returns nothing.
    {
#ifdef Q_OS_WIN
        CHECK(UbisoftLibrary::hasLiveReader());
#else
        CHECK(!UbisoftLibrary::hasLiveReader());
        CHECK(UbisoftLibrary::readRegistrySnapshot().isEmpty());
        CHECK(UbisoftLibrary::installedGames().isEmpty());
        CHECK(!UbisoftLibrary::isAvailable());
#endif
    }

    // ---- Ubisoft in the ONE PC Games folder: a source, merged with the same game's other copies ------------
    {
        QList<SteamGame> steam;
        { SteamGame g; g.appid = QStringLiteral("582160"); g.name = QStringLiteral("Assassin's Creed Origins"); steam << g; }
        QList<UbisoftGame> ubi;
        { UbisoftGame g; g.id = QStringLiteral("3539"); g.name = QStringLiteral("Assassin's Creed® Origins");
          g.installDir = QStringLiteral("C:/Games/ACO"); ubi << g; }
        { UbisoftGame g; g.id = QStringLiteral("4311"); g.name = QStringLiteral("Anno 1800");
          g.installDir = QStringLiteral("C:/Games/Anno"); ubi << g; }
        { UbisoftGame g; g.id = QStringLiteral("856"); g.name = QStringLiteral("Far Cry 5");
          g.available = false; ubi << g; }                            // shown from the #62 last-good cache
        const auto noPoster = [](const QVector<pcgame::PcGameSource>&) { return QString(); };
        const MediaCatalog c = browse::pcGamesCatalog(steam, {}, {}, {}, {}, QString(), QString(), noPoster,
                                                      {}, {}, ubi);
        CHECK(c.items.size() == 3);   // Origins merged into one, plus Anno and Far Cry

        // THE MERGE: installed on Steam AND on Ubisoft Connect is ONE entry with two sources.
        // Looked up by the id the merge mints (pcgame::itemId owns its spelling), not a hand-built string.
        const MediaItem* aco = find(c, pcgame::itemId(QStringLiteral("Assassin's Creed Origins")));
        CHECK(aco != nullptr);
        CHECK(aco && aco->mime == QStringLiteral("pcgame") && aco->pcSources.size() == 2);
        const pcgame::PcGameSource* ubiSrc = nullptr;
        const pcgame::PcGameSource* steamSrc = nullptr;
        if (aco)
            for (const pcgame::PcGameSource& s : aco->pcSources)
            {
                if (s.launcher == QStringLiteral("ubisoft")) ubiSrc = &s;
                if (s.launcher == QStringLiteral("steam"))   steamSrc = &s;
            }
        CHECK(ubiSrc && steamSrc);
        CHECK(ubiSrc && ubiSrc->kind == pcgame::PcGameSource::LauncherInstalled);
        CHECK(ubiSrc && ubiSrc->launchId == QStringLiteral("3539"));
        CHECK(ubiSrc && ubiSrc->launchUrl == QStringLiteral("uplay://launch/3539/0"));
        CHECK(ubiSrc && ubiSrc->launchUrl == UbisoftLibrary::launchUri(QStringLiteral("3539")));  // inline == canonical
        CHECK(ubiSrc && ubiSrc->label == QStringLiteral("Ubisoft Connect"));
        CHECK(ubiSrc && ubiSrc->ready);
        CHECK(ubiSrc && ubiSrc->exePath.isEmpty());          // a URI launch, never an exe
        // Steam's curated name outranks Ubisoft's ® spelling for the tile title; Ubisoft's own name rides its source.
        CHECK(aco && aco->title == QStringLiteral("Assassin's Creed Origins"));
        CHECK(ubiSrc && ubiSrc->sourceName == QStringLiteral("Assassin's Creed® Origins"));
        // Two ready copies: Play must ASK (the source picker), never guess.
        CHECK(aco && pcgame::pickAutoSource(aco->pcSources) == -1);
        // The picker/filter row a person reads for it.
        CHECK(browse::pcLauncherLabel(QStringLiteral("ubisoft")) == QStringLiteral("Ubisoft Connect"));

        // A Ubisoft-only game: one source, auto-picked.
        const MediaItem* annoItem = find(c, QStringLiteral("pcgame:anno 1800"));
        CHECK(annoItem && annoItem->pcSources.size() == 1 && pcgame::pickAutoSource(annoItem->pcSources) == 0);
        // Shown from cache (#62): not ready, badged, never auto-picked.
        const MediaItem* fc5 = find(c, QStringLiteral("pcgame:far cry 5"));
        CHECK(fc5 && fc5->pcSources.size() == 1 && !fc5->pcSources[0].ready && !fc5->pcSources[0].available);
        CHECK(fc5 && fc5->subtitle == QStringLiteral("Unavailable?"));

        // The launcher filter offers Ubisoft when it has games, and "what I have on Ubisoft" narrows to them.
        CHECK(browse::pcLaunchersPresent(steam, {}, {}, {}, {}, {}, ubi)
              == QStringList({ QStringLiteral("steam"), QStringLiteral("ubisoft") }));
        CHECK(!browse::pcLaunchersPresent(steam, {}, {}, {}).contains(QStringLiteral("ubisoft")));
        const MediaCatalog only = browse::pcGamesCatalog(steam, {}, {}, {}, {}, QString(), QStringLiteral("ubisoft"),
                                                         noPoster, {}, {}, ubi);
        CHECK(only.items.size() == 3);
        const MediaCatalog steamOnly = browse::pcGamesCatalog(steam, {}, {}, {}, {}, QString(),
                                                              QStringLiteral("steam"), noPoster, {}, {}, ubi);
        CHECK(steamOnly.items.size() == 1);
        // The pre-merge id a launch through the Ubisoft source banks its records under.
        CHECK(ubiSrc && pcgame::legacyLaunchId(*ubiSrc) == QStringLiteral("ubi:3539"));
    }

    // ---- Ubisoft Recents: the kind that relaunches it -------------------------------------------------------
    {
        using RL = RecentStore::Relaunch;
        CHECK(RecentStore::relaunchFor(QStringLiteral("ubisoftgame")) == RL::UbisoftGame);
        CHECK(browse::iconTypeForKind(QStringLiteral("ubisoftgame")) == QStringLiteral("game"));
        // Every store kind belongs with Games (the Continue shelf groups on this; a miss headed its own group
        // with the raw kind as its title).
        for (const char* k : { "game", "pcgame", "steamgame", "epicgame", "goggame", "battlenetgame", "ubisoftgame" })
            CHECK(browse::isGameRecentKind(QString::fromLatin1(k)));
        CHECK(!browse::isGameRecentKind(QStringLiteral("video")));
        CHECK(!browse::isGameRecentKind(QString()));
        // A Ubisoft Recent groups under the games catalogue's Recent, like every other store's.
        RecentItem r; r.path = QStringLiteral("uplay://launch/3539/0"); r.title = QStringLiteral("ACO");
        r.kind = QStringLiteral("ubisoftgame"); r.key = QStringLiteral("ubi:3539");
        const MediaCatalog cat = browse::recentsCatalog({ r }, QStringLiteral("game"));
        CHECK(cat.items.size() == 1 && cat.items[0].mime == QStringLiteral("ubisoftgame"));
        // A playlist entry for it relaunches through the same mime (a URI launch: nothing rides in the path).
        Playlist p; p.name = QStringLiteral("pl");
        PlaylistEntry e; e.itemId = QStringLiteral("ubi:3539"); e.title = QStringLiteral("ACO"); p.items << e;
        const MediaCatalog pl = browse::playlistItemsCatalog(p);
        CHECK(pl.items.size() == 1 && pl.items[0].mime == QStringLiteral("ubisoftgame") && pl.items[0].url.isEmpty());
    }

    if (failures == 0) { std::puts("IMPORTERS-OK"); return 0; }
    std::fprintf(stderr, "IMPORTERS: %d check(s) failed\n", failures);
    return 1;
}
