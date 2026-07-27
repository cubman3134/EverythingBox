# Stremio Addon Translator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish MMV's partial Stremio client so the existing addon ecosystem works properly — catalogs that require an `extra` stop vanishing, stream requests route by `idPrefixes`, streams keep their identity behind a "Choose source…" picker, and `behaviorHints` is acted on.

**Architecture:** All Stremio parsing moves out of `AddonManager` (a ~1500-line networking class) into a pure `StremioTranslate` unit — JSON in, MMV models out, no network. `AddonManager` keeps transport, caching and TorBox resolution and calls into it for every Stremio-shaped decision. A small `BingeStore` remembers the chosen release per series.

**Tech Stack:** Qt 6.8.3 (Core/Network/Gui/Widgets), C++17, MSVC 2022. Headless probes as the test framework.

## Global Constraints

- **Build:** `export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"`; build dir `build`; **always `--config Release`**. App target `mymediavault`.
- **Build ONLY named targets.** Never a target-less `cmake --build build` — it builds ~40 probes and stalls. Adding a source or probe needs exactly ONE reconfigure: `cmake -S native -B build -DMYMEDIAVAULT_BUILD_APP=ON` (no `-A`). Report BLOCKED past ~6 min with no progress.
- **Suite:** `BUILD_DIR=build bash native/tools/run-headless-probes.sh` must print `ALL HEADLESS PROBES PASSED`.
- **Sources are explicit, not globbed** — every new `.cpp`/`.h` goes into `qt_add_executable(mymediavault …)` at `native/CMakeLists.txt:122`.
- **A new probe must be added to THREE places:** its `add_executable` block in `native/CMakeLists.txt`, the runner list at `native/tools/run-headless-probes.sh:119`, and **the CI target list in `.github/workflows/ci.yml`** — an unbuilt probe is a non-fatal `(skip)` in the runner, so a probe missing from CI is silently never run.
- **Nav kit:** modal UI goes through `src/ui/nav` (`NavMenu`/`NavConfirm`/`Osk`) — never `QDialog`/`QMessageBox`/`QInputDialog`/top-level windows. `NavMenu` scrolls when rows overflow.
- **Exact constants:** `kMaxStreamRows = 30`. Stream sort order: **direct http before torrent, then seeders descending, then size descending**; unknown seeders (`-1`) sort last within their group.
- **`idPrefixes` routing must fall back to querying ALL providers when the filter leaves none.** Routing is an optimization and must never be the reason nothing plays.
- **bingeGroup memory is episodes-only.** A movie has no next episode; the store is never consulted or written for a one-part id.
- **Pre-commit hook** auto-bumps the patch version in `native/CMakeLists.txt` + `native/src/main.cpp`; let it. `MMV_NO_VERSION_BUMP=1` skips it for docs-only commits.
- **Commit messages** end with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Refinement of the spec:** the spec gave `Catalog` a single `presetKey`/`presetValue` pair. A catalog may declare **more than one** required extra, so this plan uses `QMap<QString, QString> presets` instead. Everything else matches the spec.

## File Structure

| File | Responsibility |
|---|---|
| `native/src/addons/StremioTranslate.{h,cpp}` | **New.** All Stremio↔MMV translation: `Extra`, `CatalogUse`, `Catalog`, `Manifest`, `StreamCandidate`; `parseManifest`, `catalogPath`, `handlesId`, `parseStreams`, `describe`, `pickAuto`. Pure — no network, no QSettings, no widgets. |
| `native/src/core/BingeStore.{h,cpp}` | **New.** `seriesKey → bingeGroup` as JSON. |
| `native/src/addons/AddonManager.{h,cpp}` | Calls the translator; stores the parsed `Manifest` per addon; extras/filters on catalog requests; `SearchOnly` catalogs into search; `idPrefixes` routing; candidate-returning stream resolution. |
| `native/src/ui/MainWindow.{h,cpp}` | The "Choose source…" action and its `NavMenu` picker. |
| `native/tools/probe_stremio.cpp` | **New.** All pure coverage, against real-manifest-shaped fixtures. Sentinel `STREMIO-OK`. |

---

### Task 1: `StremioTranslate` — manifest parsing and catalog classification

**Files:**
- Create: `native/src/addons/StremioTranslate.h`, `native/src/addons/StremioTranslate.cpp`
- Create: `native/tools/probe_stremio.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh:119`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: the whole header below. Tasks 2–5 depend on these exact names.

- [ ] **Step 1: Write the header**

Create `native/src/addons/StremioTranslate.h`:

```cpp
// Translates the Stremio addon protocol into MMV's models. Pure: JSON in, structs out — no network, no
// QSettings, no widgets, so probe_stremio can assert every rule against real manifest fixtures.
//
// This lives apart from AddonManager deliberately. The Stremio support that preceded it was written inline
// in a ~1500-line networking class and was incomplete BECAUSE it was untestable: required-extra catalogs
// were dropped, idPrefixes was never read, and stream titles were parsed away. Keeping the rules here, in
// a unit with no I/O, is what lets them be pinned.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace StremioTranslate
{
    // One declared extra on a catalog, normalized from BOTH the modern `extra[]` objects and the legacy
    // `extraRequired`/`extraSupported` string arrays, so nothing downstream has to know which form was used.
    struct Extra
    {
        QString     name;              // "search" | "genre" | "skip" | addon-defined
        bool        isRequired = false;
        QStringList options;           // possible values; empty = free-form
        int         optionsLimit = 1;  // how many a user may select (schema default is 1)
    };

    // What MMV can actually do with a declared catalog.
    enum class CatalogUse
    {
        Browse,        // a shelf; any required extra can be satisfied from its options
        SearchOnly,    // requires `search` — answers queries, never a browse shelf
        Unsatisfiable  // requires something we cannot supply — skipped WITH A REASON
    };

    struct Catalog
    {
        QString                 type, id, name;
        QVector<Extra>          extras;
        CatalogUse              use = CatalogUse::Browse;
        QString                 skipReason;   // non-empty only when Unsatisfiable
        QMap<QString, QString>  presets;      // required extra -> its first option (may hold several)

        // "type/id", the form AddonManager already uses to route a Stremio catalog.
        QString routeId() const { return type + QLatin1Char('/') + id; }
    };

    struct Manifest
    {
        QString          id, name, version, description, logo;
        QStringList      types;
        QStringList      resources;   // resource NAMES, from string entries and object entries alike
        QStringList      idPrefixes;  // manifest-level
        // Per-resource overrides from the object form, keyed by resource name. A resource present here
        // uses ITS list; anything absent falls back to the manifest-level list.
        QHash<QString, QStringList> resourceIdPrefixes;
        QHash<QString, QStringList> resourceTypes;
        QVector<Catalog> catalogs;
        bool             configurable = false;
        bool             configurationRequired = false;

        bool isValid() const { return !resources.isEmpty(); }
    };

    // Empty (isValid() == false) when the body is not a Stremio manifest.
    Manifest parseManifest(const QByteArray& body);
}
```

- [ ] **Step 2: Write the failing probe**

Create `native/tools/probe_stremio.cpp`. Fixtures are shaped like the real addons this must survive, not idealized JSON:

```cpp
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

    // ------------------------------------------------- 5. behaviorHints
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
```

- [ ] **Step 3: Wire the build (all THREE places)**

In `native/CMakeLists.txt`, add to `qt_add_executable(mymediavault …)` (starts `:122`), after the `SubtitleCache` lines:

```cmake
        src/addons/StremioTranslate.cpp src/addons/StremioTranslate.h
        src/core/BingeStore.cpp         src/core/BingeStore.h
```

> `BingeStore` is Task 4. Create `BingeStore.{h,cpp}` now as an empty stub (`#pragma once` + includes only) so the app target keeps linking between tasks; Task 4 fills it in.

Add the probe beside `probe_subs` (`:478`):

```cmake
    # Headless test for the Stremio protocol translator: manifest/extras/idPrefixes/stream parsing and the
    # catalog classification. Pure — no network, no addon installed.
    add_executable(probe_stremio tools/probe_stremio.cpp
        src/addons/StremioTranslate.cpp src/addons/StremioTranslate.h)
    target_include_directories(probe_stremio PRIVATE src src/addons)
    target_link_libraries(probe_stremio PRIVATE Qt6::Core)
```

Append to the runner list at `native/tools/run-headless-probes.sh:119`, after `"probe_segments SEGMENTS-OK"`:

```
"probe_stremio STREMIO-OK"
```

And append `probe_stremio` to the `--target` list in `.github/workflows/ci.yml:52`, matching how `probe_segments` appears there.

- [ ] **Step 4: Reconfigure and run — expect RED**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake -S native -B build -DMYMEDIAVAULT_BUILD_APP=ON && cmake --build build --config Release --target probe_stremio
```
Expected: **failure** — `StremioTranslate.cpp` has no implementation yet.

- [ ] **Step 5: Implement `StremioTranslate.cpp` (manifest half)**

```cpp
#include "StremioTranslate.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {

QStringList stringArray(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& e : v.toArray()) if (e.isString()) out << e.toString();
    return out;
}

// The modern `extra[]` objects and the legacy `extraRequired`/`extraSupported` string arrays describe the
// same thing. Normalize to one list so no caller has to branch on which form an addon happened to use.
QVector<StremioTranslate::Extra> parseExtras(const QJsonObject& c)
{
    QVector<StremioTranslate::Extra> out;
    auto find = [&out](const QString& name) -> StremioTranslate::Extra* {
        for (StremioTranslate::Extra& e : out) if (e.name == name) return &e;
        return nullptr;
    };

    for (const QJsonValue& ev : c.value(QStringLiteral("extra")).toArray())
    {
        const QJsonObject eo = ev.toObject();
        StremioTranslate::Extra e;
        e.name = eo.value(QStringLiteral("name")).toString();
        if (e.name.isEmpty()) continue;
        e.isRequired  = eo.value(QStringLiteral("isRequired")).toBool();
        e.options     = stringArray(eo.value(QStringLiteral("options")));
        e.optionsLimit = eo.contains(QStringLiteral("optionsLimit"))
                             ? eo.value(QStringLiteral("optionsLimit")).toInt(1) : 1;
        out.push_back(e);
    }

    // Legacy: names only, no options. Merge rather than replace — a manifest may carry both.
    for (const QString& n : stringArray(c.value(QStringLiteral("extraSupported"))))
        if (!find(n)) { StremioTranslate::Extra e; e.name = n; out.push_back(e); }
    for (const QString& n : stringArray(c.value(QStringLiteral("extraRequired"))))
    {
        if (StremioTranslate::Extra* e = find(n)) e->isRequired = true;
        else { StremioTranslate::Extra e2; e2.name = n; e2.isRequired = true; out.push_back(e2); }
    }
    return out;
}

// Decide what we can do with a catalog, and — when it is browsable only because we can supply a default —
// record that default. A catalog is NEVER dropped here: an Unsatisfiable one keeps a reason so the UI can
// say why it is missing instead of leaving a hole the user cannot explain.
void classify(StremioTranslate::Catalog& c)
{
    using U = StremioTranslate::CatalogUse;
    bool needsSearch = false;
    for (const StremioTranslate::Extra& e : c.extras)
    {
        if (!e.isRequired) continue;
        if (e.name == QStringLiteral("search")) { needsSearch = true; continue; }
        if (e.options.isEmpty())
        {
            c.use = U::Unsatisfiable;
            c.skipReason = QStringLiteral("needs a \"%1\" value the add-on does not list").arg(e.name);
            return;
        }
        c.presets.insert(e.name, e.options.first());
    }
    // Search dominates: a catalog that cannot answer without a query is a search source, not a shelf —
    // even when it also takes a genre we could have defaulted.
    c.use = needsSearch ? U::SearchOnly : U::Browse;
    if (needsSearch) c.presets.clear();
}

} // namespace

StremioTranslate::Manifest StremioTranslate::parseManifest(const QByteArray& body)
{
    Manifest m;
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    // The same detection AddonManager used: a Stremio manifest declares what it serves and for what.
    if (!o.contains(QStringLiteral("resources")) || !o.contains(QStringLiteral("types"))) return m;

    m.id          = o.value(QStringLiteral("id")).toString();
    m.name        = o.value(QStringLiteral("name")).toString(m.id);
    m.version     = o.value(QStringLiteral("version")).toString();
    m.description = o.value(QStringLiteral("description")).toString();
    m.logo        = o.value(QStringLiteral("logo")).toString();
    m.types       = stringArray(o.value(QStringLiteral("types")));
    m.idPrefixes  = stringArray(o.value(QStringLiteral("idPrefixes")));

    // `resources` mixes plain names and objects that scope a single resource. Take the name from both, and
    // keep the object's own types/idPrefixes — those are the per-resource overrides routing needs.
    for (const QJsonValue& r : o.value(QStringLiteral("resources")).toArray())
    {
        if (r.isString()) { m.resources << r.toString(); continue; }
        const QJsonObject ro = r.toObject();
        const QString name = ro.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) continue;
        m.resources << name;
        const QStringList pf = stringArray(ro.value(QStringLiteral("idPrefixes")));
        const QStringList ty = stringArray(ro.value(QStringLiteral("types")));
        if (!pf.isEmpty()) m.resourceIdPrefixes.insert(name, pf);
        if (!ty.isEmpty()) m.resourceTypes.insert(name, ty);
    }

    const QJsonObject bh = o.value(QStringLiteral("behaviorHints")).toObject();
    m.configurable          = bh.value(QStringLiteral("configurable")).toBool();
    m.configurationRequired = bh.value(QStringLiteral("configurationRequired")).toBool();

    for (const QJsonValue& cv : o.value(QStringLiteral("catalogs")).toArray())
    {
        const QJsonObject co = cv.toObject();
        Catalog c;
        c.type   = co.value(QStringLiteral("type")).toString();
        c.id     = co.value(QStringLiteral("id")).toString();
        c.name   = co.value(QStringLiteral("name")).toString(c.type);
        c.extras = parseExtras(co);
        classify(c);
        m.catalogs.push_back(c);
    }
    return m;
}
```

- [ ] **Step 6: Build and run — expect PASS**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_stremio && ./build/Release/probe_stremio.exe
```
Expected: `STREMIO-OK`, exit 0.

- [ ] **Step 7: Confirm the app still links**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target mymediavault
```

- [ ] **Step 8: Commit**

```bash
git add native/src/addons/StremioTranslate.h native/src/addons/StremioTranslate.cpp native/src/core/BingeStore.h native/src/core/BingeStore.cpp native/tools/probe_stremio.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: StremioTranslate — manifest, extras, and catalog classification"
```

---

### Task 2: Request building, id routing, and stream parsing

**Files:**
- Modify: `native/src/addons/StremioTranslate.{h,cpp}`, `native/tools/probe_stremio.cpp`

**Interfaces:**
- Consumes: `Manifest`, `Catalog`, `Extra`, `CatalogUse` (Task 1).
- Produces: `catalogPath`, `handlesId`, `StreamCandidate`, `parseStreams`, `describe`, `pickAuto`, `kMaxStreamRows`. Tasks 3–5 call all of these.

- [ ] **Step 1: Add to the header**

```cpp
    // Past this many rows nobody is choosing. A parse/quota bound, NOT a display bound — NavMenu scrolls.
    constexpr int kMaxStreamRows = 30;

    // "/catalog/{type}/{id}/{k}={v}&{k}={v}.json". Per the protocol the extras are a URL-encoded query
    // string living in a PATH segment. Keys are emitted sorted so the same request always produces the same
    // string — AddonManager keys its result cache on it.
    //
    // `extras` is what the CALLER wants. The catalog's own presets are merged in ONLY where the caller gave
    // no value for that key, so a required `genre` defaults to its first option while a user picking
    // "Comedy" REPLACES that default instead of appearing alongside it.
    QString catalogPath(const Catalog& c, const QMap<QString, QString>& extras);

    // May this addon be asked for `resource` about `id`? Per-resource prefixes win over manifest-level;
    // an addon declaring no prefixes at all is always eligible.
    bool handlesId(const Manifest& m, const QString& resource, const QString& id);

    struct StreamCandidate
    {
        QString url, mime, infoHash;
        int     fileIdx = -1;
        QString name;        // the addon's short label — usually provider and/or quality
        QString title;       // the release line; addons commonly pack size and seeders in here
        QString bingeGroup;  // behaviorHints.bingeGroup — "keep using this source for the next episode"
        bool    notWebReady = false;
        qint64  videoSize = 0;
        int     seeders = -1;   // scraped out of `title` when present; -1 = unknown

        bool isDirect() const { return url.startsWith(QStringLiteral("http")); }
    };

    // Usable candidates only, sorted best-first and capped at kMaxStreamRows.
    QVector<StreamCandidate> parseStreams(const QByteArray& body);

    // One picker row: "1080p · Release.Name.x265 · 42 seeders · 2.1 GB".
    QString describe(const StreamCandidate& c);

    // The automatic choice: a candidate whose bingeGroup matches `preferGroup` (when non-empty), else the
    // first in sorted order. Returns -1 when there is nothing playable.
    int pickAuto(const QVector<StreamCandidate>& all, const QString& preferGroup);
```

- [ ] **Step 2: Write the failing probe sections**

Append to `native/tools/probe_stremio.cpp` before the final `if (failures)`:

```cpp
    // ------------------------------------------------- 6. catalogPath
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

    // ------------------------------------------------- 7. handlesId
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

    // ------------------------------------------------- 8. parseStreams
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

        CHECK(!describe(t).isEmpty() && describe(t).contains(QStringLiteral("42")),
              "describe surfaces the seeder count");
        // describe must be ONE line — NavMenu word-wraps, and a row with embedded newlines reads as junk.
        CHECK(!describe(t).contains(QLatin1Char('\n')), "describe collapses the addon's newlines");
    }

    // ------------------------------------------------- 9. sort order, cap, and pickAuto
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
```

- [ ] **Step 3: Run to see it FAIL**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_stremio
```
Expected: compile errors — none of these functions exist yet.

- [ ] **Step 4: Implement**

Append to `native/src/addons/StremioTranslate.cpp` (add `#include <QRegularExpression>`, `#include <QUrl>`, `#include <algorithm>` at the top):

```cpp
QString StremioTranslate::catalogPath(const Catalog& c, const QMap<QString, QString>& extras)
{
    auto seg = [](const QString& s) {
        return QString::fromUtf8(QUrl::toPercentEncoding(s));
    };
    QString path = QStringLiteral("/catalog/") + seg(c.type) + QLatin1Char('/') + seg(c.id);

    // The caller's values win; presets only fill keys the caller left alone.
    QMap<QString, QString> merged = c.presets;
    for (auto it = extras.constBegin(); it != extras.constEnd(); ++it) merged.insert(it.key(), it.value());

    QStringList parts;
    // QMap iterates in key order, which is what makes this string stable for the result cache.
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it)
    {
        if (it.value().isEmpty()) continue;
        parts << seg(it.key()) + QLatin1Char('=') + seg(it.value());
    }
    if (!parts.isEmpty()) path += QLatin1Char('/') + parts.join(QLatin1Char('&'));
    return path + QStringLiteral(".json");
}

bool StremioTranslate::handlesId(const Manifest& m, const QString& resource, const QString& id)
{
    // A per-resource list REPLACES the manifest-level one for that resource — the object form exists
    // precisely so a stream resource can narrow what the addon as a whole claims.
    const QStringList prefixes = m.resourceIdPrefixes.contains(resource)
                                     ? m.resourceIdPrefixes.value(resource)
                                     : m.idPrefixes;
    if (prefixes.isEmpty()) return true;      // claims nothing in particular -> eligible for everything
    for (const QString& p : prefixes) if (id.startsWith(p)) return true;
    return false;
}

namespace {

// Addons put the seeder count in the title, conventionally after a 👤 (or "Seeders:"/"S:"). Best-effort:
// an unparsed count is -1, which sorts last rather than pretending to be zero.
int scrapeSeeders(const QString& title)
{
    static const QRegularExpression re(
        QStringLiteral("(?:\\x{1F464}|seeders?\\s*:?|(?<![a-z])s\\s*:)\\s*(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch mm = re.match(title);
    return mm.hasMatch() ? mm.captured(1).toInt() : -1;
}

bool validInfoHash(const QString& h)
{
    if (h.size() == 40)
    {
        for (const QChar c : h)
            if (!((c >= '0' && c <= '9') || (c.toLower() >= 'a' && c.toLower() <= 'f'))) return false;
        return true;
    }
    if (h.size() == 32)
    {
        for (const QChar c : h)
        { const QChar u = c.toUpper(); if (!((u >= 'A' && u <= 'Z') || (u >= '2' && u <= '7'))) return false; }
        return true;
    }
    return false;
}

} // namespace

QVector<StremioTranslate::StreamCandidate> StremioTranslate::parseStreams(const QByteArray& body)
{
    QVector<StreamCandidate> out;
    for (const QJsonValue& sv : QJsonDocument::fromJson(body).object()
                                    .value(QStringLiteral("streams")).toArray())
    {
        const QJsonObject s = sv.toObject();
        StreamCandidate c;
        c.url      = s.value(QStringLiteral("url")).toString();
        c.mime     = s.value(QStringLiteral("mime")).toString();
        c.infoHash = s.value(QStringLiteral("infoHash")).toString();
        c.fileIdx  = s.contains(QStringLiteral("fileIdx")) ? s.value(QStringLiteral("fileIdx")).toInt() : -1;
        c.name     = s.value(QStringLiteral("name")).toString();
        c.title    = s.value(QStringLiteral("title")).toString();
        if (c.title.isEmpty()) c.title = s.value(QStringLiteral("description")).toString();

        const QJsonObject bh = s.value(QStringLiteral("behaviorHints")).toObject();
        c.bingeGroup  = bh.value(QStringLiteral("bingeGroup")).toString();
        c.notWebReady = bh.value(QStringLiteral("notWebReady")).toBool();
        c.videoSize   = qint64(bh.value(QStringLiteral("videoSize")).toDouble());
        c.seeders     = scrapeSeeders(c.title);

        if (!c.isDirect() && !validInfoHash(c.infoHash)) continue;  // nothing playable here
        out.push_back(c);
    }

    std::stable_sort(out.begin(), out.end(), [](const StreamCandidate& a, const StreamCandidate& b) {
        if (a.isDirect() != b.isDirect()) return a.isDirect();   // instant beats needing a debrid round-trip
        if (a.seeders != b.seeders)       return a.seeders > b.seeders;
        return a.videoSize > b.videoSize;
    });
    if (out.size() > kMaxStreamRows) out.resize(kMaxStreamRows);
    return out;
}

QString StremioTranslate::describe(const StreamCandidate& c)
{
    // Addons pack several lines into name/title; NavMenu word-wraps, so a row with embedded newlines reads
    // as junk. Collapse to one line and join the parts the user actually chooses on.
    auto flat = [](QString s) { return s.replace(QLatin1Char('\n'), QLatin1Char(' ')).simplified(); };
    QStringList parts;
    if (!c.name.isEmpty())  parts << flat(c.name);
    if (!c.title.isEmpty()) parts << flat(c.title);
    if (c.seeders >= 0)     parts << QStringLiteral("%1 seeders").arg(c.seeders);
    if (c.videoSize > 0)    parts << QStringLiteral("%1 GB")
                                        .arg(double(c.videoSize) / 1073741824.0, 0, 'f', 1);
    if (c.notWebReady)      parts << QStringLiteral("may need an external player");
    return parts.join(QStringLiteral(" · "));
}

int StremioTranslate::pickAuto(const QVector<StreamCandidate>& all, const QString& preferGroup)
{
    if (all.isEmpty()) return -1;
    if (!preferGroup.isEmpty())
        for (int i = 0; i < all.size(); ++i)
            if (all[i].bingeGroup == preferGroup) return i;   // the release the user already chose
    return 0;   // already sorted best-first
}
```

- [ ] **Step 5: Build and run — expect PASS**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_stremio && ./build/Release/probe_stremio.exe
```
Expected: `STREMIO-OK`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add native/src/addons/StremioTranslate.h native/src/addons/StremioTranslate.cpp native/tools/probe_stremio.cpp
git commit -m "feat: Stremio request building, idPrefixes routing, and stream candidates"
```

---

### Task 3: Wire `AddonManager` to the translator

Replaces the inline Stremio parsing with calls into `StremioTranslate`, so required-extra catalogs stop vanishing and genre becomes a real filter.

**Files:**
- Modify: `native/src/addons/AddonManager.h` (store the parsed manifest on `LoadedAddon`)
- Modify: `native/src/addons/AddonManager.cpp` — `parseStremioManifest` (`:299-336`), `stremioCatalogUrl` (`:358-368`), `dispatchRemoteCatalog` (`:938-950`), the catalog-response path (`:944-945` drops filters)

**Interfaces:**
- Consumes: `StremioTranslate::{Manifest, Catalog, CatalogUse, parseManifest, catalogPath}`.
- Produces: `LoadedAddon::stremioManifest` (a `StremioTranslate::Manifest`). Tasks 4–5 read it.

- [ ] **Step 1: Store the parsed manifest, and flag search-only catalogs**

In `native/src/addons/AddonManager.h`, add `#include "StremioTranslate.h"` and, in `struct LoadedAddon` beside `stremioResources`:

```cpp
    // The fully parsed Stremio manifest. stremioResources/stremioTypes above remain as the quick-lookup
    // lists the rest of the class already uses; this carries everything they cannot (extras, idPrefixes,
    // behaviorHints, per-catalog classification).
    StremioTranslate::Manifest stremioManifest;
```

In `native/src/addons/AddonModels.h`, add one field to `struct AddonCatalog` (`:25-30`):

```cpp
    // This catalog cannot answer without a search term, so it is a SEARCH SOURCE and not a browse shelf.
    // It must still be listed — dropping it is what previously made search-only add-ons invisible — so the
    // browse surfaces filter on this instead.
    bool searchOnly = false;
```

Then find every place that builds browse shelves from a source's `catalogs()` — `HomeView.cpp:903-929` is the tab/shelf builder — and skip entries where `searchOnly` is true. Conversely, confirm the search fan-out (`native/src/browse/SearchAggregator.h`) reaches them; it iterates the same catalog lists, so no change should be needed there beyond *not* filtering them out.

> Verify both halves by reading: a search-only catalog must appear in **zero** browse shelves and in the search path. If the search fan-out turns out to filter on something else, say so in your report rather than forcing it.

- [ ] **Step 2: Replace `parseStremioManifest`'s body with a call into the translator**

Rewrite the static at `AddonManager.cpp:299-336` so it delegates. It keeps its existing signature (callers are unchanged) and gains an out-param for the parsed manifest:

```cpp
// Detect + parse a Stremio manifest into one of our AddonManifests. The RULES live in StremioTranslate;
// this only maps the result onto the shapes the rest of AddonManager already consumes.
static bool parseStremioManifest(const QByteArray& json, AddonManifest* outM, QStringList* outRes,
                                 QStringList* outTypes, StremioTranslate::Manifest* outSm)
{
    const StremioTranslate::Manifest sm = StremioTranslate::parseManifest(json);
    if (!sm.isValid()) return false;

    AddonManifest m;
    m.id = sm.id;
    m.name = sm.name;
    m.version = sm.version;
    m.type = QStringLiteral("media-source");   // present it like one of ours
    m.description = sm.description;

    for (const StremioTranslate::Catalog& c : sm.catalogs)
    {
        // Unsatisfiable cannot be asked at all — it stays on stremioManifest only, so Task 5 can explain
        // its absence. SearchOnly IS carried here: it must remain reachable by the search fan-out, and
        // dropping it is what would make a search-only addon invisible all over again. The searchOnly flag
        // is what keeps it out of the browse shelves.
        if (c.use == StremioTranslate::CatalogUse::Unsatisfiable) continue;
        AddonCatalog cat;
        cat.id         = c.routeId();
        cat.type       = c.type;
        cat.name       = c.name;
        cat.searchOnly = (c.use == StremioTranslate::CatalogUse::SearchOnly);
        m.catalogs.push_back(cat);
    }

    *outM = m; *outRes = sm.resources; *outTypes = sm.types; *outSm = sm;
    return true;
}
```

Update its one caller in `buildRemoteAddon` to pass a `StremioTranslate::Manifest` and assign it to `a->stremioManifest`.

- [ ] **Step 3: Route catalog requests through `catalogPath`**

Replace `stremioCatalogUrl` (`:358-368`) with a version that takes the parsed catalog and the caller's extras:

```cpp
// Build the Stremio catalog URL for a route id ("type/id"), using the parsed catalog so its required-extra
// defaults are applied. Falls back to a bare path when the manifest is unknown (a cached manifest from an
// older build), so an upgrade never leaves a catalog unreachable.
static QUrl stremioCatalogUrl(const LoadedAddon* src, const QString& routeId, const QString& query, int page,
                              const QMap<QString, QString>& filters)
{
    QMap<QString, QString> extras = filters;
    if (!query.isEmpty()) extras.insert(QStringLiteral("search"), query);
    if (page > 1)         extras.insert(QStringLiteral("skip"), QString::number((page - 1) * 100));

    for (const StremioTranslate::Catalog& c : src->stremioManifest.catalogs)
        if (c.routeId() == routeId) return QUrl(src->baseUrl + StremioTranslate::catalogPath(c, extras));

    StremioTranslate::Catalog bare;
    const int slash = routeId.indexOf(QLatin1Char('/'));
    bare.type = slash > 0 ? routeId.left(slash) : routeId;
    bare.id   = slash > 0 ? routeId.mid(slash + 1) : QString();
    return QUrl(src->baseUrl + StremioTranslate::catalogPath(bare, extras));
}
```

Update the call in `dispatchRemoteCatalog` (`:942`) to `stremioCatalogUrl(src, catalogId, query, page, filters)`.

- [ ] **Step 4: Stop discarding filters on the Stremio branch**

At `AddonManager.cpp:944-945`, the Stremio branch drops `filters`. Replace that with population from the catalog's extras: for each non-`search`, non-`skip` extra that has `options`, emit a `CatalogFilter` whose `key` is the extra's name, whose `label` is the capitalized name, and whose options are `("", "Any")` followed by each option as `(value, value)` — capped at `optionsLimit` where that is greater than 1, else the full list.

> Read how the non-Stremio branch attaches filters to its `MediaCatalog` and follow that exactly; the filter must round-trip back as the `filters` map that reaches `stremioCatalogUrl`.

- [ ] **Step 5: Build and run the suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target mymediavault && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: clean build, `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 6: Commit**

```bash
git add native/src/addons/AddonManager.h native/src/addons/AddonManager.cpp
git commit -m "feat: route Stremio catalogs through the translator; genre becomes a real filter"
```

---

### Task 4: `BingeStore` + candidate-based stream resolution with `idPrefixes` routing

**Files:**
- Modify: `native/src/core/BingeStore.{h,cpp}` (stubs from Task 1)
- Modify: `native/src/addons/AddonManager.{h,cpp}` — `resolveStremioStream` (`:1161-1243`), `parseStremioStreams` (`:463-477`), `StremioStreamOpt` (`:449`)
- Modify: `native/tools/probe_stremio.cpp`

**Interfaces:**
- Consumes: `StremioTranslate::{StreamCandidate, parseStreams, handlesId, pickAuto}`, `LoadedAddon::stremioManifest`.
- Produces: `BingeStore` (`lookup`/`put`/`load`/`save`), and `AddonManager::listStremioStreams(const MediaItem&, std::function<void(const QVector<StremioTranslate::StreamCandidate>&)>)`. Task 5 calls both.

- [ ] **Step 1: Write `BingeStore.h`**

```cpp
// Remembers which release the user chose for a series, by Stremio's own `bingeGroup` — the mechanism that
// exists so the next episode keeps using the same source. Device-local JSON; never synced.
//
// EPISODES ONLY: a movie has no next episode, so a movie's choice is neither stored nor consulted. Keys are
// the "tt…" prefix of a "tt…:S:E" id — the same series convention MediaSegments::keyFor uses.
// NOT THREAD-SAFE: put() writes the file synchronously on the calling thread. GUI-thread use only.
#pragma once
#include <utility>   // std::move (do not rely on a transitive Qt include)
#include <QHash>
#include <QString>

class BingeStore
{
public:
    explicit BingeStore(QString filePath) : file_(std::move(filePath)) {}
    void load();
    bool save() const;

    // The remembered bingeGroup for this series, or empty.
    QString lookup(const QString& seriesKey) const;
    // Overwrite (the newest choice wins). Returns false on invalid input or a failed write.
    bool put(const QString& seriesKey, const QString& bingeGroup);

    // "tt123:2:7" -> "tt123". Empty for a movie or any id without the S:E tail, which is what makes this
    // episodes-only without the caller having to remember the rule.
    static QString seriesKeyFor(const QString& imdbStreamId);

private:
    QString file_;
    QHash<QString, QString> byKey_;
};
```

- [ ] **Step 2: Write the failing probe section**

Append to `native/tools/probe_stremio.cpp` (add `#include "BingeStore.h"`, `#include <QDir>`, `#include <QTemporaryDir>`):

```cpp
    // ------------------------------------------------- 10. BingeStore
    {
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tt0903747:2:7")) == QStringLiteral("tt0903747"),
              "an episode id yields its series key");
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tt0133093")).isEmpty(),
              "a MOVIE id yields no key — binge memory is episodes-only");
        CHECK(BingeStore::seriesKeyFor(QString()).isEmpty(), "empty in, empty out");

        QTemporaryDir tmp;
        CHECK(tmp.isValid(), "temp dir");
        const QString path = QDir(tmp.path()).filePath(QStringLiteral("binge.json"));

        BingeStore st(path);
        st.load();
        CHECK(st.lookup(QStringLiteral("tt1")).isEmpty(), "an empty store has nothing");
        CHECK(st.put(QStringLiteral("tt1"), QStringLiteral("torrentio|1080p")), "put succeeds");

        BingeStore re(path);
        re.load();
        CHECK(re.lookup(QStringLiteral("tt1")) == QStringLiteral("torrentio|1080p"), "it round-trips");
        re.put(QStringLiteral("tt1"), QStringLiteral("torrentio|4k"));
        CHECK(re.lookup(QStringLiteral("tt1")) == QStringLiteral("torrentio|4k"), "the newest choice wins");
        CHECK(!re.put(QString(), QStringLiteral("g")), "an empty key is refused");

        BingeStore missing(QDir(tmp.path()).filePath(QStringLiteral("nope.json")));
        missing.load();
        CHECK(missing.lookup(QStringLiteral("tt1")).isEmpty(), "a missing file loads as empty");
    }
```

- [ ] **Step 3: Add `BingeStore.cpp` to the probe target and run — expect FAIL**

In `native/CMakeLists.txt`, inside `add_executable(probe_stremio …)`, add:

```cmake
        src/core/BingeStore.cpp src/core/BingeStore.h
```
and `src/core` to its `target_include_directories`. Then:

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake -S native -B build -DMYMEDIAVAULT_BUILD_APP=ON && cmake --build build --config Release --target probe_stremio
```
Expected: compile errors — `BingeStore` has no implementation.

- [ ] **Step 4: Implement `BingeStore.cpp`**

```cpp
#include "BingeStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

void BingeStore::load()
{
    byKey_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;        // no file yet is a normal empty store
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
    {
        const QString v = it.value().toString();
        if (!v.isEmpty()) byKey_.insert(it.key(), v);
    }
}

bool BingeStore::save() const
{
    QJsonObject root;
    for (auto it = byKey_.constBegin(); it != byKey_.constEnd(); ++it) root.insert(it.key(), it.value());
    // QSaveFile, not truncate-then-write: this file is the only record of a choice the user made by hand,
    // and a short write (disk full, AV lock) would otherwise leave truncated JSON that loads as empty.
    // Same reasoning as SegmentStore.
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (f.write(bytes) != bytes.size()) { f.cancelWriting(); return false; }
    return f.commit();
}

QString BingeStore::lookup(const QString& seriesKey) const
{
    return seriesKey.isEmpty() ? QString() : byKey_.value(seriesKey);
}

bool BingeStore::put(const QString& seriesKey, const QString& bingeGroup)
{
    if (seriesKey.isEmpty() || bingeGroup.isEmpty()) return false;
    byKey_.insert(seriesKey, bingeGroup);            // the newest choice wins
    return save();
}

QString BingeStore::seriesKeyFor(const QString& imdbStreamId)
{
    // "tt123:2:7" -> "tt123"; a movie ("tt123") or any other shape -> empty. Returning empty for a movie is
    // what makes this episodes-only WITHOUT every caller having to remember the rule.
    const QStringList p = imdbStreamId.split(QLatin1Char(':'));
    if (p.size() != 3) return QString();
    if (!p[0].startsWith(QLatin1String("tt"))) return QString();
    return p[0];
}
```

Add `#include <QFile>` if `QSaveFile` does not bring it in.

- [ ] **Step 5: Replace `StremioStreamOpt` with `StreamCandidate` and add the listing API**

Delete `StremioStreamOpt` (`:449`) and `parseStremioStreams` (`:463-477`), replacing their uses with `StremioTranslate::StreamCandidate` and `StremioTranslate::parseStreams`. `isValidInfoHash` (`:453-460`) also moves into the translator — delete the local copy rather than keeping two.

Add to `AddonManager`:

```cpp
    // Every candidate stream for an item, from every eligible provider — the picker's source of choices.
    // Ordering and the cap are the translator's; this only aggregates across addons.
    void listStremioStreams(const MediaItem& item,
                            std::function<void(const QVector<StremioTranslate::StreamCandidate>&)> cb);
```

Refactor `resolveStremioStream` so the provider fan-out and collection live in `listStremioStreams`, and `resolveStremioStream` becomes: `listStremioStreams(...)` → `pickAuto(candidates, remembered bingeGroup)` → play directly if the pick `isDirect()`, else the existing TorBox chain for its `infoHash`.

- [ ] **Step 6: Add `idPrefixes` routing WITH the fallback**

In the provider-selection loop, add the id filter — and the fallback that makes it safe:

```cpp
    QVector<LoadedAddon*> providers;
    for (LoadedAddon* s : sources_)
        if (s->stremio && isEnabled(s->manifest.id) && s->stremioResources.contains(QStringLiteral("stream"))
            && (s->stremioTypes.isEmpty() || s->stremioTypes.contains(item.type)))
            providers.push_back(s);

    // Ask only the addons that claim this id space. THE FALLBACK IS LOAD-BEARING: a mis-declared or unusual
    // manifest must degrade to the old ask-everyone behaviour, never to an unplayable item. Routing is an
    // optimization; it is not allowed to be the reason nothing plays.
    QVector<LoadedAddon*> routed;
    for (LoadedAddon* s : providers)
        if (StremioTranslate::handlesId(s->stremioManifest, QStringLiteral("stream"), item.id))
            routed.push_back(s);
    if (!routed.isEmpty()) providers = routed;
    else if (!providers.isEmpty())
        streamLog(QStringLiteral("stremio: idPrefixes matched no provider for %1 — asking all %2")
                      .arg(item.id).arg(providers.size()));
```

- [ ] **Step 7: Build and run the suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target mymediavault probe_stremio && ./build/Release/probe_stremio.exe && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: `STREMIO-OK` and `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/BingeStore.h native/src/core/BingeStore.cpp native/src/addons/AddonManager.h native/src/addons/AddonManager.cpp native/tools/probe_stremio.cpp native/CMakeLists.txt
git commit -m "feat: stream candidates, idPrefixes routing with fall-back-to-all, and binge memory"
```

---

### Task 5: The "Choose source…" picker, and surfacing what an addon can't do

**Files:**
- Modify: `native/src/ui/MainWindow.{h,cpp}`
- Modify: `native/src/addons/AddonManager.cpp` (the `configurationRequired` / `Unsatisfiable` info rows)

**Interfaces:**
- Consumes: `AddonManager::listStremioStreams`, `StremioTranslate::{describe, pickAuto}`, `BingeStore`.
- Produces: no new cross-task interface — this is the last implementation task.

- [ ] **Step 1: Own a `BingeStore`**

In `MainWindow`, beside the existing `std::unique_ptr<SubtitleCache>`:

```cpp
    std::unique_ptr<BingeStore> bingeStore_;   // remembered release per series (episodes only)
```
Construct it in the constructor with `QDir(AppPaths::dataDir()).filePath(QStringLiteral("binge.json"))` and `load()` it.

- [ ] **Step 2: Add the "Choose source…" action**

Add a row to the same action surface that already carries the item's Play actions (find where "Play" is offered for a catalog item and follow that exactly). Show it **only** when the item resolves through Stremio stream providers — a local-library item or a direct file has nothing to choose between.

Its handler:

```cpp
void MainWindow::chooseStreamSource(const MediaItem& item)
{
    if (!addons_) return;
    notifier_->playerNotice(tr("Finding sources…"), 0);   // sticky; cleared when the picker opens
    // Pin the item's series key AT REQUEST TIME. The reply is async and the user can move on; keying the
    // remembered choice off whatever is current when it lands would file it under the wrong show — the
    // subtitle picker was fixed for exactly this (a94e995).
    const QString seriesKey = BingeStore::seriesKeyFor(item.imdbStreamId);
    addons_->listStremioStreams(item, [this, seriesKey](const QVector<StremioTranslate::StreamCandidate>& all) {
        if (notifier_) notifier_->hideNotice();
        if (all.isEmpty()) { notifier_->notify(tr("No sources found for this item."), 4000); return; }
        presentStreamCandidates(all, seriesKey);
    });
}
```

- [ ] **Step 3: The picker**

```cpp
// A NavMenu of candidate streams. Rows come from StremioTranslate::describe, which flattens the addon's
// multi-line name/title into one line — NavMenu word-wraps, and raw addon text reads as junk otherwise.
void MainWindow::presentStreamCandidates(const QVector<StremioTranslate::StreamCandidate>& all,
                                         const QString& seriesKey)
{
    QStringList rows;
    rows.reserve(all.size());
    for (const StremioTranslate::StreamCandidate& c : all) rows << StremioTranslate::describe(c);

    new NavMenu(tr("Choose a source"), rows, [this, all, seriesKey](int row) {
        if (row < 0 || row >= all.size()) return;
        const StremioTranslate::StreamCandidate c = all[row];
        // Remember the release so the rest of the series uses it. Episodes only — seriesKeyFor already
        // returned empty for a movie, so this is a no-op there rather than a special case here.
        if (!seriesKey.isEmpty() && !c.bingeGroup.isEmpty() && bingeStore_)
            bingeStore_->put(seriesKey, c.bingeGroup);
        playChosenStream(c);
    }, this);
}
```

`playChosenStream` plays `c.url` directly when `c.isDirect()`; otherwise it hands `c.infoHash`/`c.fileIdx` to the same TorBox resolution `resolveStremioStream` uses, then plays the result. Reuse that path — do not duplicate the chain.

- [ ] **Step 4: Consult the remembered group on the automatic path**

`AddonManager` must **not** own a `BingeStore` — the addon layer should not depend on a UI-owned store. Instead the preference is passed in.

In `native/src/addons/AddonManager.h`, give the resolver an extra defaulted parameter:

```cpp
    // preferBingeGroup: a release the user already chose for this series, if any. Empty = no preference.
    void resolveStremioStream(const MediaItem& item,
                              std::function<void(const QString&, const QString&)> cb,
                              const QString& preferBingeGroup = QString());
```

Inside it, hand that straight to the translator:

```cpp
    const int idx = StremioTranslate::pickAuto(candidates, preferBingeGroup);
    if (idx < 0) { cb(QString(), QString()); return; }
```

Then at each **caller** in `MainWindow` that resolves a Stremio stream, compute and pass it:

```cpp
    const QString seriesKey = BingeStore::seriesKeyFor(item.imdbStreamId);
    const QString prefer = (seriesKey.isEmpty() || !bingeStore_) ? QString()
                                                                 : bingeStore_->lookup(seriesKey);
```

> `resolveStreamByImdb` also reaches `resolveStremioStream`. Thread the parameter through it the same way, defaulted, so existing callers compile unchanged and only the ones with a series context pass a preference.

- [ ] **Step 5: Surface what an addon cannot do**

Two messages, both through the existing synthetic `type:"info"` row mechanism (`AddonManager.cpp:966-978`) — do not invent a new surface:

- When a Stremio addon has `configurationRequired`, its shelves are replaced by one row reading *"%1 needs to be configured before it can show anything."* (with the addon name), and, when `configurable` is set, mentioning that its configuration page is at its base URL.
- When a catalog is `Unsatisfiable`, one row carrying its `skipReason`, e.g. *"\"By Studio\" needs a \"studio\" value this add-on does not list."*

- [ ] **Step 6: Build and run the suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target mymediavault probe_nav && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: clean build, `ALL HEADLESS PROBES PASSED`. `probe_nav` is built explicitly because this adds a `NavMenu` caller.

- [ ] **Step 7: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp native/src/addons/AddonManager.cpp
git commit -m "feat: Choose source picker, binge memory, and honest messages for unusable catalogs"
```

---

### Task 6: Close-out — live verification and merge

- [ ] **Step 1: Live-verify against real addons**

**Never launch or modify the deployed app at `C:\MyMediaVault-app` or its ini.** Copy it to a scratch dir, strip `cloud/*` and `sync/*` from the throwaway ini, drive with `MMV_UITEST=1` + a unique `MMV_UITEST_PIPE` via `python native/tools/uitest.py`. Read `verify-app-gui-capture.md` in the memory dir first. **Never print or screenshot a credential value** — report credentials only as "configured".

Install real addons and verify:
1. **A search-only catalog** now appears in search results instead of vanishing.
2. **A genre-required catalog** appears as a shelf with its first genre preselected, and the genre filter switches it.
3. **An addon whose `resources` uses the object form** with `idPrefixes` still browses and resolves.
4. **"Choose source…"** lists candidates with readable one-line rows; picking a non-default one plays it.
5. **bingeGroup memory** — after choosing a release on one episode, the next episode of that series auto-picks the matching one. Confirm via the stream log, not by inference.
6. **A movie** never writes a binge entry (`binge.json` unchanged).
7. **A `configurationRequired` addon** shows the explanatory row rather than empty shelves.
8. **No regression:** an ordinary Cinemeta-style catalog and a Torrentio-style stream resolve exactly as before.

- [ ] **Step 2: Record the outcome in the spec**

Set `**Status:** Complete` in `docs/superpowers/specs/2026-07-26-stremio-translator-design.md` and add a section stating exactly which of the eight steps ran, which were deferred and why, and any defect found. **Do not claim a step passed that did not run** — several addons may not be installable without accounts or keys, and saying so is more useful than a guess.

- [ ] **Step 3: Merge**

```bash
git checkout main && git pull --ff-only && git merge local/stremio-translator --no-edit
```
On a version-line conflict in `native/CMakeLists.txt` / `native/src/main.cpp`, take the **higher** patch number.

- [ ] **Step 4: Build EVERY probe target on the merged tree**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && T=$(grep -o 'add_executable([[:space:]]*probe_[a-z0-9_]*' native/CMakeLists.txt | sed 's/.*(\s*//' | tr '\n' ' ') && cmake --build build --config Release --target $T mymediavault
```
Expected: exit 0. This catches a latent link break in a probe that compiles a source now depending on `StremioTranslate` — that class of break has been caught at a merge gate on this project more than once, and the suite alone misses it because an unbuilt probe is silently skipped.

- [ ] **Step 5: Suite, push, delete the branch**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && BUILD_DIR=build bash native/tools/run-headless-probes.sh && git push origin main && git branch -d local/stremio-translator
```

- [ ] **Step 6: Redeploy and verify**

```bash
cp build/Release/MyMediaVault.exe /c/MyMediaVault-app/MyMediaVault.exe && md5sum build/Release/MyMediaVault.exe /c/MyMediaVault-app/MyMediaVault.exe
```
Expected: the hashes match.

- [ ] **Step 7: Update the ledger**

Append to `.superpowers/sdd/progress.md`: the merge commit, what live verification covered versus deferred, and follow-ups — including the recorded-but-unfixed `installPackage` zip flattening (`AddonManager.cpp:1534`).
