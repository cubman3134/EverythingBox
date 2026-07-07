// Headless tests for the offline metadata cache (src/core/MetaCache): the bundle a download saves so its
// poster/info keep working with no network. Asserts the contract the app relies on — and the one that
// makes the cache future-proof: merge() must PRESERVE keys it doesn't know about, so new metadata kinds
// can be added later without a migration. Prints META-OK on success; META-FAIL <what> and exits non-zero.
#include "MetaCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QStandardPaths>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "META-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // AppPaths::dataDir() is the exe's own folder (portable app), so the bundles land next to the probe
    // in the build tree; everything written here is removed again at the end.

    MediaItem item;
    item.id = QStringLiteral("igdb:1068");
    item.title = QStringLiteral("Bonk's Adventure");
    item.subtitle = QStringLiteral("1990");
    item.type = QStringLiteral("game");
    item.thumbnailUrl = QStringLiteral("https://example.invalid/bonk.jpg");
    item.mime = QStringLiteral("game");
    item.systemHint = QStringLiteral("TurboGrafx-16");
    item.altNames = { QStringLiteral("PC Genjin") };
    const QString key = MetaCache::keyFor(item);
    CHECK(key == item.id, "keyFor prefers the stable id");
    MetaCache::remove(key); // fresh start if a previous run left state

    // ---------------------------------------------------------------- item round-trip
    MetaCache::saveItem(item);
    QJsonObject obj = MetaCache::load(key);
    CHECK(obj.value(QStringLiteral("v")).toInt() == 1, "bundle carries a schema version");
    CHECK(obj.value(QStringLiteral("item")).toObject().value(QStringLiteral("title")).toString()
              == item.title, "item title round-trips");
    CHECK(obj.value(QStringLiteral("item")).toObject().value(QStringLiteral("systemHint")).toString()
              == item.systemHint, "item system hint round-trips");

    // ---------------------------------------------------------------- future-proof merge
    // Some future feature stores a kind of metadata this build knows nothing about…
    MetaCache::merge(key, { { QStringLiteral("playStats"),
                              QJsonObject{ { QStringLiteral("minutes"), 90 } } } });
    // …then today's writers run again (a re-download refreshes the item/detail)…
    MetaCache::saveItem(item);
    MediaDetail d;
    d.title = item.title;
    d.subtitle = item.subtitle;
    d.overview = QStringLiteral("Bonk fights the evil King Drool using nothing but his enormous head.");
    d.imageUrl = QStringLiteral("https://example.invalid/bonk-large.jpg");
    d.facts = { { QStringLiteral("Genre"), QStringLiteral("Platformer") },
                { QStringLiteral("Rating"), QStringLiteral("87%") } };
    d.valid = true;
    MetaCache::saveDetail(key, d);
    // …and the unknown key must still be there.
    obj = MetaCache::load(key);
    CHECK(obj.value(QStringLiteral("playStats")).toObject().value(QStringLiteral("minutes")).toInt() == 90,
          "merge preserves keys it doesn't know about (future metadata survives)");

    // ---------------------------------------------------------------- detail round-trip (offline card)
    const MediaDetail back = MetaCache::cachedDetail(key);
    CHECK(back.valid, "cached detail is valid");
    CHECK(back.overview == d.overview, "overview round-trips");
    CHECK(back.facts.size() == 2 && back.facts[1].value == QStringLiteral("87%"), "facts round-trip");
    CHECK(back.imageUrl == d.imageUrl, "with no cached artwork the card falls back to the url");

    // ---------------------------------------------------------------- artwork resolution
    CHECK(MetaCache::imagePath(key, QStringLiteral("thumb")).isEmpty(), "no artwork cached yet");
    CHECK(MetaCache::displayImage(key, item.thumbnailUrl) == item.thumbnailUrl,
          "displayImage falls back to the url when nothing is cached");
    // Simulate a completed artwork download: the file on disk + its "images" record.
    QDir().mkpath(MetaCache::dirFor(key));
    {
        QFile f(MetaCache::dirFor(key) + QStringLiteral("/thumb.jpg"));
        CHECK(f.open(QIODevice::WriteOnly), "can write into the bundle dir");
        f.write("jpegbytes");
    }
    MetaCache::merge(key, { { QStringLiteral("images"),
                              QJsonObject{ { QStringLiteral("thumb"), QStringLiteral("thumb.jpg") } } } });
    const QString local = MetaCache::imagePath(key, QStringLiteral("thumb"));
    CHECK(!local.isEmpty() && QFile::exists(local), "imagePath resolves the cached file");
    CHECK(MetaCache::displayImage(key, item.thumbnailUrl) == local,
          "displayImage prefers the cached local artwork (offline shelves)");
    CHECK(MetaCache::cachedDetail(key).imageUrl == local,
          "the offline detail card uses the cached artwork");

    // ---------------------------------------------------------------- items without a stable identity
    CHECK(MetaCache::keyFor(MediaItem{}).isEmpty(), "no id and no url -> no key");
    MetaCache::merge(QString(), { { QStringLiteral("x"), 1 } }); // must be a safe no-op
    CHECK(MetaCache::load(QString()).isEmpty(), "empty key never stores anything");

    // ---------------------------------------------------------------- uninstall cleanup
    MetaCache::remove(key);
    CHECK(MetaCache::load(key).isEmpty(), "remove deletes the bundle");
    CHECK(!QDir(MetaCache::dirFor(key)).exists(), "remove deletes the folder (artwork included)");
    QDir(QCoreApplication::applicationDirPath() + QStringLiteral("/metadata")).removeRecursively(); // probe tidy-up

    if (failures) { std::fprintf(stderr, "META-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("META-OK\n");
    return 0;
}
