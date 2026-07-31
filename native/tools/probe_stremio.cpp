// Headless coverage for the Stremio protocol translator, and for the download queue that a Stremio stream
// feeds. No addon is installed and NOTHING IS SENT: the download section builds a real queue but runs no
// event loop, so QNetworkAccessManager::get() queues a request that is never dispatched.
// Prints STREMIO-OK on success; any failure prints STREMIO-FAIL <what> and exits non-zero.
#include "StremioTranslate.h"
#include "AppPaths.h"
#include "BingeStore.h"
#include "DownloadManager.h"
#include "StreamHeaders.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                                        \
    do { if (!(cond)) { std::fprintf(stderr, "STREMIO-FAIL %s\n", (what)); ++failures; } } while (0)

using namespace StremioTranslate;

static const Catalog* byId(const Manifest& m, const QString& id)
{
    for (const Catalog& c : m.catalogs) if (c.id == id) return &c;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ------------------------------------------------- 1. not a Stremio manifest
    {
        CHECK(!parseManifest(QByteArray("{\"id\":\"x\",\"name\":\"y\"}")).isValid(),
              "a manifest with no resources is not Stremio");
        CHECK(!parseManifest(QByteArray("not json at all")).isValid(), "garbage is not Stremio");
        // Detection needs BOTH keys, so each one has to be pinned WITHOUT the other — a body carrying
        // neither would still be rejected by half a guard and proves nothing about the other half.
        CHECK(!parseManifest(QByteArray(R"({"id":"a","resources":["stream"]})")).isValid(),
              "resources without types is not Stremio");
        CHECK(!parseManifest(QByteArray(R"({"id":"a","types":["movie"]})")).isValid(),
              "types without resources is not Stremio");
    }

    // ------------------------------------------------- 2. resources: strings, objects, MIXED
    {
        // Torrentio's shape: a plain string alongside an object carrying per-resource scoping.
        const QByteArray body = R"({
          "id": "com.stremio.torrentio.addon",
          "name": "Torrentio",
          "version": "0.0.15",
          "types": ["movie", "series"],
          "idPrefixes": ["tt"],
          "resources": [
            "catalog",
            { "name": "stream", "types": ["movie"], "idPrefixes": ["tt", "kitsu:"] }
          ],
          "catalogs": []
        })";
        const Manifest m = parseManifest(body);
        CHECK(m.isValid(), "a mixed resources array parses");
        CHECK(m.resources.contains(QStringLiteral("catalog")), "the string entry yields its name");
        CHECK(m.resources.contains(QStringLiteral("stream")), "the object entry yields its name");
        CHECK(m.idPrefixes == QStringList{ QStringLiteral("tt") }, "manifest-level idPrefixes");
        CHECK(m.resourceIdPrefixes.value(QStringLiteral("stream"))
                  == (QStringList{ QStringLiteral("tt"), QStringLiteral("kitsu:") }),
              "the object's own idPrefixes are kept, not discarded");
        CHECK(m.resourceTypes.value(QStringLiteral("stream")) == QStringList{ QStringLiteral("movie") },
              "the object's own types are kept");
        CHECK(!m.resourceIdPrefixes.contains(QStringLiteral("catalog")),
              "a string resource records no per-resource override");
    }

    // ------------------------------------------------- 3. catalog classification
    {
        const QByteArray body = R"({
          "id": "org.test.catalogs",
          "types": ["movie"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "movie", "id": "plain", "name": "Popular" },
            { "type": "movie", "id": "searchonly", "name": "Search",
              "extra": [ { "name": "search", "isRequired": true } ] },
            { "type": "movie", "id": "genrereq", "name": "By Genre",
              "extra": [ { "name": "genre", "isRequired": true,
                           "options": ["Action", "Comedy", "Drama"], "optionsLimit": 2 } ] },
            { "type": "movie", "id": "opaque", "name": "Needs Something",
              "extra": [ { "name": "mystery", "isRequired": true } ] },
            { "type": "movie", "id": "optional", "name": "Discover",
              "extra": [ { "name": "genre", "options": ["Action", "Comedy"] },
                         { "name": "skip" } ] }
          ]
        })";
        const Manifest m = parseManifest(body);
        CHECK(m.catalogs.size() == 5, "no catalog is dropped at parse time — classification decides use");

        CHECK(byId(m, "plain")->use == CatalogUse::Browse, "no extras -> Browse");

        CHECK(byId(m, "searchonly")->use == CatalogUse::SearchOnly, "required search -> SearchOnly");

        const Catalog* g = byId(m, "genrereq");
        CHECK(g->use == CatalogUse::Browse, "a required extra WITH options is browsable");
        CHECK(g->presets.value(QStringLiteral("genre")) == QStringLiteral("Action"),
              "…preselecting its FIRST option");
        CHECK(g->extras.first().optionsLimit == 2, "optionsLimit is carried");

        const Catalog* o = byId(m, "opaque");
        CHECK(o->use == CatalogUse::Unsatisfiable, "a required extra with no options is unsatisfiable");
        CHECK(!o->skipReason.isEmpty(), "…and says why, so it is never silently invisible");

        const Catalog* opt = byId(m, "optional");
        CHECK(opt->use == CatalogUse::Browse, "optional extras stay browsable");
        CHECK(opt->presets.isEmpty(), "…with nothing preselected");

        CHECK(byId(m, "genrereq")->routeId() == QStringLiteral("movie/genrereq"), "routeId shape");
    }

    // ------------------------------------------------- 4. the LEGACY extra form
    {
        // extraRequired/extraSupported are plain string arrays with no options anywhere.
        const QByteArray body = R"({
          "id": "org.test.legacy",
          "types": ["series"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "series", "id": "legacysearch", "name": "Old Search",
              "extraRequired": ["search"], "extraSupported": ["search", "skip"] },
            { "type": "series", "id": "legacyopt", "name": "Old Discover",
              "extraSupported": ["genre", "skip"] }
          ]
        })";
        const Manifest m = parseManifest(body);
        const Catalog* s = byId(m, "legacysearch");
        CHECK(s->use == CatalogUse::SearchOnly, "legacy extraRequired:[search] behaves like the modern form");
        bool sawSkip = false;
        for (const Extra& e : s->extras) if (e.name == QStringLiteral("skip")) sawSkip = true;
        CHECK(sawSkip, "extraSupported entries become non-required Extras");
        CHECK(byId(m, "legacyopt")->use == CatalogUse::Browse, "extraSupported alone is not required");
    }

    // ------------------------------------------------- 5. BOTH extra forms on ONE catalog: MERGE
    {
        // Neither fixture above carries both forms, so a replace-style legacy block would pass them all.
        // A manifest may declare the modern objects AND the legacy arrays; the legacy names must merge
        // INTO the parsed objects, never stand in for them.
        const QByteArray body = R"({
          "id": "org.test.bothforms",
          "types": ["movie"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "movie", "id": "upgrade", "name": "By Genre",
              "extra": [ { "name": "genre", "options": ["Action", "Comedy"] } ],
              "extraRequired": ["genre"] },
            { "type": "movie", "id": "adds", "name": "By Genre and Studio",
              "extra": [ { "name": "genre", "options": ["Action", "Comedy"] } ],
              "extraRequired": ["studio"] }
          ]
        })";
        const Manifest m = parseManifest(body);

        // Case 1: legacy names the SAME extra — it upgrades isRequired, the object's options survive.
        const Catalog* u = byId(m, "upgrade");
        CHECK(u->extras.size() == 1, "the legacy name merges into the modern entry, it does not duplicate it");
        CHECK(u->extras.first().isRequired, "extraRequired upgrades the modern entry to required");
        CHECK(u->use == CatalogUse::Browse, "…and the modern options survive, so it is still browsable");
        CHECK(u->presets.value(QStringLiteral("genre")) == QStringLiteral("Action"),
              "…preselecting the first option the object declared");

        // Case 2: legacy names a DIFFERENT extra — it is added, option-less, so the catalog dies.
        const Catalog* a = byId(m, "adds");
        CHECK(a->extras.size() == 2, "a legacy name not in extra[] is ADDED alongside it");
        CHECK(a->use == CatalogUse::Unsatisfiable,
              "…and being required with no options makes the catalog unsatisfiable");
        CHECK(!a->skipReason.isEmpty(), "…with a reason naming what is missing");
    }

    // ------------------------------------------------- 6. MULTIPLE required extras
    {
        const QByteArray body = R"({
          "id": "org.test.multireq",
          "types": ["movie"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "movie", "id": "searchandgenre", "name": "Search by Genre",
              "extra": [ { "name": "search", "isRequired": true },
                         { "name": "genre", "isRequired": true, "options": ["Action", "Comedy"] } ] },
            { "type": "movie", "id": "genrefirst", "name": "Genre then Mystery",
              "extra": [ { "name": "genre", "isRequired": true, "options": ["Action", "Comedy"] },
                         { "name": "mystery", "isRequired": true } ] },
            { "type": "movie", "id": "mysteryfirst", "name": "Mystery then Genre",
              "extra": [ { "name": "mystery", "isRequired": true },
                         { "name": "genre", "isRequired": true, "options": ["Action", "Comedy"] } ] }
          ]
        })";
        const Manifest m = parseManifest(body);

        // Search dominates a defaultable extra — and dominating means the default is DROPPED. Left in
        // place, presets["genre"] would seed the query map and quietly filter every search to Action.
        const Catalog* sg = byId(m, "searchandgenre");
        CHECK(sg->use == CatalogUse::SearchOnly,
              "required search wins over a required extra we could have defaulted");
        CHECK(sg->presets.isEmpty(),
              "…and the default is dropped, so a search is never silently filtered by it");

        // Same manifest, extras reordered: the classifier returns on the first extra it cannot supply,
        // so the struct must be scrubbed on that path or its presets would depend on declaration order.
        const Catalog* gf = byId(m, "genrefirst");
        const Catalog* mf = byId(m, "mysteryfirst");
        CHECK(gf->use == CatalogUse::Unsatisfiable && mf->use == CatalogUse::Unsatisfiable,
              "an option-less required extra is unsatisfiable in either declaration order");
        CHECK(!gf->skipReason.isEmpty() && !mf->skipReason.isEmpty(), "…both saying why");
        CHECK(gf->presets.isEmpty(),
              "an unsatisfiable catalog keeps NO presets, even ones recorded before the verdict");
        CHECK(gf->presets == mf->presets, "…so reordering the extras cannot change the result");
    }

    // ------------------------------------------------- 7. duplicate names inside extra[]
    {
        const QByteArray body = R"({
          "id": "org.test.dupes",
          "types": ["movie"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "movie", "id": "dupe", "name": "Twice",
              "extra": [ { "name": "genre", "options": ["Action", "Comedy"] },
                         { "name": "genre", "options": ["Sci-Fi"] } ] }
          ]
        })";
        const Manifest m = parseManifest(body);
        const Catalog* d = byId(m, "dupe");
        CHECK(d->extras.size() == 1, "a name declared twice in extra[] yields ONE Extra, not a doubled control");
        CHECK(d->extras.first().options == (QStringList{ QStringLiteral("Action"), QStringLiteral("Comedy") }),
              "…the FIRST declaration wins, as on the legacy path");
    }

    // ------------------------------------------------- 8. behaviorHints
    {
        const QByteArray body = R"({
          "id": "org.test.cfg", "types": ["movie"], "resources": ["stream"],
          "behaviorHints": { "configurable": true, "configurationRequired": true }
        })";
        const Manifest m = parseManifest(body);
        CHECK(m.configurable && m.configurationRequired, "manifest behaviorHints are read");

        const Manifest none = parseManifest(QByteArray(
            R"({"id":"a","types":["movie"],"resources":["stream"]})"));
        CHECK(!none.configurable && !none.configurationRequired, "absent behaviorHints default to false");
    }

    // ------------------------------------------------- 9. catalogPath
    {
        Catalog c;
        c.type = QStringLiteral("movie");
        c.id   = QStringLiteral("top");
        CHECK(catalogPath(c, {}) == QStringLiteral("/catalog/movie/top.json"), "no extras -> bare path");

        // Sorted keys, so the same request is always the same string (the result cache keys on it).
        QMap<QString, QString> two;
        two.insert(QStringLiteral("skip"), QStringLiteral("100"));
        two.insert(QStringLiteral("genre"), QStringLiteral("Action"));
        CHECK(catalogPath(c, two) == QStringLiteral("/catalog/movie/top/genre=Action&skip=100.json"),
              "extras are emitted in sorted key order");

        // A value containing the separators must not be able to forge extra params.
        QMap<QString, QString> nasty;
        nasty.insert(QStringLiteral("search"), QStringLiteral("a b&c=d"));
        CHECK(catalogPath(c, nasty) == QStringLiteral("/catalog/movie/top/search=a%20b%26c%3Dd.json"),
              "spaces, & and = in a value are percent-encoded");

        // Only VALUES were exercised above, and every key in this file is unreserved — so the encoder
        // could be dropped from the key path and from type/id entirely without a single check moving.
        // A catalog id and an extra KEY that both need escaping pin those two call sites.
        Catalog wild;
        wild.type = QStringLiteral("movie");
        wild.id   = QStringLiteral("top rated&x");
        QMap<QString, QString> wildkey;
        wildkey.insert(QStringLiteral("release year"), QStringLiteral("2019"));
        CHECK(catalogPath(wild, wildkey)
                  == QStringLiteral("/catalog/movie/top%20rated%26x/release%20year=2019.json"),
              "the catalog id AND the extra key are percent-encoded, not just the value");

        // An empty value carries no information and must not become "key=" — some addons 400 on it.
        QMap<QString, QString> blank;
        blank.insert(QStringLiteral("search"), QString());
        CHECK(catalogPath(c, blank) == QStringLiteral("/catalog/movie/top.json"),
              "an empty value is dropped rather than emitted as a bare key=");

        // Presets fill gaps; a caller value REPLACES rather than joins.
        Catalog g = c;
        g.presets.insert(QStringLiteral("genre"), QStringLiteral("Action"));
        CHECK(catalogPath(g, {}) == QStringLiteral("/catalog/movie/top/genre=Action.json"),
              "a preset supplies the default");
        QMap<QString, QString> chosen;
        chosen.insert(QStringLiteral("genre"), QStringLiteral("Comedy"));
        CHECK(catalogPath(g, chosen) == QStringLiteral("/catalog/movie/top/genre=Comedy.json"),
              "the caller's value replaces the preset, never appends");
    }

    // ------------------------------------------------- 10. handlesId
    {
        Manifest m;
        m.resources << QStringLiteral("stream") << QStringLiteral("meta");
        m.idPrefixes << QStringLiteral("tt");
        m.resourceIdPrefixes.insert(QStringLiteral("stream"),
                                    { QStringLiteral("kitsu:") });

        CHECK(handlesId(m, QStringLiteral("stream"), QStringLiteral("kitsu:1234")),
              "the per-resource prefix matches");
        CHECK(!handlesId(m, QStringLiteral("stream"), QStringLiteral("tt0903747")),
              "the per-resource list OVERRIDES the manifest-level one for that resource");
        CHECK(handlesId(m, QStringLiteral("meta"), QStringLiteral("tt0903747")),
              "a resource with no override falls back to manifest-level");
        CHECK(!handlesId(m, QStringLiteral("meta"), QStringLiteral("kitsu:1")),
              "…and is filtered by it");

        Manifest open;
        open.resources << QStringLiteral("stream");
        CHECK(handlesId(open, QStringLiteral("stream"), QStringLiteral("anything")),
              "an addon declaring NO prefixes answers for everything");
    }

    // ------------------------------------------------- 11. parseStreams
    {
        // The torrent row's title deliberately carries NO size token, and its videoSize renders as
        // "3.0 GB" — a string that appears nowhere in the fixture. describe() copies the title verbatim,
        // so a size that merely echoes the title (the old "2.1 GB") asserted nothing: delete the size
        // branch and the copied title still satisfied it.
        const QByteArray body = R"({"streams":[
          { "name": "Torrentio\n1080p", "title": "Movie.2019.1080p.x265\n👤 42",
            "infoHash": "0123456789abcdef0123456789abcdef01234567", "fileIdx": 0,
            "behaviorHints": { "bingeGroup": "torrentio|1080p|x265", "videoSize": 3221225472 } },
          { "name": "Direct", "url": "https://example.com/a.mkv", "mime": "video/x-matroska",
            "behaviorHints": { "notWebReady": true } },
          { "name": "DescOnly", "description": "Fallback.Release.720p 👤 7 💾 1.5 GB",
            "infoHash": "89abcdef0123456789abcdef0123456789abcdef",
            "behaviorHints": { "videoSize": 3221225472 } },
          { "name": "Broken", "infoHash": "#", "title": "please report this issue" },
          { "name": "Nothing useful", "title": "no url and no hash" }
        ]})";
        const QVector<StreamCandidate> v = parseStreams(body);
        CHECK(v.size() == 3, "rows with neither a usable url nor a valid infoHash are dropped");

        // Sorted: direct http before torrent.
        CHECK(v[0].isDirect(), "a direct url sorts ahead of a torrent");
        CHECK(v[0].notWebReady, "stream-level behaviorHints.notWebReady is kept");
        CHECK(v[0].mime == QStringLiteral("video/x-matroska"), "mime is kept");

        const StreamCandidate& t = v[1];
        CHECK(t.name.contains(QStringLiteral("1080p")), "name is kept (it carries the quality)");
        CHECK(t.title.contains(QStringLiteral("x265")), "title is kept (it carries the release)");
        CHECK(t.bingeGroup == QStringLiteral("torrentio|1080p|x265"), "bingeGroup is kept");
        CHECK(t.videoSize == 3221225472LL, "videoSize is kept");
        CHECK(t.seeders == 42, "seeders are scraped out of the title");
        CHECK(t.fileIdx == 0, "fileIdx is kept");

        // The size comes from behaviorHints, so it is genuinely additive here — and "3.0 GB" appears
        // nowhere in the copied title, which is what makes this fail if the size branch is deleted.
        CHECK(describe(t).contains(QStringLiteral("3.0 GB")),
              "describe appends the behaviorHints size when the title carries none");
        // Seeders are scraped FROM the title being displayed, so appending them can only duplicate it.
        CHECK(!describe(t).contains(QStringLiteral("seeders")),
              "describe never appends the seeder count — the title it renders already shows it");
        // describe must be ONE line — NavMenu word-wraps, and a row with embedded newlines reads as junk.
        CHECK(!describe(t).contains(QLatin1Char('\n')), "describe collapses the addon's newlines");

        // An addon that puts its release line in `description` instead of `title`. Without the fallback
        // it loses ALL picker text and — because the scrape reads `title` — its whole catalogue lands at
        // -1 seeders and orders by size alone.
        const StreamCandidate& d = v[2];
        CHECK(d.title.startsWith(QStringLiteral("Fallback.Release.720p")),
              "an empty title falls back to `description`");
        CHECK(d.seeders == 7, "…and the fallen-back text is scraped for seeders like any other title");
        // fileIdx is read behind a contains() guard. Drop it and an absent field becomes 0, which
        // AddonManager reads as "play files[0]" — the sample or the NFO in a season pack — instead of
        // falling back to the largest video file.
        CHECK(d.fileIdx == -1, "an absent fileIdx stays -1, it does not collapse to file 0");
        // This row's own text already carries "1.5 GB", so the behaviorHints size must NOT be appended.
        CHECK(!describe(d).contains(QStringLiteral("3.0 GB")),
              "the size is suppressed when the title already carries one");
    }

    // ------------------------------------------------- 11a. describe() is capped to ONE picker row
    {
        // A real raw-torrent release line. Flattening newlines keeps a row one PARAGRAPH; only the length cap
        // keeps it one ROW — NavMenu word-wraps, so an over-long candidate silently occupies two rows and the
        // list stops being scannable. Asserted against kMaxDescribeChars (not a literal) so the cap and the
        // contract move together.
        StreamCandidate longRow;
        longRow.name  = QStringLiteral("Torrentio\n1080p");
        longRow.title = QStringLiteral(
            "The.Very.Long.Release.Name.2019.1080p.BluRay.REMUX.AVC.DTS-HD.MA.7.1-GROUPNAME.Extended."
            "Directors.Cut.With.Commentary.And.A.Great.Many.Further.Tokens.Nobody.Reads 👤 42");
        longRow.url = QStringLiteral("https://example.com/a.mkv");
        CHECK(longRow.title.size() > kMaxDescribeChars, "the fixture is actually longer than the cap");

        const QString row = describe(longRow);
        CHECK(row.size() <= kMaxDescribeChars,
              "describe elides to kMaxDescribeChars so one candidate stays one row");
        CHECK(row.endsWith(QChar(0x2026)), "an elided row says so with an ellipsis");
        CHECK(!row.contains(QLatin1Char('\n')), "an elided row is still one line");

        // A row that already fits is untouched — the cap must not trim or ellipsise short candidates.
        StreamCandidate shortRow;
        shortRow.name = QStringLiteral("1080p");
        shortRow.title = QStringLiteral("Short.Release.x265");
        shortRow.url = QStringLiteral("https://example.com/b.mkv");
        CHECK(describe(shortRow) == QStringLiteral("1080p · Short.Release.x265"),
              "a row within the cap is rendered verbatim");
    }

    // ------------------------------------------------- 11b. seeder scraping is PRIORITY, not leftmost
    {
        // One combined alternation is leftmost-wins, so a season token pre-empts the real marker whenever
        // it sits further left — and a release that reads as 2 seeders sorts below every unknown (-1)
        // row, so auto-play silently lands on a different, possibly dead torrent.
        auto seedersOf = [](const QByteArray& title) {
            const QByteArray b = QByteArrayLiteral("{\"streams\":[{\"name\":\"x\",\"title\":\"") + title
                + "\",\"infoHash\":\"0123456789abcdef0123456789abcdef01234567\"}]}";
            const QVector<StreamCandidate> got = parseStreams(b);
            return got.size() == 1 ? got.at(0).seeders : -2;
        };
        const QByteArray person = QByteArrayLiteral("\xF0\x9F\x91\xA4");   // the seeder emoji
        const QByteArray disk   = QByteArrayLiteral("\xF0\x9F\x92\xBE");   // the size emoji

        CHECK(seedersOf(QByteArrayLiteral("Show S:2 E:5 ") + person + " 500 " + disk + " 12 GB") == 500,
              "the seeder emoji wins over an S: season token sitting to its LEFT");
        CHECK(seedersOf(QByteArrayLiteral("1080p S:2 E:5")) == 2,
              "with no stronger marker present the bare s: form is still read");
        CHECK(seedersOf(QByteArrayLiteral("S: 42 | L: 3")) == 42,
              "the legitimate bare s: form — the reason that alternative exists at all");
        CHECK(seedersOf(QByteArrayLiteral("Seeders: 9 | Peers: 3")) == 9, "the spelled-out form");

        // Verified-clean shapes that must keep reading as unknown: the (?<![a-z]) lookbehind and the
        // required colon are what protect them.
        CHECK(seedersOf(QByteArrayLiteral("Movie.2019 2.1 GB")) == -1, "a size is not a seeder count");
        CHECK(seedersOf(QByteArrayLiteral("Movie.2019.1080p")) == -1, "a resolution is not one either");
        CHECK(seedersOf(QByteArrayLiteral("Show.S01E05.2160p")) == -1, "nor a scene episode token");
        CHECK(seedersOf(QByteArrayLiteral("Sens8.S01 1080p")) == -1,
              "…nor a season token glued to a title that ends in s");
    }

    // ------------------------------------------------- 12. sort order, cap, and pickAuto
    {
        QByteArray body = QByteArrayLiteral("{\"streams\":[");
        for (int i = 0; i < 50; ++i)
            body += QByteArray("{\"name\":\"t") + QByteArray::number(i)
                  + "\",\"title\":\"\xF0\x9F\x91\xA4 " + QByteArray::number(i)
                  + "\",\"infoHash\":\"0123456789abcdef0123456789abcdef0123456" + (i % 10 ? "7" : "8")
                  + "\"},";
        body.chop(1);
        body += "]}";
        const QVector<StreamCandidate> v = parseStreams(body);
        CHECK(v.size() == kMaxStreamRows, "the candidate list is capped");
        CHECK(v[0].seeders >= v[1].seeders, "higher seeders sort first");
        // The cap must be applied AFTER the sort. Capping first also leaves a descending list — so the
        // check above passes on that mutation, and only the actual best row's value catches it.
        CHECK(v[0].seeders == 49 && v[v.size() - 1].seeders == 20,
              "the cap keeps the BEST rows, not the first 30 the addon happened to list");

        // pickAuto: a remembered bingeGroup wins over sort order; a miss falls back to the first row.
        QVector<StreamCandidate> three;
        StreamCandidate a; a.url = QStringLiteral("https://a"); a.bingeGroup = QStringLiteral("g1");
        StreamCandidate b; b.url = QStringLiteral("https://b"); b.bingeGroup = QStringLiteral("g2");
        // The third carries NO bingeGroup, and sits LAST. That is what gives the empty-preferGroup guard
        // teeth: without the guard the match loop runs with preferGroup == "", finds this row's empty
        // bingeGroup and returns 2 — handing "no preference" the worst row instead of the best one.
        // With both candidates grouped (as this fixture used to be) the guard could be deleted unnoticed.
        StreamCandidate c; c.url = QStringLiteral("https://c");
        three << a << b << c;
        CHECK(pickAuto(three, QStringLiteral("g2")) == 1, "a remembered bingeGroup is chosen");
        CHECK(pickAuto(three, QStringLiteral("nope")) == 0, "an unmatched group falls back to the first");
        CHECK(pickAuto(three, QString()) == 0,
              "no preference -> the BEST row, not the first one that happens to have no group");
        CHECK(pickAuto({}, QStringLiteral("g1")) == -1, "nothing playable -> -1");
    }

    // ------------------------------------------------- 13. routeProviders: the id filter and its fallback
    {
        auto mf = [](const QString& res, const QStringList& types, const QStringList& prefixes) {
            Manifest m;
            m.resources = QStringList{res};
            m.types = types;
            m.idPrefixes = prefixes;
            return m;
        };
        const QStringList movie{QStringLiteral("movie")};
        QVector<Manifest> ms;
        ms << mf(QStringLiteral("stream"), movie, {QStringLiteral("tt")})        // 0: imdb only
           << mf(QStringLiteral("stream"), movie, {QStringLiteral("kitsu:")})    // 1: anime only
           << mf(QStringLiteral("catalog"), movie, {QStringLiteral("tt")})       // 2: wrong resource
           << mf(QStringLiteral("stream"), {QStringLiteral("series")}, {QStringLiteral("tt")}); // 3: wrong type

        bool fell = true;
        QVector<int> r = routeProviders(ms, QStringLiteral("stream"), QStringLiteral("movie"),
                                        QStringLiteral("tt0133093"), &fell);
        CHECK(r == (QVector<int>{0}), "only the stream addon of this type that claims the id space");
        CHECK(!fell, "…and that is a real match, not the fallback");

        r = routeProviders(ms, QStringLiteral("stream"), QStringLiteral("movie"),
                           QStringLiteral("kitsu:1"), &fell);
        CHECK(r == (QVector<int>{1}), "a different id space selects a different addon");
        CHECK(!fell, "still a real match");

        // THE FALLBACK, the reason routing is allowed to exist at all: an id no manifest claims must still
        // reach every stream provider. Without it a mis-declared idPrefixes makes an item silently unplayable.
        r = routeProviders(ms, QStringLiteral("stream"), QStringLiteral("movie"),
                           QStringLiteral("weird:99"), &fell);
        CHECK(r == (QVector<int>{0, 1}), "an unclaimed id falls back to ASKING EVERY stream provider");
        CHECK(fell, "and says so, so the log can explain it");
        // The fallback widens the ID filter ONLY. A resource/type mismatch is not a routing guess, it is the
        // addon saying it does not serve this at all — indices 2 and 3 must stay out even here.
        CHECK(!r.contains(2) && !r.contains(3),
              "the fallback does not resurrect the wrong resource or the wrong type");

        // An addon declaring NO prefixes claims everything, so it absorbs the unclaimed id on its own merits.
        // That is a match, not a fallback — and the addons that DID declare a mismatching prefix stay out.
        QVector<Manifest> withOpen = ms;
        withOpen << mf(QStringLiteral("stream"), movie, {});                      // 4: claims everything
        r = routeProviders(withOpen, QStringLiteral("stream"), QStringLiteral("movie"),
                           QStringLiteral("weird:99"), &fell);
        CHECK(r == (QVector<int>{4}) && !fell,
              "an addon claiming no prefixes takes the unclaimed id WITHOUT triggering the fallback");

        // Nothing offers the resource -> empty, and NOT flagged as a fallback: there was nothing to widen to.
        QVector<Manifest> none;
        none << mf(QStringLiteral("catalog"), movie, {});
        r = routeProviders(none, QStringLiteral("stream"), QStringLiteral("movie"), QStringLiteral("tt1"), &fell);
        CHECK(r.isEmpty() && !fell, "no provider offers streams at all -> empty, and not a fallback");
        CHECK(routeProviders({}, QStringLiteral("stream"), QStringLiteral("movie"), QStringLiteral("tt1")).isEmpty(),
              "an empty roster is handled without the out-param");

        // A per-resource idPrefixes narrows what the manifest-level list claims (handlesId's rule, reached
        // through the router — the seam AddonManager actually calls).
        Manifest perRes = mf(QStringLiteral("stream"), movie, {QStringLiteral("tt")});
        perRes.resourceIdPrefixes.insert(QStringLiteral("stream"), {QStringLiteral("kitsu:")});
        QVector<Manifest> two; two << perRes;
        CHECK(routeProviders(two, QStringLiteral("stream"), QStringLiteral("movie"), QStringLiteral("kitsu:1"))
                  == (QVector<int>{0}), "a per-resource prefix list routes on its own terms");
    }

    // ------------------------------------------------- 14. mergeCandidates: the AGGREGATE across addons
    {
        // Two addons' blocks, EACH ALREADY SORTED, exactly as listStremioStreams hands them over: addon A's
        // torrents (90, 40) and addon B's instant http url ahead of its own torrent (70). Both blocks are
        // internally correct, so a plain concatenation reads as sorted — and puts A's cold torrent first.
        // Merging is what stops auto-play taking it over B's instant stream.
        QVector<StreamCandidate> a, b;
        StreamCandidate a1; a1.infoHash = QStringLiteral("a"); a1.seeders = 90;
        StreamCandidate a2; a2.infoHash = QStringLiteral("b"); a2.seeders = 40;
        StreamCandidate b1; b1.url = QStringLiteral("https://b/direct");
        StreamCandidate b2; b2.infoHash = QStringLiteral("c"); b2.seeders = 70;
        a << a1 << a2;
        b << b1 << b2;
        // Sanity: each block IS sorted on its own, so the failure this pins is the merge's, not the fixture's.
        QVector<StreamCandidate> aSorted = a, bSorted = b;
        sortCandidates(aSorted); sortCandidates(bSorted);
        CHECK(aSorted[0].infoHash == a[0].infoHash && bSorted[0].url == b[0].url,
              "both fixture blocks are already individually sorted");

        const QVector<StreamCandidate> agg = mergeCandidates({a, b});
        CHECK(agg.size() == 4, "the merge keeps every row from every addon");
        // THE assertion: delete the sort inside mergeCandidates and this is the line that fails.
        CHECK(agg[0].url == QStringLiteral("https://b/direct"),
              "the second addon's instant url outranks the first addon's best torrent");
        CHECK(agg[1].seeders == 90 && agg[2].seeders == 70 && agg[3].seeders == 40,
              "and the torrents interleave by seeders ACROSS addons");
        CHECK(pickAuto(agg, QString()) == 0, "so auto-play lands on the instant url");

        // Degenerate shapes the aggregator really does hand it: no providers, and providers that answered
        // with nothing (a failed request leaves an empty slot in place).
        CHECK(mergeCandidates({}).isEmpty(), "no providers -> nothing");
        CHECK(mergeCandidates({{}, {}}).isEmpty(), "providers that all answered empty -> nothing");
        CHECK(mergeCandidates({{}, a, {}}).size() == 2, "empty slots do not disturb the rows that arrived");
    }

    // -------------------------------------- 14b. the per-response cap is a PARAMETER, not the debrid bound
    {
        // 50 rows from ONE addon — the common single-stream-addon setup. The picker's bound must still be 30,
        // but the resolution path asks for more so the batch cached-check can reach a release ranked 31-60.
        // With the cap hardcoded to kMaxStreamRows, a user whose only cached torrent sat at row 31 went from
        // playing fine to "nothing cached".
        QByteArray body = QByteArrayLiteral("{\"streams\":[");
        for (int i = 0; i < 50; ++i)
            body += QByteArray("{\"name\":\"t") + QByteArray::number(i)
                  + "\",\"title\":\"\xF0\x9F\x91\xA4 " + QByteArray::number(i)
                  + "\",\"infoHash\":\"0123456789abcdef0123456789abcdef0123456" + (i % 10 ? "7" : "8")
                  + "\"},";
        body.chop(1);
        body += "]}";
        CHECK(parseStreams(body).size() == kMaxStreamRows, "the default is still the picker's bound");
        const QVector<StreamCandidate> deep = parseStreams(body, 60);
        CHECK(deep.size() == 50, "a larger bound keeps every row the addon offered");
        CHECK(deep[0].seeders == 49 && deep[30].seeders == 19,
              "…including the ones past row 30, still in rank order");
        CHECK(parseStreams(body, 5).size() == 5, "and a smaller bound is honoured too");
    }

    // ---------------------------------- 14c. behaviorHints.proxyHeaders: parsing (#43)
    {
        const QByteArray body = R"({"streams":[{
          "url": "https://cdn.example.test/a.mp4",
          "name": "Direct",
          "behaviorHints": {
            "proxyHeaders": {
              "request":  { "Referer": "https://embed.example.test/watch",
                            "user-agent": "EB-Probe/1.0",
                            "X-Token": "abc,def" },
              "response": { "Content-Type": "video/mp4" }
            }
          }
        }]})";
        const QVector<StreamCandidate> v = parseStreams(body);
        CHECK(v.size() == 1, "the header-gated stream parses");
        const StreamHeaders::Headers h = v.value(0).requestHeaders;
        CHECK(h.size() == 3, "all three request headers are carried onto the candidate");
        CHECK(h.value(QStringLiteral("Referer")) == QStringLiteral("https://embed.example.test/watch"),
              "Referer survives verbatim");
        // Name canonicalisation is not cosmetic: everything downstream (the mpv split, the field list, the
        // log summary) looks these up by name, so "user-agent" and "User-Agent" MUST be one key.
        CHECK(h.value(QStringLiteral("User-Agent")) == QStringLiteral("EB-Probe/1.0"),
              "a lowercase name is canonicalised, not carried as a second key");
        CHECK(h.value(QStringLiteral("X-Token")) == QStringLiteral("abc,def"),
              "a value containing a comma is carried whole");
        // The `response` half describes what the addon expects BACK. Carrying it would only create a second
        // thing to accidentally send.
        CHECK(!h.contains(QStringLiteral("Content-Type")), "the response half is not carried");

        // A stream with no behaviorHints at all carries no headers — the overwhelmingly common case, and the
        // one that has to stay empty rather than picking up a default.
        CHECK(parseStreams(R"({"streams":[{"url":"https://h.test/b.mp4"}]})").value(0).requestHeaders.isEmpty(),
              "a stream without proxyHeaders carries none");

        // canonicalName: mpv's option is spelled `referrer`, HTTP's header is `Referer`. Folding them means a
        // stream cannot end up with two spellings holding two different values.
        CHECK(StreamHeaders::canonicalName(QStringLiteral("referrer")) == QStringLiteral("Referer"),
              "the two-r spelling folds onto the HTTP one");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X-FORWARDED-for")) == QStringLiteral("X-Forwarded-For"),
              "each dash-separated part is capitalised");

        // The RFC 7230 `token` charset, asserted on canonicalName directly because that is where the rule
        // lives and every consumer of a header name goes through it. The parse-level fixture in 14d pins
        // the consequence; these pin the rule, one refused character class at a time.
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X-Ok_1.2*")) == QStringLiteral("X-Ok_1.2*"),
              "the punctuation RFC 7230 actually permits is not refused");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X-A\r\nRange")).isEmpty(),
              "a name carrying CRLF is refused outright — the request-smuggling primitive");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X A")).isEmpty(), "…as is one containing a space");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X:A")).isEmpty(), "…or its own colon");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X-Café")).isEmpty(),
              "…or a non-ASCII letter, which QChar::isLetterOrNumber would have waved through");
        CHECK(StreamHeaders::canonicalName(QString(QChar(u'\0'))).isEmpty(),
              "…or a lone NUL, which strchr's terminator match would otherwise accept");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("  Referer  ")) == QStringLiteral("Referer"),
              "surrounding whitespace is still tolerated — the charset rule applies to the trimmed name");
    }

    // ---------------------------------- 14d. proxyHeaders: what is REFUSED on the way in (#43)
    {
        const QByteArray body = R"({"streams":[{
          "url": "https://cdn.example.test/a.mp4",
          "behaviorHints": { "proxyHeaders": { "request": {
            "Referer":   "https://ok.example.test/",
            "Host":      "victim.example.test",
            "Range":     "bytes=0-99",
            "Content-Length": "12",
            "Connection": "keep-alive",
            "Transfer-Encoding": "chunked",
            "Upgrade":   "websocket",
            "X-Inject":  "v\r\nX-Evil: 1",
            "X-Empty":   "",
            "":          "nameless",
            "X-Number":  7,
            "X-Smuggle\r\nRange": "bytes=0-1",
            "X Spaced":  "v",
            "X-Colon: Y": "v",
            "X-é":  "v"
          } } }
        }]})";
        const StreamHeaders::Headers h = parseStreams(body).value(0).requestHeaders;
        CHECK(h.keys() == QStringList{ QStringLiteral("Referer") },
              "exactly one header survives the filter — every other entry above is refused");
        // Named individually, because a single blanket assertion above would still pass if the filter kept
        // the WRONG one of them, and each of these is refused for its own reason.
        CHECK(!h.contains(QStringLiteral("Host")), "Host is refused: it re-points the request");
        CHECK(!h.contains(QStringLiteral("Range")),
              "Range is refused: the player issues its own per seek, and a pinned one freezes playback");
        CHECK(!h.contains(QStringLiteral("Content-Length")) && !h.contains(QStringLiteral("Connection"))
                  && !h.contains(QStringLiteral("Transfer-Encoding")) && !h.contains(QStringLiteral("Upgrade")),
              "hop-by-hop / body-shaping fields are refused");
        CHECK(!h.contains(QStringLiteral("X-Inject")),
              "a value containing CRLF is refused — one field must not become two");
        // The NAME half of the same guard, and the sharper one. trimmed() strips only leading/trailing
        // whitespace, so an EMBEDDED CRLF used to survive canonicalisation, miss the blocklist (the key is
        // not spelled `range`) and reach mpv's field list verbatim — which puts the bytes on the socket.
        // Asserted three ways because each defeats the filter differently.
        // Matched case-INSENSITIVELY and by substring, deliberately. Asserting `!h.contains("X-Spaced")`
        // would be inert: canonicalName only capitalises after a dash, so a surviving mangled key is
        // spelled "X spaced" / "X-Smuggle\r\nrange" and an exact-spelling assertion would pass while the
        // field sat in the map. (Both of these WERE inert until mutation C1-a showed them staying green.)
        CHECK(h.keys().filter(QStringLiteral("smuggle"), Qt::CaseInsensitive).isEmpty()
                  && h.keys().filter(QStringLiteral("range"), Qt::CaseInsensitive).isEmpty(),
              "a blocked field smuggled as a CRLF continuation of a permitted name does not survive — "
              "under any spelling");
        CHECK(h.keys().filter(QStringLiteral("spaced"), Qt::CaseInsensitive).isEmpty(),
              "a name containing a space is refused, not repaired into a legal one");
        CHECK(h.keys().filter(QStringLiteral("colon"), Qt::CaseInsensitive).isEmpty(),
              "a name carrying its own colon is refused — it would write two fields");
        CHECK(h.keys().filter(QStringLiteral("x-"), Qt::CaseInsensitive).isEmpty(),
              "…so no X- name from this fixture reaches the map at all");
        CHECK(!h.contains(QStringLiteral("X-Empty")), "an empty value is refused");
        // A number/array/object value stringifies to "" and is refused by that same rule — asserted because
        // it is the OBSERVABLE behaviour an addon can trip, not because a separate guard implements it.
        CHECK(!h.contains(QStringLiteral("X-Number")), "a non-string value is refused too");
    }

    // ---------------------------------- 14e. forPlayUrl: headers belong to ONE origin (#43)
    {
        StreamHeaders::Headers h;
        h.insert(QStringLiteral("Referer"), QStringLiteral("https://a.test/watch"));
        const QString declared = QStringLiteral("https://cdn.a.test:8443/movie.mp4");

        CHECK(StreamHeaders::forPlayUrl(h, declared, declared) == h,
              "the url they were declared for keeps them");
        CHECK(StreamHeaders::forPlayUrl(h, declared, QStringLiteral("https://cdn.a.test:8443/other.mp4")) == h,
              "…as does another path on the same origin");
        // THE hygiene assertion. A debrid/CDN substitute is a different host, and sending host A's Referer to
        // host B both leaks where the user came from and gets refused for a reason nobody would guess.
        CHECK(StreamHeaders::forPlayUrl(h, declared, QStringLiteral("https://cdn.b.test:8443/movie.mp4")).isEmpty(),
              "a DIFFERENT host gets none of them");
        CHECK(StreamHeaders::forPlayUrl(h, declared, QStringLiteral("https://cdn.a.test/movie.mp4")).isEmpty(),
              "a different PORT on the same host gets none of them");
        CHECK(StreamHeaders::forPlayUrl(h, declared, QStringLiteral("http://cdn.a.test:8443/movie.mp4")).isEmpty(),
              "a downgrade to http gets none of them");
        // Default ports are made explicit on both sides, so the two spellings of one origin still match.
        CHECK(StreamHeaders::forPlayUrl(h, QStringLiteral("https://a.test/x"),
                                        QStringLiteral("https://a.test:443/x")) == h,
              "an implicit and an explicit default port are the same origin");
        // Two ways to be originless, pinned separately because ONE of them alone leaves the other's guard
        // untestable: a magnet has no host at all, while an ftp url has a perfectly good host and differs
        // only by being a scheme we do not speak. (The second case was added after mutation testing showed
        // the magnet assertion was being satisfied by the empty-host branch, leaving the scheme check inert.)
        CHECK(StreamHeaders::forPlayUrl(h, QStringLiteral("magnet:?xt=urn:btih:0"),
                                        QStringLiteral("magnet:?xt=urn:btih:0")).isEmpty(),
              "a hostless url can never match — not even itself");
        CHECK(StreamHeaders::forPlayUrl(h, QStringLiteral("ftp://cdn.a.test/movie.mp4"),
                                        QStringLiteral("ftp://cdn.a.test/movie.mp4")).isEmpty(),
              "a non-http scheme can never match either — not even itself");
        // …and a third: an http url whose host is missing. Without this the empty-host guard is unreachable
        // (the magnet above is refused by the scheme check first) and two malformed urls would compare equal.
        CHECK(StreamHeaders::forPlayUrl(h, QStringLiteral("http:///movie.mp4"),
                                        QStringLiteral("http:///movie.mp4")).isEmpty(),
              "an http url with no host is not an origin either");
        // There is deliberately NO "no headers in, none out" assertion here. It existed, and it was inert:
        // with `declared` empty, deleting the early return, inverting it or replacing it with anything at all
        // still yields an empty map, because every other branch returns either {} or the empty input. It
        // pinned nothing and read as coverage. The guard it was aimed at has been removed too — same
        // reasoning as the isString() guard in parseProxyHeaders.
    }

    // ---------------------------------- 14f. applyTo: the player assignment set, and its CLEARS (#43)
    {
        // Record what a player would be told. MpvWidget::play drives the identical call with an mpv-writing
        // sink, so what is asserted here is what reaches libmpv.
        struct Assign { QString property; QStringList values; };
        auto record = [](const StreamHeaders::Headers& h) {
            QVector<Assign> out;
            StreamHeaders::applyTo(h, [&out](const QString& p, const QStringList& v) { out.push_back({ p, v }); });
            return out;
        };
        // Every index below goes through this. CHECK is non-fatal by design — it counts a failure and carries
        // on — so a size assertion does NOT protect the indexing that follows it: the exact regression these
        // assertions exist to catch (applyTo stopping emitting one of the three) would run off the end of the
        // vector and crash the probe instead of printing STREMIO-FAIL. Out of range yields a default Assign,
        // which matches no expectation, so the failure stays a failure and stays readable.
        auto at = [](const QVector<Assign>& v, int i) { return (i >= 0 && i < v.size()) ? v.at(i) : Assign{}; };

        StreamHeaders::Headers h;
        h.insert(QStringLiteral("User-Agent"), QStringLiteral("EB-Probe/1.0"));
        h.insert(QStringLiteral("Referer"), QStringLiteral("https://embed.a.test/watch"));
        h.insert(QStringLiteral("X-Token"), QStringLiteral("abc,def"));
        const QVector<Assign> got = record(h);
        CHECK(got.size() == 3, "exactly three properties are written");
        CHECK(at(got, 0).property == QStringLiteral("user-agent")
                  && at(got, 0).values == QStringList{ QStringLiteral("EB-Probe/1.0") },
              "User-Agent goes to mpv's dedicated user-agent property");
        CHECK(at(got, 1).property == QStringLiteral("referrer")
                  && at(got, 1).values == QStringList{ QStringLiteral("https://embed.a.test/watch") },
              "Referer goes to mpv's dedicated referrer property");
        CHECK(at(got, 2).property == QStringLiteral("http-header-fields"),
              "everything else goes to the header field list");
        CHECK(at(got, 2).values == QStringList{ QStringLiteral("X-Token: abc,def") },
              "…as one 'Name: value' entry, comma in the value and all");
        // Lifting the two out of the field list is what stops mpv sending each of them twice.
        CHECK(!at(got, 2).values.join(QLatin1Char('|')).contains(QStringLiteral("User-Agent"))
                  && !at(got, 2).values.join(QLatin1Char('|')).contains(QStringLiteral("Referer")),
              "the two dedicated ones are NOT also in the field list");

        // THE clear-between-streams assertion. A stream that needs no headers must still produce all three
        // assignments, EMPTY — because "emit nothing when there is nothing" is exactly the bug where the
        // previous source's Referer stays live for the next stream, on a different host.
        const QVector<Assign> cleared = record({});
        CHECK(cleared.size() == 3, "a headerless stream still writes all three properties");
        CHECK(at(cleared, 0).property == QStringLiteral("user-agent") && at(cleared, 0).values.isEmpty(),
              "…user-agent cleared");
        CHECK(at(cleared, 1).property == QStringLiteral("referrer") && at(cleared, 1).values.isEmpty(),
              "…referrer cleared");
        CHECK(at(cleared, 2).property == QStringLiteral("http-header-fields") && at(cleared, 2).values.isEmpty(),
              "…and the field list cleared");
        // The order is fixed, so a caller can never write a stale value after a fresh one.
        CHECK(at(cleared, 0).property == at(got, 0).property && at(cleared, 1).property == at(got, 1).property
                  && at(cleared, 2).property == at(got, 2).property,
              "the same properties in the same order, headers or not");

        // A stream carrying ONLY a User-Agent still clears the referrer the previous one set.
        StreamHeaders::Headers uaOnly;
        uaOnly.insert(QStringLiteral("User-Agent"), QStringLiteral("EB-Probe/2.0"));
        const QVector<Assign> partial = record(uaOnly);
        CHECK(partial.size() == 3, "a partial header set still writes all three properties");
        // The property NAMES are checked alongside the emptiness, and that is not decoration: an out-of-range
        // at() yields a default Assign whose values are also empty, so "cleared" and "never emitted" would be
        // indistinguishable if only emptiness were asserted — the assertion would pass on exactly the
        // regression it exists to catch.
        CHECK(at(partial, 1).property == QStringLiteral("referrer") && at(partial, 1).values.isEmpty(),
              "a partial header set clears the referrer it does not use");
        CHECK(at(partial, 2).property == QStringLiteral("http-header-fields")
                  && at(partial, 2).values.isEmpty(),
              "…and the field list it does not use");
    }

    // ---------------------------------- 14g. logSummary is names-only, and the external-player call (#43)
    {
        StreamHeaders::Headers h;
        h.insert(QStringLiteral("Referer"), QStringLiteral("https://embed.a.test/watch?token=SECRET"));
        h.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer SECRET"));
        const QString s = StreamHeaders::logSummary(h);
        // These values are the whole reason the rule exists: proxyHeaders routinely carry a signed-URL token
        // or a session cookie, and stream_debug.log is a file users paste into bug reports.
        CHECK(!s.contains(QStringLiteral("SECRET")), "no header VALUE reaches the log line");
        CHECK(!s.contains(QStringLiteral("Bearer")), "…not even the scheme half of one");
        CHECK(s.contains(QStringLiteral("Referer")) && s.contains(QStringLiteral("Authorization")),
              "the header NAMES do — that is the diagnosable part");
        CHECK(s.contains(QStringLiteral("2")), "and how many there are");
        CHECK(StreamHeaders::logSummary({}).isEmpty(), "nothing to say about a stream with no headers");

        // The external-player decision. Headers cannot follow a URL out to VLC/MPC-HC/an Android intent, so a
        // gated stream stays in the built-in player (which can satisfy the gate) and the user is told why.
        CHECK(StreamHeaders::externalRoute({}) == StreamHeaders::ExternalRoute::HandOff,
              "an ordinary stream still goes to the configured external player");
        CHECK(StreamHeaders::externalRoute(h) == StreamHeaders::ExternalRoute::FallBackToBuiltin,
              "a header-gated stream does not");
    }

    // ------------------------------------------------- 15. BingeStore
    // (the brief numbered this 10; 10-12 were already taken by handlesId/parseStreams/sort, so it lands later)
    {
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tt0903747:2:7")) == QStringLiteral("tt0903747"),
              "an episode id yields its series key");
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tt0133093")).isEmpty(),
              "a MOVIE id yields no key — binge memory is episodes-only");
        CHECK(BingeStore::seriesKeyFor(QString()).isEmpty(), "empty in, empty out");
        // SHAPE, not just arity — the same landmine MediaSegments::keyFor documents. Without the "tt" test
        // every TMDB-catalogued show keys as "tmdb" and one show's remembered release is handed to another.
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tmdb:tv:1396")).isEmpty(),
              "a 3-part id that is not an imdb episode is NOT a series key");
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tt1:2:7:extra")).isEmpty(),
              "and a longer id is not one either");

        QTemporaryDir tmp;
        CHECK(tmp.isValid(), "temp dir");
        const QString path = QDir(tmp.path()).filePath(QStringLiteral("binge.json"));

        BingeStore st(path);
        st.load();
        CHECK(st.lookup(QStringLiteral("tt1")).isEmpty(), "an empty store has nothing");
        CHECK(st.put(QStringLiteral("tt1"), QStringLiteral("torrentio|1080p")), "put succeeds");
        CHECK(st.lookup(QStringLiteral("tt1")) == QStringLiteral("torrentio|1080p"),
              "and the value is visible on the SAME instance, without a reload");
        CHECK(st.lookup(QStringLiteral("tt-other")).isEmpty(), "an unrelated series is still unknown");

        BingeStore re(path);
        re.load();
        CHECK(re.lookup(QStringLiteral("tt1")) == QStringLiteral("torrentio|1080p"), "it round-trips");
        re.put(QStringLiteral("tt1"), QStringLiteral("torrentio|4k"));
        CHECK(re.lookup(QStringLiteral("tt1")) == QStringLiteral("torrentio|4k"), "the newest choice wins");
        CHECK(!re.put(QString(), QStringLiteral("g")), "an empty key is refused");
        CHECK(!re.put(QStringLiteral("tt2"), QString()), "and so is an empty group");
        CHECK(re.lookup(QStringLiteral("tt2")).isEmpty(), "a refused put stores nothing");

        // load() must REPLACE what is in memory, not merge into it: a store whose file vanished must forget,
        // otherwise a cleared/rewritten file leaves the old choice quietly in effect for the rest of the run.
        CHECK(QFile::remove(path), "remove the backing file");
        re.load();
        CHECK(re.lookup(QStringLiteral("tt1")).isEmpty(), "load() clears what was there before");

        BingeStore missing(QDir(tmp.path()).filePath(QStringLiteral("nope.json")));
        missing.load();
        CHECK(missing.lookup(QStringLiteral("tt1")).isEmpty(), "a missing file loads as empty");

        // A hand-written file carrying values load() cannot use (a number, an empty string) alongside a good
        // one. The unusable keys sort FIRST — QJsonObject iterates by key — so a load() that BAILS on the
        // first value it cannot use, rather than skipping it, loses the entry that follows.
        //
        // That reachability is the only assertable part: a value skipped versus stored as "" is invisible
        // through lookup(), which cannot tell a stored "" from "never chosen". Asserting lookup("bEmpty")
        // is empty would pass either way, so it is deliberately not asserted here.
        const QString hand = QDir(tmp.path()).filePath(QStringLiteral("hand.json"));
        {
            QFile f(hand);
            CHECK(f.open(QIODevice::WriteOnly), "write a hand-made store");
            f.write("{\"aBad\":5,\"bEmpty\":\"\",\"ttA\":\"g1\"}");
        }
        BingeStore h(hand);
        h.load();
        CHECK(h.lookup(QStringLiteral("ttA")) == QStringLiteral("g1"),
              "a usable entry is read even when unusable ones precede it");
        CHECK(h.lookup(QStringLiteral("ttZ")).isEmpty(), "a key the file never had stays unknown");

        // A put whose WRITE fails must not leave memory ahead of disk. The store points into a directory that
        // does not exist, so QSaveFile cannot open and save() returns false; put() has to undo its insert.
        // Otherwise put() reports false while lookup() reports the new value for the rest of the run — and
        // the first later put that DOES succeed persists this one too, silently.
        BingeStore ro(QDir(tmp.path()).filePath(QStringLiteral("no-such-dir/binge.json")));
        ro.load();
        CHECK(!ro.put(QStringLiteral("ttA"), QStringLiteral("g1")), "a put that cannot be written fails");
        CHECK(ro.lookup(QStringLiteral("ttA")).isEmpty(), "…and leaves nothing behind in memory");
        // Same, but OVERWRITING an existing value: the rollback must restore the previous one, not erase it.
        BingeStore ov(QDir(tmp.path()).filePath(QStringLiteral("ov.json")));
        ov.load();
        CHECK(ov.put(QStringLiteral("ttA"), QStringLiteral("good")), "seed a value that did write");
        CHECK(QFile::remove(QDir(tmp.path()).filePath(QStringLiteral("ov.json"))), "drop its file");
        CHECK(QDir().mkpath(QDir(tmp.path()).filePath(QStringLiteral("ov.json"))),
              "…and put a DIRECTORY in its place so the next write cannot succeed");
        CHECK(!ov.put(QStringLiteral("ttA"), QStringLiteral("bad")), "the overwrite fails to write");
        CHECK(ov.lookup(QStringLiteral("ttA")) == QStringLiteral("good"),
              "and the previous choice is restored, not erased");
    }

    // ---------------------------------- 16. the download queue: a start that FAILS must not strand the rest
    // One download runs at a time and pump() is the only thing that picks the next, so every path that takes
    // a job out of the running has to call it. The ones that end a live job do (finishActive, cancel, retry);
    // the ones that refuse a job BEFORE there is a request are the ones to watch, because there is no reply
    // and therefore no finished() coming later to pump on their behalf.
    //
    // A restored queue is the case with teeth: the constructor pumps exactly once, so one job whose folder
    // cannot be written would otherwise hold every job behind it — with no error against them and nothing
    // running — until the user happened to touch the panel and pump it by hand.
    //
    // Driven through queue.json rather than enqueue(), because enqueue() pumps per job: a queue that is
    // Queued all the way down with only ONE pump against it is what a restart produces and nothing else does.
    //
    // No event loop runs here, so the job that does start sends nothing: get() queues the request and returns.
    {
        const QString downloads = AppPaths::dataDir() + QStringLiteral("/downloads");
        CHECK(QDir().mkpath(downloads), "the probe's own downloads folder");

        // An ordinary FILE standing where the failing jobs' folder would have to be. mkpath() cannot make a
        // directory under it and QFile cannot create the .part inside it — on every platform, and without
        // needing a permission the CI box may not be able to set.
        const QString blocked = downloads + QStringLiteral("/blocked");
        {
            QFile b(blocked);
            CHECK(b.open(QIODevice::WriteOnly), "a file standing where a folder is needed");
            b.write("x");
        }
        // Port 1 on loopback: nothing listens, and with no event loop nothing is even attempted.
        const QString url = QStringLiteral("http://127.0.0.1:1/x.bin");
        const QString bad1 = blocked + QStringLiteral("/first.bin");
        const QString bad2 = blocked + QStringLiteral("/second.bin");
        const QString good = downloads + QStringLiteral("/third.bin");

        QJsonArray restored;
        for (const QString& dest : { bad1, bad2, good })
            restored.append(QJsonObject{
                { QStringLiteral("id"), QFileInfo(dest).fileName() },
                { QStringLiteral("title"), QFileInfo(dest).fileName() },
                { QStringLiteral("url"), url },
                { QStringLiteral("dest"), dest },
                { QStringLiteral("kind"), QStringLiteral("video") },
                { QStringLiteral("state"), int(DownloadJob::Queued) } });
        {
            QFile q(downloads + QStringLiteral("/queue.json"));
            CHECK(q.open(QIODevice::WriteOnly), "write the queue a restart would find");
            q.write(QJsonDocument(restored).toJson(QJsonDocument::Compact));
        }

        DownloadManager dm;    // load() + one pump(): the restart
        const QVector<DownloadJob>& jobs = dm.jobs();
        CHECK(jobs.size() == 3, "all three queued jobs came back");
        if (jobs.size() == 3)
        {
            CHECK(jobs.at(0).dest == bad1, "the unwritable one is first, so it is the one that pump picks");
            CHECK(jobs.at(0).state == DownloadJob::Failed, "…and it fails rather than pretending to run");
            CHECK(!jobs.at(0).error.isEmpty(), "…saying so, since nothing else will");
            // THE assertion, twice over. Queued here would mean the queue stopped dead behind a job that is
            // not even running. Two failures in a row also pin that the retry is not a loop: each one is
            // Failed before the next pump, so pump keeps finding a DIFFERENT job and eventually none.
            CHECK(jobs.at(1).state == DownloadJob::Failed,
                  "the second unwritable job is reached too — a refused start does not strand the queue");
            // The control that makes those mean something: a job that CAN be written still starts. Without
            // it, a start() that refused everything would satisfy both assertions above while having broken
            // every download in the app.
            CHECK(jobs.at(2).state == DownloadJob::Active,
                  "…and the startable job behind them runs, so the failures are about the folder alone");
            // Close the job we just started. Not tidiness for its own sake: the manager holds its .part open
            // until the request ends, and on Windows an open handle keeps the probe's scratch directory from
            // being removed when the process exits — one leaked directory per hand-run of this probe.
            const QString startedId = jobs.at(2).id;   // a COPY: cancel() removes the job this points into
            dm.cancel(startedId);
        }
    }

    if (failures) { std::fprintf(stderr, "STREMIO-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("STREMIO-OK\n");
    return 0;
}
