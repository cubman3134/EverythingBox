// Hermetic integration probe for the CatalogResolver SHOW-dispatch seam (where the TV-resolution C1 bug hid:
// an untyped search that a pure probe can't catch). Stands up a REAL AddonManager pointed at a temp dir with a
// JsLocal series fixture (zero network), drives a CatalogResolver show job through the typed requestCatalog
// dispatch, and asserts the cache recorded the series tile id. Prints SHOWDISPATCH-OK on success.
#include "AddonManager.h"
#include "CatalogResolver.h"
#include "LocalResolveCache.h"
#include "LocalLibrary.h"
#include "LocalMetaMerge.h"
#include "MetaCache.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QEventLoop>
#include <functional>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if(!(c)){ std::fprintf(stderr,"SHOWDISPATCH-FAIL %s (line %d)\n", #c, __LINE__); ++failures; } } while(0)

static bool spinUntil(const std::function<bool()>& pred, int timeoutMs)
{
    QElapsedTimer t; t.start();
    while (!pred() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}
static bool writeText(const QString& p, const QByteArray& b)
{ QFile f(p); if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate)) return false; return f.write(b)==b.size(); }

// A JsLocal media-source whose "shows" (series) catalog returns a canned series row matching any query — the
// exact shape aiocatalog's TMDB /search/tv yields. `serieslike`: if false, returns MOVIE-typed rows instead
// (the negative control — a source that can't serve series).
static bool makeSeriesFixture(const QString& root, const QString& id, bool serieslike)
{
    const QString dir = root + "/" + id;
    if (!QDir().mkpath(dir)) return false;
    const QByteArray manifest =
        "{\n  \"id\": \"" + id.toUtf8() + "\",\n  \"name\": \"Series Fixture\",\n  \"version\": \"1.0.0\",\n"
        "  \"type\": \"media-source\",\n  \"entry\": \"main.js\",\n  \"permissions\": [],\n"
        "  \"catalogs\": [ { \"id\": \"shows\", \"name\": \"Shows\", \"type\": \"series\" } ]\n}\n";
    const char* typeTok = serieslike ? "'series'" : "'movie'";
    const char* idTok   = serieslike ? "'tmdb:tv:1396'" : "'tmdb:movie:1'";
    const QByteArray js = QByteArray(
        "function J(s){try{return JSON.parse(s);}catch(e){return null;}}\n"
        "function getCatalog(argJson){var a=J(argJson)||{};\n"
        "  var t=") + typeTok + ";\n"
        "  var items=[{id:" + idTok + ",\n"
        "    title:'Breaking Bad', type:t, subtitle:'2008', thumbnailUrl:'', url:''}];\n"
        "  return JSON.stringify({title:'r', items:items, hasMore:false});\n"
        "}\n";
    return writeText(dir + "/manifest.json", manifest) && writeText(dir + "/main.js", js);
}

static LocalLibrary::VideoEntry ep(const QString& show, int s, int e, const QString& path)
{ LocalLibrary::VideoEntry v; v.kind = LocalLibrary::Kind::Episode; v.show = show; v.season = s; v.episode = e; v.path = path; return v; }

// A JsLocal media-source with a MOVIES catalog that returns a canned Inception row for any query, AND a
// getMeta that answers a scraped card (overview + poster). This is the #73 meta-fetch path's fixture: the
// resolver matches the movie, then fetches THIS source's getMeta and persists it to MetaCache.
static bool makeMovieFixture(const QString& root, const QString& id)
{
    const QString dir = root + "/" + id;
    if (!QDir().mkpath(dir)) return false;
    const QByteArray manifest =
        "{\n  \"id\": \"" + id.toUtf8() + "\",\n  \"name\": \"Movie Fixture\",\n  \"version\": \"1.0.0\",\n"
        "  \"type\": \"media-source\",\n  \"entry\": \"main.js\",\n  \"permissions\": [],\n"
        "  \"catalogs\": [ { \"id\": \"movies\", \"name\": \"Movies\", \"type\": \"movie\" } ]\n}\n";
    const QByteArray js =
        "function J(s){try{return JSON.parse(s);}catch(e){return null;}}\n"
        "function getCatalog(argJson){\n"
        "  var items=[{id:'tmdb:movie:27205', title:'Inception', type:'movie', subtitle:'2010', thumbnailUrl:'', url:''}];\n"
        "  return JSON.stringify({title:'r', items:items, hasMore:false});\n"
        "}\n"
        "function getMeta(argJson){\n"
        "  return JSON.stringify({title:'Inception', overview:'SCRAPED_OVERVIEW', image:'http://poster/inception.jpg'});\n"
        "}\n";
    return writeText(dir + "/manifest.json", manifest) && writeText(dir + "/main.js", js);
}

static LocalLibrary::VideoEntry mov(const QString& title, int year, const QString& path,
                                    const QString& imdb = QString(), const QString& plot = QString())
{
    LocalLibrary::VideoEntry v; v.kind = LocalLibrary::Kind::Movie;
    v.title = title; v.year = year; v.path = path; v.imdbId = imdb; v.plot = plot; return v;
}

// Drive one movie entry all the way through resolve → getMeta → MetaCache, and return the persisted card.
static MediaDetail runMovieCase(const LocalLibrary::VideoEntry& entry)
{
    QTemporaryDir root; QTemporaryDir data;
    makeMovieFixture(root.path(), "fixture.movies");
    qputenv("EB_ADDONS_ROOT", root.path().toUtf8());
    AddonManager mgr;
    LocalResolveCache cache(data.path() + "/localresolve.json"); cache.load();
    CatalogResolver resolver(&mgr, &cache);
    resolver.enqueue({ entry });
    const QString key = LocalLibrary::tileId(entry);
    // The resolve writes the cache; the meta-fetch phase then persists "detail" into MetaCache asynchronously.
    // Wait for the persisted detail (or bail after the resolve settles with nothing, so a regression fails).
    spinUntil([&]{ return MetaCache::load(key).contains(QStringLiteral("detail")); }, 12000);
    return MetaCache::cachedDetailScraped(key);
}

// "Zero network" holds because AddonManager's constructor skips every startup network kick (default-source
// seeding, remote-manifest refresh, addon self-update) whenever EB_ADDONS_ROOT is set, and runCase() sets
// that override before constructing the manager. The gate is still needed for the network kicks; what it no
// longer has to stand in for is a remote source configured in the ini, because the ini is this process's own
// scratch file and starts empty (issue #42). Both halves used to matter: a cached Cinemeta manifest answering
// the show job before the fixture did would give the positive case an IMDB id instead of the canned
// tmdb:tv:1396 and let the movie-only negative control match anyway.
//
// resolveOnline is likewise no longer pinned here. It defaults to true and nothing has written the key, so
// the enqueue path below now runs on the real default rather than on one this probe set for itself.
static bool runCase(bool serieslike, QStringList& outIds)
{
    QTemporaryDir root; QTemporaryDir data;
    makeSeriesFixture(root.path(), "fixture.series", serieslike);
    qputenv("EB_ADDONS_ROOT", root.path().toUtf8());
    AddonManager mgr;                                   // real manager, loads the JsLocal fixture, no network
    LocalResolveCache cache(data.path() + "/localresolve.json"); cache.load();
    CatalogResolver resolver(&mgr, &cache);
    resolver.enqueue({ ep("Breaking Bad", 1, 1, data.path() + "/BB.S01E01.mkv") });
    const QString sk = LocalLibrary::showKeyFor(ep("Breaking Bad", 1, 1, QString()));
    // In-memory cache is updated in finishJob (before the resolved() debounce); wait for either a matched
    // entry OR the nomatch path (isShowFresh true) so the negative case terminates too.
    spinUntil([&]{ return !cache.seriesIdsByShow().value(sk).isEmpty()
                        || cache.isShowFresh(sk, 4102444800LL /*far future*/); }, 8000);
    outIds = cache.seriesIdsByShow().value(sk);
    return true;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QStringList pos; runCase(/*serieslike=*/true, pos);
    CHECK(pos.contains(QStringLiteral("tmdb:tv:1396")));   // typed dispatch → series result → matched + cached

    QStringList neg; runCase(/*serieslike=*/false, neg);
    CHECK(neg.isEmpty());                                  // movie-only source → no series match → not cached

    // #73: a resolved NFO-LESS movie fetches the owning addon's getMeta and persists it to MetaCache under the
    // local tile key, so the bare-filename tile gains a plot + poster. This exercises the WHOLE pipeline —
    // real AddonManager + JS getMeta + the resolver's meta-fetch phase — not just the pure merge.
    {
        const MediaDetail card = runMovieCase(mov(QStringLiteral("Inception"), 2010, QStringLiteral("/lib/Inception.mkv")));
        CHECK(card.overview == QStringLiteral("SCRAPED_OVERVIEW"));            // scraped plot landed in MetaCache
        CHECK(card.imageUrl == QStringLiteral("http://poster/inception.jpg")); // scraped poster landed too
    }
    // #73: the same pipeline, but the file carries a .nfo plot — which is AUTHORITATIVE and must survive the
    // scrape (scraped fields fill blanks only). Keyed by the .nfo imdb id (tileId's other branch).
    {
        const MediaDetail card = runMovieCase(mov(QStringLiteral("Inception"), 2010,
            QStringLiteral("/lib/Inception2.mkv"), QStringLiteral("tt1375666"), QStringLiteral("MY NFO PLOT")));
        CHECK(card.overview == QStringLiteral("MY NFO PLOT"));                // .nfo plot beat the scraped overview
    }

    if (failures == 0) { std::puts("SHOWDISPATCH-OK"); return 0; }
    std::fprintf(stderr, "SHOWDISPATCH: %d check(s) failed\n", failures);
    return 1;
}
