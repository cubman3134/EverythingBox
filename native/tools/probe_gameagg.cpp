// Headless verification of the QUEUED game-metadata aggregator (GameMetaAggregator): entering a console
// prefetches + caches ALL its games in the background (throttled), a hover scrapes at priority, every job's
// result is cached (a scroll-past never drops a scrape), and an already-cached game is never re-scraped.
// Uses a canned KEYLESS provider written into the exe's addons dir, so it needs no API keys; everything lands
// in the build tree and is cleaned up. Prints GAMEAGG-OK; GAMEAGG-FAIL <what> + non-zero on failure.
#include "AddonManager.h"
#include "GameMetaAggregator.h"
#include "MetaCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

static int failures = 0;
#define CHECK(c, w) do { if (!(c)) { std::fprintf(stderr, "GAMEAGG-FAIL %s (line %d)\n", w, __LINE__); ++failures; } } while (0)

static void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString addonDir = exeDir + QStringLiteral("/addons/testgamemeta");

    // A canned, keyless game meta provider. Returns http urls that just get stored in the art blob (the
    // downloads to x.invalid fail harmlessly; the urls are what we assert on).
    writeFile(addonDir + QStringLiteral("/manifest.json"),
        R"JSON({ "id":"com.mymediavault.testgamemeta","name":"Test Game Meta","version":"1.0.0",
                 "type":"media-source","entry":"main.js","permissions":[],"metaFor":["game"],"catalogs":[] })JSON");
    writeFile(addonDir + QStringLiteral("/main.js"),
        R"JS( function getMeta(argJson){ var a; try{a=JSON.parse(argJson);}catch(e){return "{}";}
              if(!a||a.type!=="game") return "{}";
              return JSON.stringify({ title:a.title||"Game", overview:"Canned meta for "+(a.title||""),
                image:"http://x.invalid/cover.jpg",
                images:{ logo:["http://x.invalid/logo.png"], box:["http://x.invalid/box.jpg"] },
                facts:[{label:"Developer",value:"TestSoft"},{label:"Genre",value:"Action"}],
                meta:{developer:"TestSoft"} }); } )JS");

    QDir(exeDir + QStringLiteral("/metadata")).removeRecursively(); // fresh cache

    AddonManager mgr;
    CHECK(!mgr.metaProvidersFor(QStringLiteral("game")).isEmpty(), "canned game provider discovered");

    GameMetaAggregator agg(&mgr);
    CHECK(agg.hasProviders(), "aggregator sees a provider");

    QVector<MediaItem> games;
    for (int i = 1; i <= 3; ++i)
    {
        MediaItem g; g.id = QStringLiteral("game:%1").arg(i); g.title = QStringLiteral("Game %1").arg(i); g.type = QStringLiteral("game");
        games << g;
    }

    auto logoOf = [](const MediaItem& g) { return MetaCache::loadArt(MetaCache::keyFor(g)).image(QStringLiteral("logo")); };

    // Entering a console: prefetch all three -> each scraped + cached in the background (throttled).
    agg.prefetch(games, QStringLiteral("SNES"));
    QElapsedTimer t; t.start();
    auto allCached = [&] { for (const MediaItem& g : games) if (logoOf(g).isEmpty()) return false; return true; };
    while (t.elapsed() < 10000 && !allCached()) QCoreApplication::processEvents(QEventLoop::AllEvents, 30);

    for (const MediaItem& g : games)
    {
        const QString key = MetaCache::keyFor(g);
        const MediaArt art = MetaCache::loadArt(key);
        CHECK(art.image(QStringLiteral("logo")) == QStringLiteral("http://x.invalid/logo.png"), "prefetch cached the logo art");
        CHECK(art.meta.value(QStringLiteral("developer")).toString() == QStringLiteral("TestSoft"), "prefetch cached the meta bag");
        CHECK(MetaCache::cachedDetail(key).facts.size() >= 2, "prefetch cached the facts");
    }

    // A hover on an already-cached game returns the cached card WITHOUT re-scraping (fires synchronously).
    bool cachedHit = false;
    agg.request(games[0], QStringLiteral("SNES"), [&](const MediaDetail& d) { cachedHit = d.valid || !d.art.isEmpty(); });
    CHECK(cachedHit, "hover on a cached game returns the cache immediately");

    // A fresh (uncached) game hover scrapes + caches on demand (priority).
    MediaItem fresh; fresh.id = QStringLiteral("game:fresh"); fresh.title = QStringLiteral("Fresh Game"); fresh.type = QStringLiteral("game");
    bool freshDone = false;
    agg.request(fresh, QStringLiteral("SNES"), [&](const MediaDetail& d) { freshDone = !d.art.isEmpty(); });
    t.restart();
    while (t.elapsed() < 8000 && !freshDone) QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(freshDone, "priority hover scrapes an uncached game");
    CHECK(!logoOf(fresh).isEmpty(), "priority hover cached the result");

    QDir(addonDir).removeRecursively();                 // clean the build tree
    QDir(exeDir + QStringLiteral("/metadata")).removeRecursively();

    if (failures) { std::fprintf(stderr, "GAMEAGG-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("GAMEAGG-OK\n");
    return 0;
}
