// Headless tests for the offline metadata cache (src/core/MetaCache): the bundle a download saves so its
// poster/info keep working with no network. Asserts the contract the app relies on — and the one that
// makes the cache future-proof: merge() must PRESERVE keys it doesn't know about, so new metadata kinds
// can be added later without a migration. Prints META-OK on success; META-FAIL <what> and exits non-zero.
#include "AddonModels.h"
#include "MetaCache.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
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

    // ================================================================ MediaArt: the extensible artwork/
    // videos/audio/metadata schema themes bind to, the aggregator merge, and offline round-tripping.
    {
        // -- parse: images (object with string|array), flat role keys, synonyms, videos/audio/meta --------
        const QByteArray providerJson = QJsonDocument(QJsonObject{
            { QStringLiteral("title"), QStringLiteral("Chrono Trigger") },
            { QStringLiteral("overview"), QStringLiteral("A time-travel RPG.") },
            { QStringLiteral("image"), QStringLiteral("https://x.invalid/cover.jpg") },
            { QStringLiteral("images"), QJsonObject{
                { QStringLiteral("logo"), QStringLiteral("https://x.invalid/logo.png") },
                { QStringLiteral("screenshot"), QJsonArray{ QStringLiteral("https://x.invalid/s1.jpg"),
                                                            QStringLiteral("https://x.invalid/s2.jpg") } } } },
            { QStringLiteral("boxart"), QStringLiteral("https://x.invalid/box.jpg") }, // synonym -> "box"
            { QStringLiteral("videos"), QJsonArray{ QStringLiteral("https://x.invalid/trailer.mp4") } },
            { QStringLiteral("audio"), QJsonArray{ QStringLiteral("https://x.invalid/theme.mp3") } },
            { QStringLiteral("meta"), QJsonObject{ { QStringLiteral("developer"), QStringLiteral("Square") } } },
        }).toJson(QJsonDocument::Compact);
        const MediaDetail pd = MediaDetail::fromJson(providerJson);
        CHECK(pd.art.image(QStringLiteral("logo")) == QStringLiteral("https://x.invalid/logo.png"),
              "art: images.logo parses");
        CHECK(pd.art.images.value(QStringLiteral("screenshot")).size() == 2, "art: screenshot list parses");
        CHECK(pd.art.image(QStringLiteral("box")) == QStringLiteral("https://x.invalid/box.jpg"),
              "art: flat 'boxart' key canonicalizes to role 'box'");
        CHECK(pd.art.image(QStringLiteral("poster")) == QStringLiteral("https://x.invalid/cover.jpg"),
              "art: back-compat 'image' registers as poster role");
        CHECK(pd.art.videos.size() == 1 && pd.art.audio.size() == 1, "art: videos + audio parse");
        CHECK(pd.art.meta.value(QStringLiteral("developer")).toString() == QStringLiteral("Square"),
              "art: free-form meta bag parses");

        // -- writeInto: scalar aliases + images sub-map, never clobbering reserved row keys ---------------
        QVariantMap row{ { QStringLiteral("title"), QStringLiteral("Chrono Trigger") },
                         { QStringLiteral("image"), QStringLiteral("grid-thumb.jpg") } };
        pd.art.writeInto(row);
        CHECK(row.value(QStringLiteral("logo")).toString() == QStringLiteral("https://x.invalid/logo.png"),
              "writeInto: selected.logo scalar alias");
        CHECK(row.value(QStringLiteral("box")).toString() == QStringLiteral("https://x.invalid/box.jpg"),
              "writeInto: selected.box scalar alias");
        CHECK(row.value(QStringLiteral("image")).toString() == QStringLiteral("grid-thumb.jpg"),
              "writeInto: never clobbers a reserved key already on the row");
        CHECK(row.value(QStringLiteral("images")).toMap().value(QStringLiteral("screenshot")).toStringList().size() == 2,
              "writeInto: images sub-map carries the full list for galleries");
        CHECK(row.value(QStringLiteral("videos")).toStringList().size() == 1, "writeInto: videos list");
        CHECK(row.value(QStringLiteral("meta")).toMap().value(QStringLiteral("developer")).toString() == QStringLiteral("Square"),
              "writeInto: meta bag passes through");

        // -- mergeLowerPriority: the aggregator's role precedence (first source that has a role wins) ------
        MediaArt best;                            // "SteamGridDB": great logo + box, no video
        best.addImage(QStringLiteral("logo"), QStringLiteral("sgdb/logo.png"));
        best.addImage(QStringLiteral("box"),  QStringLiteral("sgdb/box.jpg"));
        MediaArt lower;                           // "IGDB": a different logo + a video + meta
        lower.addImage(QStringLiteral("logo"), QStringLiteral("igdb/logo.png"));
        lower.videos << QStringLiteral("igdb/trailer.mp4");
        lower.meta.insert(QStringLiteral("rating"), 92);
        best.mergeLowerPriority(lower);
        CHECK(best.image(QStringLiteral("logo")) == QStringLiteral("sgdb/logo.png"),
              "merge: higher-priority source keeps the role it has (logo stays SGDB)");
        CHECK(best.images.value(QStringLiteral("logo")).size() == 2,
              "merge: the lower source's logo is kept as an extra candidate");
        CHECK(best.videos.value(0) == QStringLiteral("igdb/trailer.mp4"),
              "merge: a role only the lower source has is backfilled (video from IGDB)");
        CHECK(best.meta.value(QStringLiteral("rating")).toInt() == 92, "merge: meta backfills too");

        // -- offline: saveArt records urls + prefetch record; loadArt puts the cached file first ----------
        const QString akey = QStringLiteral("art:probe");
        MetaCache::remove(akey);
        MetaCache::saveArt(akey, pd.art);
        QDir().mkpath(MetaCache::dirFor(akey));
        { QFile f(MetaCache::dirFor(akey) + QStringLiteral("/logo.png")); f.open(QIODevice::WriteOnly); f.write("png"); }
        MetaCache::merge(akey, { { QStringLiteral("images"),
            QJsonObject{ { QStringLiteral("logo"), QStringLiteral("logo.png") } } } }); // simulate finished download
        const MediaArt reloaded = MetaCache::loadArt(akey);
        CHECK(reloaded.images.value(QStringLiteral("logo")).first().endsWith(QStringLiteral("logo.png"))
                  && !reloaded.images.value(QStringLiteral("logo")).first().startsWith(QStringLiteral("http")),
              "loadArt: cached local file is offered before the remote url (offline-first)");
        CHECK(reloaded.videos.size() == 1, "loadArt: videos survive the round-trip");
        CHECK(reloaded.meta.value(QStringLiteral("developer")).toString() == QStringLiteral("Square"),
              "loadArt: meta bag survives the round-trip");
        MetaCache::remove(akey);

        std::printf("ART-OK\n");
    }

    // ================================================================ image-cache size cap + eviction
    // Browsing persists every scrolled poster (storeImage), so the cache must stay bounded: beyond the
    // cap, the oldest-accessed thumb-role images go first — but art of downloaded/favorited (pinned)
    // items is never evicted; that's the offline-first promise.
    {
        const QString kOld = QStringLiteral("cap:old");   // oldest-accessed -> evicted first
        const QString kFav = QStringLiteral("cap:fav");   // pinned (a favourite) -> never evicted
        const QString kNew = QStringLiteral("cap:new");   // recently accessed -> evicted last
        for (const QString& k : { kOld, kFav, kNew }) MetaCache::remove(k);

        const QByteArray bytes(1000, 'x');
        for (const QString& k : { kOld, kFav, kNew })
            MetaCache::storeImage(k, QStringLiteral("thumb"), QStringLiteral("https://x.invalid/p.png"),
                                  QStringLiteral("image/png"), bytes);
        CHECK(!MetaCache::imagePath(kOld, QStringLiteral("thumb")).isEmpty(), "storeImage persists the poster");

        // Age the files: kOld least recently accessed, kFav in between, kNew freshest.
        auto setMtime = [](const QString& key, int daysAgo) {
            QFile f(MetaCache::dirFor(key) + QStringLiteral("/thumb.png"));
            if (f.open(QIODevice::ReadWrite))
                f.setFileTime(QDateTime::currentDateTime().addDays(-daysAgo), QFileDevice::FileModificationTime);
        };
        setMtime(kOld, 3);
        setMtime(kFav, 2);
        setMtime(kNew, 1);
        MetaCache::setPinnedKeysProvider([kFav] { return QSet<QString>{ kFav }; });

        CHECK(MetaCache::enforceImageCacheCap(1024 * 1024) == 0, "under the cap nothing is evicted");
        CHECK(MetaCache::enforceImageCacheCap(2500) >= 1, "over the cap eviction runs");
        CHECK(MetaCache::imagePath(kOld, QStringLiteral("thumb")).isEmpty(),
              "the oldest-accessed thumb is evicted first");
        CHECK(MetaCache::load(kOld).value(QStringLiteral("images")).toObject()
                  .value(QStringLiteral("thumb")).toString().isEmpty(),
              "eviction also drops the bundle's images record");
        CHECK(!MetaCache::imagePath(kNew, QStringLiteral("thumb")).isEmpty(),
              "a recently accessed thumb survives when evicting the oldest suffices");

        // Even a cap smaller than the pinned art alone must never touch it.
        MetaCache::enforceImageCacheCap(1);
        CHECK(MetaCache::imagePath(kNew, QStringLiteral("thumb")).isEmpty(), "unpinned art goes when the cap demands");
        CHECK(!MetaCache::imagePath(kFav, QStringLiteral("thumb")).isEmpty(),
              "downloaded/favorited art is NEVER evicted (offline-first promise)");

        // Serving a cached image refreshes its recency (LRU-ish), so browsed-again art isn't first out.
        const QString kSeen = QStringLiteral("cap:seen");
        MetaCache::remove(kSeen);
        MetaCache::storeImage(kSeen, QStringLiteral("thumb"), QStringLiteral("https://x.invalid/p.png"),
                              QStringLiteral("image/png"), bytes);
        setMtime(kSeen, 30);
        const QString seenPath = MetaCache::imagePath(kSeen, QStringLiteral("thumb"));
        CHECK(!seenPath.isEmpty()
                  && QFileInfo(seenPath).lastModified() > QDateTime::currentDateTime().addDays(-1),
              "serving a cached image bumps its access recency");

        MetaCache::setPinnedKeysProvider({});
        for (const QString& k : { kOld, kFav, kNew, kSeen }) MetaCache::remove(k);
        std::printf("EVICT-OK\n");
    }

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
