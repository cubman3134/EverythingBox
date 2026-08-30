// Headless check of the addon JS engine: load an addon's main.js through Duktape, call getCatalog(),
// and parse the returned catalog - the exact path the Library uses.
#include "JsAddon.h"
#include "AddonContext.h"
#include "AddonModels.h"
#include "AddonManager.h"
#include "CatalogPrefetcher.h"
#include "BuiltinSecrets.h" // generated (build tree): expected lengths for the credscope asserts
#include "miniz.h"          // build a fixture .addon package for the reserved-namespace install guard

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <functional>
#include <memory>
#include <cstdio>
#include <cstring>

// With "--manager": construct an AddonManager (scans AppPaths::dataDir()/addons, which under a probe build is
// this process's own scratch directory, not the exe folder - issue #42), list its sources and the first
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
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/everythingbox-addon-probe"));
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
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/everythingbox-addon-probe"));
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

// With "--getmeta <main.js> <title> [systemHint] [addonId]": call a metaFor provider's getMeta() directly
// for a game title (no catalog needed) - used to exercise the ScreenScraper embedded-devcred path end to end.
// addonId defaults to "probe"; pass the real addon id so getConfig() reads that addon's stored user account
// (ssid/sspassword). Prints only the returned game metadata (never any credential).
static int probeGetMeta(const QString& jsPath, const QString& title, const QString& systemHint, const QString& addonId)
{
    QFile f(jsPath);
    if (!f.open(QIODevice::ReadOnly)) { printf("can't read %s\n", jsPath.toUtf8().constData()); return 1; }
    AddonManifest m;
    m.id = addonId.isEmpty() ? QStringLiteral("probe") : addonId;
    m.permissions << QStringLiteral("network");
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/everythingbox-addon-probe"));
    QString err;
    auto addon = JsAddon::load(QString::fromUtf8(f.readAll()), std::move(ctx), &err);
    if (!addon) { printf("load failed: %s\n", err.toUtf8().constData()); return 1; }
    printf("has getMeta: %s   addonId: %s\n", addon->hasFunction(QStringLiteral("getMeta")) ? "yes" : "no",
           m.id.toUtf8().constData());

    QJsonObject arg{ { QStringLiteral("type"), QStringLiteral("game") }, { QStringLiteral("title"), title } };
    if (!systemHint.isEmpty()) arg.insert(QStringLiteral("systemHint"), systemHint);
    const QString argJson = QString::fromUtf8(QJsonDocument(arg).toJson(QJsonDocument::Compact));
    const MediaDetail d = MediaDetail::fromJson(addon->invoke(QStringLiteral("getMeta"), argJson).toUtf8());
    printf("getMeta(title=\"%s\", system=\"%s\") -> valid=%s  title=\"%s\"  facts=%d  image=%s  overview=%s\n",
           title.toUtf8().constData(), systemHint.toUtf8().constData(), d.valid ? "yes" : "no",
           d.title.toUtf8().constData(), int(d.facts.size()), d.imageUrl.isEmpty() ? "(none)" : "set",
           d.overview.isEmpty() ? "(none)" : "set");
    for (const MediaFact& fc : d.facts) printf("    %s: %s\n", fc.label.toUtf8().constData(), fc.value.toUtf8().constData());
    printf("%s\n", d.valid ? "GETMETA WORKS" : "GETMETA: no metadata (check creds/network/title)");
    return d.valid ? 0 : 1;
}

// With "--mangaflow <main.js>": exercise the keyless MangaDex path end to end - catalog -> manga meta ->
// chapters (getDetail) -> chapter meta. Verifies the manga drill-down + both detail headers offline.
static int probeMangaFlow(const QString& jsPath)
{
    QFile f(jsPath);
    if (!f.open(QIODevice::ReadOnly)) { printf("can't read %s\n", jsPath.toUtf8().constData()); return 1; }
    AddonManifest m; m.id = QStringLiteral("probe"); m.permissions << QStringLiteral("network");
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/everythingbox-addon-probe"));
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
// Spins its own deterministic JsLocal fixtures in an isolated temp addons root (EB_ADDONS_ROOT) and drives
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

// Build a minimal .addon package (a zip with a top-level manifest.json + main.js) at pkgPath, carrying the
// given manifest id — used to exercise AddonManager::installPackage's reserved-namespace guard.
static bool makeAddonPackage(const QString& pkgPath, const QString& id)
{
    const QByteArray manifest =
        "{\n  \"id\": \"" + id.toUtf8() + "\",\n  \"name\": \"Pkg Fixture\",\n  \"version\": \"1.0.0\",\n"
        "  \"type\": \"media-source\",\n  \"entry\": \"main.js\",\n  \"permissions\": [],\n"
        "  \"catalogs\": [ { \"id\": \"movies\", \"name\": \"Movies\", \"type\": \"movie\" } ]\n}\n";
    static const char* JS = "function getCatalog(){return JSON.stringify({title:'x',items:[],hasMore:false});}\n";
    mz_zip_archive zip; std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, pkgPath.toUtf8().constData(), 0)) return false;
    bool ok = mz_zip_writer_add_mem(&zip, "manifest.json", manifest.constData(), size_t(manifest.size()), MZ_BEST_SPEED)
           && mz_zip_writer_add_mem(&zip, "main.js", JS, std::strlen(JS), MZ_BEST_SPEED);
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    return ok;
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

// "Touches no network" holds because AddonManager's constructor skips every startup network kick
// (default-source seeding, remote-manifest refresh, addon self-update) whenever EB_ADDONS_ROOT is set,
// and this probe sets that override before constructing any manager. So the fixtures really are the only
// sources, and the slot-accounting asserts below ("issued exactly one request per job") count only their
// requests. This probe used to scrub the shared portable ini by hand to get the same guarantee; that
// belongs in the production gate, not test-side, and the hand-scrub is gone.
static int probePrefetch()
{
    int pass = 0, fail = 0;
    auto check = [&](const char* name, bool ok) {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name); if (ok) ++pass; else ++fail;
    };

    // ---- credential fallback selection (issue #88) -------------------------------------------------------
    // (a) The PURE selection rule AddonContext::selectCredential() — the single source of truth every game-art
    // provider applies through providerCredential(): a user-set value wins when present, else the embedded
    // builtin, else "" (the provider stays dormant, never firing a request with an empty key). Tested directly
    // with independent hand-picked fixtures (NOT computed from the function), with the two operands distinct so
    // a mutant that returns the wrong one is killed.
    {
        check("select: user present + builtin present -> user wins",
              AddonContext::selectCredential(QStringLiteral("USER-KEY"), QStringLiteral("BUILTIN-KEY"))
                  == QStringLiteral("USER-KEY"));
        check("select: user present + builtin absent -> user",
              AddonContext::selectCredential(QStringLiteral("USER-KEY"), QString())
                  == QStringLiteral("USER-KEY"));
        check("select: user absent + builtin present -> builtin",
              AddonContext::selectCredential(QString(), QStringLiteral("BUILTIN-KEY"))
                  == QStringLiteral("BUILTIN-KEY"));
        check("select: user absent + builtin absent -> empty (dormant)",
              AddonContext::selectCredential(QString(), QString()).isEmpty());
    }

    // (b) builtinCredential scoping: the global is bound into EVERY JsLocal addon, and the C++ side allowlists
    // by (addon id, key). A third-party addon — or a bundled provider asking for another provider's key — must
    // get EMPTY; the matching bundled id passes through. Exercised via the REAL JS binding, asserted by LENGTH
    // only (credential values are never printed). Allow-path lengths self-adjust to the build: they equal the
    // embedded header lengths (0 when no secrets file is present, non-zero when the owner placed one).
    {
        static const char* kCredJs =
            "function builtinLen(k){ return String(builtinCredential(k).length); }\n"
            "function provLen(k){ return String(providerCredential(k).length); }\n";
        auto len = [&](const QString& addonId, const char* fn, const QString& key) -> int {
            AddonManifest m; m.id = addonId;
            auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/eb-credscope-probe"));
            QString err;
            auto a = JsAddon::load(QString::fromUtf8(kCredJs), std::move(ctx), &err);
            if (!a) { printf("credscope fixture load failed: %s\n", err.toUtf8().constData()); return -1; }
            return a->invoke(QString::fromUtf8(fn), key).toInt();
        };
        auto blen = [&](const QString& id, const QString& key) { return len(id, "builtinLen", key); };
        using namespace eb_secrets;
        const QString ss   = QStringLiteral("com.everythingbox.screenscraper");
        const QString tgdb = QStringLiteral("com.everythingbox.thegamesdb");
        const QString igdb = QStringLiteral("com.everythingbox.igdb");
        const QString evil = QStringLiteral("com.evil.thirdparty");

        check("credscope: third-party id gets EMPTY for every provider key",
              blen(evil, QStringLiteral("devid")) == 0 && blen(evil, QStringLiteral("apikey")) == 0
              && blen(evil, QStringLiteral("clientId")) == 0);
        check("credscope: screenscraper id -> its own keys (lengths match the embedded header)",
              blen(ss, QStringLiteral("devid")) == kSS_DevId_ALen + kSS_DevId_BLen
              && blen(ss, QStringLiteral("devpassword")) == kSS_DevPw_ALen + kSS_DevPw_BLen);
        check("credscope: thegamesdb id -> its apikey (length matches the embedded header)",
              blen(tgdb, QStringLiteral("apikey")) == kTgdb_Key_ALen + kTgdb_Key_BLen);
        check("credscope: igdb id -> its clientId/clientSecret (lengths match the embedded header)",
              blen(igdb, QStringLiteral("clientId")) == kIgdb_Id_ALen + kIgdb_Id_BLen
              && blen(igdb, QStringLiteral("clientSecret")) == kIgdb_Secret_ALen + kIgdb_Secret_BLen);
        check("credscope: cross-key isolation — screenscraper id asking for apikey gets EMPTY",
              blen(ss, QStringLiteral("apikey")) == 0 && blen(tgdb, QStringLiteral("devid")) == 0);

        // (c) providerCredential runtime glue via the REAL JS binding: a user-set value wins over any builtin
        // and is returned verbatim (asserted by its known length). Writes to the probe's ISOLATED data dir
        // (EB_ISOLATED_DATA_DIR), never a real ini. This gives the user-wins branch teeth independent of any
        // embedded secret: with a user value set, provLen must equal the user string's length regardless of
        // whether a builtin exists.
        {
            const QString userVal = QStringLiteral("USER-TGDB-abc123");
            AddonContext::writeConfig(tgdb, QStringLiteral("apikey"), userVal);
            check("providerCredential: user-set value wins and round-trips verbatim",
                  len(tgdb, "provLen", QStringLiteral("apikey")) == userVal.length());
            AddonContext::writeConfig(tgdb, QStringLiteral("apikey"), QString()); // clear
            check("providerCredential: no user value -> falls back to builtin length (0 without secrets)",
                  len(tgdb, "provLen", QStringLiteral("apikey")) == kTgdb_Key_ALen + kTgdb_Key_BLen);
        }
    }

    const QString root = QDir::tempPath() + QStringLiteral("/eb-prefetch-fixture-")
                       + QString::number(QCoreApplication::applicationPid());
    QDir(root).removeRecursively();
    const QStringList ids = { QStringLiteral("probe.fixture.0"), QStringLiteral("probe.fixture.1"),
                              QStringLiteral("probe.fixture.2") };
    for (const QString& id : ids) if (!makeFixture(root, id)) { printf("fixture write failed\n"); return 2; }
    qputenv("EB_ADDONS_ROOT", root.toUtf8());

    // ---- Manager A: comfortably-long TTL for the peek/disable/signal steps ----
    qputenv("EB_PREFETCH_TTL_S", "30");
    AddonManager mgr;
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

    // ---- Manager G: the gameplay gate (burst-hitch fix). While a game holds the main-thread frame loop the
    // prefetcher must issue NOTHING; on return to browse a catch-up resweep re-warms everything. Fresh empty
    // cache so a working sweep would have 6 jobs to do — proving the pause actually suppressed dispatch. ----
    AddonManager mgrG;
    for (const QString& id : ids) mgrG.setEnabled(id, true);
    CatalogPrefetcher pfG(&mgrG, &mgrG);
    pfG.setPeriodicResweep(false);
    pfG.setPaused(true);
    check("setPaused(true) reflected by isPaused()", pfG.isPaused());
    pfG.start();                 // gated: start() is a no-op while paused
    pfG.resweep();               // gated too
    spin(200);
    check("paused: no request issued despite an empty cache",
          pfG.issued() == 0 && pfG.inFlight() == 0 && pfG.queued() == 0 && pfG.idle());
    pfG.setPaused(false);        // return to browse -> catch-up resweep
    check("setPaused(false) reflected by isPaused()", !pfG.isPaused());
    spinUntil([&] { return pfG.idle(); }, 8000);
    check("resume issued exactly one request per job", pfG.issued() == totalJobs);
    bool allCachedG = true;
    for (const QString& id : ids) {
        LoadedAddon* s = mgrG.sourceById(id);
        for (const QString& c : { QStringLiteral("movies"), QStringLiteral("shows") })
            if (!mgrG.cachedCatalog(s, c, QString(), 1, {}).has_value()) allCachedG = false;
    }
    check("resume re-warmed every source x catalog", allCachedG);

    // ---- Manager R: reload() mid-sweep (the use-after-free case). AddonManager::reload() (fired from a
    // remote-manifest refresh ~0.5-3s after startup, install/remove, and self-update) destroys EVERY
    // LoadedAddon and clears the cache, then emits sourcesChanged. With 3 jobs parked in the queue holding
    // freed source pointers, the old raw-pointer Job dereferenced freed memory at the next pump. The fix:
    // jobs hold source IDS (re-resolved at pump), and sourcesChanged flushes the queue + slot bookkeeping
    // BEFORE the resweep re-enqueues fresh jobs against the rebuilt source set. 6 catalogs (>3) so the sweep
    // is provably mid-flight (3 in flight, 3 queued) at the moment reload() lands. ----
    AddonManager mgrR;
    for (const QString& id : ids) mgrR.setEnabled(id, true);
    CatalogPrefetcher pfR(&mgrR, &mgrR);
    pfR.setPeriodicResweep(false);
    pfR.start();
    check("reload case: sweep is mid-flight before reload (3 in flight, 3 queued)",
          pfR.inFlight() == CatalogPrefetcher::kMaxInFlight && pfR.queued() == 3);
    // Force the exact hazard: free every LoadedAddon while jobs sit queued, then fire the signal reload()
    // always pairs with. No event-loop turn happens between these two calls, so no stale-pointer pump can
    // occur before flush() clears the queue. Reaching the asserts below at all = no crash / no UAF.
    mgrR.reload();
    QMetaObject::invokeMethod(&mgrR, "sourcesChanged", Qt::DirectConnection);
    const bool drainedR = spinUntil([&] { return pfR.idle(); }, 8000);
    check("reload mid-sweep: prefetcher drained without crash or wedge", drainedR);
    bool allCachedR = true;
    for (const QString& id : ids) {
        LoadedAddon* s = mgrR.sourceById(id); // re-resolved against the REBUILT source set
        for (const QString& c : { QStringLiteral("movies"), QStringLiteral("shows") })
            if (!mgrR.hasCachedCatalog(s, c, QString(), 1, {})) allCachedR = false;
    }
    check("reload mid-sweep: resweep re-covered every dropped key (all catalogs cached)", allCachedR);

    // ---- Manager S: 1-second TTL to exercise expiry ----
    qputenv("EB_PREFETCH_TTL_S", "1");
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
    qputenv("EB_ADDONS_ROOT", rootW.toUtf8());
    qputenv("EB_PREFETCH_TTL_S", "30");      // roomy TTL: cache entries must outlive the asserts below
    qputenv("EB_PREFETCH_WATCHDOG_S", "1");  // expire a silent in-flight job after ~1s
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
    qunsetenv("EB_PREFETCH_WATCHDOG_S");

    // ---- Reserved-namespace install guard: a package claiming a com.everythingbox.* id must be REFUSED
    // (bundled ids never arrive via installPackage; a side-loaded one could impersonate/overwrite one). ----
    {
        const QString rootI = root + QStringLiteral("-inst");
        QDir(rootI).removeRecursively(); QDir().mkpath(rootI);
        qputenv("EB_ADDONS_ROOT", rootI.toUtf8());
        AddonManager mgrI;
        const QString reservedId = QStringLiteral("com.everythingbox.evil");
        const QString okId = QStringLiteral("com.thirdparty.ok");
        const QString pkgBad = rootI + QStringLiteral("/reserved.addon");
        const QString pkgOk  = rootI + QStringLiteral("/ok.addon");
        bool built = makeAddonPackage(pkgBad, reservedId) && makeAddonPackage(pkgOk, okId);
        QString errBad, errOk;
        const bool refused = built && !mgrI.installPackage(pkgBad, &errBad);
        const bool folderAbsent = !QDir(rootI + QStringLiteral("/") + reservedId).exists();
        check("install guard: reserved com.everythingbox.* package refused (not installed)",
              refused && folderAbsent && !errBad.isEmpty());
        // Control: a normal third-party id still installs through the same path (guard isn't over-broad).
        const bool okInstalled = built && mgrI.installPackage(pkgOk, &errOk)
                              && QDir(rootI + QStringLiteral("/") + okId).exists();
        check("install guard: a normal third-party id still installs", okInstalled);
        QDir(rootI).removeRecursively();
    }

    QDir(root).removeRecursively();
    qunsetenv("EB_ADDONS_ROOT");
    qunsetenv("EB_PREFETCH_TTL_S");

    printf("prefetch: %d passed, %d failed\n", pass, fail);
    printf("%s\n", fail == 0 ? "ADDON-OK" : "ADDON-FAIL");
    return fail == 0 ? 0 : 1;
}

// With "--comicorder <main.js>": the Comic Vine issue ORDER, offline and with no API key. Comic Vine keeps
// issue_number as a STRING column, so its own sort=issue_number:asc hands back #1, #10, #11 … #2, #20 —
// which is exactly what the comics browse was showing. The addon therefore has to order the issues itself,
// and this mode pins that it does: main.js is loaded with httpGet/getConfig replaced by a stand-in Comic
// Vine that reproduces the live API's semantics (each one checked against the live API on 2026-08-30) — a
// STRING sort on issue_number, a real date sort on cover_date with undated issues FIRST, and a hard ceiling
// of 100 results per request. Two volumes are walked page by page and the concatenated sequence asserted
// ascending and complete, so the assertion is on the ORDER THE ADDON RETURNS and any fetch strategy that
// gets there passes.
static const char* kComicVineStub = R"JS(
var __cvRequests = [], __cvIssues = [];

getConfig = function (k) { return k === "comicVineApiKey" ? "PROBEKEY" : ""; };

function __cvIssue(n, date) {
    return { id: 1000 + n, name: "Volume " + n, issue_number: String(n), image: null, cover_date: date };
}
function __cvDate(n) {  // ascending with the issue number, the way a periodical's cover dates run
    var y = 2006 + Math.floor((n - 1) / 6), mo = ((n - 1) % 6) * 2 + 1;
    return y + "-" + (mo < 10 ? "0" + mo : String(mo)) + "-01";
}

// "volume"  - 65 issues, one request's worth, ending in the two cases a cover-date sort ALONE cannot get
//             right, both of them real Comic Vine data: an issue with NO cover date (the API returns those
//             FIRST on an ascending date sort) and one that SHARES its neighbour's cover date.
// "longrun" - 250 plainly dated issues: a run past the API's 100-per-request ceiling, to pin the paging.
function __cvUseFixture(which) {
    __cvRequests = []; __cvIssues = [];
    var last = (which === "longrun") ? 250 : 63;
    for (var n = 1; n <= last; n++) __cvIssues.push(__cvIssue(n, __cvDate(n)));
    if (which !== "longrun") {
        __cvIssues.push(__cvIssue(64, null));
        __cvIssues.push(__cvIssue(65, __cvDate(63)));
    }
    return String(__cvIssues.length);
}

function __cvParam(url, k) {
    var m = new RegExp("[?&]" + k + "=([^&]*)").exec(url);
    return m ? decodeURIComponent(m[1]) : "";
}
function __cvCmp(a, b) { return a < b ? -1 : (a > b ? 1 : 0); }

httpGet = function (url) {
    __cvRequests.push(url);
    if (url.indexOf("/issues/") < 0) return "{}";
    var sort = __cvParam(url, "sort");
    var limit = parseInt(__cvParam(url, "limit"), 10) || 100;
    var offset = parseInt(__cvParam(url, "offset"), 10) || 0;
    if (limit > 100) limit = 100;  // the API's ceiling, silently applied
    var all = __cvIssues.slice();
    if (sort.indexOf("issue_number") === 0)      // a STRING column: "10" sorts before "2"
        all.sort(function (a, b) { return __cvCmp(a.issue_number, b.issue_number); });
    else if (sort.indexOf("cover_date") === 0)   // a date column; SQL puts NULL first, ascending
        all.sort(function (a, b) { return __cvCmp(a.cover_date || "", b.cover_date || ""); });
    var page = all.slice(offset, offset + limit);
    return JSON.stringify({ error: "OK", limit: limit, offset: offset,
                            number_of_page_results: page.length,
                            number_of_total_results: all.length, results: page });
};

// How many requests the addon has spent so far - the probe reads it between pages.
function __cvRequestCount() { return String(__cvRequests.length); }
)JS";

static int probeComicOrder(const QString& jsPath)
{
    QFile f(jsPath);
    if (!f.open(QIODevice::ReadOnly)) { printf("can't read %s\n", jsPath.toUtf8().constData()); return 1; }
    AddonManifest m; m.id = QStringLiteral("probe"); m.permissions << QStringLiteral("network");
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/everythingbox-addon-probe"));
    QString err;
    // The stub goes in FRONT of the addon body: both globals it replaces are only ever CALLED by the addon,
    // never redefined, so the addon itself runs unmodified against the stand-in server.
    auto addon = JsAddon::load(QString::fromUtf8(kComicVineStub) + QString::fromUtf8(f.readAll()), std::move(ctx), &err);
    if (!addon) { printf("load failed: %s\n", err.toUtf8().constData()); return 1; }

    int fail = 0;
    auto check = [&](bool ok, const char* what) {
        printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++fail;
    };

    // Walk every page of one volume's drill-down and return the issue numbers in the order shown.
    auto walk = [&](const char* fixture, bool* pagesOk, bool* boundedOk) {
        const int count = addon->invoke(QStringLiteral("__cvUseFixture"), QString::fromUtf8(fixture)).toInt();
        printf("  volume \"%s\": %d issues\n", fixture, count);
        QVector<double> seen;
        int requests = 0;
        for (int page = 1; page <= 10; ++page)
        {
            const QByteArray arg = QByteArray("{\"id\":\"comicvine:volume:46777\",\"type\":\"comic\",\"page\":")
                                 + QByteArray::number(page) + "}";
            const MediaCatalog cat = MediaCatalog::fromJson(
                addon->invoke(QStringLiteral("getDetail"), QString::fromUtf8(arg)).toUtf8());
            if (cat.items.isEmpty() || cat.items.first().type == QStringLiteral("info"))
            {
                printf("    page %d: no issues (%s)\n", page, cat.items.isEmpty()
                       ? "empty" : cat.items.first().title.toUtf8().constData());
                *pagesOk = false;
                break;
            }
            QStringList head;
            for (const MediaItem& it : cat.items)
            {
                if (!it.title.startsWith(QLatin1Char('#'))) { *pagesOk = false; continue; }
                // Every row is "#<issue> — <name>"; the number is what the shelf reads in order.
                seen.append(it.title.mid(1).section(QLatin1Char(' '), 0, 0).toDouble());
                if (head.size() < 6) head << it.title.section(QLatin1Char(' '), 0, 0);
            }
            printf("    page %d: %3d issue(s) [%s…] hasMore=%s\n", page, int(cat.items.size()),
                   head.join(QStringLiteral(", ")).toUtf8().constData(), cat.hasMore ? "yes" : "no");

            // A page must not fan out into a burst of requests. An addon call gets 5 SECONDS and every
            // httpGet inside it BLOCKS, so "fetch every page, then sort" is not a strategy that survives a
            // 900-issue run — and Comic Vine's own rate limit is per hour, not per page view.
            const int spent = addon->invoke(QStringLiteral("__cvRequestCount"), QStringLiteral("{}")).toInt();
            if (spent - requests > 3) *boundedOk = false;
            requests = spent;
            if (!cat.hasMore) break;
        }
        return seen;
    };

    auto ascending = [](const QVector<double>& v) {
        for (int i = 1; i < v.size(); ++i) if (v[i] <= v[i - 1]) return false;
        return true;
    };
    auto show = [](const QVector<double>& v) {
        QStringList out;
        for (int i = 0; i < v.size() && i < 16; ++i) out << QString::number(v[i]);
        printf("    order was: %s …\n", out.join(QStringLiteral(", ")).toUtf8().constData());
    };

    bool pagesOk = true, boundedOk = true;

    // 1. One volume, one request's worth of issues - a manga volume list, and most comic series.
    const QVector<double> one = walk("volume", &pagesOk, &boundedOk);
    if (!ascending(one)) show(one);
    check(ascending(one), "issues read in ascending NUMERIC order (#1, #2, #3 - not #1, #10, #11)");
    check(one.size() == 65, "every issue was listed exactly once (65)");
    check(!one.isEmpty() && one.first() == 1.0 && one.last() == 65.0,
          "#1 first and #65 last - the undated and date-sharing issues land in place");

    // 2. A run past the API's 100-per-request ceiling: the paging arithmetic, across pages.
    const QVector<double> many = walk("longrun", &pagesOk, &boundedOk);
    if (!ascending(many)) show(many);
    check(ascending(many), "a 250-issue run stays ascending ACROSS pages");
    check(many.size() == 250, "a 250-issue run lists every issue once (no gap, no repeat, at the page seam)");

    check(pagesOk, "every page returned issue rows");
    check(boundedOk, "no page spent more than 3 requests (a 5s call budget with blocking http)");

    printf("%s\n", fail == 0 ? "COMICORDER-OK" : "COMICORDER-FAIL");
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
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--comicorder"))
        return probeComicOrder(QString::fromLocal8Bit(argv[2]));
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--getmeta"))
        return probeGetMeta(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]),
                            argc >= 5 ? QString::fromLocal8Bit(argv[4]) : QString(),
                            argc >= 6 ? QString::fromLocal8Bit(argv[5]) : QString());
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
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/everythingbox-addon-probe"));

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
