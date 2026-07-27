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

    if (failures) { std::fprintf(stderr, "STREMIO-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("STREMIO-OK\n");
    return 0;
}
