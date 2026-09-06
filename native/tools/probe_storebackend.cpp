// Headless test for the STORE-BACKEND seam and the legendary (Epic) backend — issue #118, increment 1.
//
// WHAT THIS FEATURE IS: owning a game on a storefront with no store CLIENT installed. Our four PC importers
// read a launcher's local state and hand launches back to it, which on Linux means the Epic importer imports
// nothing, because there is no Epic Games Launcher there. legendary is a small third-party executable that
// talks to Epic directly; we shell out to it and never reimplement anything of Epic's.
//
// NO LOGIN IS EVER PERFORMED HERE and no legendary installation is required. Every listing assertion runs
// against a recorded-shape fixture body; every process assertion runs against THIS BINARY re-executed as its
// own child (see childMain below), which is the only way to exercise a real hang, a real non-zero exit and a
// real absent executable identically on Windows and on Linux — the platform whose behaviour the whole feature
// exists for, and the one the local MSVC gate cannot speak for.
//
// What it pins:
//   * TOOL DISCOVERY, pure — the candidate file names always END with the bare, extension-less name (the only
//     name the tool has off Windows), no candidate carries a backslash, directory order beats name order, and
//     the joined path uses '/'.
//   * THE LISTING PARSE — a normal library, an EMPTY library (Ok and empty, NOT a failure), a malformed body
//     (the ONLY Malformed case), a single unreadable record costing that record and not the library, DLC in
//     all three spellings legendary uses, a title-less entitlement falling back to its app name, dedupe, and a
//     TOTAL sort order.
//   * THE STATUS PARSE — signed in, legendary's "<not logged in>" placeholder, and a non-object body.
//   * THE FOUR FAILURE MAPPINGS — absent tool, timeout, non-zero exit and malformed output each produce their
//     own status and a distinct, non-empty reason naming the tool and the store.
//   * THE REAL PROCESS — a bounded run kills a hung child and reports timedOut; a failing child reports its
//     exit code; a child's stdout is captured; a nonexistent program is `started == false`, not a crash.
//   * THE CACHE — round-trip, TTL freshness (including the zero/future-timestamp rules), and the refresh
//     decision table.
//   * THE MERGE, which is the property the whole seam is shaped around: a backend-listed Epic game and an
//     already-imported copy of the same game are ONE entry with two sources, never two shelf rows. Plus the
//     Epic-side dedupe (an AppName that is installed AND owned is one Epic row) and the not-ready rule (an
//     entitlement can never be what a single Play keypress launches).
//   * THE CREDENTIAL — the authorization code reaches the child's argv and NOTHING ELSE: after a full
//     sign-in run the entire ini is byte-scanned for it, and so is the cached listing blob.
//
// Prints STOREBACKEND-OK on success; any failure prints STOREBACKEND-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the ini starts empty and
// is removed at exit.
#include "StoreBackend.h"
#include "LegendaryBackend.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "PcGameId.h"
#include "EpicLibrary.h"
#include "SteamLibrary.h"
#include "../browse/SyntheticCatalogs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QSettings>
#include <QStringList>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "STOREBACKEND-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- this binary, acting as the child process --------------------------------------------------------
//
// Handled BEFORE QCoreApplication is constructed so a child costs nothing and never creates a scratch dir.
// Three behaviours, which are exactly the three the production code has to survive: one that never answers,
// one that refuses, and one that answers with whatever bytes the test wants.
static int childMain(int argc, char** argv)
{
    const char* mode = argv[1];
    // "auth" is legendary's own first argument, not a flag of ours: it is what LegendaryBackend::authArgs
    // builds, and section 8 points a backend at THIS binary to drive a real sign-in. Exiting 0 here is the
    // "the code was accepted" child. Without this arm the child would re-run the whole probe — which spawns
    // children of its own — and the suite would fork until the machine gave up.
    if (std::strcmp(mode, "auth") == 0) return 0;
    if (std::strcmp(mode, "--child-sleep") == 0)
    {
        const int secs = argc > 2 ? std::atoi(argv[2]) : 10;
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        return 0;
    }
    if (std::strcmp(mode, "--child-fail") == 0)
        return 3;
    if (std::strcmp(mode, "--child-emit") == 0)
    {
        if (argc > 2) std::fwrite(argv[2], 1, std::strlen(argv[2]), stdout);
        std::fflush(stdout);
        return 0;
    }
    return 0;
}

// ---- recorded fixture bodies -------------------------------------------------------------------------
//
// The SHAPE of `legendary list --json` (a dump of its Game objects) and `legendary status --json`, trimmed to
// the keys this build reads plus enough neighbours that a parser keying on position rather than name would
// fail. Hand-authored from legendary's documented output rather than captured from a live account, because a
// captured body is an account's entitlement list and does not belong in a repository.
// A CUSTOM RAW-STRING DELIMITER, not the bare R"( ) form: one fixture title contains ")" and the default
// delimiter would end the literal there, which the compiler reports thirty lines later as an unrelated error.
static const char* kListBody = R"JSON([
  { "app_name": "Sugar", "app_title": "Rocket League",
    "asset_infos": { "Windows": { "build_version": "1.0" } },
    "metadata": { "title": "Rocket League", "categories": [ { "path": "games" } ] } },
  { "app_name": "Fortnite", "app_title": "Fortnite",
    "metadata": { "title": "Fortnite", "categories": [ { "path": "games" } ] } },
  { "app_name": "SugarDLC", "app_title": "Rocket League - Season Pass", "is_dlc": true },
  { "app_name": "FortniteExtra", "app_title": "Fortnite - Skin Pack",
    "metadata": { "categories": [ { "path": "addons" } ] } },
  { "app_name": "OtherDlc", "app_title": "Something Extra",
    "metadata": { "mainGameItem": { "id": "abc" } } },
  { "app_name": "Namely", "metadata": { "categories": [ { "path": "games" } ] } },
  { "app_name": "Sugar", "app_title": "Rocket League (duplicate entitlement)" },
  "not even an object",
  { "app_title": "no app name at all" }
])JSON";

static const char* kStatusSignedIn  = R"({"account":"player@example.invalid","games_available":42,
                                          "games_installed":0,"egl_sync_enabled":false})";
static const char* kStatusSignedOut = R"({"account":"<not logged in>","games_available":0,
                                          "games_installed":0})";

static const StoreGame* findGame(const QVector<StoreGame>& v, const QString& app)
{
    for (const StoreGame& g : v) if (g.appName == app) return &g;
    return nullptr;
}
static const MediaItem* findItem(const MediaCatalog& c, const QString& id)
{
    for (const MediaItem& i : c.items) if (i.id == id) return &i;
    return nullptr;
}

int main(int argc, char** argv)
{
    if (argc > 1 && (std::strncmp(argv[1], "--child-", 8) == 0
                     || std::strcmp(argv[1], "auth") == 0)) return childMain(argc, argv);
    QCoreApplication app(argc, argv);
    const QString self = QCoreApplication::applicationFilePath();

    // ---- 1. Tool discovery, pure and platform-honest ---------------------------------------------------
    {
        const QStringList names = storeback::toolFileNames(QStringLiteral("legendary"));
        CHECK(!names.isEmpty());
        // THE CROSS-PLATFORM INVARIANT. On Linux and macOS the tool is called exactly "legendary" and there
        // is no second spelling; a candidate list that lost the bare name would find nothing there while
        // still passing every Windows test. Asserted as a property rather than as a platform-specific list,
        // so this same assertion is meaningful on both CI legs.
        CHECK(names.last() == QStringLiteral("legendary"));
        for (const QString& n : names)
        {
            CHECK(n.startsWith(QStringLiteral("legendary")));
            CHECK(!n.contains(QLatin1Char('\\')));   // a backslash here is a Windows assumption in a name
            CHECK(!n.contains(QLatin1Char('/')));    // ...and a name is not a path
        }
        CHECK(storeback::toolFileNames(QString()).isEmpty());

        // DIRECTORY ORDER BEATS NAME ORDER: a copy in <app>/tools wins over one further down the search list,
        // which is what lets an appliance image ship a known-good binary. Driven with a fake predicate, so it
        // is a statement about the rule and not about this machine's disk.
        const QStringList dirs{ QStringLiteral("/opt/eb/tools"), QStringLiteral("/usr/bin") };
        auto onlyUsrBin = [](const QString& p) { return p.startsWith(QStringLiteral("/usr/bin/")); };
        auto everything = [](const QString&) { return true; };
        auto nothing    = [](const QString&) { return false; };
        CHECK(storeback::pickTool(QStringLiteral("legendary"), dirs, onlyUsrBin)
              == QStringLiteral("/usr/bin/") + storeback::toolFileNames(QStringLiteral("legendary")).first());
        CHECK(storeback::pickTool(QStringLiteral("legendary"), dirs, everything)
              .startsWith(QStringLiteral("/opt/eb/tools/")));
        CHECK(storeback::pickTool(QStringLiteral("legendary"), dirs, nothing).isEmpty());
        CHECK(storeback::pickTool(QString(), dirs, everything).isEmpty());
        // A trailing slash on a search dir must not produce a doubled separator, and the join is '/' on every
        // platform (a native separator here would be a path legendary is not at).
        CHECK(storeback::pickTool(QStringLiteral("legendary"), { QStringLiteral("/opt/eb/tools/") }, everything)
              == QStringLiteral("/opt/eb/tools/") + storeback::toolFileNames(QStringLiteral("legendary")).first());
        CHECK(!storeback::pickTool(QStringLiteral("legendary"), dirs, everything).contains(QLatin1Char('\\')));

        // The real search dirs always start with <app>/tools and are never empty.
        const QStringList real = storeback::toolSearchDirs();
        CHECK(!real.isEmpty());
        CHECK(real.first().endsWith(QStringLiteral("/tools")));
    }

    // ---- 2. The listing parse --------------------------------------------------------------------------
    {
        bool malformed = true;
        const QVector<StoreGame> games = LegendaryBackend::parseList(QByteArray(kListBody), &malformed);
        CHECK(!malformed);
        // Sugar (deduped), Fortnite, Namely. The three DLC spellings, the non-object element and the
        // app_name-less record are all gone; nothing else is.
        CHECK(games.size() == 3);
        CHECK(findGame(games, QStringLiteral("Sugar")) != nullptr);
        CHECK(findGame(games, QStringLiteral("Fortnite")) != nullptr);
        CHECK(findGame(games, QStringLiteral("Namely")) != nullptr);
        CHECK(findGame(games, QStringLiteral("SugarDLC")) == nullptr);        // is_dlc
        CHECK(findGame(games, QStringLiteral("FortniteExtra")) == nullptr);   // categories[].path == addons
        CHECK(findGame(games, QStringLiteral("OtherDlc")) == nullptr);        // metadata.mainGameItem
        // A title-less entitlement is still owned: it falls back to its app name rather than vanishing.
        const StoreGame* namely = findGame(games, QStringLiteral("Namely"));
        CHECK(namely && namely->title == QStringLiteral("Namely"));
        // Sorted by title, case-insensitively, so the folder cannot reshuffle between two refreshes.
        CHECK(games.size() == 3 && games[0].title == QStringLiteral("Fortnite"));
        CHECK(games.size() == 3 && games[1].title == QStringLiteral("Namely"));
        CHECK(games.size() == 3 && games[2].title == QStringLiteral("Rocket League (duplicate entitlement)"));

        // AN EMPTY LIBRARY IS A WELL-FORMED ANSWER. An account that owns nothing must not read as a broken
        // backend — that is the difference between "you own nothing on Epic" and "something is wrong here",
        // and only one of them is actionable.
        malformed = true;
        CHECK(LegendaryBackend::parseList(QByteArray("[]"), &malformed).isEmpty());
        CHECK(!malformed);

        // NOT AN ARRAY is the only malformed case: a traceback, a prompt, an empty body.
        for (const char* body : { "Traceback (most recent call last):", "{\"account\":\"x\"}", "", "null" })
        {
            malformed = false;
            CHECK(LegendaryBackend::parseList(QByteArray(body), &malformed).isEmpty());
            CHECK(malformed);
        }
        // The out-parameter is optional.
        CHECK(LegendaryBackend::parseList(QByteArray(kListBody)).size() == 3);
    }

    // ---- 3. The status parse ---------------------------------------------------------------------------
    {
        const StoreAuth in = LegendaryBackend::parseStatus(QByteArray(kStatusSignedIn));
        CHECK(in.status == StoreStatus::Ok);
        CHECK(in.signedIn);
        // legendary FILLS the key with a human placeholder instead of omitting it. Read as a name, that is a
        // signed-in account called "<not logged in>" and the folder would go on to run a listing that fails.
        const StoreAuth out = LegendaryBackend::parseStatus(QByteArray(kStatusSignedOut));
        CHECK(out.status == StoreStatus::Ok);
        CHECK(!out.signedIn);
        const StoreAuth missing = LegendaryBackend::parseStatus(QByteArray("{}"));
        CHECK(missing.status == StoreStatus::Ok && !missing.signedIn);
        const StoreAuth bad = LegendaryBackend::parseStatus(QByteArray("not json at all"));
        CHECK(bad.status == StoreStatus::Malformed && !bad.signedIn);
    }

    // ---- 4. The four failure mappings, and the reason each one gives -----------------------------------
    {
        storeback::ToolRun r;                                   // never started
        CHECK(LegendaryBackend::listingFromRun(r).status == StoreStatus::ToolMissing);
        r.started = true; r.timedOut = true;
        CHECK(LegendaryBackend::listingFromRun(r).status == StoreStatus::Timeout);
        r.timedOut = false; r.exitCode = 1;
        CHECK(LegendaryBackend::listingFromRun(r).status == StoreStatus::Failed);
        r.exitCode = 0; r.out = QByteArray("Traceback");
        CHECK(LegendaryBackend::listingFromRun(r).status == StoreStatus::Malformed);
        r.out = QByteArray("[]");
        const StoreListing okEmpty = LegendaryBackend::listingFromRun(r);
        CHECK(okEmpty.status == StoreStatus::Ok && okEmpty.games.isEmpty());
        r.out = QByteArray(kListBody);
        const StoreListing okFull = LegendaryBackend::listingFromRun(r);
        CHECK(okFull.status == StoreStatus::Ok && okFull.games.size() == 3);

        // Each failure says something different, and each names the tool and the store — a row that reads
        // "something went wrong" is a row that teaches the user to ignore it.
        const QString tool = QStringLiteral("legendary"), st = QStringLiteral("Epic Games");
        QStringList reasons;
        for (StoreStatus s : { StoreStatus::ToolMissing, StoreStatus::NotSignedIn, StoreStatus::Timeout,
                               StoreStatus::Failed, StoreStatus::Malformed })
        {
            const QString msg = storeback::reasonFor(s, tool, st);
            CHECK(!msg.isEmpty());
            CHECK(msg.contains(tool));
            CHECK(msg.contains(st));
            CHECK(!reasons.contains(msg));   // five distinct sentences, not one with five names
            reasons << msg;
        }
        CHECK(storeback::reasonFor(StoreStatus::Ok, tool, st).isEmpty());  // success has nothing to say
    }

    // ---- 5. The REAL bounded process -------------------------------------------------------------------
    // This is the half a pure function cannot state: that the bound is actually applied, that a hung child is
    // actually killed, and that an absent executable is an answer rather than a crash. The child is this
    // binary, so it behaves identically on both CI legs and needs nothing installed.
    {
        // A NONEXISTENT PROGRAM. Not an error path: started == false is how "no such tool" arrives.
        const storeback::ToolRun missing =
            storeback::run(QStringLiteral("/nonexistent/eb-not-a-real-tool-118"), {}, 3000);
        CHECK(!missing.started);
        CHECK(LegendaryBackend::listingFromRun(missing).status == StoreStatus::ToolMissing);
        CHECK(!storeback::run(QString(), {}, 3000).started);   // and an empty path spawns nothing at all

        // A HANG. The child sleeps far past the bound; run() must come back at the bound with the child dead.
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        const storeback::ToolRun hung = storeback::run(self, { QStringLiteral("--child-sleep"),
                                                               QStringLiteral("30") }, 900);
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - t0;
        CHECK(hung.started);
        CHECK(hung.timedOut);
        CHECK(elapsed < 20000);   // generous, but a build where the bound is ignored takes 30s and fails here
        CHECK(LegendaryBackend::listingFromRun(hung).status == StoreStatus::Timeout);

        // A REFUSAL.
        const storeback::ToolRun failed = storeback::run(self, { QStringLiteral("--child-fail") }, 15000);
        CHECK(failed.started && !failed.timedOut);
        CHECK(failed.exitCode == 3);
        CHECK(LegendaryBackend::listingFromRun(failed).status == StoreStatus::Failed);

        // STDOUT IS CAPTURED, and a listing parsed from a real child's bytes is the same listing.
        const storeback::ToolRun emitted = storeback::run(self, { QStringLiteral("--child-emit"),
                                                                  QStringLiteral("[]") }, 15000);
        CHECK(emitted.started && emitted.exitCode == 0);
        CHECK(emitted.out.trimmed() == QByteArray("[]"));
        CHECK(LegendaryBackend::listingFromRun(emitted).status == StoreStatus::Ok);

        // A backend pointed at a tool that is not there answers with the same status through the whole
        // object, not just through the pure helper.
        const LegendaryBackend absent(QStringLiteral("/nonexistent/eb-not-a-real-tool-118"));
        CHECK(absent.ownedGames().status == StoreStatus::ToolMissing);
        CHECK(absent.authState().status == StoreStatus::ToolMissing);
        CHECK(absent.signIn(QStringLiteral("whatever")) == StoreStatus::ToolMissing);

        // Identity: the backend feeds the "epic" launcher, which is what makes its games merge with the Epic
        // importer's rather than landing in a group of their own.
        const LegendaryBackend b;
        CHECK(b.id() == QStringLiteral("legendary"));
        CHECK(b.launcherId() == QStringLiteral("epic"));
        CHECK(b.toolName() == QStringLiteral("legendary"));
        CHECK(!b.releasesUrl().isEmpty() && b.releasesUrl().startsWith(QStringLiteral("https://")));
        CHECK(!b.signInUrl().isEmpty() && b.signInUrl().startsWith(QStringLiteral("https://")));
        CHECK(storeback::legendary()->id() == QStringLiteral("legendary"));
        CHECK(storeback::all().size() == 1);
    }

    // ---- 6. The cache ----------------------------------------------------------------------------------
    {
        QVector<StoreGame> games;
        games.push_back({ QStringLiteral("Sugar"), QStringLiteral("Rocket League"), false });
        games.push_back({ QStringLiteral("Fortnite"), QStringLiteral("Fortnite"), true });
        const QVector<StoreGame> back = storeback::decodeGames(storeback::encodeGames(games));
        CHECK(back.size() == 2);
        CHECK(back.size() == 2 && back[0].appName == QStringLiteral("Sugar"));
        CHECK(back.size() == 2 && back[0].title == QStringLiteral("Rocket League"));
        CHECK(back.size() == 2 && back[1].installed);
        CHECK(storeback::decodeGames(QByteArray("not json")).isEmpty());

        // TTL. A zero timestamp is never fresh (nothing was ever fetched) and neither is a FUTURE one — a
        // clock that jumped forward would otherwise pin a stale list in place until it jumped back.
        CHECK(storeback::cacheFresh(1000, 1500, 600));
        CHECK(!storeback::cacheFresh(1000, 2000, 600));
        CHECK(!storeback::cacheFresh(0, 2000, 600));
        CHECK(!storeback::cacheFresh(5000, 2000, 600));
        CHECK(!storeback::cacheFresh(1000, 1500, 0));

        // The refresh decision table.
        const qint64 now = 10000;
        CHECK(storeback::refreshDecision(false, now - 10, now, 600) == storeback::Refresh::NotAvailable);
        CHECK(storeback::refreshDecision(true,  now - 10, now, 600) == storeback::Refresh::CacheHit);
        CHECK(storeback::refreshDecision(true,  now - 900, now, 600) == storeback::Refresh::Fetch);
        CHECK(storeback::refreshDecision(true,  0, now, 600) == storeback::Refresh::Fetch);

        // Round-trip through the ini, including the freshness gate on the read.
        const QString bid = QStringLiteral("legendary");
        CHECK(storeback::cachedOwned(bid, storeback::kOwnedTtlSecs).isEmpty());   // nothing stored yet
        storeback::storeOwned(bid, games);
        CHECK(storeback::cachedOwned(bid, storeback::kOwnedTtlSecs).size() == 2);
        CHECK(storeback::cachedOwned(bid, 0).isEmpty());                          // a zero TTL is never fresh
        CHECK(storeback::cachedOwnedAt(bid) > 0);
        CHECK(!storeback::cachedSignedIn(bid));
        storeback::storeSignedIn(bid, true);
        CHECK(storeback::cachedSignedIn(bid));

        // The cache is DEVICE-LOCAL by key prefix; probe_cloudmerge owns the carve-out assertion itself, but
        // the prefix it keys on is asserted here so the two cannot drift apart silently.
        QSettings raw(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                      QSettings::IniFormat);
        bool sawPrefixed = false;
        for (const QString& k : raw.allKeys())
            if (k.startsWith(QStringLiteral("storebackend/"))) sawPrefixed = true;
        CHECK(sawPrefixed);
    }

    // ---- 7. THE MERGE: a backend-listed game and an imported one are ONE entry --------------------------
    // The reason the seam hands the folder EpicGame-shaped entitlements instead of inventing a shelf of its
    // own. A user who owns Hades on Epic and has it installed on Steam has ONE game, and the whole
    // PcGameId/PcGameRemap layer exists so their star, their marks and their hours attach to that one entry.
    {
        auto poster = [](const QVector<pcgame::PcGameSource>& v) {
            return v.isEmpty() ? QString() : QStringLiteral("cap://") + v.first().launchId;
        };
        // Installed on Steam; owned on Epic per the backend. Two stores, one game.
        QList<SteamGame> steam{ { QStringLiteral("100"), QStringLiteral("Hades") } };
        QList<EpicGame>  epicOwned{
            { QStringLiteral("Sugar"),   QStringLiteral("Hades"),        QString(), true },
            { QStringLiteral("Fortnite"), QStringLiteral("Fortnite"),    QString(), true },
        };
        const MediaCatalog cat = browse::pcGamesCatalog(steam, {}, {}, {}, {}, QString(), QString(),
                                                        poster, {}, epicOwned);
        CHECK(cat.items.size() == 2);   // Hades (merged) + Fortnite — NOT three, which is the duplicate shelf

        const MediaItem* hades = findItem(cat, pcgame::itemId(QStringLiteral("Hades")));
        CHECK(hades != nullptr);
        CHECK(hades && hades->pcSources.size() == 2);              // one Steam copy, one Epic entitlement
        if (hades && hades->pcSources.size() == 2)
        {
            int steamSrc = -1, epicSrc = -1;
            for (int i = 0; i < hades->pcSources.size(); ++i)
            {
                if (hades->pcSources[i].launcher == QStringLiteral("steam")) steamSrc = i;
                if (hades->pcSources[i].launcher == QStringLiteral("epic"))  epicSrc = i;
            }
            CHECK(steamSrc >= 0 && epicSrc >= 0);
            if (epicSrc >= 0)
            {
                const pcgame::PcGameSource& e = hades->pcSources[epicSrc];
                CHECK(e.kind == pcgame::PcGameSource::LauncherOwned);
                // NEVER READY, and with nothing to activate: installing through the backend is a later
                // increment, and a single Play keypress must not be able to start a download.
                CHECK(!e.ready);
                CHECK(e.launchUrl.isEmpty());
                CHECK(e.launchId == QStringLiteral("Sugar"));
                // The pre-merge id a launch would bank records under is the one the remap migrates FROM, and
                // it is spelt exactly like an installed Epic entry's.
                CHECK(pcgame::legacyLaunchId(e) == QStringLiteral("epic:Sugar"));
            }
            // pickAutoSource must pick the Steam copy: it is the only ready one.
            CHECK(pcgame::pickAutoSource(hades->pcSources) == steamSrc);
        }

        // AN INSTALLED EPIC COPY AND ITS ENTITLEMENT ARE ONE EPIC ROW. They would group anyway — same title —
        // but as two "Epic Games" rows in the picker, which is the duplicate the merged folder abolishes.
        QList<EpicGame> epicInstalled{ { QStringLiteral("Sugar"), QStringLiteral("Hades"),
                                         QStringLiteral("/games/hades"), true } };
        const MediaCatalog both = browse::pcGamesCatalog({}, epicInstalled, {}, {}, {}, QString(), QString(),
                                                         poster, {}, epicOwned);
        const MediaItem* one = findItem(both, pcgame::itemId(QStringLiteral("Hades")));
        CHECK(one != nullptr);
        CHECK(one && one->pcSources.size() == 1);
        CHECK(one && one->pcSources.size() == 1
              && one->pcSources[0].kind == pcgame::PcGameSource::LauncherInstalled);
        CHECK(one && one->pcSources.size() == 1 && one->pcSources[0].ready);

        // An entitlement whose AppName is NOT installed still contributes its own entry.
        CHECK(findItem(both, pcgame::itemId(QStringLiteral("Fortnite"))) != nullptr);

        // The launcher filter offers Epic on the strength of entitlements alone, which is the ONLY way it can
        // appear on Linux — there is no Epic Games Launcher there for the installed scan to find.
        const QStringList present = browse::pcLaunchersPresent({}, {}, {}, {}, {}, epicOwned);
        CHECK(present.contains(QStringLiteral("epic")));
        CHECK(present.contains(QLatin1String(browse::kPcFilterOwnedNotInstalled)));
        CHECK(browse::pcLaunchersPresent({}, {}, {}, {}, {}, {}).isEmpty());
        // Filtering ON epic keeps the merged game, because it HAS an Epic source.
        const MediaCatalog epicOnly = browse::pcGamesCatalog(steam, {}, {}, {}, {}, QString(),
                                                             QStringLiteral("epic"), poster, {}, epicOwned);
        CHECK(findItem(epicOnly, pcgame::itemId(QStringLiteral("Hades"))) != nullptr);

        // NO REGRESSION for a caller that passes no entitlements: the folder is exactly what it was.
        const MediaCatalog plain = browse::pcGamesCatalog(steam, {}, {}, {}, {}, QString(), QString(), poster);
        CHECK(plain.items.size() == 1);
    }

    // ---- 8. THE CREDENTIAL: the pasted code reaches the child's argv and nothing else -------------------
    {
        const QString code = QStringLiteral("EB118-FIXTURE-AUTHORIZATION-CODE-ZZZ");
        const QStringList args = LegendaryBackend::authArgs(code);
        CHECK(args.size() == 3);
        CHECK(args.size() == 3 && args[0] == QStringLiteral("auth"));
        CHECK(args.size() == 3 && args[1] == QStringLiteral("--code"));
        CHECK(args.size() == 3 && args[2] == code);   // the tool's only non-interactive interface

        // A full sign-in against a child that ACCEPTS it, followed by a listing refresh — i.e. every write
        // path this feature has — and then a byte-scan of the entire settings file. A credential that reached
        // an ini would ride the synced bundle to another machine on the day someone forgets the carve-out;
        // the durable answer is that it is never written at all.
        const LegendaryBackend fake(QCoreApplication::applicationFilePath());
        CHECK(fake.signIn(QStringLiteral("  ")) == StoreStatus::Failed);   // nothing to send: no process
        // (The real child ignores the auth args and exits 0, which is exactly the "accepted" path.)
        CHECK(fake.signIn(code) == StoreStatus::Ok);
        storeback::storeSignedIn(QStringLiteral("legendary"), true);
        QVector<StoreGame> listed = LegendaryBackend::parseList(QByteArray(kListBody));
        storeback::storeOwned(QStringLiteral("legendary"), listed);

        QSettings raw(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                      QSettings::IniFormat);
        raw.sync();
        QFile ini(raw.fileName());
        CHECK(ini.open(QIODevice::ReadOnly));
        const QByteArray bytes = ini.readAll();
        ini.close();
        CHECK(!bytes.isEmpty());                                     // scanning nothing proves nothing
        CHECK(!bytes.contains(code.toUtf8()));                       // THE ASSERTION
        CHECK(!bytes.contains(QByteArray("authorizationCode")));
        // ...and the cached blob carries entitlements only: no account, no token, no local path.
        const QByteArray blob = storeback::encodeGames(listed);
        CHECK(!blob.contains(QByteArray("account")));
        CHECK(!blob.contains(code.toUtf8()));
        CHECK(blob.contains(QByteArray("Sugar")));                   // ...but it did carry the games
        // No reason string can leak it either: they interpolate the tool and the store, never the child.
        for (StoreStatus s : { StoreStatus::ToolMissing, StoreStatus::NotSignedIn, StoreStatus::Timeout,
                               StoreStatus::Failed, StoreStatus::Malformed })
            CHECK(!storeback::reasonFor(s, QStringLiteral("legendary"), QStringLiteral("Epic Games"))
                   .contains(code));
    }

    if (failures == 0) { std::printf("STOREBACKEND-OK\n"); return 0; }
    std::fprintf(stderr, "STOREBACKEND had %d failure(s)\n", failures);
    return 1;
}
