// Headless tests for the offline metadata cache (src/core/MetaCache): the bundle a download saves so its
// poster/info keep working with no network. Asserts the contract the app relies on — and the one that
// makes the cache future-proof: merge() must PRESERVE keys it doesn't know about, so new metadata kinds
// can be added later without a migration. Prints META-OK on success; META-FAIL <what> and exits non-zero.
//
// Also covers the per-item OVERRIDE layer (src/core/MetaOverrides, issue #24): the record's single canonical
// spelling, the override-beats-scraped composite, the fact that all three MetaCache read primitives run it,
// that a re-scrape cannot discard it, and that reset restores the scraped values. The cross-device merge half
// of that store lives in probe_cloudmerge §20 — it needs CloudMerge, which this probe does not link.
#include "AddonModels.h"
#include "AppPaths.h"
#include "MetaCache.h"
#include "MetaOverrides.h"
#include "ScrapedSnapshot.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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
    // The bundles land under AppPaths::dataDir(), which for a probe build is this process's own scratch
    // directory (issue #42) — not the exe's folder, and not any other probe's. Everything written here is
    // removed again at the end anyway.

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

    // ================================================================ per-item metadata overrides (issue #24)
    // The user's correction to a wrong scrape. Two things are being pinned here and they are different: the
    // pure composite (override beats scraped, field by field, and an unset field changes nothing), and the
    // fact that MetaCache's three READ primitives all run it — which is what makes one edit visible on the
    // grid tile, the detail card, the XMB panel and the offline fallback without touching any of them.
    {
        const QString ok1 = QStringLiteral("igdb:24000");
        MetaCache::remove(ok1);
        MetaOverrides::reset(ok1);   // start from a known-clear state (see below: reset is not a deletion)

        // -- the record's ONE canonical spelling ------------------------------------------------------
        // Two devices that made the same correction must produce byte-identical records, or CloudMerge's
        // equal-timestamp tie-break would read incidental whitespace as a content difference and flip one
        // device onto the other's copy for no reason (the #58 lesson, answered at write time instead).
        {
            MetaOverrides::Override padded;
            padded.title = QStringLiteral("  Bonk's Adventure  ");
            padded.subtitle = QStringLiteral("\t1990\n");
            MetaOverrides::Override tight;
            tight.title = QStringLiteral("Bonk's Adventure");
            tight.subtitle = QStringLiteral("1990");
            CHECK(QJsonDocument(MetaOverrides::toJson(padded)).toJson(QJsonDocument::Compact)
                      == QJsonDocument(MetaOverrides::toJson(tight)).toJson(QJsonDocument::Compact),
                  "override: incidental whitespace is trimmed at write, so the record has one spelling");
            // An unset field is ABSENT, never "" — one spelling for "not overridden", so the two can never
            // be compared as different bytes.
            CHECK(!MetaOverrides::toJson(tight).contains(QStringLiteral("overview")),
                  "override: an unset field is absent from the record, not an empty string");
            CHECK(MetaOverrides::toJson(tight).contains(QStringLiteral("title")),
                  "override: a set field is present");
            // The reset husk is a real record (it carries its stamp) that composites as nothing.
            const QJsonObject husk = MetaOverrides::toJson(MetaOverrides::Override{});
            CHECK(husk.contains(QStringLiteral("updatedAt")) && husk.size() == 1,
                  "override: a reset record is a timestamp-only husk");
            CHECK(MetaOverrides::fromJson(husk).isEmpty(), "override: a husk reads back as no override");
            // Same key space, same hash scheme as the other per-item stores — not a fifth scheme.
            CHECK(MetaOverrides::hashKey(ok1)
                      == QString::fromLatin1(QCryptographicHash::hash(ok1.toUtf8(),
                                                                      QCryptographicHash::Md5).toHex()),
                  "override: the item hash is MD5-hex over UTF-8, as ItemMarks uses");
        }

        // -- the pure composite ------------------------------------------------------------------------
        {
            MediaDetail scraped;
            scraped.title    = QStringLiteral("Bonk 3");        // wrong game
            scraped.subtitle = QStringLiteral("1993");
            scraped.overview = QStringLiteral("The wrong synopsis.");
            scraped.imageUrl = QStringLiteral("https://x.invalid/wrong.jpg");
            scraped.art.addImage(QStringLiteral("poster"), QStringLiteral("https://x.invalid/wrong.jpg"));
            scraped.valid = true;

            MetaOverrides::Override ov;
            ov.title = QStringLiteral("Bonk's Adventure");
            ov.image = QStringLiteral("https://x.invalid/right.jpg");

            MediaDetail d = scraped;
            MetaOverrides::applyTo(ov, d);
            CHECK(d.title == QStringLiteral("Bonk's Adventure"), "composite: an overridden field wins");
            CHECK(d.subtitle == QStringLiteral("1993"), "composite: an UNSET field leaves the scrape alone");
            CHECK(d.overview == QStringLiteral("The wrong synopsis."),
                  "composite: an unset overview leaves the scraped one alone");
            CHECK(d.imageUrl == QStringLiteral("https://x.invalid/right.jpg"), "composite: the poster is replaced");
            CHECK(d.art.image(QStringLiteral("poster")) == QStringLiteral("https://x.invalid/right.jpg"),
                  "composite: the corrected image LEADS the poster role, so selected.poster binds to it");
            CHECK(d.art.image(QStringLiteral("thumb")) == QStringLiteral("https://x.invalid/right.jpg"),
                  "composite: …and the thumb role, so the grid tile changes too");
            CHECK(d.art.images.value(QStringLiteral("poster")).size() == 2,
                  "composite: the scraped candidate stays behind it (nothing is thrown away)");

            // An empty override changes nothing at all — the identity case a reset relies on.
            MediaDetail untouched = scraped;
            MetaOverrides::applyTo(MetaOverrides::Override{}, untouched);
            CHECK(untouched.title == scraped.title && untouched.imageUrl == scraped.imageUrl
                      && untouched.art.images.value(QStringLiteral("poster")).size() == 1,
                  "composite: an empty override is a no-op");

            // A MediaItem composites the same way (the grid row reads these three fields directly).
            MediaItem row;
            row.title = QStringLiteral("Bonk 3");
            row.subtitle = QStringLiteral("1993");
            row.thumbnailUrl = QStringLiteral("https://x.invalid/wrong.jpg");
            MetaOverrides::applyTo(ov, row);
            CHECK(row.title == QStringLiteral("Bonk's Adventure"), "composite: MediaItem title");
            CHECK(row.subtitle == QStringLiteral("1993"), "composite: MediaItem unset field untouched");
            CHECK(row.thumbnailUrl == QStringLiteral("https://x.invalid/right.jpg"), "composite: MediaItem thumb");

            // A card with NOTHING scraped but a correction on it is showable — otherwise the one screen
            // where the user could fix a blank item would keep reporting itself as empty.
            MediaDetail blank;
            MetaOverrides::applyTo(ov, blank);
            CHECK(blank.valid, "composite: a corrected-but-unscraped item becomes a valid card");

            // The themed row map a theme binds through. Some surfaces assemble it from a session art cache
            // that never went near MediaArt on this pass, so the composite has to write the scalar role
            // aliases itself — otherwise a corrected poster would show on the detail cover and NOT on the
            // element bound to selected.poster, on the same screen.
            QVariantMap themed;
            scraped.art.writeInto(themed);
            themed.insert(QStringLiteral("title"), scraped.title);
            themed.insert(QStringLiteral("subtitle"), scraped.subtitle);
            themed.insert(QStringLiteral("image"), scraped.imageUrl);
            MetaOverrides::applyTo(ov, themed);
            CHECK(themed.value(QStringLiteral("title")).toString() == QStringLiteral("Bonk's Adventure"),
                  "row map: selected.title takes the correction");
            CHECK(themed.value(QStringLiteral("subtitle")).toString() == QStringLiteral("1993"),
                  "row map: an unset field leaves the scrape alone");
            CHECK(themed.value(QStringLiteral("image")).toString() == QStringLiteral("https://x.invalid/right.jpg"),
                  "row map: selected.image takes the corrected poster");
            CHECK(themed.value(QStringLiteral("poster")).toString() == QStringLiteral("https://x.invalid/right.jpg"),
                  "row map: the selected.poster scalar alias too");
            CHECK(themed.value(QStringLiteral("thumb")).toString() == QStringLiteral("https://x.invalid/right.jpg"),
                  "row map: …and selected.thumb");
            CHECK(themed.value(QStringLiteral("images")).toMap()
                      .value(QStringLiteral("poster")).toStringList().value(0)
                      == QStringLiteral("https://x.invalid/right.jpg"),
                  "row map: the correction leads the poster gallery list");
            QVariantMap untouchedRow{ { QStringLiteral("title"), QStringLiteral("Bonk 3") } };
            MetaOverrides::applyTo(MetaOverrides::Override{}, untouchedRow);
            CHECK(untouchedRow.size() == 1 && untouchedRow.value(QStringLiteral("title")).toString()
                                                  == QStringLiteral("Bonk 3"),
                  "row map: an empty override adds nothing and changes nothing");
        }

        // -- through MetaCache's read primitives, and ACROSS a re-scrape --------------------------------
        {
            MediaItem wrong;
            wrong.id = ok1;
            wrong.title = QStringLiteral("Bonk 3");
            wrong.subtitle = QStringLiteral("1993");
            wrong.thumbnailUrl = QStringLiteral("https://x.invalid/wrong.jpg");
            wrong.type = QStringLiteral("game");
            MetaCache::saveItem(wrong);
            MediaDetail wd;
            wd.valid = true;
            wd.title = QStringLiteral("Bonk 3");
            wd.subtitle = QStringLiteral("1993");
            wd.overview = QStringLiteral("The wrong synopsis.");
            wd.imageUrl = QStringLiteral("https://x.invalid/wrong.jpg");
            wd.art.addImage(QStringLiteral("poster"), QStringLiteral("https://x.invalid/wrong.jpg"));
            MetaCache::saveDetail(ok1, wd);
            // A finished poster download: the WRONG art, cached locally. displayImage would normally serve
            // this in preference to any url, which is exactly why the correction has to outrank it.
            MetaCache::storeImage(ok1, QStringLiteral("thumb"), QStringLiteral("https://x.invalid/wrong.png"),
                                  QStringLiteral("image/png"), QByteArray(64, 'x'));
            CHECK(!MetaCache::imagePath(ok1, QStringLiteral("thumb")).isEmpty(),
                  "fixture: the wrong poster really is cached on disk");
            CHECK(MetaCache::displayImage(ok1, QStringLiteral("https://x.invalid/wrong.jpg"))
                      == MetaCache::imagePath(ok1, QStringLiteral("thumb")),
                  "fixture: without an override the cached file is what gets served");

            MetaOverrides::Override fix;
            fix.title = QStringLiteral("Bonk's Adventure");
            fix.overview = QStringLiteral("The right synopsis.");
            fix.image = QStringLiteral("https://x.invalid/right.jpg");
            MetaOverrides::set(ok1, fix);

            MediaDetail got = MetaCache::cachedDetail(ok1);
            CHECK(got.title == QStringLiteral("Bonk's Adventure"), "cachedDetail composites the correction");
            CHECK(got.overview == QStringLiteral("The right synopsis."), "cachedDetail composites the overview");
            CHECK(got.subtitle == QStringLiteral("1993"), "cachedDetail leaves an uncorrected field scraped");
            CHECK(got.imageUrl == QStringLiteral("https://x.invalid/right.jpg"),
                  "cachedDetail: the correction outranks the locally cached poster");
            CHECK(MetaCache::loadArt(ok1).image(QStringLiteral("poster"))
                      == QStringLiteral("https://x.invalid/right.jpg"),
                  "loadArt composites the correction ahead of the cached file");
            CHECK(MetaCache::displayImage(ok1, QStringLiteral("https://x.invalid/wrong.jpg"))
                      == QStringLiteral("https://x.invalid/right.jpg"),
                  "displayImage: the correction outranks the cached WRONG file (grid tiles change too)");

            // …and offline-first is not suspended for the correction. The corrected poster caches under its
            // OWN role: cacheImage's "already cached" guard reads imagePath(key, role), and thumb/poster
            // hold the WRONG art, so under those roles the corrected poster was never fetched and the one
            // item the user had fixed was the one that rendered as nothing offline.
            const QString fixRole = MetaCache::fixedImageRole(QStringLiteral("https://x.invalid/right.jpg"));
            CHECK(MetaCache::imagePath(ok1, fixRole).isEmpty(),
                  "the wrong art cached under thumb does not answer for the correction's role");
            // A finished download of the CORRECTED poster (storeImage is the same persist path cacheImage
            // ends in, without the network).
            MetaCache::storeImage(ok1, fixRole, QStringLiteral("https://x.invalid/right.jpg"),
                                  QStringLiteral("image/jpeg"), QByteArray(48, 'r'));
            CHECK(!MetaCache::imagePath(ok1, fixRole).isEmpty(),
                  "fixture: the corrected poster really is cached on disk");
            CHECK(MetaCache::displayImage(ok1, QStringLiteral("https://x.invalid/wrong.jpg"))
                      == MetaCache::imagePath(ok1, fixRole),
                  "displayImage serves the CORRECTED poster's cached copy (the fixed item renders offline)");
            // A second correction is a different poster, and must not be served the first one's file.
            MetaOverrides::Override fix2 = MetaOverrides::get(ok1);
            fix2.image = QStringLiteral("https://x.invalid/righter.jpg");
            MetaOverrides::set(ok1, fix2);
            CHECK(MetaCache::displayImage(ok1, QStringLiteral("https://x.invalid/wrong.jpg"))
                      == QStringLiteral("https://x.invalid/righter.jpg"),
                  "a second correction is not served the first correction's cached file");
            MetaOverrides::set(ok1, fix);   // back to the correction the rest of this section asserts against

            // The editor's poster baseline: the same offline-first read with the correction left OFF, so the
            // field shows what it replaces and a retype of the visible value is not stored as an override.
            CHECK(MetaCache::scrapedImage(ok1, QStringLiteral("https://x.invalid/wrong.jpg"))
                      == MetaCache::imagePath(ok1, QStringLiteral("thumb")),
                  "scrapedImage keeps the scraped artwork, correction or not");

            // The editor's baseline. It shows each correction OVER the value it replaces and offers to reset
            // back to it, so it needs the card WITHOUT the override — seeding it from the composited card
            // would present the user's own edit as the thing being overridden, and reset would look like it
            // restored the edit. Every other caller wants the composited one.
            const MediaDetail raw = MetaCache::cachedDetailScraped(ok1);
            CHECK(raw.title == QStringLiteral("Bonk 3"), "cachedDetailScraped keeps the scraped title");
            CHECK(raw.overview == QStringLiteral("The wrong synopsis."),
                  "cachedDetailScraped keeps the scraped overview");
            // Pinned to the cached WRONG file by identity, not merely "not the correction": != would also
            // hold for an EMPTY imageUrl, i.e. for a baseline that lost the target reset is supposed to
            // restore. The reset assertion further down reads the same path, so the two agree.
            CHECK(raw.imageUrl == MetaCache::imagePath(ok1, QStringLiteral("thumb")),
                  "cachedDetailScraped keeps the scraped artwork, so reset has a target");

            // THE POINT OF THE FEATURE: the scraper runs again and writes the wrong data back. The
            // correction must still win — an override a refresh silently discards is worse than none,
            // because the user is never told it happened.
            MetaCache::saveItem(wrong);
            MetaCache::saveDetail(ok1, wd);
            MediaDetail after = MetaCache::cachedDetail(ok1);
            CHECK(after.title == QStringLiteral("Bonk's Adventure"), "a re-scrape does NOT discard the correction");
            CHECK(after.imageUrl == QStringLiteral("https://x.invalid/right.jpg"),
                  "a re-scrape does NOT discard the corrected artwork");
            CHECK(MetaCache::load(ok1).value(QStringLiteral("detail")).toObject()
                      .value(QStringLiteral("title")).toString() == QStringLiteral("Bonk 3"),
                  "the scraped value is still stored underneath, unedited — which is what reset restores");

            // …and clearing ONE field falls back to the scrape for that field only.
            MetaOverrides::Override partial = MetaOverrides::get(ok1);
            partial.overview.clear();
            MetaOverrides::set(ok1, partial);
            MediaDetail mixed = MetaCache::cachedDetail(ok1);
            CHECK(mixed.overview == QStringLiteral("The wrong synopsis."),
                  "clearing one field restores the scrape for that field only");
            CHECK(mixed.title == QStringLiteral("Bonk's Adventure"), "…and leaves the others corrected");

            // -- reset to scraped ----------------------------------------------------------------------
            MetaOverrides::reset(ok1);
            MediaDetail back = MetaCache::cachedDetail(ok1);
            CHECK(back.title == QStringLiteral("Bonk 3"), "reset restores the scraped title");
            CHECK(back.overview == QStringLiteral("The wrong synopsis."), "reset restores the scraped overview");
            CHECK(back.imageUrl == MetaCache::imagePath(ok1, QStringLiteral("thumb")),
                  "reset restores the cached scraped artwork");
            CHECK(!MetaOverrides::has(ok1), "reset leaves nothing overridden");
            MetaCache::remove(ok1);
        }

        // -- miximage is the preferred tile art when a card exists (issue #183) --------------------------
        // scrapedImage() is the host-fed tile-role pick every grid/shelf goes through. The composited card
        // (issue #90) is one more cached role, "miximage"; #183 makes the tile prefer it. The two ends of the
        // rail are asserted here on an item built WITHOUT the compositor — the files and the images-map record
        // are written directly, so these fixtures are not a fixed point of Miximage or of scrapedImage.
        {
            const QString mk = QStringLiteral("mix:tile");
            QDir().mkpath(MetaCache::dirFor(mk));
            // Fall-back-when-absent: only the ordinary tile art is cached, no card. Today's tile stands.
            {
                QFile f(MetaCache::dirFor(mk) + QStringLiteral("/thumb.jpg"));
                CHECK(f.open(QIODevice::WriteOnly), "fixture: can write the tile thumb");
                f.write("thumbbytes");
            }
            MetaCache::merge(mk, { { QStringLiteral("images"),
                                     QJsonObject{ { QStringLiteral("thumb"), QStringLiteral("thumb.jpg") } } } });
            const QString thumbPath = MetaCache::imagePath(mk, QStringLiteral("thumb"));
            CHECK(!thumbPath.isEmpty(), "fixture: the thumb resolves on disk");
            CHECK(MetaCache::imagePath(mk, QStringLiteral("miximage")).isEmpty(),
                  "fixture: no card composited yet");
            CHECK(MetaCache::scrapedImage(mk, QStringLiteral("https://x.invalid/u.jpg")) == thumbPath,
                  "no miximage card -> the tile is exactly today's art (opt-in falls back, no regression)");

            // Prefer-when-present: a card is now on disk under its own role (recordLocalImage's shape, written
            // here without running the compositor). The tile switches to it, over the still-present thumb.
            {
                QFile f(MetaCache::dirFor(mk) + QStringLiteral("/miximage.png"));
                CHECK(f.open(QIODevice::WriteOnly), "fixture: can write the composited card");
                f.write("cardbytes");
            }
            MetaCache::merge(mk, { { QStringLiteral("images"),
                                     QJsonObject{ { QStringLiteral("thumb"), QStringLiteral("thumb.jpg") },
                                                  { QStringLiteral("miximage"), QStringLiteral("miximage.png") } } } });
            const QString cardPath = MetaCache::imagePath(mk, QStringLiteral("miximage"));
            CHECK(!cardPath.isEmpty() && cardPath != thumbPath, "fixture: the card resolves, distinct from the thumb");
            CHECK(MetaCache::scrapedImage(mk, QStringLiteral("https://x.invalid/u.jpg")) == cardPath,
                  "a composited card is the preferred tile art (the uniform shelf), over the thumb");
            // displayImage with no correction rides the same pick -> the grid tile shows the card.
            CHECK(MetaCache::displayImage(mk, QStringLiteral("https://x.invalid/u.jpg")) == cardPath,
                  "displayImage surfaces the card on the grid tile when no correction is in play");

            // A user correction still outranks the auto-composited card: the card can be built from the very
            // art the user is correcting, so the explicit fix must win (the miximage preference lives below the
            // correction, in scrapedImage, not above it).
            MetaOverrides::Override fix;
            fix.image = QStringLiteral("https://x.invalid/corrected.jpg");
            MetaOverrides::set(mk, fix);
            CHECK(MetaCache::displayImage(mk, QStringLiteral("https://x.invalid/u.jpg"))
                      == QStringLiteral("https://x.invalid/corrected.jpg"),
                  "a correction still outranks the composited card (the fix the user made wins)");
            MetaOverrides::reset(mk);
            MetaCache::remove(mk);
        }

        // -- clearAll: the settings-side escape hatch ----------------------------------------------------
        {
            const QString a = QStringLiteral("clear:a"), b = QStringLiteral("clear:b");
            MetaOverrides::Override ov; ov.title = QStringLiteral("x");
            MetaOverrides::set(a, ov);
            MetaOverrides::set(b, ov);
            CHECK(MetaOverrides::count() == 2, "count reports the items carrying a correction");
            MetaOverrides::clearAll();
            CHECK(MetaOverrides::count() == 0, "clearAll resets every corrected item");
            CHECK(!MetaOverrides::has(a) && !MetaOverrides::has(b), "clearAll: nothing is left overridden");
        }

        // An empty key is a safe no-op on every entry point (same contract as the other per-item stores).
        // KILL-MATRIX NOTE: the has() line below takes a COMPOUND mutation, and that is a property of the
        // implementation rather than a weakness here — two independent guards each answer "nothing" for an
        // empty key (set() refuses to write one, get() refuses to look one up), so removing either alone
        // still leaves has() false. Both have to go. The count() line beneath it kills set()'s guard on its
        // own. Same shape as the third-party favourite in probe_cloudmerge section 19; noted so the single
        // compound entry in the matrix is not read as an oversight.
        MetaOverrides::Override any; any.title = QStringLiteral("nope");
        MetaOverrides::set(QString(), any);
        MetaOverrides::reset(QString());
        CHECK(!MetaOverrides::has(QString()), "an item with no identity can carry no override");
        CHECK(MetaOverrides::count() == 0, "…and storing under an empty key writes nothing");

        // -- the editor's baseline snapshot, keyed (src/core/ScrapedSnapshot.h) --------------------------
        // The editor corrects an item against what the PROVIDERS said, and the live /meta reply is richer
        // than the cache — so the open card's own reply is held for it. That reply is written only when one
        // ARRIVES: an item whose addon returns nothing (offline, or gone upstream) writes none, and a bare
        // member would still be holding the PREVIOUS item's card. The editor, opened on this item's key,
        // then seeded from another item's title/synopsis/poster, compared "typed back what the scraper
        // found" against it, and wrote that content into THIS item's override — which syncs everywhere.
        //
        // The surfaces that use this are Qt Widgets/QML classes no headless probe links; the rule they rest
        // on is pure, so it is asserted here, and the source gate in run-headless-probes.sh pins those
        // surfaces to it rather than to a member they could read unkeyed again.
        {
            MediaDetail a;
            a.valid = true;
            a.title = QStringLiteral("Item A");
            a.overview = QStringLiteral("A's synopsis.");
            a.imageUrl = QStringLiteral("https://x.invalid/a.jpg");

            MetaEdit::ScrapedSnapshot snap;
            snap.remember(QStringLiteral("A"), a);
            CHECK(snap.forKey(QStringLiteral("A")).title == QStringLiteral("Item A"),
                  "the snapshot answers for the item it was taken for");
            CHECK(snap.forKey(QStringLiteral("A")).overview == QStringLiteral("A's synopsis."),
                  "…with the whole provider card, not just its title");
            // THE CRITICAL ONE: open A online, go back, open B whose addon returns nothing. B's editor must
            // NOT be handed A's card — committing a field there writes A's content into B's override.
            CHECK(!snap.forKey(QStringLiteral("B")).valid,
                  "another item's card is never the baseline (the correction cannot cross items)");
            CHECK(snap.forKey(QStringLiteral("B")).title.isEmpty(),
                  "…and nothing of it leaks through field by field either");
            // One slot PER SURFACE, and the slot is instance state. The classic detail card and the themed
            // detail card each hold their own; if the storage were shared, the card the user is not looking
            // at would supply the baseline for the one they are — the same leak across surfaces instead of
            // across items.
            MetaEdit::ScrapedSnapshot other;
            CHECK(!other.forKey(QStringLiteral("A")).valid,
                  "a second surface's snapshot is its own slot, not a shared one");
            // Moving on to an item that DOES answer re-stamps the snapshot; A's card is gone with it.
            MediaDetail b;
            b.valid = true;
            b.title = QStringLiteral("Item B");
            snap.remember(QStringLiteral("B"), b);
            CHECK(snap.forKey(QStringLiteral("B")).title == QStringLiteral("Item B"),
                  "the newest provider answer is the one held");
            CHECK(!snap.forKey(QStringLiteral("A")).valid, "…and the previous item's is no longer readable");
            // An item with no identity (keyFor() gave nothing) owns no snapshot — otherwise the next
            // identity-less card would read this one's answer back out.
            snap.remember(QString(), a);
            CHECK(!snap.forKey(QString()).valid, "an item with no identity carries no snapshot");
            CHECK(!snap.forKey(QStringLiteral("B")).valid,
                  "…and storing under no key drops what was held rather than leaving it addressable");
        }

        std::printf("OVERRIDE-OK\n");
    }

    // ---------------------------------------------------------------- items without a stable identity
    CHECK(MetaCache::keyFor(MediaItem{}).isEmpty(), "no id and no url -> no key");
    MetaCache::merge(QString(), { { QStringLiteral("x"), 1 } }); // must be a safe no-op
    CHECK(MetaCache::load(QString()).isEmpty(), "empty key never stores anything");

    // ---------------------------------------------------------------- uninstall cleanup
    MetaCache::remove(key);
    CHECK(MetaCache::load(key).isEmpty(), "remove deletes the bundle");
    CHECK(!QDir(MetaCache::dirFor(key)).exists(), "remove deletes the folder (artwork included)");
    if (failures) { std::fprintf(stderr, "META-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("META-OK\n");
    return 0;
}
