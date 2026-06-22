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
static int probeManager()
{
    AddonManager mgr;
    printf("addons root: %s\n", mgr.addonsRoot().toUtf8().constData());
    printf("media sources: %d\n", mgr.sources().size());
    if (mgr.sources().isEmpty()) { printf("no sources discovered\n"); return 1; }

    LoadedAddon* s = mgr.sources().first();
    printf("source: %s (%s)\n", s->manifest.name.toUtf8().constData(), s->manifest.id.toUtf8().constData());
    const MediaCatalog cat = mgr.catalog(s);
    printf("catalog \"%s\": %d item(s)\n", cat.title.toUtf8().constData(), cat.items.size());
    bool allResolved = !cat.items.isEmpty();
    for (const MediaItem& it : cat.items)
    {
        const bool remote = it.url.contains(QStringLiteral("://"));
        const bool ok = remote || QFile::exists(it.url);
        if (!ok) allResolved = false;
        printf("  - %s -> %s  [%s]\n", it.title.toUtf8().constData(), it.url.toUtf8().constData(),
               ok ? "resolved" : "MISSING");
    }
    printf("%s\n", allResolved ? "ADDON MANAGER WORKS: discovered + resolved" : "some items unresolved");
    return allResolved ? 0 : 1;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--manager"))
        return probeManager();
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

    printf("has getCatalog: %s\n", addon->hasFunction(QStringLiteral("getCatalog")) ? "yes" : "no");
    printf("has search:     %s\n", addon->hasFunction(QStringLiteral("search")) ? "yes" : "no");

    const QString json = addon->invoke(QStringLiteral("getCatalog"), QStringLiteral("{}"));
    printf("getCatalog -> %s\n", json.toUtf8().constData());

    const MediaCatalog cat = MediaCatalog::fromJson(json.toUtf8());
    printf("catalog: \"%s\"  (%d item%s)\n", cat.title.toUtf8().constData(),
           cat.items.size(), cat.items.size() == 1 ? "" : "s");
    for (const MediaItem& it : cat.items)
        printf("  - [%s] %s — %s\n      url: %s\n",
               it.type.toUtf8().constData(), it.title.toUtf8().constData(),
               it.subtitle.toUtf8().constData(), it.url.toUtf8().constData());

    printf("%s\n", cat.items.isEmpty() ? "no items" : "ADDON ENGINE WORKS: JS ran + catalog parsed");
    return cat.items.isEmpty() ? 1 : 0;
}
