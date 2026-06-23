# AIO Catalog — Cloudflare Worker

A remote (HTTP) port of the bundled **aiocatalog** addon, running as a Cloudflare Worker. Same media
sources and logic as the local JS addon.

**Keys are per-user.** The Worker is keyless and shared — each person enters *their own* API keys in the
app, and the app sends them to the Worker on each request. So one deployment serves everyone, and nobody
shares anyone else's keys or rate limits.

| Catalog | Source | Key (entered in-app) |
|---|---|---|
| Movies / TV | TMDB | TMDB API key |
| Games | IGDB (Twitch OAuth) | IGDB client id + secret |
| Music | MusicBrainz | none |
| Books / Audiobooks | Google Books → Open Library | Google Books key (optional; OL is the keyless fallback) |
| Comics | Comic Vine | Comic Vine key |
| Manga | MangaDex | none (optional adult toggle) |

## Deploy (no secrets needed)

```
npm i -g wrangler
wrangler login
wrangler deploy
```

Then in the app:
1. **Add-ons → Add by URL**, paste `https://mymediavault-aiocatalog.<your-subdomain>.workers.dev`.
2. Select it in the source list → **Configure…** → enter your own API keys.

## How per-user config works

- The manifest declares `settings` (the keys), so the app's **Configure…** dialog renders a form.
- Those values are sent as `X-MMV-Config: base64url(json)` on every request.
- The Worker reads them in an `AsyncLocalStorage` scope (so concurrent users never see each other's keys)
  and falls back to env vars only if a request carries no config (handy for a private self-host).

## Migrating from the local addon

Distinct id (`com.mymediavault.aiocatalog-worker`), so it runs side by side with the bundled local addon.
To fully migrate: add the Worker URL, Configure your keys, confirm it works, then **disable or Remove** the
local "AIO Catalog" in the Library.

## Notes on the port

- The addon's synchronous `httpRequest` became async `fetch`; every data function is now `async`/`await`.
- The IGDB/Twitch token is cached per client-id (so per user), not in a shared global.
- The browse-by-console tiles are bundled SVGs the Worker can't serve, so it points their `thumbnailUrl`
  at the app repo's `raw.githubusercontent.com` copies.
- It's a metadata source, so `/stream` returns an empty list — opening an item shows its detail page.
- Verified against all live APIs with per-user config headers (catalogs, metadata, IGDB OAuth) before shipping.
