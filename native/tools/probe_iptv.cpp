// Headless check of the Live TV / IPTV persistent-sources feature (#75, increment 2):
//
//   * IptvSourceStore (src/core/IptvSourceStore) — a per-profile source list round-trips add/update/remove,
//     the reserved epgUrl field survives a round-trip, and two profiles never cross;
//   * browse::liveTvSourcesCatalog (src/browse/SyntheticCatalogs) — one row per saved source plus a trailing
//     "add a source" row, and JUST the add row when the list is empty;
//   * browse::liveTvChannelsCatalog — the channels of one source SECTIONED by group (groups case-insensitive,
//     the empty-group "Ungrouped" bucket last, a header row introducing each section), each channel carrying
//     the stream url + tvg-logo, and a favourited channel marked (type "livetv" only).
//
// QtCore-only (a QSettings/JSON store + pure builders), so it runs under the offscreen QPA in CI. Prints
// IPTV-OK on success; any failure prints IPTV-FAIL <cond> (line) and exits non-zero.
//
// FIXTURE INDEPENDENCE: the M3uEntry channel fixtures and the FavoriteItem fixtures are hand-built here
// (aggregate/field init), NOT produced by StreamResolver::parseM3u or FavoritesStore — the expected group
// ordering and the expected marked-channel are computed by hand below, so the builders are measured against
// an oracle that does not run them. Increment 1's parseM3u is proven separately by probe_m3u.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (EB_ISOLATED_DATA_DIR), so the
// everythingbox.ini IptvSourceStore opens starts empty and is removed at exit.
#include "IptvSourceStore.h"
#include "SyntheticCatalogs.h"
#include "StreamResolver.h"   // M3uEntry
#include "FavoritesStore.h"   // FavoriteItem
#include "AddonModels.h"      // MediaItem / MediaCatalog
#include "ProfileStore.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "IPTV-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A channel fixture. Hand-built M3uEntry: { title, url, logo, group, tvgId, tvgName }.
static M3uEntry chan(const QString& title, const QString& url, const QString& logo, const QString& group)
{
    M3uEntry e; e.title = title; e.url = url; e.logo = logo; e.group = group; return e;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ================= 1. IptvSourceStore per-profile round-trip =================================
    ProfileStore::setCurrent(QStringLiteral("probeIptvA"));
    CHECK(IptvSourceStore::list().isEmpty());   // a fresh profile has no sources

    IptvSource s1; s1.name = QStringLiteral("My Provider"); s1.url = QStringLiteral("http://host/pl.m3u");
    s1.epgUrl = QStringLiteral("http://host/epg.xml.gz");   // a NON-empty epg to prove it round-trips
    const QString id1 = IptvSourceStore::add(s1);
    CHECK(!id1.isEmpty());                        // add mints a stable id

    IptvSource s2; s2.name = QStringLiteral("Second"); s2.url = QStringLiteral("C:/tv/local.m3u");
    const QString id2 = IptvSourceStore::add(s2); // epgUrl empty (the increment-2 case)
    CHECK(!id2.isEmpty() && id2 != id1);

    {
        const QList<IptvSource> all = IptvSourceStore::list();
        CHECK(all.size() == 2);
        IptvSource got;
        CHECK(IptvSourceStore::get(id1, got));
        CHECK(got.name == QStringLiteral("My Provider"));
        CHECK(got.url == QStringLiteral("http://host/pl.m3u"));
        CHECK(got.epgUrl == QStringLiteral("http://host/epg.xml.gz"));   // reserved field preserved (inc 3)
        IptvSource got2;
        CHECK(IptvSourceStore::get(id2, got2));
        CHECK(got2.epgUrl.isEmpty());                                    // empty epg round-trips as empty
    }

    // update: replace name + url + epgUrl for id1, disturbing nothing else.
    {
        IptvSource up; up.id = id1; up.name = QStringLiteral("Renamed");
        up.url = QStringLiteral("http://host/other.m3u"); up.epgUrl = QStringLiteral("http://host/g2.xml");
        IptvSourceStore::update(up);
        IptvSource got;
        CHECK(IptvSourceStore::get(id1, got));
        CHECK(got.name == QStringLiteral("Renamed"));
        CHECK(got.url == QStringLiteral("http://host/other.m3u"));
        CHECK(got.epgUrl == QStringLiteral("http://host/g2.xml"));
        CHECK(IptvSourceStore::list().size() == 2);   // update never adds/removes
    }

    // remove: drops exactly id1; id2 is untouched.
    {
        IptvSourceStore::remove(id1);
        IptvSource gone;
        CHECK(!IptvSourceStore::get(id1, gone));
        CHECK(IptvSourceStore::list().size() == 1);
        CHECK(IptvSourceStore::list().first().id == id2);
    }

    // Per-profile: profile B sees none of A's sources.
    {
        ProfileStore::setCurrent(QStringLiteral("probeIptvB"));
        CHECK(IptvSourceStore::list().isEmpty());
        ProfileStore::setCurrent(QStringLiteral("probeIptvA"));
        CHECK(IptvSourceStore::list().size() == 1);   // A's survived B's read
    }

    // ================= 2. liveTvSourcesCatalog ===================================================
    {
        // Empty list -> JUST the add row.
        const MediaCatalog empty = browse::liveTvSourcesCatalog({});
        CHECK(empty.items.size() == 1);
        CHECK(empty.items.last().type == QStringLiteral("_newlivetv"));
        CHECK(empty.items.last().mime == QStringLiteral("newlivetv"));

        IptvSource a; a.id = QStringLiteral("aaa"); a.name = QStringLiteral("Alpha"); a.url = QStringLiteral("http://a/pl");
        IptvSource b; b.id = QStringLiteral("bbb"); b.name = QStringLiteral("Beta");  b.url = QStringLiteral("http://b/pl");
        const MediaCatalog cat = browse::liveTvSourcesCatalog({ a, b });
        CHECK(cat.items.size() == 3);                            // 2 sources + the add row
        CHECK(cat.items[0].type == QStringLiteral("_livetvsource"));
        CHECK(cat.items[0].title == QStringLiteral("Alpha"));
        CHECK(cat.items[0].subtitle == QStringLiteral("http://a/pl"));
        CHECK(cat.items[0].mime == QStringLiteral("livetvsource:aaa"));   // carries the id for the fresh fetch
        CHECK(cat.items[1].mime == QStringLiteral("livetvsource:bbb"));
        CHECK(cat.items.last().type == QStringLiteral("_newlivetv"));     // add row always LAST
        CHECK(cat.items.last().mime == QStringLiteral("newlivetv"));
    }

    // ================= 3. liveTvChannelsCatalog ==================================================
    {
        // Channels in a deliberately unsorted group order. Groups present: News, Sports, music, and one with
        // NO group (-> "Ungrouped"). CNN precedes BBC within News (playlist order must be preserved).
        QVector<M3uEntry> entries;
        entries << chan(QStringLiteral("CNN"),    QStringLiteral("http://x/cnn.ts"),  QStringLiteral("http://x/cnn.png"), QStringLiteral("News"));
        entries << chan(QStringLiteral("BBC"),    QStringLiteral("http://x/bbc.ts"),  QString(),                          QStringLiteral("News"));
        entries << chan(QStringLiteral("ESPN"),   QStringLiteral("http://x/espn.ts"), QStringLiteral("http://x/espn.png"), QStringLiteral("Sports"));
        entries << chan(QStringLiteral("Local9"), QStringLiteral("http://x/l9.ts"),   QString(),                          QString());     // Ungrouped
        entries << chan(QStringLiteral("MTV"),    QStringLiteral("http://x/mtv.ts"),  QString(),                          QStringLiteral("music"));

        // Independently computed oracle. Case-insensitive group order: music < News < Sports, then Ungrouped
        // (the empty bucket) LAST. Header rows introduce each section; channels keep playlist order in a group.
        //   [hdr music] MTV | [hdr News] CNN BBC | [hdr Sports] ESPN | [hdr Ungrouped] Local9
        //
        // Favourites: CNN is a real "livetv" favourite (must be starred). BBC carries a SAME-URL "movie"
        // favourite that must NOT mark it — the type filter is what keeps a film's star off a channel.
        QList<FavoriteItem> favs;
        FavoriteItem fCnn; fCnn.type = QStringLiteral("livetv"); fCnn.itemId = QStringLiteral("livetv:http://x/cnn.ts");
        FavoriteItem fBbc; fBbc.type = QStringLiteral("movie");  fBbc.itemId = QStringLiteral("livetv:http://x/bbc.ts");
        favs << fCnn << fBbc;

        const MediaCatalog cat = browse::liveTvChannelsCatalog(QStringLiteral("My Provider"), entries, favs);
        CHECK(cat.title == QStringLiteral("My Provider"));

        // The exact sectioned sequence (types + titles). This one assertion kills a broken group sort, a
        // misplaced Ungrouped bucket, a dropped header, and lost within-group order all at once.
        struct Row { const char* type; QString title; };
        const QVector<Row> want = {
            { "_livetvheader", QStringLiteral("music") },
            { "livetv",        QStringLiteral("MTV") },
            { "_livetvheader", QStringLiteral("News") },
            { "livetv",        QStringLiteral("\u2605  CNN") },   // ★ marked
            { "livetv",        QStringLiteral("BBC") },           // NOT marked (movie fav, wrong type)
            { "_livetvheader", QStringLiteral("Sports") },
            { "livetv",        QStringLiteral("ESPN") },
            { "_livetvheader", QStringLiteral("Ungrouped") },
            { "livetv",        QStringLiteral("Local9") },
        };
        CHECK(cat.items.size() == want.size());
        if (cat.items.size() == want.size())
            for (int i = 0; i < want.size(); ++i)
            {
                CHECK(cat.items[i].type == QLatin1String(want[i].type));
                CHECK(cat.items[i].title == want[i].title);
            }

        // Header rows are non-playable: no url.
        CHECK(cat.items[0].url.isEmpty());

        // A channel carries the stream url (in url AND its stable id) and the tvg-logo as tile art.
        const MediaItem& cnn = cat.items[3];
        CHECK(cnn.url == QStringLiteral("http://x/cnn.ts"));
        CHECK(cnn.thumbnailUrl == QStringLiteral("http://x/cnn.png"));
        CHECK(cnn.id == QStringLiteral("livetv:http://x/cnn.ts"));
        CHECK(cnn.mime == QStringLiteral("livetv"));

        // The favourite/id helpers agree with the catalog's marking (one definition of a channel's identity).
        CHECK(browse::liveTvChannelId(entries[0]) == QStringLiteral("livetv:http://x/cnn.ts"));
        const FavoriteItem cf = browse::liveTvChannelFavorite(entries[0]);
        CHECK(cf.itemId == QStringLiteral("livetv:http://x/cnn.ts"));
        CHECK(cf.type == QStringLiteral("livetv"));
        CHECK(cf.path == QStringLiteral("http://x/cnn.ts"));   // re-open target
        CHECK(cf.thumbnailUrl == QStringLiteral("http://x/cnn.png"));
    }

    // ================= 4. now/next subtitle override (#75 inc 3) ==================================
    // liveTvChannelsCatalog gained an optional nowNextByTvgId map: when a channel's tvg-id has an entry, that
    // one-liner REPLACES the group as the subtitle; a channel with no match keeps its group. Hand-built map
    // (the EPG computation itself is proven in probe_xmltv) — here we pin only the subtitle-selection rule.
    {
        M3uEntry withId; withId.title = QStringLiteral("CNN"); withId.url = QStringLiteral("http://x/cnn.ts");
        withId.group = QStringLiteral("News"); withId.tvgId = QStringLiteral("cnn.us");
        M3uEntry noId;   noId.title = QStringLiteral("Local9"); noId.url = QStringLiteral("http://x/l9.ts");
        noId.group = QStringLiteral("News");   // same group, but no tvg-id -> keeps the group subtitle
        QVector<M3uEntry> es; es << withId << noId;

        QHash<QString, QString> nn;
        nn.insert(QStringLiteral("cnn.us"), QStringLiteral("Now: News Hour · Next: Talk Show"));

        const MediaCatalog cat = browse::liveTvChannelsCatalog(QStringLiteral("Prov"), es, {}, nn);
        // rows: [hdr News] CNN Local9
        CHECK(cat.items.size() == 3);
        if (cat.items.size() == 3)
        {
            CHECK(cat.items[1].title == QStringLiteral("CNN"));
            CHECK(cat.items[1].subtitle == QStringLiteral("Now: News Hour · Next: Talk Show")); // now/next wins
            CHECK(cat.items[2].title == QStringLiteral("Local9"));
            CHECK(cat.items[2].subtitle == QStringLiteral("News"));   // no EPG match -> group subtitle kept
        }
        // With no map (the increment-2 default), the matched channel falls back to its group.
        const MediaCatalog base = browse::liveTvChannelsCatalog(QStringLiteral("Prov"), es, {});
        CHECK(base.items.size() == 3 && base.items[1].subtitle == QStringLiteral("News"));
    }

    if (failures == 0) { std::puts("IPTV-OK"); return 0; }
    std::fprintf(stderr, "IPTV: %d check(s) failed\n", failures);
    return 1;
}
