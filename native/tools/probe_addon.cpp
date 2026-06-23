// Headless check of the addon JS engine: load an addon's main.js through Duktape, call getCatalog(),
// and parse the returned catalog - the exact path the Library uses.
#include "JsAddon.h"
#include "AddonContext.h"
#include "AddonModels.h"
#include "AddonManager.h"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
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
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/goliath-addon-probe"));
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
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/goliath-addon-probe"));
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
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/goliath-addon-probe"));
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
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
    auto ctx = std::make_unique<AddonContext>(m, QDir::tempPath() + QStringLiteral("/goliath-addon-probe"));

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
