# AIO Catalog — Cloudflare Worker

A remote (HTTP) port of the bundled **aiocatalog** addon, running as a Cloudflare Worker. Same media
sources and logic as the local JS addon, but the API keys live as Worker secrets (not in every user's
settings file), and you can update the addon by redeploying instead of redistributing files.

| Catalog | Source | Secret |
|---|---|---|
| Movies / TV | TMDB | `TMDB_API_KEY` (required) |
| Games | IGDB (Twitch OAuth) | `IGDB_CLIENT_ID` + `IGDB_CLIENT_SECRET` |
| Music | MusicBrainz | none |
| Books / Audiobooks | Google Books → Open Library | `GOOGLE_BOOKS_API_KEY` (optional; OL is the keyless fallback) |
| Comics | Comic Vine | `COMIC_VINE_API_KEY` |
| Manga | MangaDex | none (optional `MANGA_SHOW_ADULT="true"`) |

## Deploy

```
npm i -g wrangler
wrangler login
wrangler secret put TMDB_API_KEY
wrangler secret put IGDB_CLIENT_ID
wrangler secret put IGDB_CLIENT_SECRET
wrangler secret put GOOGLE_BOOKS_API_KEY     # optional
wrangler secret put COMIC_VINE_API_KEY       # optional (Comics)
wrangler deploy
```

Then in the app: **Add-ons → Add by URL**, paste
`https://mymediavault-aiocatalog.<your-subdomain>.workers.dev`.

## Migrating from the local addon

This Worker has a distinct id (`com.mymediavault.aiocatalog-worker`), so it can run side by side with the
bundled local addon. To fully migrate: add the Worker URL, confirm it works, then **disable or Remove** the
local "AIO Catalog" in the Library.

## Notes on the port

- The addon's synchronous `httpRequest` became async `fetch`; every data function is now `async`/`await`.
- `getConfig(key)` reads Worker env vars; the IGDB/Twitch token is cached per isolate.
- The browse-by-console tiles are bundled SVGs the Worker can't serve, so it points their `thumbnailUrl`
  at the app repo's `raw.githubusercontent.com` copies.
- It's a metadata source, so `/stream` returns an empty list — opening an item shows its detail page.
- Verified against all live APIs (catalogs, metadata, IGDB drill-down) before shipping.
