// Hermetic integration probe for the CatalogResolver SHOW-dispatch seam (where the TV-resolution C1 bug hid:
// an untyped search that a pure probe can't catch). Stands up a REAL AddonManager pointed at a temp dir with a
// JsLocal series fixture (zero network), drives a CatalogResolver show job through the typed requestCatalog
// dispatch, and asserts the cache recorded the series tile id. Prints SHOWDISPATCH-OK on success.
#include "AddonManager.h"
#include "CatalogResolver.h"
#include "LocalResolveCache.h"
#include "LocalLibrary.h"
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

static bool runCase(bool serieslike, QStringList& outIds)
{
    QTemporaryDir root; QTemporaryDir data;
    makeSeriesFixture(root.path(), "fixture.series", serieslike);
    qputenv("MMV_ADDONS_ROOT", root.path().toUtf8());
    Settings::setResolveOnline(true);                   // enqueue is gated on this; default is true, pin it anyway
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

    if (failures == 0) { std::puts("SHOWDISPATCH-OK"); return 0; }
    std::fprintf(stderr, "SHOWDISPATCH: %d check(s) failed\n", failures);
    return 1;
}
