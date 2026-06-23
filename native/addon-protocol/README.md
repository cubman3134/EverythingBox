# My Media Vault — Remote Addon Protocol

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

Your addon URL becomes `https://mymediavault-tmdb-addon.<subdomain>.workers.dev`.

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
