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
#include "CoverFetch.h"
#include "MetaCache.h"
#include "MetaOverrides.h"
#include "ScrapedSnapshot.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdio>
#include <initializer_list>
#include <memory>

static int failures = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "META-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

// ---- #387 fixtures: a loopback art host this probe starts itself, and the bytes it serves ----------------------
namespace
{
QByteArray bytesOf(std::initializer_list<int> v)
{
    QByteArray b;
    for (int c : v) b.append(char(c));
    return b;
}

// What a reverse proxy, captive portal or login page answers with 200 - served below as "image/jpeg", because the
// header is exactly the part a misbehaving proxy gets wrong.
const QByteArray kHtmlPage = QByteArrayLiteral(
    "<!DOCTYPE html>\n<html><head><title>502 Bad Gateway</title></head>\n"
    "<body><h1>502 Bad Gateway</h1><p>The upstream server did not answer in time.</p></body></html>\n");

// fmt, the extension its url carries, the content type it is served with, its bytes, and the extension that
// content type alone picks for a url with none (MetaCache.cpp, imageExt).
// Bodies that must never be stored as art. The page is what a proxy says; the other two start with a new raster
// signature and are still not pictures: an ICO header that declares ZERO images, and text that starts with "BM".
QByteArray zeroCountIco()
{
    QByteArray b = QByteArray::fromHex("000001000000") + QByteArray(64, char(0x5a));
    return b;
}
const QByteArray kBmText = QByteArrayLiteral("BMW 3 Series review: the sports sedan, measured and driven\n");

struct Picture { QString fmt; QString ext; QByteArray ctype; QByteArray bytes; QString ctypeExt; };
const QVector<Picture>& pictures()
{
    static const QVector<Picture> all = {
        { QStringLiteral("jpeg"), QStringLiteral("jpg"), "image/jpeg",
          bytesOf({ 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10 }) + "JFIF" + bytesOf({ 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
                                                                                0x00, 0x01, 0x00, 0x00, 0xFF, 0xD9 }), QStringLiteral("jpg") },
        { QStringLiteral("png"), QStringLiteral("png"), "image/png",
          bytesOf({ 0x89 }) + "PNG" + bytesOf({ 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D }) + "IHDR"
              + bytesOf({ 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00 }), QStringLiteral("png") },
        { QStringLiteral("webp"), QStringLiteral("webp"), "image/webp",
          QByteArray("RIFF") + bytesOf({ 0x1A, 0x00, 0x00, 0x00 }) + "WEBPVP8L"
              + bytesOf({ 0x0D, 0x00, 0x00, 0x00, 0x2F, 0x00, 0x00, 0x00, 0x10, 0x07, 0x10, 0x11, 0x11 }), QStringLiteral("webp") },
        { QStringLiteral("gif"), QStringLiteral("gif"), "image/gif",
          QByteArray("GIF89a") + bytesOf({ 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                                           0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02,
                                           0x02, 0x44, 0x01, 0x00, 0x3B }), QStringLiteral("gif") },
        { QStringLiteral("svg"), QStringLiteral("svg"), "image/svg+xml",
          QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- Generator: probe fixture -->\n"
                            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"3\">"
                            "<rect width=\"2\" height=\"3\" fill=\"#c33\"/></svg>\n"), QStringLiteral("svg") },
        // The formats Qt decodes that the classic grid could store as a thumb (#387 follow-up): each a 2x2 image
        // written and read back by an independent encoder (Pillow), hex so no escape reaches a CHECK.
        { QStringLiteral("bmp"), QStringLiteral("bmp"), "image/bmp", QByteArray::fromHex(
              "424d460000000000000036000000280000000200000002000000010018000000000010000000c40e0000c40e0000"
              "00000000000000002828c82828c800002828c82828c80000"), QStringLiteral("bmp") },
        { QStringLiteral("ico"), QStringLiteral("ico"), "image/x-icon", QByteArray::fromHex(
              "00000100010002020000000018003a00000016000000280000000200000004000000010018000000000010000000"
              "c40e0000c40e000000000000000000002828c82828c800002828c82828c800000000"), QStringLiteral("ico") },
        { QStringLiteral("cur"), QStringLiteral("cur"), "image/vnd.microsoft.icon", QByteArray::fromHex(
              "00000200010002020000010001003a00000016000000280000000200000004000000010018000000000010000000"
              "c40e0000c40e000000000000000000002828c82828c800002828c82828c800000000"), QStringLiteral("ico") },
        { QStringLiteral("tiff-le"), QStringLiteral("tiff"), "image/tiff", QByteArray::fromHex(
              "49492a00080000000a0000010400010000000200000001010400010000000200000002010300030000008600000003"
              "010300010000000100000006010300010000000200000011010400010000008c0000001501030001000000030000"
              "0016010400010000000200000017010400010000000c0000001c0103000100000001000000000000000800080008"
              "00c82828c82828c82828c82828"), QStringLiteral("tif") },
        { QStringLiteral("tiff-be"), QStringLiteral("tif"), "image/tiff", QByteArray::fromHex(
              "4d4d002a00000008000901000003000000010002000001010003000000010002000001020003000000030000007a"
              "0103000300000001000100000106000300000001000200000111000400000001000000800115000300000001000300"
              "000116000300000001000200000117000400000001000000"
              "0c00000000000800080008c82828c82828c82828c82828"), QStringLiteral("tif") },
    };
    return all;
}

// #389: formats Qt DECODES that the cache does not hold - text (PBM/PGM/PPM, XBM, XPM) or signature-less (TGA). Each a
// real 2x2 image: the binary ones written and read back by Pillow 12.3, the text ones Pillow reads back too. Hex or
// plain ASCII, so no escape reaches a CHECK. The extension and content type are the ones a host would serve them with.
struct Decodable { QString fmt; QString ext; QByteArray ctype; QByteArray bytes; };
const QVector<Decodable>& decodableNotPictures()
{
    static const QVector<Decodable> all = {
        { QStringLiteral("ppm"), QStringLiteral("ppm"), "image/x-portable-pixmap",
          QByteArray::fromHex("50360a3220320a3235350ac828282828c82828c8c82828") },
        { QStringLiteral("pgm"), QStringLiteral("pgm"), "image/x-portable-graymap",
          QByteArray::fromHex("50350a3220320a3235350a583a3a58") },
        { QStringLiteral("pbm"), QStringLiteral("pbm"), "image/x-portable-bitmap", QByteArrayLiteral("P1\n2 2\n0 1\n1 0\n") },
        { QStringLiteral("xbm"), QStringLiteral("xbm"), "image/x-xbitmap",
          QByteArrayLiteral("#define im_width 2\n#define im_height 2\nstatic char im_bits[] = {\n0x00,0x02\n};\n") },
        { QStringLiteral("xpm"), QStringLiteral("xpm"), "image/x-xpixmap",
          QByteArrayLiteral("/* XPM */\nstatic char * probe_xpm[] = {\n\"2 2 2 1\",\n\"  c #C82828\",\n\". c #2828C8\",\n"
                            "\" .\",\n\". \"};\n") },
        // Uncompressed true-colour TGA: it opens 00 00 02 00, which is CUR's signature - and a CUR count of zero.
        { QStringLiteral("tga"), QStringLiteral("tga"), "image/x-tga", QByteArray::fromHex(
              "000002000000000000000000020002001800c828282828c82828c8c82828000000000000000054525545564953494f4e2d5846494c452e00") },
    };
    return all;
}

// Serves exactly the routes it is given and counts every request by path. Anything unrouted is a 404.
struct ArtHost
{
    QTcpServer srv;
    QHash<QByteArray, QPair<QByteArray, QByteArray>> routes;   // path -> (content type, body)
    QHash<QByteArray, int> served;

    bool start()
    {
        if (!srv.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            while (QTcpSocket* c = srv.nextPendingConnection())
            {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, buf] {
                    buf->append(c->readAll());
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray path = buf->left(end).split('\n').value(0).trimmed().split(' ').value(1);
                    buf->clear();
                    served[path] += 1;
                    const auto hit = routes.constFind(path);
                    QByteArray resp;
                    if (hit == routes.constEnd())
                        resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    else
                        resp = "HTTP/1.1 200 OK\r\nContent-Type: " + hit->first + "\r\nContent-Length: "
                               + QByteArray::number(hit->second.size()) + "\r\nConnection: close\r\n\r\n"
                               + hit->second;
                    c->write(resp);
                    c->flush();
                    c->disconnectFromHost();
                });
                QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
            }
        });
        return true;
    }
    QString route(const QByteArray& path, const QByteArray& ctype, const QByteArray& body)
    {
        routes.insert(path, { ctype, body });
        return QStringLiteral("http://127.0.0.1:%1%2").arg(srv.serverPort()).arg(QString::fromLatin1(path));
    }
    int count(const QByteArray& path) const { return served.value(path); }
};

// Pump the event loop until `done` holds, bounded. Callers assert on what is on disk and what was served
// afterwards - never on how long it took.
template <typename F> bool pumpUntil(F done, int boundMs = 8000)
{
    QElapsedTimer t;
    t.start();
    while (!done())
    {
        if (t.elapsed() > boundMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QByteArray readAllOf(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// The file an item's "images" record names for a role, whether or not it exists - "" when there is no record.
QString recordedFile(const QString& key, const QString& role)
{
    const QString file = MetaCache::load(key).value(QStringLiteral("images")).toObject().value(role).toString();
    return file.isEmpty() ? QString() : MetaCache::dirFor(key) + QLatin1Char('/') + file;
}

// How many files of this role are in the item's folder (<role>.<anything>).
int roleFiles(const QString& key, const QString& role)
{
    return int(QDir(MetaCache::dirFor(key)).entryList({ role + QStringLiteral(".*") }, QDir::Files).size());
}

QJsonObject bundleWithoutImages(const QString& key)
{
    QJsonObject o = MetaCache::load(key);
    o.remove(QStringLiteral("images"));
    return o;
}
} // namespace

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
        f.write(pictures()[0].bytes);   // a JPEG by its bytes: the readers judge a stored file by them (#387)
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
        { QFile f(MetaCache::dirFor(akey) + QStringLiteral("/logo.png")); f.open(QIODevice::WriteOnly); f.write(pictures()[1].bytes); }
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
                                  QStringLiteral("image/png"), pictures()[1].bytes);
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
                                  QStringLiteral("image/jpeg"), pictures()[0].bytes);
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
                f.write(pictures()[0].bytes);
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

    // ================================================================ #387: the general image cache holds only
    // pictures, and heals what it held badly. cacheImage used to store ANY successful body - a proxy's 200 error page
    // under poster.jpg - and its "already cached" guard then kept it for good (the cap sweep evicts only thumb.*).
    // Everything here is served by a loopback host this probe starts; request counts and stored bytes are the
    // evidence, never how long anything took.
    // -- the pure bytes rule the gate and the read-back share (CoverFetch::isPicture / storedCoverIntact) ---------
    {
        for (const Picture& pic : pictures())
        {
            const QByteArray is = QStringLiteral("387 rule: a %1 is a picture").arg(pic.fmt).toLatin1();
            CHECK(CoverFetch::isPicture(pic.bytes), is.constData());
            CHECK(CoverFetch::storedCoverIntact(pic.bytes, pic.bytes.size()), "387 rule: the whole file is intact");
            if (pic.fmt == QStringLiteral("svg")) continue;   // text: decided from the longer prefix (#382)
            // A raster signature decides from the SIGNATURE prefix alone - the read verifiedImagePath makes first.
            const QByteArray head = pic.bytes.left(CoverFetch::kSignatureBytes);
            const QByteArray alone = QStringLiteral("387 rule: a %1 is decided by its signature prefix").arg(pic.fmt).toLatin1();
            CHECK(CoverFetch::isPicture(head), alone.constData());
            CHECK(CoverFetch::storedCoverIntact(head, 10 * 1024 * 1024), alone.constData());
        }
        CHECK(CoverFetch::kSignatureBytes <= 64, "387 rule: the signature prefix stays a few bytes");

        // Starts like a new signature, is not a picture.
        auto fixture = [](const char* fmt) {
            for (const Picture& pic : pictures()) if (pic.fmt == QLatin1String(fmt)) return pic.bytes;
            return QByteArray();
        };
        const QByteArray bmp = fixture("bmp"), ico = fixture("ico"), cur = fixture("cur");
        const QByteArray tiffLe = fixture("tiff-le"), tiffBe = fixture("tiff-be");
        CHECK(bmp.startsWith("BM") && ico.size() > 6 && cur.size() > 6 && tiffLe.startsWith("II") && tiffBe.startsWith("MM"),
              "387 rule: fixture - the variants start from the right files");
        QByteArray icoZero = ico;  icoZero[4] = 0; icoZero[5] = 0;      // declares no images
        QByteArray curZero = cur;  curZero[4] = 0; curZero[5] = 0;
        QByteArray bmpDib = bmp;   bmpDib[14] = char(0x99);             // no DIB header has that size
        QByteArray tiffOff = tiffLe; tiffOff[4] = 4;                    // the first IFD inside the 8-byte header
        const QVector<QPair<const char*, QByteArray>> refused = {
            { "ico with zero images", icoZero }, { "cur with zero images", curZero },
            { "the zero-count ico blob", zeroCountIco() },
            { "a bare ico signature", QByteArray::fromHex("00000100") },
            { "an ico header cut before its count", QByteArray::fromHex("0000010001") },
            { "bmp with an unknown DIB header size", bmpDib },
            { "a bmp file header with no DIB header", bmp.left(14) },
            { "bare BM", QByteArrayLiteral("BM") },
            { "text starting BM", kBmText },
            { "a tiff whose first IFD is inside its header", tiffOff },
            { "a bare little-endian tiff signature", tiffLe.left(4) },
            { "a bare big-endian tiff signature", tiffBe.left(4) },
            { "an html page", kHtmlPage },
            { "an xml error", QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error><Code>AccessDenied</Code>"
                                                "<Message>Access Denied</Message></Error>") },
            { "json", QByteArrayLiteral("{\"error\":\"Unauthorized\",\"statusCode\":401}") },
            { "plain text", QByteArrayLiteral("Bad Gateway") },
            { "empty", QByteArray() },
        };
        for (const auto& r : refused)
        {
            const QByteArray what = QByteArray("387 rule: not a picture - ") + r.first;
            CHECK(!CoverFetch::isPicture(r.second), what.constData());
            CHECK(!CoverFetch::storedCoverIntact(r.second, r.second.size()), what.constData());
        }
        // NO TEXT BODY PASSES. Every new signature holds a NUL byte (ICO, CUR, TIFF) or needs a DIB size with NULs in it
        // (BMP), and text has none - pinned over EVERY two-character printable start, with a tail that also reads like
        // the rest of a header ("*", digits): "BM", "II*", "MM", all of them.
        int textPassed = 0;
        for (int c1 = 0x20; c1 < 0x7f; ++c1)
            for (int c2 = 0x20; c2 < 0x7f; ++c2)
            {
                QByteArray t;
                t.append(char(c1)).append(char(c2));
                t += "*\t0*00 1 (plain text, a header's worth of it) 0123456789 abcdefghijklmnopqrstuvwxyz\r\n";
                if (CoverFetch::isPicture(t)) ++textPassed;
            }
        CHECK(textPassed == 0, "387 rule: no text body is ever a picture, whatever its first two characters");
    }

    {
        ArtHost http;
        CHECK(http.start(), "387: the loopback art host listens");
        const QStringList roles = { QStringLiteral("poster"), QStringLiteral("logo"), QStringLiteral("box"),
                                    QStringLiteral("fanart"), QStringLiteral("screenshot"), QStringLiteral("thumb") };
        auto art = [](const QString& role, const QString& url) { MediaArt a; a.addImage(role, url); return a; };
        // Records the art bundle WITHOUT saveArt's prefetch, so a reader can be asked before anything is fetched.
        auto recordArt = [&](const QString& key, const QString& role, const QString& url) {
            MetaCache::merge(key, { { QStringLiteral("art"), QJsonObject::fromVariantMap(art(role, url).toVariant()) } });
        };
        // A bounded settle: one more genuine picture, requested after everything above, lands. Anything the calls
        // above asked the host for has reached it by then.
        int settles = 0;
        auto settle = [&] {
            const QString key = QStringLiteral("387:settle:%1").arg(++settles);
            MetaCache::remove(key);
            const QByteArray path = "/settle/" + QByteArray::number(settles) + ".png";
            MetaCache::cacheImage(key, QStringLiteral("poster"), http.route(path, "image/png", pictures()[1].bytes));
            const bool landed = pumpUntil([&] { return readAllOf(recordedFile(key, QStringLiteral("poster")))
                                                           == pictures()[1].bytes; });
            MetaCache::remove(key);
            return landed;
        };

        struct NotPicture { QByteArray tag; QByteArray body; };
        const QVector<NotPicture> notPictures = { { "page", kHtmlPage }, { "ico0", zeroCountIco() }, { "bmtext", kBmText } };

        // -- 1. a 200 page is NOT stored, whatever it is labelled --------------------------------------------------
        for (const NotPicture& np : notPictures)
        for (const QString& role : roles)
        {
            const QString key = QStringLiteral("387:gate:") + QString::fromLatin1(np.tag) + QLatin1Char(':') + role;
            MetaCache::remove(key);
            const QByteArray path = "/gate/" + np.tag + "/" + role.toLatin1() + ".jpg";
            const QString url = http.route(path, "image/jpeg", np.body);
            MetaCache::saveArt(key, art(role, url));              // the real prefetch path: saveArt -> cacheImage
            CHECK(pumpUntil([&] { return http.count(path) == 1; }), "387 gate: the page is requested once");
            // The only proof its reply FINISHED without storing: the same art may be asked for again. Stored, the
            // "already cached" guard would refuse every later ask and the count would stay at 1.
            CHECK(pumpUntil([&] { MetaCache::cacheImage(key, role, url); return http.count(path) >= 2; }),
                  "387 gate: a 200 page is not stored - the same art is asked for again");
            CHECK(roleFiles(key, role) == 0, "387 gate: no file of that role is written");
            CHECK(recordedFile(key, role).isEmpty(), "387 gate: no images entry is recorded");
            CHECK(MetaCache::loadArt(key).image(role) == url, "387 gate: loadArt still offers the url");
            CHECK(settle(), "387 gate: settle");
            CHECK(roleFiles(key, role) == 0, "387 gate: ...and the second answer was not stored either");
            MetaCache::remove(key);
        }

        // -- 2. a page ALREADY stored is removed and replaced by the next fetch -------------------------------------
        for (const NotPicture& np : notPictures)
        for (const QString& role : roles)
        {
            const QString key = QStringLiteral("387:heal:") + QString::fromLatin1(np.tag) + QLatin1Char(':') + role;
            MetaCache::remove(key);
            const QByteArray path = "/heal/" + np.tag + "/" + role.toLatin1() + ".png";
            const QString url = http.route(path, "image/png", pictures()[1].bytes);
            // What a build before this fix wrote (storeImage never looked at bytes, and still does not).
            MetaCache::storeImage(key, role, QStringLiteral("https://x.invalid/") + role + QStringLiteral(".jpg"),
                                  QStringLiteral("image/jpeg"), np.body);
            const QString planted = recordedFile(key, role);
            CHECK(!planted.isEmpty() && readAllOf(planted) == np.body, "387 heal: fixture - the page is stored");
            MetaCache::saveArt(key, art(role, url));
            CHECK(pumpUntil([&] { return readAllOf(recordedFile(key, role)) == pictures()[1].bytes; }),
                  "387 heal: the next fetch replaces a stored page with the real picture");
            CHECK(http.count(path) == 1, "387 heal: ...in exactly one request");
            CHECK(!QFileInfo::exists(planted), "387 heal: the page file is gone");
            CHECK(roleFiles(key, role) == 1, "387 heal: one file of that role is left");
            for (int i = 0; i < 3; ++i) MetaCache::cacheImage(key, role, url);
            CHECK(settle(), "387 heal: settle");
            CHECK(http.count(path) == 1, "387 heal: the healed picture is not fetched again");
            MetaCache::remove(key);
        }

        // -- 3. a stored page is never SHOWN: every reader counts it as not cached, and asks the host for nothing ---
        struct Reader { QString name; QStringList roles; };
        const QVector<Reader> readers = {
            { QStringLiteral("loadArt"), roles },
            { QStringLiteral("cachedDetailScraped"), { QStringLiteral("poster"), QStringLiteral("thumb") } },
            { QStringLiteral("cachedDetail"), { QStringLiteral("poster"), QStringLiteral("thumb") } },
            { QStringLiteral("scrapedImage"), { QStringLiteral("poster"), QStringLiteral("thumb") } },
            { QStringLiteral("displayImage"), { QStringLiteral("poster"), QStringLiteral("thumb") } },
        };
        for (const Reader& reader : readers)
            for (const QString& role : reader.roles)
            {
                const QString key = QStringLiteral("387:shown:%1:%2").arg(reader.name, role);
                MetaCache::remove(key);
                const QByteArray path = "/shown/" + reader.name.toLatin1() + "/" + role.toLatin1() + ".gif";
                const QString url = http.route(path, "image/gif", pictures()[3].bytes);
                MetaCache::merge(key, { { QStringLiteral("item"),
                                          QJsonObject{ { QStringLiteral("title"), QStringLiteral("Shown") },
                                                       { QStringLiteral("thumbnailUrl"), url } } } });
                recordArt(key, role, url);
                MetaCache::storeImage(key, role, QStringLiteral("https://x.invalid/p.jpg"), QStringLiteral("image/jpeg"),
                                      kHtmlPage);
                const QString planted = recordedFile(key, role);
                CHECK(QFileInfo::exists(planted), "387 shown: fixture - the page is stored");
                const QJsonObject restBefore = bundleWithoutImages(key);

                QString got;
                if (reader.name == QStringLiteral("loadArt")) got = MetaCache::loadArt(key).image(role);
                else if (reader.name == QStringLiteral("cachedDetailScraped")) got = MetaCache::cachedDetailScraped(key).imageUrl;
                else if (reader.name == QStringLiteral("cachedDetail")) got = MetaCache::cachedDetail(key).imageUrl;
                else if (reader.name == QStringLiteral("scrapedImage")) got = MetaCache::scrapedImage(key, url);
                else got = MetaCache::displayImage(key, url);
                const QByteArray what = QStringLiteral("387 shown: %1(%2) hands out the url, never the stored page")
                                            .arg(reader.name, role).toLatin1();
                CHECK(got == url, what.constData());
                CHECK(!QFileInfo::exists(planted), "387 shown: the page file is removed");
                CHECK(recordedFile(key, role).isEmpty(), "387 shown: its images entry is dropped");
                CHECK(bundleWithoutImages(key) == restBefore, "387 shown: nothing else in the bundle changes");
                CHECK(settle(), "387 shown: settle");
                CHECK(http.count(path) == 0, "387 shown: a reader asks the host for nothing");
                MetaCache::cacheImage(key, role, url);
                CHECK(pumpUntil([&] { return readAllOf(recordedFile(key, role)) == pictures()[3].bytes; }),
                      "387 shown: then the normal fetch stores the real picture");
                CHECK(http.count(path) == 1, "387 shown: ...in one request");
                MetaCache::remove(key);
            }

        // -- 4. the corrected poster's own role (fix-*), through displayImage ---------------------------------------
        {
            const QString key = QStringLiteral("387:fix");
            MetaCache::remove(key);
            const QByteArray path = "/fix/right.webp";
            const QString fixUrl = http.route(path, "image/webp", pictures()[2].bytes);
            MetaOverrides::Override ov;
            ov.image = fixUrl;
            MetaOverrides::set(key, ov);
            const QString role = MetaCache::fixedImageRole(fixUrl);
            MetaCache::storeImage(key, role, fixUrl, QStringLiteral("image/jpeg"), kHtmlPage);
            const QString planted = recordedFile(key, role);
            CHECK(QFileInfo::exists(planted), "387 fix: fixture - a page is stored under the correction's role");
            CHECK(MetaCache::displayImage(key, QStringLiteral("https://x.invalid/tile.jpg")) == fixUrl,
                  "387 fix: displayImage never serves the stored page for a correction");
            CHECK(!QFileInfo::exists(planted), "387 fix: the page file is removed");
            CHECK(pumpUntil([&] { return readAllOf(recordedFile(key, role)) == pictures()[2].bytes; }),
                  "387 fix: ...and the corrected poster is fetched in its place");
            CHECK(http.count(path) == 1, "387 fix: in one request");
            CHECK(MetaCache::displayImage(key, QStringLiteral("https://x.invalid/tile.jpg")) == recordedFile(key, role),
                  "387 fix: then the cached corrected poster is what is served");
            CHECK(settle(), "387 fix: settle");
            CHECK(http.count(path) == 1, "387 fix: and it is not fetched again");
            MetaOverrides::reset(key);
            MetaCache::remove(key);
        }

        // -- 5. a GENUINE picture in every role and format is never removed and never fetched again -----------------
        // Both ways a picture gets there: fetched by cacheImage (so the gate lets every format through), and already
        // on disk from an earlier session (so the read-back never removes one).
        for (const Picture& pic : pictures())
            for (const QString& role : roles)
            {
                const QString tag = pic.fmt + QLatin1Char(':') + role;
                // (a) fetched
                {
                    const QString key = QStringLiteral("387:fetch:") + tag;
                    MetaCache::remove(key);
                    const QByteArray path = "/fetch/" + pic.fmt.toLatin1() + "/" + role.toLatin1() + "." + pic.ext.toLatin1();
                    const QString url = http.route(path, pic.ctype, pic.bytes);
                    MetaCache::saveArt(key, art(role, url));
                    const QByteArray landed = QStringLiteral("387 keep: a fetched %1 %2 is stored").arg(pic.fmt, role).toLatin1();
                    CHECK(pumpUntil([&] { return readAllOf(recordedFile(key, role)) == pic.bytes; }), landed.constData());
                    const QByteArray named = QStringLiteral("387 keep: a fetched %1 %2 lands as %2.%3").arg(pic.fmt, role, pic.ext).toLatin1();
                    CHECK(QFileInfo(recordedFile(key, role)).fileName() == role + QLatin1Char('.') + pic.ext, named.constData());
                    CHECK(MetaCache::verifiedImagePath(key, role) == recordedFile(key, role),
                          "387 keep: the fetched picture reads back through the bytes check");
                    for (int i = 0; i < 4; ++i)
                    {
                        MetaCache::cacheImage(key, role, url);
                        MetaCache::saveArt(key, art(role, url));
                        CHECK(MetaCache::loadArt(key).image(role) == recordedFile(key, role),
                              "387 keep: loadArt serves the fetched picture");
                    }
                    CHECK(settle(), "387 keep: settle");
                    const QByteArray once = QStringLiteral("387 keep: a fetched %1 %2 is requested exactly once").arg(pic.fmt, role).toLatin1();
                    CHECK(http.count(path) == 1, once.constData());
                    CHECK(readAllOf(recordedFile(key, role)) == pic.bytes && roleFiles(key, role) == 1,
                          "387 keep: the fetched picture's bytes are unchanged");
                    MetaCache::remove(key);
                }
                // (b) already on disk
                {
                    const QString key = QStringLiteral("387:kept:") + tag;
                    MetaCache::remove(key);
                    const QByteArray path = "/kept/" + pic.fmt.toLatin1() + "/" + role.toLatin1() + "." + pic.ext.toLatin1();
                    const QString url = http.route(path, pic.ctype, pic.bytes);
                    MetaCache::merge(key, { { QStringLiteral("item"),
                                              QJsonObject{ { QStringLiteral("title"), QStringLiteral("Kept") },
                                                           { QStringLiteral("thumbnailUrl"), url } } } });
                    MetaCache::storeImage(key, role, url, QString::fromLatin1(pic.ctype), pic.bytes);
                    const QString stored = recordedFile(key, role);
                    CHECK(readAllOf(stored) == pic.bytes, "387 kept: fixture - the picture is on disk");
                    for (int i = 0; i < 4; ++i)
                    {
                        MetaCache::saveArt(key, art(role, url));
                        MetaCache::cacheImage(key, role, url);
                        const QByteArray served = QStringLiteral("387 kept: loadArt serves the stored %1 %2").arg(pic.fmt, role).toLatin1();
                        CHECK(MetaCache::loadArt(key).image(role) == stored, served.constData());
                        if (role == QStringLiteral("poster") || role == QStringLiteral("thumb"))
                        {
                            CHECK(MetaCache::cachedDetailScraped(key).imageUrl == stored, "387 kept: cachedDetailScraped serves it");
                            CHECK(MetaCache::scrapedImage(key, url) == stored, "387 kept: scrapedImage serves it");
                            CHECK(MetaCache::displayImage(key, url) == stored, "387 kept: displayImage serves it");
                        }
                    }
                    CHECK(settle(), "387 kept: settle");
                    const QByteArray never = QStringLiteral("387 kept: a stored %1 %2 is never fetched again").arg(pic.fmt, role).toLatin1();
                    CHECK(http.count(path) == 0, never.constData());
                    CHECK(readAllOf(stored) == pic.bytes && roleFiles(key, role) == 1, "387 kept: bytes unchanged, one file");
                    MetaCache::remove(key);
                }
            }
        // A url with NO extension: the content type alone names the file, and it reads back. One role is enough -
        // imageExt does not look at the role.
        for (const Picture& pic : pictures())
        {
            const QString key = QStringLiteral("387:ctype:") + pic.fmt;
            MetaCache::remove(key);
            const QByteArray path = "/ctype/" + pic.fmt.toLatin1();
            const QString url = http.route(path, pic.ctype, pic.bytes);
            MetaCache::cacheImage(key, QStringLiteral("thumb"), url);
            const QByteArray landed = QStringLiteral("387 ctype: a %1 served as %2 is stored").arg(pic.fmt, QString::fromLatin1(pic.ctype)).toLatin1();
            CHECK(pumpUntil([&] { return readAllOf(recordedFile(key, QStringLiteral("thumb"))) == pic.bytes; }), landed.constData());
            const QByteArray named = QStringLiteral("387 ctype: ...as thumb.%1").arg(pic.ctypeExt).toLatin1();
            CHECK(QFileInfo(recordedFile(key, QStringLiteral("thumb"))).fileName() == QStringLiteral("thumb.") + pic.ctypeExt,
                  named.constData());
            CHECK(MetaCache::scrapedImage(key, url) == recordedFile(key, QStringLiteral("thumb")),
                  "387 ctype: and it is what the tile is served");
            CHECK(settle(), "387 ctype: settle");
            CHECK(http.count(path) == 1, "387 ctype: in one request");
            MetaCache::remove(key);
        }

        // ...and the corrected poster's role, in every format.
        for (const Picture& pic : pictures())
        {
            const QString key = QStringLiteral("387:keptfix:") + pic.fmt;
            MetaCache::remove(key);
            const QByteArray path = "/keptfix/right." + pic.ext.toLatin1();
            const QString fixUrl = http.route(path, pic.ctype, pic.bytes);
            MetaOverrides::Override ov;
            ov.image = fixUrl;
            MetaOverrides::set(key, ov);
            const QString role = MetaCache::fixedImageRole(fixUrl);
            MetaCache::storeImage(key, role, fixUrl, QString::fromLatin1(pic.ctype), pic.bytes);
            const QString stored = recordedFile(key, role);
            for (int i = 0; i < 4; ++i)
                CHECK(MetaCache::displayImage(key, QStringLiteral("https://x.invalid/tile.jpg")) == stored,
                      "387 kept fix: displayImage serves the stored corrected poster");
            CHECK(settle(), "387 kept fix: settle");
            const QByteArray never = QStringLiteral("387 kept fix: a stored %1 correction is never fetched again").arg(pic.fmt).toLatin1();
            CHECK(http.count(path) == 0, never.constData());
            CHECK(readAllOf(stored) == pic.bytes, "387 kept fix: bytes unchanged");
            MetaOverrides::reset(key);
            MetaCache::remove(key);
        }
        std::printf("IMAGEGATE-OK\n");

        // ============================================================ #389: the classic grid caches a thumb only if the
        // cache will keep it. HomeView::pumpThumbnails decodes each remote thumb and used to store whatever decoded, so a
        // PPM/XPM/TGA was stored, judged broken by the next read-back, removed - and written again on every visit.
        // gridVisit walks that path as far as a probe without QtGui can: populate() resolves the tile through
        // displayImage (HomeView / SyntheticCatalogs set thumbnailUrl so); loadThumbnails paints a LOCAL tile straight
        // from disk; pumpThumbnails GETs a REMOTE one with the same redirect policy and, when it decoded, stores it under
        // "thumb" through the grid's gate (CoverFetch::gridThumbCacheable) and paints it. `decoded` stands for
        // QPixmap::loadFromData's answer. Request counts and stored files are the evidence, never timings.
        {
            QNetworkAccessManager gridNam;
            struct Visit { QString tile; QByteArray painted; };
            auto gridVisit = [&](const QString& key, const QString& url, bool decoded) {
                Visit v;
                v.tile = MetaCache::displayImage(key, url);
                if (!v.tile.startsWith(QStringLiteral("http"))) { v.painted = readAllOf(v.tile); return v; }
                QNetworkRequest req{ QUrl(v.tile) };
                req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply* reply = gridNam.get(req);
                const bool finished = pumpUntil([&] { return reply->isFinished(); });
                if (finished && reply->error() == QNetworkReply::NoError)
                {
                    const QByteArray data = reply->readAll();
                    if (decoded)
                    {
                        if (CoverFetch::gridThumbCacheable(decoded, data))
                            MetaCache::storeImage(key, QStringLiteral("thumb"), reply->url().toString(),
                                                  reply->header(QNetworkRequest::ContentTypeHeader).toString(), data);
                        v.painted = data;
                    }
                }
                reply->deleteLater();
                return v;
            };
            const QString thumb = QStringLiteral("thumb");
            const int kVisits = 5;

            // -- the pure rule: decoded AND a picture to the cache ----------------------------------------------------
            for (const Decodable& dn : decodableNotPictures())
            {
                const QByteArray what = QStringLiteral("389 rule: a %1 is not a picture to the cache").arg(dn.fmt).toLatin1();
                CHECK(!CoverFetch::isPicture(dn.bytes), what.constData());
                const QByteArray grid = QStringLiteral("389 rule: a decoded %1 is not cacheable").arg(dn.fmt).toLatin1();
                CHECK(!CoverFetch::gridThumbCacheable(true, dn.bytes), grid.constData());
            }
            for (const Picture& pic : pictures())
            {
                const QByteArray yes = QStringLiteral("389 rule: a decoded %1 is cacheable").arg(pic.fmt).toLatin1();
                CHECK(CoverFetch::gridThumbCacheable(true, pic.bytes), yes.constData());
                const QByteArray no = QStringLiteral("389 rule: an undecoded %1 is not").arg(pic.fmt).toLatin1();
                CHECK(!CoverFetch::gridThumbCacheable(false, pic.bytes), no.constData());
            }
            CHECK(!CoverFetch::gridThumbCacheable(true, kHtmlPage), "389 rule: a page is never cacheable, decoded or not");
            CHECK(!CoverFetch::gridThumbCacheable(true, QByteArray()), "389 rule: an empty body is never cacheable");

            // -- 1. a decodable non-picture thumb is SHOWN but never stored, visit after visit -------------------------
            for (const Decodable& dn : decodableNotPictures())
            {
                const QString key = QStringLiteral("389:grid:") + dn.fmt;
                MetaCache::remove(key);
                const QByteArray path = "/grid/" + dn.fmt.toLatin1() + "/cover." + dn.ext.toLatin1();
                const QString url = http.route(path, dn.ctype, dn.bytes);
                for (int i = 1; i <= kVisits; ++i)
                {
                    const Visit v = gridVisit(key, url, true);
                    const QByteArray src = QStringLiteral("389 grid: a %1 tile is its source url - nothing cached to serve").arg(dn.fmt).toLatin1();
                    CHECK(v.tile == url, src.constData());
                    const QByteArray shown = QStringLiteral("389 grid: a %1 thumb is still painted, from the bytes fetched").arg(dn.fmt).toLatin1();
                    CHECK(v.painted == dn.bytes, shown.constData());
                    const QByteArray once = QStringLiteral("389 grid: a %1 thumb takes one request per visit, never more").arg(dn.fmt).toLatin1();
                    CHECK(http.count(path) == i, once.constData());
                    const QByteArray none = QStringLiteral("389 grid: a %1 thumb is not stored").arg(dn.fmt).toLatin1();
                    CHECK(roleFiles(key, thumb) == 0, none.constData());
                    CHECK(recordedFile(key, thumb).isEmpty(), "389 grid: ...and no images entry is recorded");
                }
                CHECK(settle(), "389 grid: settle");
                const QByteArray total = QStringLiteral("389 grid: %1 visits to a %2 thumb made %1 requests").arg(kVisits).arg(dn.fmt).toLatin1();
                CHECK(http.count(path) == kVisits, total.constData());
                CHECK(roleFiles(key, thumb) == 0 && recordedFile(key, thumb).isEmpty(), "389 grid: still nothing stored");
                MetaCache::remove(key);
            }

            // -- 2. one an earlier build STORED is removed once, and never written again ---------------------------------
            for (const Decodable& dn : decodableNotPictures())
            {
                const QString key = QStringLiteral("389:gridheal:") + dn.fmt;
                MetaCache::remove(key);
                const QByteArray path = "/gridheal/" + dn.fmt.toLatin1() + "/cover." + dn.ext.toLatin1();
                const QString url = http.route(path, dn.ctype, dn.bytes);
                MetaCache::storeImage(key, thumb, url, QString::fromLatin1(dn.ctype), dn.bytes);   // the old grid's write
                const QString planted = recordedFile(key, thumb);
                CHECK(readAllOf(planted) == dn.bytes, "389 grid heal: fixture - the old build's thumb is stored");
                for (int i = 1; i <= kVisits; ++i)
                {
                    const Visit v = gridVisit(key, url, true);
                    CHECK(v.tile == url && v.painted == dn.bytes, "389 grid heal: the thumb is painted from its source");
                    const QByteArray gone = QStringLiteral("389 grid heal: the stored %1 is gone and not written again").arg(dn.fmt).toLatin1();
                    CHECK(!QFileInfo::exists(planted) && roleFiles(key, thumb) == 0 && recordedFile(key, thumb).isEmpty(),
                          gone.constData());
                    CHECK(http.count(path) == i, "389 grid heal: one request per visit");
                }
                MetaCache::remove(key);
            }

            // -- 3. every format the cache keeps is stored by the grid ONCE and served from disk after ------------------
            for (const Picture& pic : pictures())
            {
                const QString key = QStringLiteral("389:gridkeep:") + pic.fmt;
                MetaCache::remove(key);
                const QByteArray path = "/gridkeep/" + pic.fmt.toLatin1() + "/cover." + pic.ext.toLatin1();
                const QString url = http.route(path, pic.ctype, pic.bytes);
                const Visit first = gridVisit(key, url, true);
                CHECK(first.tile == url && first.painted == pic.bytes, "389 grid keep: the first visit paints the fetched thumb");
                const QString stored = recordedFile(key, thumb);
                const QByteArray landed = QStringLiteral("389 grid keep: a %1 thumb is stored by the grid").arg(pic.fmt).toLatin1();
                CHECK(readAllOf(stored) == pic.bytes, landed.constData());
                const QByteArray named = QStringLiteral("389 grid keep: ...as thumb.%1").arg(pic.ext).toLatin1();
                CHECK(QFileInfo(stored).fileName() == thumb + QLatin1Char('.') + pic.ext, named.constData());
                for (int i = 2; i <= kVisits; ++i)
                {
                    const Visit v = gridVisit(key, url, true);
                    const QByteArray local = QStringLiteral("389 grid keep: a stored %1 tile is the file on disk").arg(pic.fmt).toLatin1();
                    CHECK(v.tile == stored && v.painted == pic.bytes, local.constData());
                    CHECK(MetaCache::scrapedImage(key, url) == stored, "389 grid keep: scrapedImage serves it");
                    CHECK(MetaCache::verifiedImagePath(key, thumb) == stored, "389 grid keep: the bytes check keeps it");
                }
                CHECK(settle(), "389 grid keep: settle");
                const QByteArray never = QStringLiteral("389 grid keep: a %1 thumb is fetched once in %2 visits").arg(pic.fmt).arg(kVisits).toLatin1();
                CHECK(http.count(path) == 1, never.constData());
                CHECK(readAllOf(stored) == pic.bytes && roleFiles(key, thumb) == 1, "389 grid keep: bytes unchanged, one file");
                MetaCache::remove(key);
            }

            // -- 4. the decode check still stands: bytes the grid could not decode are neither painted nor stored -------
            {
                const QString key = QStringLiteral("389:gridundecoded");
                MetaCache::remove(key);
                const QByteArray path = "/gridundecoded/cover.png";
                const QString url = http.route(path, "image/png", pictures()[1].bytes);
                for (int i = 1; i <= 2; ++i)
                {
                    const Visit v = gridVisit(key, url, false);
                    CHECK(v.tile == url && v.painted.isEmpty(), "389 grid undecoded: nothing painted");
                    CHECK(roleFiles(key, thumb) == 0 && recordedFile(key, thumb).isEmpty(), "389 grid undecoded: nothing stored");
                    CHECK(http.count(path) == i, "389 grid undecoded: one request per visit");
                }
                MetaCache::remove(key);
            }
            std::printf("GRIDTHUMB-OK\n");
        }
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
