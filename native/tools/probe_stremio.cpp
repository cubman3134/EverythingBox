// Headless coverage for the Stremio protocol translator. Pure — no network, no addon installed.
// Prints STREMIO-OK on success; any failure prints STREMIO-FAIL <what> and exits non-zero.
#include "StremioTranslate.h"

#include <QCoreApplication>
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
        const QByteArray body = R"({"streams":[
          { "name": "Torrentio\n1080p", "title": "Movie.2019.1080p.x265\n👤 42 💾 2.1 GB",
            "infoHash": "0123456789abcdef0123456789abcdef01234567", "fileIdx": 0,
            "behaviorHints": { "bingeGroup": "torrentio|1080p|x265", "videoSize": 2254857830 } },
          { "name": "Direct", "url": "https://example.com/a.mkv", "mime": "video/x-matroska",
            "behaviorHints": { "notWebReady": true } },
          { "name": "Broken", "infoHash": "#", "title": "please report this issue" },
          { "name": "Nothing useful", "title": "no url and no hash" }
        ]})";
        const QVector<StreamCandidate> v = parseStreams(body);
        CHECK(v.size() == 2, "rows with neither a usable url nor a valid infoHash are dropped");

        // Sorted: direct http before torrent.
        CHECK(v[0].isDirect(), "a direct url sorts ahead of a torrent");
        CHECK(v[0].notWebReady, "stream-level behaviorHints.notWebReady is kept");
        CHECK(v[0].mime == QStringLiteral("video/x-matroska"), "mime is kept");

        const StreamCandidate& t = v[1];
        CHECK(t.name.contains(QStringLiteral("1080p")), "name is kept (it carries the quality)");
        CHECK(t.title.contains(QStringLiteral("x265")), "title is kept (it carries the release)");
        CHECK(t.bingeGroup == QStringLiteral("torrentio|1080p|x265"), "bingeGroup is kept");
        CHECK(t.videoSize == 2254857830LL, "videoSize is kept");
        CHECK(t.seeders == 42, "seeders are scraped out of the title");
        CHECK(t.fileIdx == 0, "fileIdx is kept");

        // "42" alone would already be in the copied title, so assert the rendered PHRASE — otherwise this
        // still passes with the seeder part deleted from describe().
        CHECK(describe(t).contains(QStringLiteral("42 seeders")), "describe surfaces the seeder count");
        CHECK(describe(t).contains(QStringLiteral("2.1 GB")), "describe surfaces the size");
        // describe must be ONE line — NavMenu word-wraps, and a row with embedded newlines reads as junk.
        CHECK(!describe(t).contains(QLatin1Char('\n')), "describe collapses the addon's newlines");
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
        three << a << b;
        CHECK(pickAuto(three, QStringLiteral("g2")) == 1, "a remembered bingeGroup is chosen");
        CHECK(pickAuto(three, QStringLiteral("nope")) == 0, "an unmatched group falls back to the first");
        CHECK(pickAuto(three, QString()) == 0, "no preference -> the first");
        CHECK(pickAuto({}, QStringLiteral("g1")) == -1, "nothing playable -> -1");
    }

    if (failures) { std::fprintf(stderr, "STREMIO-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("STREMIO-OK\n");
    return 0;
}
