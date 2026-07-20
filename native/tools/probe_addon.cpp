// Headless check of the addon JS engine: load an addon's main.js through Duktape, call getCatalog(),
// and parse the returned catalog - the exact path the Library uses.
#include "JsAddon.h"
#include "AddonContext.h"
#include "AddonModels.h"
#include "AddonManager.h"
#include "CatalogPrefetcher.h"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <functional>
#include <memory>
#include <cstdio>

// With "--manager": construct an AddonManager (scans <exe dir>/addons), list its sources and the first
// source's catalog with resolved URLs - the full discovery + resolution path the app uses.
static int probeManager(const QString& catalogId, int page)
{
    AddonManager mgr;
    printf("addons root: %s\n", mgr.addonsRoot().toUtf8().constData());
    printf("media sources: %d\n", int(mgr.sources().size()));
    if (mgr.sources().isEmpty()) { printf("no sources discovered\n"); return 1; }

    LoadedAddon* s = mgr.sources().first();
    printf("source: %s (%s)\n", s->manifest.name.toUtf8().constData(), s->manifest.id.toUtf8().constData());
    const MediaCatalog cat = catalogId.isEmpty() ? mgr.catalog(s) : mgr.catalog(s, catalogId, QString(), page);
    printf("catalog \"%s\" (page %d): %d item(s)\n", cat.title.toUtf8().constData(), page, int(cat.items.size()));
    int missing = 0;
    for (const MediaItem& it : cat.items)
    {
        // Report the resolved THUMBNAIL path (and whether it exists) - that's what the grid loads.
        const QString thumb = it.thumbnailUrl;
        const bool remote = thumb.contains(QStringLiteral("://"));
        const bool present = thumb.isEmpty() || remote || QFile::exists(thumb);
        if (!thumb.isEmpty() && !present) ++missing;
        printf("  - %-22s thumb: %s  [%s]\n", it.title.toUtf8().constData(),
               thumb.isEmpty() ? "(none)" : thumb.toUtf8().constData(),
               thumb.isEmpty() ? "-" : (remote ? "remote" : (present ? "FILE OK" : "MISSING")));
    }
    printf("%s\n", missing == 0 ? "ADDON MANAGER WORKS: thumbnails resolved" : "SOME THUMBNAILS MISSING");
    return missing == 0 ? 0 : 1;
}

// With "--metaflow <main.js>": exercise the getMeta() detail-header path end to end using the keyless
// MusicBrainz source - catalog -> album meta -> tracks -> track meta (no API keys required).
static int probeMetaFlow(const QString& jsPath)
{
    QFile f(jsPath);
    if (!f.open(QIODevice::ReadOnly)) { printf("can't read %s\n", jsPath.toUtf8().constData()); return 1; }
    AddonManifest m; m.id = QStringLiteral("probe"); m.permissions << QStringLiteral("network");
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/mymediavault-addon-probe"));
    QString err;
    auto addon = JsAddon::load(QString::fromUtf8(f.readAll()), std::move(ctx), &err);
    if (!addon) { printf("load failed: %s\n", err.toUtf8().constData()); return 1; }
    printf("has getMeta: %s\n", addon->hasFunction(QStringLiteral("getMeta")) ? "yes" : "no");

    auto meta = [&](const MediaItem& it) {
        const QByteArray arg = QByteArray("{\"id\":\"") + it.id.toUtf8() + "\",\"type\":\"" + it.type.toUtf8() + "\"}";
        const MediaDetail d = MediaDetail::fromJson(addon->invoke(QStringLiteral("getMeta"), QString::fromUtf8(arg)).toUtf8());
        printf("  getMeta(%s) -> valid=%s  title=\"%s\"  facts=%d  image=%s\n",
               it.type.toUtf8().constData(), d.valid ? "yes" : "no", d.title.toUtf8().constData(),
               int(d.facts.size()), d.imageUrl.isEmpty() ? "(none)" : "set");
        for (const MediaFact& fc : d.facts)
            printf("      %s: %s\n", fc.label.toUtf8().constData(), fc.value.toUtf8().constData());
        if (!d.overview.isEmpty())
            printf("      overview: %.80s...\n", d.overview.toUtf8().constData());
        return d.valid;
    };

    const MediaCatalog cat = MediaCatalog::fromJson(
        addon->invoke(QStringLiteral("getCatalog"), QStringLiteral("{\"catalog\":\"music\"}")).toUtf8());
    const MediaItem* album = nullptr;
    for (const MediaItem& it : cat.items) if (it.type == QStringLiteral("album")) { album = &it; break; }
    if (!album) { printf("no album in music catalog\n"); return 1; }
    printf("album: %s\n", album->title.toUtf8().constData());
    const bool albumOk = meta(*album);

    const QByteArray darg = QByteArray("{\"id\":\"") + album->id.toUtf8() + "\",\"type\":\"album\"}";
    const MediaCatalog tracks = MediaCatalog::fromJson(addon->invoke(QStringLiteral("getDetail"), QString::fromUtf8(darg)).toUtf8());
    const MediaItem* track = nullptr;
    for (const MediaItem& it : tracks.items) if (it.type == QStringLiteral("track")) { track = &it; break; }
    bool trackOk = false;
    if (track) { printf("track: %s\n", track->title.toUtf8().constData()); trackOk = meta(*track); }
    else printf("no track found to test\n");

    const bool ok = albumOk && trackOk;
    printf("%s\n", ok ? "META FLOW WORKS: album + track metadata returned" : "META FLOW: incomplete (check keys/network)");
    return ok ? 0 : 1;
}

// With "--metaone <main.js> <catalog>": load a catalog, take its first real item and call getMeta() on it
// (verifies any single catalog + its detail header; use a keyless catalog like "manga" for offline checks).
static int probeMetaOne(const QString& jsPath, const QString& catalogId)
{
    QFile f(jsPath);
    if (!f.open(QIODevice::ReadOnly)) { printf("can't read %s\n", jsPath.toUtf8().constData()); return 1; }
    AddonManifest m; m.id = QStringLiteral("probe"); m.permissions << QStringLiteral("network");
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/mymediavault-addon-probe"));
    QString err;
    auto addon = JsAddon::load(QString::fromUtf8(f.readAll()), std::move(ctx), &err);
    if (!addon) { printf("load failed: %s\n", err.toUtf8().constData()); return 1; }

    const QString carg = QStringLiteral("{\"catalog\":\"%1\"}").arg(catalogId);
    const MediaCatalog cat = MediaCatalog::fromJson(addon->invoke(QStringLiteral("getCatalog"), carg).toUtf8());
    printf("catalog \"%s\": %d item(s)\n", cat.title.toUtf8().constData(), int(cat.items.size()));
    const MediaItem* pick = nullptr;
    for (const MediaItem& it : cat.items) if (it.type != QStringLiteral("info") && it.type != QStringLiteral("_open")) { pick = &it; break; }
    if (!pick) { printf("no real item in catalog\n"); return 1; }
    printf("item: [%s] %s\n", pick->type.toUtf8().constData(), pick->title.toUtf8().constData());

    const QByteArray arg = QByteArray("{\"id\":\"") + pick->id.toUtf8() + "\",\"type\":\"" + pick->type.toUtf8() + "\"}";
    const MediaDetail d = MediaDetail::fromJson(addon->invoke(QStringLiteral("getMeta"), QString::fromUtf8(arg)).toUtf8());
    printf("getMeta -> valid=%s  title=\"%s\"  facts=%d  image=%s\n", d.valid ? "yes" : "no",
           d.title.toUtf8().constData(), int(d.facts.size()), d.imageUrl.isEmpty() ? "(none)" : "set");
    for (const MediaFact& fc : d.facts) printf("    %s: %s\n", fc.label.toUtf8().constData(), fc.value.toUtf8().constData());
    if (!d.overview.isEmpty()) printf("    overview: %.90s...\n", d.overview.toUtf8().constData());
    printf("%s\n", d.valid ? "META ONE WORKS" : "META ONE: no metadata");
    return d.valid ? 0 : 1;
}

// With "--mangaflow <main.js>": exercise the keyless MangaDex path end to end - catalog -> manga meta ->
// chapters (getDetail) -> chapter meta. Verifies the manga drill-down + both detail headers offline.
static int probeMangaFlow(const QString& jsPath)
{
    QFile f(jsPath);
    if (!f.open(QIODevice::ReadOnly)) { printf("can't read %s\n", jsPath.toUtf8().constData()); return 1; }
    AddonManifest m; m.id = QStringLiteral("probe"); m.permissions << QStringLiteral("network");
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/mymediavault-addon-probe"));
    QString err;
    auto addon = JsAddon::load(QString::fromUtf8(f.readAll()), std::move(ctx), &err);
    if (!addon) { printf("load failed: %s\n", err.toUtf8().constData()); return 1; }

    auto meta = [&](const MediaItem& it) {
        const QByteArray arg = QByteArray("{\"id\":\"") + it.id.toUtf8() + "\",\"type\":\"" + it.type.toUtf8() + "\"}";
        const MediaDetail d = MediaDetail::fromJson(addon->invoke(QStringLiteral("getMeta"), QString::fromUtf8(arg)).toUtf8());
        printf("  getMeta(%s) -> valid=%s  title=\"%s\"  facts=%d  image=%s\n", it.type.toUtf8().constData(),
               d.valid ? "yes" : "no", d.title.toUtf8().constData(), int(d.facts.size()), d.imageUrl.isEmpty() ? "(none)" : "set");
        for (const MediaFact& fc : d.facts) printf("      %s: %s\n", fc.label.toUtf8().constData(), fc.value.toUtf8().constData());
        return d.valid;
    };

    const MediaCatalog cat = MediaCatalog::fromJson(
        addon->invoke(QStringLiteral("getCatalog"), QStringLiteral("{\"catalog\":\"manga\"}")).toUtf8());
    const MediaItem* manga = nullptr;
    for (const MediaItem& it : cat.items) if (it.type == QStringLiteral("manga")) { manga = &it; break; }
    if (!manga) { printf("no manga in catalog\n"); return 1; }
    printf("manga: %s  (expandable=%s)\n", manga->title.toUtf8().constData(), manga->expandable ? "yes" : "no");
    const bool mangaOk = meta(*manga);

    const QByteArray darg = QByteArray("{\"id\":\"") + manga->id.toUtf8() + "\",\"type\":\"manga\",\"page\":1}";
    const MediaCatalog chs = MediaCatalog::fromJson(addon->invoke(QStringLiteral("getDetail"), QString::fromUtf8(darg)).toUtf8());
    printf("chapters: %d (hasMore=%s)\n", int(chs.items.size()), chs.hasMore ? "yes" : "no");
    for (int i = 0; i < chs.items.size() && i < 5; ++i)
        printf("    · [%s] %s — %s\n", chs.items[i].type.toUtf8().constData(),
               chs.items[i].title.toUtf8().constData(), chs.items[i].subtitle.toUtf8().constData());

    const MediaItem* chapter = nullptr;
    for (const MediaItem& it : chs.items) if (it.type == QStringLiteral("manga_chapter")) { chapter = &it; break; }
    bool chapterOk = false;
    if (chapter) { printf("chapter: %s\n", chapter->title.toUtf8().constData()); chapterOk = meta(*chapter); }
    else printf("no chapter found\n");

    const bool ok = mangaOk && !chs.items.isEmpty() && chs.items[0].type != QStringLiteral("info") && chapterOk;
    printf("%s\n", ok ? "MANGA FLOW WORKS: catalog + chapters + metadata" : "MANGA FLOW: incomplete");
    return ok ? 0 : 1;
}

// With "--remote <baseUrl> [catalogId]": exercise a REMOTE (HTTP) addon end to end via the AddonManager
// sync path - catalog -> first item's meta -> a container's children. baseUrl may be http(s):// or a
// file:// path to a static fixture (both serve the same {base}/catalog/{id}.json layout).
static int probeRemote(const QString& url, const QString& catalogId)
{
    AddonManager mgr; // only its sync methods are used; the stack addon need not be in its source list
    LoadedAddon a;
    a.transport = LoadedAddon::RemoteHttp;
    a.manifest.type = QStringLiteral("media-source");
    a.baseUrl = url;
    if (a.baseUrl.endsWith(QStringLiteral("/manifest.json"))) a.baseUrl.chop(int(strlen("/manifest.json")));
    while (a.baseUrl.endsWith(QLatin1Char('/'))) a.baseUrl.chop(1);
    printf("remote base: %s\n", a.baseUrl.toUtf8().constData());

    const MediaCatalog cat = mgr.catalog(&a, catalogId, QString(), 1);
    printf("catalog \"%s\": %d item(s) (hasMore=%s)\n", cat.title.toUtf8().constData(),
           int(cat.items.size()), cat.hasMore ? "yes" : "no");
    for (int i = 0; i < cat.items.size() && i < 6; ++i)
        printf("  - [%s%s] %s — %s\n", cat.items[i].type.toUtf8().constData(),
               cat.items[i].expandable ? ",container" : "", cat.items[i].title.toUtf8().constData(),
               cat.items[i].subtitle.toUtf8().constData());
    if (cat.items.isEmpty()) { printf("REMOTE: no items (is the URL reachable?)\n"); return 1; }

    const MediaItem* pick = nullptr;
    for (const MediaItem& it : cat.items)
        if (it.type != QStringLiteral("info") && it.type != QStringLiteral("_open")) { pick = &it; break; }
    bool metaOk = false;
    if (pick)
    {
        const MediaDetail d = mgr.meta(&a, *pick);
        metaOk = d.valid;
        printf("getMeta(%s) -> valid=%s  title=\"%s\"  facts=%d  image=%s\n", pick->type.toUtf8().constData(),
               d.valid ? "yes" : "no", d.title.toUtf8().constData(), int(d.facts.size()),
               d.imageUrl.isEmpty() ? "(none)" : "set");
        for (const MediaFact& fc : d.facts) printf("    %s: %s\n", fc.label.toUtf8().constData(), fc.value.toUtf8().constData());
        const QString stream = mgr.resolveStreamSync(&a, *pick);
        printf("resolveStream(%s) -> %s\n", pick->title.toUtf8().constData(),
               stream.isEmpty() ? "(no stream - would open the detail page)" : stream.toUtf8().constData());
    }

    const MediaItem* cont = nullptr;
    for (const MediaItem& it : cat.items) if (it.expandable) { cont = &it; break; }
    if (cont)
    {
        const MediaCatalog det = mgr.detail(&a, *cont, 1);
        printf("getDetail(%s): %d child item(s)\n", cont->title.toUtf8().constData(), int(det.items.size()));
        for (int i = 0; i < det.items.size() && i < 6; ++i)
            printf("    · [%s] %s — %s\n", det.items[i].type.toUtf8().constData(),
                   det.items[i].title.toUtf8().constData(), det.items[i].subtitle.toUtf8().constData());
    }

    const bool ok = !cat.items.isEmpty() && (!pick || metaOk);
    printf("%s\n", ok ? "REMOTE ADDON WORKS: catalog + meta over HTTP" : "REMOTE ADDON: check output");
    return ok ? 0 : 1;
}

// --- prefetch / cache-peek / enable-disable harness (Feature-Track Task 1) --------------------------
// Spins its own deterministic JsLocal fixtures in an isolated temp addons root (MMV_ADDONS_ROOT) and drives
// the catalog cache peek + the CatalogPrefetcher entirely offline, asserting the Task-2 contract. No network.

// Process the event loop for `ms`, delivering queued catalogReady/QtConcurrent completions.
static void spin(int ms)
{
    QElapsedTimer t; t.start();
    do { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); } while (t.elapsed() < ms);
}
// Spin until `pred` holds or `timeoutMs` elapses; returns the final pred() value.
static bool spinUntil(const std::function<bool()>& pred, int timeoutMs)
{
    QElapsedTimer t; t.start();
    while (!pred() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

static bool writeText(const QString& path, const QByteArray& text)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(text) == text.size();
}

// A minimal offline media-source: 2 catalogs, a getCatalog that serves a fixed 3-item page and embeds a
// per-catalog invocation counter in the title (so a test can tell a FRESH run from a cache-served copy).
static bool makeFixture(const QString& root, const QString& id)
{
    const QString dir = root + QStringLiteral("/") + id;
    if (!QDir().mkpath(dir)) return false;
    const QByteArray manifest =
        "{\n"
        "  \"id\": \"" + id.toUtf8() + "\",\n"
        "  \"name\": \"Prefetch Fixture\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"type\": \"media-source\",\n"
        "  \"entry\": \"main.js\",\n"
        "  \"permissions\": [],\n"
        "  \"catalogs\": [\n"
        "    { \"id\": \"movies\", \"name\": \"Movies\", \"type\": \"movie\" },\n"
        "    { \"id\": \"shows\",  \"name\": \"Shows\",  \"type\": \"series\" }\n"
        "  ]\n"
        "}\n";
    static const char* JS =
        "function J(s){try{return JSON.parse(s);}catch(e){return null;}}\n"
        "function getCatalog(argJson){\n"
        "  var a=J(argJson)||{};var cat=a.catalog||'mixed';\n"
        "  var n=parseInt(getStorage('gen_'+cat)||'0',10)+1;setStorage('gen_'+cat,String(n));\n"
        "  var items=[];for(var i=1;i<=3;i++)items.push({id:cat+':item'+i,title:cat+' Item '+i,\n"
        "    type:'movie',subtitle:'gen'+n,thumbnailUrl:'',url:''});\n"
        "  return JSON.stringify({title:cat+' #'+n,items:items,hasMore:false});\n"
        "}\n";
    return writeText(dir + QStringLiteral("/manifest.json"), manifest)
        && writeText(dir + QStringLiteral("/main.js"), QByteArray(JS));
}

// A "lost reply" fixture for the liveness watchdog: 3 catalogs whose getCatalog busy-waits ~4s (inside the
// 5s Duktape deadline) before answering — long past a 1s test watchdog, so from the prefetcher's view the
// reply is missing. 3 catalogs = every in-flight slot wedges at once, the exact stall the watchdog fixes.
static bool makeSlowFixture(const QString& root, const QString& id)
{
    const QString dir = root + QStringLiteral("/") + id;
    if (!QDir().mkpath(dir)) return false;
    const QByteArray manifest =
        "{\n"
        "  \"id\": \"" + id.toUtf8() + "\",\n"
        "  \"name\": \"Slow Fixture\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"type\": \"media-source\",\n"
        "  \"entry\": \"main.js\",\n"
        "  \"permissions\": [],\n"
        "  \"catalogs\": [\n"
        "    { \"id\": \"slow-a\", \"name\": \"Slow A\", \"type\": \"movie\" },\n"
        "    { \"id\": \"slow-b\", \"name\": \"Slow B\", \"type\": \"movie\" },\n"
        "    { \"id\": \"slow-c\", \"name\": \"Slow C\", \"type\": \"movie\" }\n"
        "  ]\n"
        "}\n";
    static const char* JS =
        "function getCatalog(argJson){\n"
        "  var end=Date.now()+4000; while(Date.now()<end){}\n"
        "  return JSON.stringify({title:'slow',items:[{id:'s1',title:'Slow 1',type:'movie',url:''}]});\n"
        "}\n";
    return writeText(dir + QStringLiteral("/manifest.json"), manifest)
        && writeText(dir + QStringLiteral("/main.js"), QByteArray(JS));
}

static int probePrefetch()
{
    int pass = 0, fail = 0;
    auto check = [&](const char* name, bool ok) {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name); if (ok) ++pass; else ++fail;
    };

    const QString root = QDir::tempPath() + QStringLiteral("/mmv-prefetch-fixture-")
                       + QString::number(QCoreApplication::applicationPid());
    QDir(root).removeRecursively();
    const QStringList ids = { QStringLiteral("probe.fixture.0"), QStringLiteral("probe.fixture.1"),
                              QStringLiteral("probe.fixture.2") };
    for (const QString& id : ids) if (!makeFixture(root, id)) { printf("fixture write failed\n"); return 2; }
    qputenv("MMV_ADDONS_ROOT", root.toUtf8());

    // ---- Manager A: comfortably-long TTL for the peek/disable/signal steps ----
    qputenv("MMV_PREFETCH_TTL_S", "30");
    AddonManager mgr;
    for (const QString& id : ids) mgr.setEnabled(id, true); // isEnabled persists in the shared ini across runs
    check("discovered the 3 fixtures", mgr.sources().size() == 3);
    LoadedAddon* s0 = mgr.sourceById(ids[0]);
    if (!s0) { printf("fixture 0 not loaded\n"); return 2; }

    // Capture every delivered catalog by reqId so we can inspect the async path's actual result.
    QHash<int, MediaCatalog> got;
    QObject::connect(&mgr, &AddonManager::catalogReady, &mgr,
                     [&](int rid, const MediaCatalog& c) { got.insert(rid, c); });

    // (1) peek miss before any fetch
    check("cachedCatalog miss -> nullopt", !mgr.cachedCatalog(s0, QStringLiteral("movies"), QString(), 1, {}).has_value());

    // (2) after a requestCatalog completes -> hit with the same 3 items
    mgr.requestCatalog(s0, QStringLiteral("movies"), QString(), 1, {});
    spinUntil([&] { return mgr.cachedCatalog(s0, QStringLiteral("movies"), QString(), 1, {}).has_value(); }, 5000);
    const auto hit = mgr.cachedCatalog(s0, QStringLiteral("movies"), QString(), 1, {});
    check("cachedCatalog hit after fetch", hit.has_value() && hit->items.size() == 3);
    check("cached items match the served page",
          hit && hit->items.size() == 3 && hit->items[0].id == QStringLiteral("movies:item1"));
    const QString firstTitle = hit ? hit->title : QString();

    // (3) disabled source -> nullopt even when cached, AND the async path fails fast: no cached serve AND no
    // fetch either (a fetch would re-populate the cache for a source the user just turned off).
    mgr.setEnabled(ids[0], false);
    check("disabled source peek -> nullopt", !mgr.cachedCatalog(s0, QStringLiteral("movies"), QString(), 1, {}).has_value());
    got.clear();
    const int r2 = mgr.requestCatalog(s0, QStringLiteral("movies"), QString(), 1, {});
    spin(300); // give a (wrong) queued cache re-emit or fetch the chance to deliver
    check("async path fail-fasts a disabled source (-1, nothing delivered)",
          r2 == -1 && got.isEmpty() && !firstTitle.isEmpty());
    mgr.setEnabled(ids[0], true); // restore

    // (4) setEnabled(false) emits sourceEnabledChanged(id, false)
    QString sigId; bool sigVal = true, sigFired = false;
    QObject::connect(&mgr, &AddonManager::sourceEnabledChanged, &mgr,
                     [&](const QString& id, bool en) { sigFired = true; sigId = id; sigVal = en; });
    mgr.setEnabled(ids[1], false);
    check("sourceEnabledChanged emitted with (id,false)", sigFired && sigId == ids[1] && sigVal == false);
    mgr.setEnabled(ids[1], true); // restore

    // ---- Manager P: fresh empty cache so the prefetcher has all 6 (3 sources x 2 catalogs) jobs to do ----
    AddonManager mgrP;
    for (const QString& id : ids) mgrP.setEnabled(id, true);
    CatalogPrefetcher pf(&mgrP, &mgrP);
    pf.setPeriodicResweep(false);            // deterministic: only explicit sweeps, no wall-clock timer
    pf.start();
    // Deterministic in-flight-cap proof: start() dispatches synchronously and catalogReady is queued, so no
    // job has completed yet — exactly kMaxInFlight are outstanding and the remainder are parked in the queue.
    const int totalJobs = 3 * 2;
    check("in-flight capped at kMaxInFlight right after start",
          pf.inFlight() == CatalogPrefetcher::kMaxInFlight);
    check("surplus jobs are queued, not dispatched",
          pf.queued() == totalJobs - CatalogPrefetcher::kMaxInFlight);
    int maxSeen = pf.inFlight();
    bool everOver = false;
    spinUntil([&] {
        maxSeen = qMax(maxSeen, pf.inFlight());
        if (pf.inFlight() > CatalogPrefetcher::kMaxInFlight) everOver = true;
        return pf.idle();
    }, 8000);
    check("never exceeded the cap while draining", !everOver && maxSeen <= CatalogPrefetcher::kMaxInFlight);
    check("cap was actually reached (throttle engaged)", maxSeen == CatalogPrefetcher::kMaxInFlight);
    check("issued exactly one request per job", pf.issued() == totalJobs);
    bool allCached = true;
    for (const QString& id : ids) {
        LoadedAddon* s = mgrP.sourceById(id);
        for (const QString& c : { QStringLiteral("movies"), QStringLiteral("shows") })
            if (!mgrP.cachedCatalog(s, c, QString(), 1, {}).has_value()) allCached = false;
    }
    check("every source x catalog landed in the cache", allCached);

    // (5) resweep skips still-fresh entries -> no new requests are issued
    const int before = pf.issued();
    pf.resweep();
    spin(200);
    check("resweep skips fresh entries (request count unchanged)", pf.issued() == before && pf.idle());

    // ---- Manager S: 1-second TTL to exercise expiry ----
    qputenv("MMV_PREFETCH_TTL_S", "1");
    AddonManager mgrS;
    mgrS.setEnabled(ids[0], true);
    LoadedAddon* ss = mgrS.sourceById(ids[0]);
    mgrS.requestCatalog(ss, QStringLiteral("movies"), QString(), 1, {});
    spinUntil([&] { return mgrS.cachedCatalog(ss, QStringLiteral("movies"), QString(), 1, {}).has_value(); }, 5000);
    check("peek hit within TTL", mgrS.cachedCatalog(ss, QStringLiteral("movies"), QString(), 1, {}).has_value());
    spin(1300); // exceed the 1s TTL
    check("peek miss after TTL expiry", !mgrS.cachedCatalog(ss, QStringLiteral("movies"), QString(), 1, {}).has_value());

    // ---- Manager W: liveness watchdog. All 3 slots wedge on never-answering (4s) jobs; a 1s watchdog must
    // reclaim them so the queued fast jobs still run — the pre-fix behavior was a queue stalled at cap. ----
    const QString rootW = root + QStringLiteral("-wd");
    QDir(rootW).removeRecursively();
    const QString slowId = QStringLiteral("a.slow"), fastId = QStringLiteral("b.fast");
    if (!makeSlowFixture(rootW, slowId) || !makeFixture(rootW, fastId)) { printf("wd fixture write failed\n"); return 2; }
    qputenv("MMV_ADDONS_ROOT", rootW.toUtf8());
    qputenv("MMV_PREFETCH_TTL_S", "30");      // roomy TTL: cache entries must outlive the asserts below
    qputenv("MMV_PREFETCH_WATCHDOG_S", "1");  // expire a silent in-flight job after ~1s
    AddonManager mgrW;
    mgrW.setEnabled(slowId, true); mgrW.setEnabled(fastId, true);
    LoadedAddon* fastSrc = mgrW.sourceById(fastId);
    CatalogPrefetcher pfW(&mgrW, &mgrW);
    pfW.setPeriodicResweep(false);
    pfW.start(); // FIFO: a.slow loads first -> its 3 slow jobs take all 3 slots; b.fast's 2 jobs are queued
    check("watchdog scenario: all slots wedged on silent jobs",
          pfW.inFlight() == CatalogPrefetcher::kMaxInFlight && pfW.queued() == 2);
    const bool drained = spinUntil([&] { return pfW.idle(); }, 6000);
    check("watchdog reclaimed the wedged slots (queue not stalled at cap)", drained && pfW.expired() == 3);
    check("queued jobs ran after reclamation",
          pfW.issued() == 5
          && mgrW.cachedCatalog(fastSrc, QStringLiteral("movies"), QString(), 1, {}).has_value()
          && mgrW.cachedCatalog(fastSrc, QStringLiteral("shows"),  QString(), 1, {}).has_value());
    // The slow replies DO arrive eventually (~4s): the manager may still cache them, but the prefetcher must
    // ignore the late reqIds — no slot bookkeeping left to corrupt, no double-expiry, no phantom in-flight.
    LoadedAddon* slowSrc = mgrW.sourceById(slowId);
    spinUntil([&] { return mgrW.cachedCatalog(slowSrc, QStringLiteral("slow-a"), QString(), 1, {}).has_value(); }, 8000);
    spin(300);
    check("late replies ignored cleanly (prefetcher stays idle, counts unchanged)",
          pfW.idle() && pfW.expired() == 3 && pfW.issued() == 5);
    QDir(rootW).removeRecursively();
    qunsetenv("MMV_PREFETCH_WATCHDOG_S");

    QDir(root).removeRecursively();
    qunsetenv("MMV_ADDONS_ROOT");
    qunsetenv("MMV_PREFETCH_TTL_S");

    printf("prefetch: %d passed, %d failed\n", pass, fail);
    printf("%s\n", fail == 0 ? "ADDON-OK" : "ADDON-FAIL");
    return fail == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--prefetch"))
        return probePrefetch();
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--remote"))
        return probeRemote(QString::fromLocal8Bit(argv[2]),
                           argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString());
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--mangaflow"))
        return probeMangaFlow(QString::fromLocal8Bit(argv[2]));
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--manager"))
        return probeManager(argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString(),
                            argc >= 4 ? QString::fromLocal8Bit(argv[3]).toInt() : 1);
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--metaflow"))
        return probeMetaFlow(QString::fromLocal8Bit(argv[2]));
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--metaone"))
        return probeMetaOne(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    if (argc < 2) { printf("usage: probe_addon <main.js> | --manager\n"); return 2; }

    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { printf("can't read %s\n", argv[1]); return 1; }

    AddonManifest m;
    m.id = QStringLiteral("probe");
    m.permissions << QStringLiteral("network");
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/mymediavault-addon-probe"));

    QString err;
    auto addon = JsAddon::load(QString::fromUtf8(f.readAll()), std::move(ctx), &err);
    if (!addon) { printf("load failed: %s\n", err.toUtf8().constData()); return 1; }

    printf("has getCatalog: %s   has getDetail: %s   has search: %s\n",
           addon->hasFunction(QStringLiteral("getCatalog")) ? "yes" : "no",
           addon->hasFunction(QStringLiteral("getDetail")) ? "yes" : "no",
           addon->hasFunction(QStringLiteral("search")) ? "yes" : "no");

    // Optional 2nd arg: the getCatalog argument JSON, e.g. '{"catalog":"music"}'. Default "{}".
    const QString catArg = (argc >= 3) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("{}");
    const MediaCatalog cat = MediaCatalog::fromJson(addon->invoke(QStringLiteral("getCatalog"), catArg).toUtf8());
    printf("getCatalog(%s): \"%s\"  (%d item%s, hasMore=%s)\n", catArg.toUtf8().constData(), cat.title.toUtf8().constData(),
           int(cat.items.size()), cat.items.size() == 1 ? "" : "s", cat.hasMore ? "yes" : "no");
    int shown = 0;
    for (const MediaItem& it : cat.items)
    {
        if (shown++ >= 6) { printf("  ... (%d more)\n", int(cat.items.size()) - 6); break; }
        printf("  - [%s%s] %s — %s\n", it.type.toUtf8().constData(), it.expandable ? ",container" : "",
               it.title.toUtf8().constData(), it.subtitle.toUtf8().constData());
    }

    // Drill-down: if the catalog has a container, fetch its children via getDetail.
    if (argc >= 3)
    {
        const MediaItem* container = nullptr;
        for (const MediaItem& it : cat.items) if (it.expandable) { container = &it; break; }
        if (container)
        {
            const QByteArray arg = QByteArray("{\"id\":\"") + container->id.toUtf8() + "\",\"type\":\"" + container->type.toUtf8() + "\"}";
            const MediaCatalog det = MediaCatalog::fromJson(addon->invoke(QStringLiteral("getDetail"), QString::fromUtf8(arg)).toUtf8());
            printf("getDetail(%s): \"%s\"  (%d child item%s)\n", container->title.toUtf8().constData(),
                   det.title.toUtf8().constData(), int(det.items.size()), det.items.size() == 1 ? "" : "s");
            for (int i = 0; i < det.items.size() && i < 6; ++i)
                printf("    · [%s] %s — %s\n", det.items[i].type.toUtf8().constData(),
                       det.items[i].title.toUtf8().constData(), det.items[i].subtitle.toUtf8().constData());
            const bool ok = !cat.items.isEmpty() && !det.items.isEmpty() && det.items[0].type != QStringLiteral("info");
            printf("%s\n", ok ? "DRILL-DOWN WORKS: catalog + getDetail returned real items" : "check output");
            return ok ? 0 : 1;
        }
        printf("%s\n", cat.items.isEmpty() ? "no items" : "catalog ok (no container to drill into)");
        return cat.items.isEmpty() ? 1 : 0;
    }

    // No catalog arg: also exercise getConfig round-trip (hello-source style).
    AddonContext::writeConfig(QStringLiteral("probe"), QStringLiteral("greeting"), QStringLiteral("Configured Title"));
    AddonContext::writeConfig(QStringLiteral("probe"), QStringLiteral("showExtra"), QStringLiteral("true"));
    const MediaCatalog cat2 = MediaCatalog::fromJson(addon->invoke(QStringLiteral("getCatalog"), QStringLiteral("{}")).toUtf8());
    const bool configWorks = cat2.title == QStringLiteral("Configured Title");
    printf("after setConfig: title=\"%s\"  -> getConfig %s\n", cat2.title.toUtf8().constData(), configWorks ? "WORKS" : "n/a");
    printf("%s\n", !cat.items.isEmpty() ? "ADDON ENGINE WORKS" : "no items");
    return cat.items.isEmpty() ? 1 : 0;
}
