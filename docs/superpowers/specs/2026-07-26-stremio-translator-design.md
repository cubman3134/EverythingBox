# Stremio Addon Translator (roadmap #3) — Design

**Date:** 2026-07-26
**Status:** Draft — approved through brainstorming; awaiting user spec review before plan.
**Origin:** Roadmap #3 ("Stremio addon translator"). The roadmap line carried no description; brainstorming
established the direction as **finishing MMV's Stremio client** so the existing addon ecosystem works
properly here — not serving a manifest outward, and not adapting other ecosystems in.

## What exists today

MMV speaks **two** protocols. Its own (`native/addon-protocol/README.md`) is the richer one — custom media
types, settings forms, metadata-only providers, a `/detail` route. Alongside it, `AddonManager` contains a
**partial Stremio client**, written inline in a ~1500-line networking class:

- `parseStremioManifest` (`AddonManager.cpp:299-336`) — detection is "has `resources` AND `types`".
- `stremioCatalogUrl` (`:358-368`) — already builds `/catalog/{type}/{id}/{extras}.json` with `search=` and
  `skip=`, which **is** the correct Stremio form.
- `parseStremioCatalog` / `parseStremioVideos` / `parseStremioMeta` (`:376-446`).
- `parseStremioStreams` (`:463-477`) — keeps `url`, `mime`, `infoHash`, `fileIdx` and **discards everything
  else**.
- `resolveStremioStream` (`:1161-1243`) — queries every stream addon in parallel, prefers a direct http url,
  else batch-checks up to 60 infoHashes against TorBox.

## The gaps this closes

Verified against the [Stremio addon protocol](https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/protocol.md)
and [manifest schema](https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/api/responses/manifest.md).

1. **The object form of `resources` is read for its name only; its `types` and `idPrefixes` are dropped.**
   The schema permits both `["catalog","stream"]` and
   `[{"name":"stream","types":["movie"],"idPrefixes":["tt"]}]`, mixed freely. MMV *does* handle both shapes
   for the resource **name** (`AddonManager.cpp:314` ternaries on `isString()`), so this is not the
   "installs and does nothing" failure it first looked like — but everything else the object carries is
   discarded, which is where per-resource routing would have come from.
2. **Catalogs that require an `extra` are dropped entirely** (`:321-327`). Search-only and genre-required
   catalogs — a large fraction of real addons — vanish silently.
3. **`idPrefixes` is unreferenced anywhere in the repo**, so every stream request fans out to every provider.
4. **Stream identity is thrown away.** `name`, `title`/`description`, `behaviorHints` never survive the
   parse, so quality and release labels are unavailable and no picker is possible without a model change.
5. **`behaviorHints` is unreferenced.** `configurationRequired` addons present as empty shelves rather than
   as "configure me".
6. **Genre never becomes a filter**, and `extra.options` is never enumerated into UI.

## Design

### 1. `StremioTranslate` — the translator, extracted and pure (`native/src/addons/StremioTranslate.{h,cpp}`)

The parsing above moves out of `AddonManager` into a pure unit: JSON in, MMV models out, no network, no
`QSettings`, no widgets. This is the point of the track — the current code is incomplete *because* it is
untestable, and extraction is what lets `probe_stremio` assert against real manifest fixtures.

```cpp
namespace StremioTranslate
{
    // One declared extra on a catalog, normalized from BOTH the modern `extra[]` objects and the legacy
    // `extraRequired`/`extraSupported` string arrays, so downstream code sees exactly one shape.
    struct Extra
    {
        QString     name;              // "search" | "genre" | "skip" | addon-defined
        bool        isRequired = false;
        QStringList options;           // possible values; empty = free-form
        int         optionsLimit = 1;  // how many the user may select (schema default 1)
    };

    // What MMV can actually do with a declared catalog.
    enum class CatalogUse
    {
        Browse,        // a shelf — required extras (if any) can be satisfied from `options`
        SearchOnly,    // requires `search`: answers queries, never a browse shelf
        Unsatisfiable  // requires something with no options we can supply — skipped, WITH A REASON
    };

    struct Catalog
    {
        QString        type, id, name;
        QVector<Extra> extras;
        CatalogUse     use = CatalogUse::Browse;
        QString        skipReason;     // non-empty only when Unsatisfiable; shown, never swallowed
        QMap<QString,QString> presets;  // each required extra -> its first option. A MAP, not a single
                                        // pair: a catalog may declare more than one required extra.
    };

    struct Manifest
    {
        QString          id, name, version, description, logo;
        QStringList      types;
        QStringList      resources;        // resource NAMES, from strings and objects alike
        QStringList      idPrefixes;       // manifest-level
        QHash<QString, QStringList> resourceIdPrefixes;  // per-resource override, keyed by resource name
        QVector<Catalog> catalogs;
        bool             configurable = false;
        bool             configurationRequired = false;
    };

    // Accepts strings AND objects in `resources`; normalizes both `extra[]` forms; classifies each catalog.
    Manifest parseManifest(const QByteArray& body);

    // "/catalog/{type}/{id}/{k}={v}&{k}={v}.json" — extras are a URL-encoded query string in a PATH
    // segment, per the protocol doc. Keys are emitted in a stable order so the result cache keys stably.
    //
    // `extras` is what the CALLER wants (a chosen filter value, a search term, a skip offset). The
    // catalog's own presets are merged in ONLY where the caller supplied no value for that
    // key — so a required `genre` defaults to the first option, and a user picking "Comedy" overrides it
    // rather than being appended alongside it.
    QString catalogPath(const Catalog&, const QMap<QString, QString>& extras);

    // Which providers may answer for this id. Per-resource prefixes win over manifest-level; an addon
    // declaring none is always eligible.
    bool handlesId(const Manifest&, const QString& resource, const QString& id);

    struct StreamCandidate
    {
        QString url, mime, infoHash;
        int     fileIdx = -1;
        QString name;        // the addon's short label — usually the provider/quality
        QString title;       // the release line (may be multi-line; addons put size/seeders here)
        QString bingeGroup;  // behaviorHints.bingeGroup — "keep using this source for the next episode"
        bool    notWebReady = false;
        qint64  videoSize = 0;
        int     seeders = -1;   // parsed out of `title` when the addon encodes it; -1 = unknown
    };
    // maxRows bounds the returned list. The PICKER asks for kMaxStreamRows (30); the resolution path asks
    // for kMaxHashes (60), because the debrid batch-check must not inherit a display bound — with a single
    // stream addon a 30-row cap made a cached release ranked 31-60 unreachable.
    QVector<StreamCandidate> parseStreams(const QByteArray& body, int maxRows = kMaxStreamRows);

    // The human-readable row for the picker: "1080p · Release.Name.x265 · 42 seeders · 2.1 GB".
    QString describe(const StreamCandidate&);
}
```

`AddonManager` keeps the transport, caching and TorBox resolution; it calls into this for every
Stremio-shaped decision.

### 2. Catalog classification

Per catalog, from its normalized extras:

| Declares | `CatalogUse` | Behavior |
|---|---|---|
| `search` required | `SearchOnly` | Registered with the search fan-out; **not** a browse shelf |
| A required extra **with** `options` | `Browse` | Shelf appears with the **first option preselected** (`presets`); the options become a `CatalogFilter`, and the preselected value is the row shown rather than a misleading "Any" |
| A required extra **without** `options` | `Unsatisfiable` | Skipped with `skipReason` — surfaced, never silent |
| Only optional extras | `Browse` | `genre` becomes a `CatalogFilter`; `skip` drives paging |

`CatalogFilter` already exists (`AddonModels.h:34-39`) with exactly the `key` / `label` / `options` shape
this needs, so the Stremio branch stops discarding filters (`AddonManager.cpp:944-945`) and populates them.

### 3. `idPrefixes` routing — with a mandatory fallback

At stream-resolve time, query only providers where `handlesId(manifest, "stream", id)` is true. Per-resource
prefixes override the manifest-level list; an addon declaring none stays eligible.

**If the filter leaves zero providers, query all of them anyway.** A mis-declared or unusual manifest must
degrade to today's fan-out, not to an unplayable item. Routing is an optimization; it must never be the
reason nothing plays.

### 4. Streams keep their identity, and the picker

`parseStreams` preserves the fields above. The existing auto-resolution is unchanged in behavior — direct
http url first, else the first TorBox-cached infoHash — it simply now runs over richer candidates.

- **"Choose source…"** opens a `NavMenu` of `describe()` rows. Choosing one plays it.
- On choice, the stream's `bingeGroup` is remembered against the series. On a later episode, a candidate
  with that same `bingeGroup` is preferred **before** the auto rule. That is precisely what `bingeGroup`
  exists for, and it makes the override one decision per series instead of one per episode.
- Candidates are sorted **direct http before torrent, then by seeders descending, then by size descending**
  (unknown seeders sort last within their group), then capped at **`kMaxStreamRows = 30`** for the picker. The
cap is a **parameter**, not a constant of the parse: the debrid resolution path asks for `kMaxHashes` (60)
instead, because a display bound must not decide which releases are even checked for a cached copy. The cap is a
  parse/quota bound, **not** a display bound — `NavMenu` scrolls, and past thirty rows nobody is choosing.

**`BingeStore`** (`native/src/core/BingeStore.{h,cpp}`) — device-local JSON at
`AppPaths::dataDir()/binge.json`, modeled on `SubtitleCache`: `seriesKey → bingeGroup`. The series key is
the `tt…` prefix of a `tt…:S:E` episode id — the same convention `MediaSegments::keyFor` uses.
**Episodes only:** a movie has no next episode, so a movie's choice is not remembered and the store is never
consulted for a one-part id. A remembered group that matches nothing this episode is ignored, never fatal.

### 5. `behaviorHints`

- `configurationRequired: true` → the addon's shelves are replaced by one row saying it needs configuring,
  linking its `configurable` URL when declared. Today this presents as an empty shelf with no explanation.
- `notWebReady` on a stream → carried into `StreamCandidate` and labelled in the picker.

## Data flow

```
manifest.json ─→ StremioTranslate::parseManifest
                   ├─ resources (strings AND objects)     ─→ capability detection
                   ├─ catalogs[] + normalized extras      ─→ Browse / SearchOnly / Unsatisfiable
                   ├─ idPrefixes (manifest + per-resource)─→ stream routing
                   └─ behaviorHints                       ─→ "needs configuring" surfacing

browse  ─→ catalogPath(catalog, {genre:"Action", skip:"100"}) ─→ parseCatalog ─→ MediaCatalog + CatalogFilter
search  ─→ SearchOnly + search-capable catalogs, extras {search:q}
play    ─→ providers filtered by handlesId (fallback: all) ─→ parseStreams ─→ candidates
             ├─ remembered bingeGroup matches? ─→ that one
             └─ else the existing auto rule (direct http, else TorBox-cached)
"Choose source…" ─→ candidates ─→ NavMenu ─→ play + remember bingeGroup
```

## Error / edge handling

| Situation | Behavior |
|---|---|
| `resources` mixes strings and objects | Both parsed; resource names normalized into one list |
| `"resources": []`, or objects whose `name` is missing/empty | **Behaviour change worth a release note.** Detection tightened from "has `resources` and `types` keys" to "`resources` is non-empty", so such an addon no longer registers as Stremio, falls through to MMV's own manifest parse, is rejected there, and **vanishes from the source list entirely** — where before it appeared with its catalogs. A manifest declaring catalogs but no resources is malformed, so this is the right call; it is recorded because it is a visible removal, not a silent degrade |
| `configurationRequired: true` | One explanatory row instead of empty shelves; links `configurable` when present |
| Catalog requires an extra with no `options` | Skipped with a **named reason**, surfaced through the existing info-row mechanism |
| **`idPrefixes` filters out every provider** | **Query all of them.** A bad manifest must degrade to today's behavior, never to a dead end |
| Remembered `bingeGroup` matches nothing | Ignored; fall back to the auto rule. A stale preference must never block playback |
| Legacy `extraRequired` / `extraSupported` | Normalized into `Extra[]`; `extraRequired` implies `isRequired` |
| `optionsLimit` | How many values a user may **select** (schema default 1). It does **not** bound how many options exist — an earlier draft capped the rendered list by it, which hid 17 of 20 genres for an addon declaring `optionsLimit: 3` |
| Extra value containing a space, `&`, or `=` | URL-encoded within the path segment; asserted in the probe |
| Stream flagged `notWebReady` | Still offered, labelled in the picker |
| Addon returns an HTTP error | Existing synthetic `type:"info"` row, with a specific message rather than a generic one |
| Zero streams after filtering | Existing no-stream path, unchanged |
| Very long release names | `NavMenu` scrolls; the list is still sorted and capped |
| Addon declares `catalog` but zero catalogs | Not an error — it may exist for `meta`/`stream` only |

## Verification

- **`probe_stremio`** (new, pure, RED-first, sentinel `STREMIO-OK`), asserting against **literal real-world
  manifest fixtures** (Cinemeta and Torrentio shapes) rather than idealized JSON:
  - `resources` as strings, as objects, and **mixed** — including that the object form no longer yields an
    unusable addon (the live bug).
  - Both `extra[]` forms and the legacy `extraRequired`/`extraSupported`, normalized to the same result.
  - All three `CatalogUse` outcomes, and that `Unsatisfiable` carries a non-empty `skipReason`.
  - `catalogPath` — key order stability, and escaping a value containing a space, `&` and `=`;
    `skip` paging arithmetic.
  - `handlesId` — manifest-level, per-resource override, no-prefixes-means-eligible, and the
    **all-filtered-out fallback**.
  - `parseStreams` — `name`/`title`/`bingeGroup`/`notWebReady`/`videoSize`, infoHash validity, seeders
    pulled from a title, and that a stream with neither a usable url nor a valid infoHash is dropped.
  - bingeGroup selection: remembered group matches → chosen; no match → the auto rule; a one-part (movie)
    id never consults the store.
  - Candidate ordering at each tie-break level, the `kMaxStreamRows` cap, and that the cap is a parameter
    so the resolution path can ask for more than the picker shows.
  - `catalogPath` preset merging: a required `genre` with no caller value uses the first option; a caller
    value **replaces** it rather than appearing alongside it.
- **Live:** install a real Stremio addon with a search-only catalog and a genre-required one; confirm both
  now appear and behave; confirm an object-form `resources` addon works; open "Choose source…", pick a
  non-default stream, and confirm the choice carries to the next episode.
- Suite + app compile. No perf run — this is parse-time work on paths that already do network I/O.

## Non-goals

- Serving a Stremio manifest outward (no HTTP server exists in MMV; that is its own subsystem).
- `addonCatalogs` / `addon_catalog` — addon discovery through addons.
- The `subtitles` resource — OpenSubtitles already owns subtitles (roadmap #5, shipped).
- A torrent streaming layer; torrents continue to resolve only through the user's own TorBox key.
- Any change to MMV's own addon protocol, or to the JsLocal engine.
- **Recorded as a follow-up, not fixed here:** `installPackage` flattens all zip paths to `fileName()`
  (`AddonManager.cpp:1534`), so an addon shipping `icons/` or `lib/` loses its structure.
