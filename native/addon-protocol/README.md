# EverythingBox — Remote Addon Protocol

A **remote addon** is a media-source addon the app reaches over HTTP instead of running locally.
The app stores **only the addon's URL + a cached copy of its manifest** — never any code — and calls it
live as you browse (architecture inspired by subscribe-by-link media apps, but it's our own protocol).

This sits alongside the existing **local JS addons** (bundled `manifest.json` + `main.js` run in Duktape).
Both kinds appear identically in the UI; the difference is only the transport.

## The contract

A remote addon returns the **exact same JSON** a local JS addon's functions return — `MediaCatalog`
(`{ title, hasMore, items[] }`) and `MediaDetail` (`{ title, subtitle, overview, image, facts[] }`). The
only difference is the functions become HTTP routes. Every route ends in `.json` so the same layout works
whether it's served by a **Worker** (dynamic) or by **static files** (GitHub Pages / a local folder).

| Route | Returns | Maps to the JS function |
|---|---|---|
| `GET {base}/manifest.json` | the addon manifest | — |
| `GET {base}/catalog/{catalogId}.json` | `MediaCatalog` | `getCatalog({catalog})` |
| `GET {base}/catalog/{catalogId}/search={q}.json` | `MediaCatalog` | `getCatalog({query})` / `search` |
| `GET {base}/catalog/{catalogId}/page={n}.json` | `MediaCatalog` | `getCatalog({page})` |
| `GET {base}/meta/{type}/{id}.json` | `MediaDetail` | `getMeta({id,type})` |
| `GET {base}/detail/{type}/{id}.json` | `MediaCatalog` (children) | `getDetail({id,type})` |
| `GET {base}/stream/{type}/{id}.json` | a playable source | (new — resolves the file to play) |
| `GET {base}/chapters/{type}/{seriesId}.json` | a serial's chapter list | `getChapters({id,type,page})` |
| `GET {base}/pages/{type}/{chapterId}.json` | one chapter's page images | `getPages({id,type})` |

`base` = the manifest URL minus `/manifest.json`. Extras can combine: `/catalog/movies/search=batman&page=2.json`.

### Playback (the `/stream` route)

Catalog/detail items don't carry a playable `url` (metadata and sources are separate). When you open a
leaf item (movie / episode / track), the app fetches `/stream/{type}/{id}.json` and plays the first source.
The response is either a single object or a list:

```json
{ "url": "https://…/video.mp4", "mime": "video/mp4" }
{ "streams": [ { "url": "https://…/video.mp4", "mime": "video/mp4", "title": "1080p" } ] }
```

A metadata-only addon (like the TMDB Worker) returns `{ "streams": [] }`, so opening an item shows its
detail page instead. Direct http(s) URLs play through libmpv; torrent/magnet sources would need a separate
streaming layer (not built).

### Serial works: the `chapters` and `pages` routes

A **serial** is anything read in installments — a manga, a web novel, a serialised comic. Two routes cover
it, and unlike the routes above they are **declared** rather than assumed: the app asks for them only where
the manifest says they exist, so an addon written before they did is never sent a request it cannot answer.

| Route | Returns | Maps to the JS function |
|---|---|---|
| `GET {base}/chapters/{type}/{seriesId}.json` | the ordered chapter list | `getChapters({id,type,page})` |
| `GET {base}/pages/{type}/{chapterId}.json` | one chapter's page images | `getPages({id,type})` |

`{type}` is the **family** media type on both routes — the type of the *series* (`manga`), never the leaf's
(`manga_chapter`). One declaration therefore covers a series and every chapter under it. `/chapters` accepts
the same optional `/page={n}` segment `/detail` does; a source that returns a whole series at once ignores it.

**Declare them in the manifest**, beside `catalogs`:

```json
"resources": [
  { "name": "chapters", "types": ["manga"] },
  { "name": "pages",    "types": ["manga"] }
]
```

An entry with no `types` answers for every type. Omit the array entirely and the app uses the older path —
a container's chapters then come from `/detail` as before, and its chapters cannot be read as page images.

#### `chapters`

```json
{
  "chapters": [
    { "id": "ch-9",    "number": "9",    "volume": "1", "title": "The Long Walk",
      "language": "en", "group": "Fan Scans", "published": "2019-04-02", "pageCount": 18 },
    { "id": "ch-9-5",  "number": "9.5",  "volume": "1", "title": "Extra: Hot Springs", "language": "en" },
    { "id": "ch-10",   "number": "10",   "volume": "2", "language": "en", "pageCount": 21 }
  ],
  "hasMore": false
}
```

`id` is opaque and is handed straight back as `/pages`' `{chapterId}` — it never has to look like anything.
Every other field is optional. `number` is a **string**, because sources publish `"9.5"`, `"10.1"` and
`"Extra"`, and it is what the app orders by: a **natural sort**, so `"10"` comes after `"9.5"`, with
un-numbered entries last and ties keeping the order you listed them in. Return the list in whatever order
suits you; the ordering rule is the app's, so it is the same rule for every source. `volume` is a label
only — ordering never uses it, so a series whose volumes restart their numbering still reads correctly.
`hasMore` is optional and defaults to `false`.

#### `pages`

```json
{
  "pages": [
    { "url": "https://img.example.net/ch10/01.jpg", "width": 1114, "height": 1600,
      "headers": { "Referer": "https://reader.example.net/" } },
    { "url": "https://img.example.net/ch10/02.jpg" },
    "https://img.example.net/ch10/03.jpg"
  ]
}
```

**In reading order** — pages are never reordered. A page may be a bare URL string when it needs nothing
else. `width`/`height` are the source's own numbers where it has them. `headers` is the same vocabulary as
`behaviorHints.proxyHeaders.request` and is subject to the same rules: hop-by-hop and request-shaping
fields (`Host`, `Range`, `Connection`, …) are refused, values carrying CR/LF are refused, and the headers
reach **only** the URL that declared them — a cross-origin redirect drops the request rather than
forwarding them. This is where a `Referer`-gated image CDN becomes readable instead of returning 403.

The app feeds this list into the same comic reader a local `.cbz` and a remote book server go through, so
prefetch, caching, zoom/fit, per-item resume and paging into the next chapter all work with no further
involvement from the addon.

#### Worked example

A static addon at `https://example.github.io/serials` serving one series:

```
serials/
  manifest.json
  catalog/serials.json
  detail/manga/the-long-walk.json          (optional — /chapters supersedes it)
  chapters/manga/the-long-walk.json
  pages/manga/ch-9.json
  pages/manga/ch-10.json
```

`manifest.json`

```json
{
  "id": "net.example.serials", "name": "Example Serials", "version": "1.0.0",
  "type": "media-source",
  "catalogs": [ { "id": "serials", "name": "Serials", "type": "manga" } ],
  "resources": [
    { "name": "chapters", "types": ["manga"] },
    { "name": "pages",    "types": ["manga"] }
  ]
}
```

`catalog/serials.json` — the series, marked `expandable` so it can be drilled into:

```json
{ "title": "Serials", "items": [
  { "id": "the-long-walk", "title": "The Long Walk", "type": "manga",
    "thumbnailUrl": "https://example.github.io/serials/covers/tlw.jpg", "expandable": true }
] }
```

`chapters/manga/the-long-walk.json` and `pages/manga/ch-10.json` are exactly the two payloads above. That is
the whole addon: browsing the catalog shows the series, opening it lists `Vol. 1 · Ch. 9`, `Vol. 1 · Ch. 9.5`,
`Vol. 2 · Ch. 10`, and opening a chapter reads it.

A dynamic addon answers the same two routes from `getChapters` / `getPages` instead of from files —
`addons/aiocatalog/main.js` (local JS) and `addon-protocol/aiocatalog-worker/` (Worker) both do, and are
the reference implementations.

**A second implementer.** EverythingBoxServer adopts this vocabulary verbatim for server-side serial
sources, so treat the field names and semantics above as the interface, not as one client's shape. Nothing
here is client-only: it is JSON, every field is optional except `id` and `url`, and the ordering rule is
stated rather than implied.

## Two ways to host

### Static (no server) — `sample-static/`
Just files laid out by the routes above. Host the folder on **GitHub Pages**, a Worker's static assets, or
even a local folder, and add its URL. Great for **curated/fixed catalogs**. No live search of arbitrary
queries (you'd have to precompute `search=*.json` files), no live data.

### Dynamic (Cloudflare Worker) — `worker/`
A real backend that can query an API, search, and paginate. The included Worker is **TMDB-backed**:

```
cd worker
npm i -g wrangler
wrangler login
wrangler secret put TMDB_API_KEY     # your TMDB v3 key
wrangler deploy
```

Your addon URL becomes `https://everythingbox-tmdb-addon.<subdomain>.workers.dev`.

## Add it in the app

**Add-ons → Add addon by URL**, paste the manifest (or base) URL. The app fetches the manifest, validates
it's a `media-source`, and stores just the URL — the addon's catalogs then appear like any other source.

## Test it headlessly

```
probe_addon --remote <baseUrl> [catalogId]
# e.g. against the static sample over file://:
probe_addon --remote file:///path/to/sample-static movies
```

Exercises catalog → first item's `meta` → a container's `detail`, exactly as the app does.
