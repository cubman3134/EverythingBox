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
#include "LiveTvIdentity.h"   // #203: the durable, credential-free channel identity
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
        // #203: the id is the CHANNEL IDENTITY, not the url. These fixtures carry no tvg-id, so it is the
        // name spelling — which is exactly the arm a hand-written favourite has to agree with.
        FavoriteItem fCnn; fCnn.type = QStringLiteral("livetv"); fCnn.itemId = QStringLiteral("livetv:name:cnn");
        FavoriteItem fBbc; fBbc.type = QStringLiteral("movie");  fBbc.itemId = QStringLiteral("livetv:name:bbc");
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
        CHECK(cnn.id == QStringLiteral("livetv:name:cnn"));     // #203: the identity, never the url
        CHECK(cnn.mime == QStringLiteral("livetv"));

        // The favourite/id helpers agree with the catalog's marking (one definition of a channel's identity).
        CHECK(browse::liveTvChannelId(entries, 0) == QStringLiteral("livetv:name:cnn"));
        const FavoriteItem cf = browse::liveTvChannelFavorite(entries, 0);
        CHECK(cf.itemId == QStringLiteral("livetv:name:cnn"));
        CHECK(cf.type == QStringLiteral("livetv"));
        // #203: THE RE-OPEN TARGET IS THE IDENTITY, NOT THE STREAM. A starred channel that stored its url
        // would replay a link whose credential has since rotated — and would put that credential into a
        // synced store. Both fields hold the name; the url is looked up at open time.
        CHECK(cf.path == QStringLiteral("livetv:name:cnn"));
        CHECK(!cf.path.contains(QStringLiteral("://")));
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


    // ================= 5. #203: a channel's DURABLE, CREDENTIAL-FREE IDENTITY ======================
    //
    // NO REAL CREDENTIAL APPEARS ANYWHERE IN THIS FILE. The fixture below uses an invented provider path of
    // the shape a real one has (`/live/<user>/<pass>/<id>.ts`); only its SHAPE is real, and the assertions
    // compare strings rather than printing them.
    //
    // The old identity was `"livetv:" + e.url`, which put the provider's credential into two synced stores
    // and stopped working the moment the provider rotated it. The rule now: the EPG id when the entry carries
    // one, else the normalised name — and the url is resolved FROM the id at open time.
    {
        auto ch = [](const QString& tvgId, const QString& tvgName, const QString& title, const QString& url) {
            LiveTvIdentity::Channel c; c.tvgId = tvgId; c.tvgName = tvgName; c.title = title; c.url = url;
            return c;
        };

        // 5a. TVG-ID WINS, and it is taken verbatim — it is an id, not a display string, so normalising it
        // would break the one thing that already keys on it (the XMLTV guide).
        CHECK(LiveTvIdentity::idForTvgId(QStringLiteral("CNN.us")) == QStringLiteral("livetv:CNN.us"));
        CHECK(LiveTvIdentity::idForTvgId(QStringLiteral("  cnn.us  ")) == QStringLiteral("livetv:cnn.us")); // trimmed
        CHECK(LiveTvIdentity::idForTvgId(QString()).isEmpty());

        // 5b. NO TVG-ID -> the normalised name: trim, collapse internal whitespace, case-fold. tvg-name is
        // preferred over the display title when the entry carries one (it is the canonical name; the title is
        // frequently decorated by the provider).
        {
            QVector<LiveTvIdentity::Channel> cs;
            cs << ch(QString(), QString(), QStringLiteral("  Sky   News  "), QStringLiteral("http://p/1.ts"));
            cs << ch(QString(), QStringLiteral("Film Four"), QStringLiteral("UK| FILM4"), QStringLiteral("http://p/2.ts"));
            const QVector<QString> ids = LiveTvIdentity::idsFor(cs);
            CHECK(ids.size() == 2);
            CHECK(ids[0] == QStringLiteral("livetv:name:sky news"));
            CHECK(ids[1] == QStringLiteral("livetv:name:film four"));
        }

        // 5c. THE QUALITY-TAG RULE, BOTH WAYS — the case that cannot be decided from one entry, which is why
        // the rule takes the whole list.
        {
            // Both spellings present: they are one channel listed twice, so the tag comes off and the two
            // share one identity. The FIRST in playlist order is what that identity resolves to — the
            // provider's own preference order, and it is reported as a collision so a load can log it once.
            QVector<LiveTvIdentity::Channel> both;
            both << ch(QString(), QString(), QStringLiteral("CNN HD"), QStringLiteral("http://p/hd.ts"));
            both << ch(QString(), QString(), QStringLiteral("CNN"),    QStringLiteral("http://p/sd.ts"));
            QStringList clashes;
            const QVector<QString> ids = LiveTvIdentity::idsFor(both, &clashes);
            CHECK(ids[0] == QStringLiteral("livetv:name:cnn"));
            CHECK(ids[1] == QStringLiteral("livetv:name:cnn"));
            CHECK(clashes == QStringList{ QStringLiteral("livetv:name:cnn") });
            CHECK(clashes.size() == 1);   // reported ONCE, not once per duplicate
            CHECK(LiveTvIdentity::urlFor(both, QStringLiteral("livetv:name:cnn"))
                  == QStringLiteral("http://p/hd.ts"));           // first in list order
            // NEITHER ROW IS HIDDEN. A channel list is a view of the provider's playlist; dropping a row the
            // user can see in their provider's own app would be a worse bug than a shared id.
            CHECK(ids.size() == both.size());
        }
        {
            // Only the tagged spelling present: "CNN HD" is simply what this provider calls the channel, and
            // stripping the tag would invent a name nothing carries.
            QVector<LiveTvIdentity::Channel> only;
            only << ch(QString(), QString(), QStringLiteral("CNN HD"), QStringLiteral("http://p/hd.ts"));
            QStringList clashes;
            const QVector<QString> ids = LiveTvIdentity::idsFor(only, &clashes);
            CHECK(ids[0] == QStringLiteral("livetv:name:cnn hd"));
            CHECK(clashes.isEmpty());
        }
        // The tag may be bracketed, and only a MATCHING pair is peeled.
        CHECK(LiveTvIdentity::withoutQualityTag(QStringLiteral("cnn (hd)")) == QStringLiteral("cnn"));
        CHECK(LiveTvIdentity::withoutQualityTag(QStringLiteral("cnn [4k]")) == QStringLiteral("cnn"));
        CHECK(LiveTvIdentity::withoutQualityTag(QStringLiteral("cnn (hd]")) == QStringLiteral("cnn (hd]"));
        // A word that is not in the closed set is part of the name.
        CHECK(LiveTvIdentity::withoutQualityTag(QStringLiteral("cnn news")) == QStringLiteral("cnn news"));
        // A one-token name has no tag to take off (and must not become empty).
        CHECK(LiveTvIdentity::withoutQualityTag(QStringLiteral("hd")) == QStringLiteral("hd"));

        // 5d. A DUPLICATE TVG-ID collides the same way, and resolution keeps the first.
        {
            QVector<LiveTvIdentity::Channel> cs;
            cs << ch(QStringLiteral("cnn.us"), QString(), QStringLiteral("CNN Main"),   QStringLiteral("http://p/a.ts"));
            cs << ch(QStringLiteral("cnn.us"), QString(), QStringLiteral("CNN Backup"), QStringLiteral("http://p/b.ts"));
            QStringList clashes;
            const QVector<QString> ids = LiveTvIdentity::idsFor(cs, &clashes);
            CHECK(ids[0] == QStringLiteral("livetv:cnn.us") && ids[1] == QStringLiteral("livetv:cnn.us"));
            CHECK(clashes == QStringList{ QStringLiteral("livetv:cnn.us") });
            CHECK(LiveTvIdentity::urlFor(cs, QStringLiteral("livetv:cnn.us")) == QStringLiteral("http://p/a.ts"));
        }

        // 5e. NEITHER A TVG-ID NOR A NAME. It still gets an identity — a channel that cannot be starred at
        // all is worse than one whose name is positional — and it is still credential-free.
        {
            QVector<LiveTvIdentity::Channel> cs;
            cs << ch(QString(), QString(), QString(), QStringLiteral("http://p/x.ts"));
            const QVector<QString> ids = LiveTvIdentity::idsFor(cs);
            CHECK(ids.size() == 1 && !ids[0].isEmpty());
            CHECK(!ids[0].contains(QStringLiteral("://")));
            CHECK(LiveTvIdentity::urlFor(cs, ids[0]) == QStringLiteral("http://p/x.ts"));
        }

        // 5f. RESOLUTION READS THE CURRENT LIST — the whole reason the identity is stored instead of the url.
        // Same channel, same id, a rotated credential in the path: the id resolves to the NEW url, and the old
        // one is nowhere in the answer.
        {
            const QString before = QStringLiteral("http://p.example/live/user/tok-aaaa/55.ts");
            const QString after  = QStringLiteral("http://p.example/live/user/tok-bbbb/55.ts");
            QVector<LiveTvIdentity::Channel> was, now;
            was << ch(QStringLiteral("cnn.us"), QString(), QStringLiteral("CNN"), before);
            now << ch(QStringLiteral("cnn.us"), QString(), QStringLiteral("CNN"), after);
            const QString id = LiveTvIdentity::idsFor(was).at(0);
            CHECK(id == QStringLiteral("livetv:cnn.us"));
            CHECK(!id.contains(QStringLiteral("tok-")));                 // the identity carries no token
            CHECK(LiveTvIdentity::urlFor(was, id) == before);
            CHECK(LiveTvIdentity::urlFor(now, id) == after);             // the CURRENT url, not the saved one
        }

        // 5g. A CHANNEL THIS DEVICE DOES NOT CARRY resolves to nothing — which is the caller's cue to say
        // "unavailable" and leave the row where it is. An empty id never matches anything either.
        {
            QVector<LiveTvIdentity::Channel> cs;
            cs << ch(QStringLiteral("cnn.us"), QString(), QStringLiteral("CNN"), QStringLiteral("http://p/1.ts"));
            CHECK(LiveTvIdentity::urlFor(cs, QStringLiteral("livetv:bbc.uk")).isEmpty());
            CHECK(LiveTvIdentity::urlFor(cs, QString()).isEmpty());
            CHECK(LiveTvIdentity::urlFor({}, QStringLiteral("livetv:cnn.us")).isEmpty());
        }

        // 5h. WHAT COUNTS AS A LEGACY (credential-carrying) ID — the test CloudMerge's filter is built on.
        CHECK(LiveTvIdentity::isLiveTvId(QStringLiteral("livetv:cnn.us")));
        CHECK(!LiveTvIdentity::isLiveTvId(QStringLiteral("tt0111161")));
        CHECK(LiveTvIdentity::isCredentialShaped(
                  QStringLiteral("livetv:http://p.example/live/user/tok-aaaa/55.ts")));
        CHECK(!LiveTvIdentity::isCredentialShaped(QStringLiteral("livetv:cnn.us")));
        CHECK(!LiveTvIdentity::isCredentialShaped(QStringLiteral("livetv:name:cnn")));
        CHECK(!LiveTvIdentity::isCredentialShaped(QStringLiteral("tt0111161")));
        // THE NAME SPELLING IS NOT TRUSTED BY ITS PREFIX. A channel whose #EXTINF label IS a url would
        // otherwise carry one out through the fallback arm.
        CHECK(LiveTvIdentity::isCredentialShaped(
                  QStringLiteral("livetv:name:http://p.example/live/user/tok-aaaa/55.ts")));

        // 5i. THE WIRE SPELLING of a row nothing could re-identify: the name rule, and never a url. A
        // titleless row still gets a name, so the playlist merge always has a key to match on.
        CHECK(LiveTvIdentity::wireId(QStringLiteral("  CNN  HD ")) == QStringLiteral("livetv:name:cnn hd"));
        CHECK(!LiveTvIdentity::wireId(QString()).isEmpty());
        CHECK(!LiveTvIdentity::isCredentialShaped(LiveTvIdentity::wireId(QString())));
        CHECK(!LiveTvIdentity::isCredentialShaped(
                  LiveTvIdentity::wireId(QStringLiteral("http://p.example/live/user/tok-aaaa/55.ts"))));
    }

    if (failures == 0) { std::puts("IPTV-OK"); return 0; }
    std::fprintf(stderr, "IPTV: %d check(s) failed\n", failures);
    return 1;
}
