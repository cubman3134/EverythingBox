// AIO Catalog, as a Cloudflare Worker — a remote port of the bundled aiocatalog addon.
//
//   Movies & TV -> TMDB,  Games -> IGDB,  Music -> MusicBrainz,  Books -> Google Books / Open Library,
//   Comics -> Comic Vine,  Manga -> MangaDex.
//
// Same logic as the local JS addon, made async (Workers have only async fetch). Keys are PER USER: each
// user enters their own in the app's Configure… dialog and the app sends them in the X-MMV-Config header,
// so the Worker stays keyless and shared. (Worker env vars are only an optional fallback for a self-host.)
// Serves the My Media Vault remote-addon protocol.

const UA = "MyMediaVault/1.0 (+https://github.com/cubman3134/MyMediaVault)";
// The bundled console tiles live in the app repo; reference them absolutely since the Worker can't serve them.
const CONSOLE_ICONS = "https://raw.githubusercontent.com/cubman3134/MyMediaVault/main/native/addons/aiocatalog/consoles/";

const MANIFEST = {
  id: "com.mymediavault.aiocatalog-worker",
  name: "AIO Catalog (Worker)",
  version: "1.0.0",
  type: "media-source",
  author: "My Media Vault",
  description: "All-in-one metadata catalog served from a Cloudflare Worker: Movies & TV (TMDB), Games (IGDB), Music (MusicBrainz), Books (Google Books / Open Library), Comics (Comic Vine), Manga (MangaDex).",
  catalogs: [
    { id: "movies", name: "Movies", type: "movie" },
    { id: "tv", name: "TV Shows", type: "series" },
    { id: "games", name: "Games", type: "game" },
    { id: "music", name: "Music", type: "album" },
    { id: "books", name: "Books", type: "book" },
    { id: "audiobooks", name: "Audiobooks", type: "audiobook" },
    { id: "comics", name: "Comics", type: "comic" },
    { id: "manga", name: "Manga", type: "manga" },
  ],
  // Per-user settings: the app's Configure… dialog renders these and sends the values back as config (each
  // user uses their own keys; nothing is baked into the Worker).
  settings: [
    { key: "tmdbApiKey", label: "TMDB API Key", type: "password", description: "themoviedb.org API key — enables Movies and TV Shows." },
    { key: "igdbClientId", label: "IGDB / Twitch Client ID", type: "text", description: "Twitch application client id — enables Games (IGDB)." },
    { key: "igdbClientSecret", label: "IGDB / Twitch Client Secret", type: "password", description: "Twitch application client secret — enables Games (IGDB)." },
    { key: "googleBooksApiKey", label: "Google Books Key (optional)", type: "password", description: "Optional Google Books API key — higher rate limits for Books." },
    { key: "comicVineApiKey", label: "Comic Vine API Key", type: "password", description: "Free comicvine.gamespot.com key — enables Comics. (Manga needs no key.)" },
    { key: "mangaShowAdult", label: "Show adult manga (18+)", type: "checkbox", default: false, description: "Include adult (NSFW) titles in the Manga catalog. Off by default." },
  ],
};

// ---- host-API shims (replace the addon's getConfig / httpGet / httpRequest) --------------------------
// Config is PER USER: the app sends each user's keys in the X-MMV-Config header. We stash them in an
// AsyncLocalStorage scope per request so concurrent requests (which share an isolate) can't see each
// other's keys. Worker env vars are only an optional fallback for a personal self-host.
import { AsyncLocalStorage } from "node:async_hooks";
const cfgStore = new AsyncLocalStorage();
let ENV = {};
function envFallback(key) {
  switch (key) {
    case "tmdbApiKey": return ENV.TMDB_API_KEY || "";
    case "igdbClientId": return ENV.IGDB_CLIENT_ID || "";
    case "igdbClientSecret": return ENV.IGDB_CLIENT_SECRET || "";
    case "steamGridDbKey": return ENV.STEAMGRIDDB_KEY || "";
    case "googleBooksApiKey": return ENV.GOOGLE_BOOKS_API_KEY || "";
    case "comicVineApiKey": return ENV.COMIC_VINE_API_KEY || "";
    case "mangaShowAdult": return ENV.MANGA_SHOW_ADULT || "false";
    default: return "";
  }
}
function getConfig(key) {
  const cfg = cfgStore.getStore();
  if (cfg && cfg[key] !== undefined && cfg[key] !== "") return String(cfg[key]);
  return envFallback(key);
}
async function httpGet(url) {
  const r = await fetch(url, { headers: { "User-Agent": UA, "Accept": "application/json" } });
  return await r.text();
}
async function httpRequest(opt) {
  const r = await fetch(opt.url, {
    method: opt.method || "GET",
    body: opt.body,
    headers: Object.assign({ "User-Agent": UA }, opt.headers || {}),
  });
  return await r.text();
}

// ---- pure helpers (unchanged from the addon) --------------------------------------------------------

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }
function info(title, msg) { return JSON.stringify({ title: title, items: [{ id: "_info", title: msg, type: "info" }] }); }
function year(d) { return (d || "").substring(0, 4); }
function page1(p) { return (p && p > 0) ? p : 1; }
const PAGE = 40;
function result(title, items, hasMore) { return JSON.stringify({ title: title, items: items, hasMore: !!hasMore }); }
function metaFact(label, value) { return { label: label, value: (value == null) ? "" : String(value) }; }
function metaResult(o) { return JSON.stringify(o); }
function rating10(v) { return (v || v === 0) ? (Math.round(v * 10) / 10) + " / 10" : ""; }
function stripHtml(s) { return (s || "").replace(/<[^>]*>/g, " ").replace(/\s+/g, " ").trim(); }
function namesOf(arr, field, max) {
  if (!arr) return "";
  const out = [];
  for (let i = 0; i < arr.length && (!max || i < max); i++) out.push(field ? arr[i][field] : arr[i]);
  return out.join(", ");
}
function msToClock(ms) { return Math.floor(ms / 60000) + ":" + ("0" + Math.floor((ms % 60000) / 1000)).slice(-2); }

// ---------------------------------------------------------------------------- TMDB
const TMDB = "https://api.themoviedb.org/3";
const TMDB_IMG = "https://image.tmdb.org/t/p/w342";
const TMDB_IMG_LG = "https://image.tmdb.org/t/p/w500";

async function tmdbList(kind, query, page) {
  page = page1(page);
  const key = getConfig("tmdbApiKey");
  const label = kind === "tv" ? "TV Shows" : "Movies";
  if (!key) return info(label, "Set your TMDB API key in Configure… to load " + kind + ".");
  const path = kind === "tv" ? "tv" : "movie";
  const url = query
    ? TMDB + "/search/" + path + "?api_key=" + key + "&page=" + page + "&query=" + enc(query)
    : TMDB + "/trending/" + path + "/week?api_key=" + key + "&page=" + page;
  const r = J(await httpGet(url));
  if (!r || !r.results) return info(label, "Could not reach TMDB (check the key).");
  const items = [];
  for (const m of r.results) {
    items.push({
      id: "tmdb:" + path + ":" + m.id, title: m.title || m.name,
      subtitle: year(m.release_date || m.first_air_date),
      type: kind === "tv" ? "series" : "movie",
      thumbnailUrl: m.poster_path ? TMDB_IMG + m.poster_path : "",
      expandable: kind === "tv", url: "",
    });
  }
  return result(label, items, r.page < r.total_pages);
}

async function tmdbSeasons(showId) {
  const key = getConfig("tmdbApiKey"); if (!key) return info("Seasons", "Set your TMDB API key in Configure…");
  const r = J(await httpGet(TMDB + "/tv/" + showId + "?api_key=" + key));
  if (!r || !r.seasons) return info("Seasons", "Could not load seasons.");
  const items = [];
  for (const s of r.seasons) {
    items.push({
      id: "tmdb:season:" + showId + ":" + s.season_number,
      title: s.name || ("Season " + s.season_number), subtitle: (s.episode_count || 0) + " episodes",
      type: "season", thumbnailUrl: s.poster_path ? TMDB_IMG + s.poster_path : "", expandable: true, url: "",
    });
  }
  return JSON.stringify({ title: r.name || "Seasons", items: items });
}

async function tmdbEpisodes(showId, seasonNo) {
  const key = getConfig("tmdbApiKey"); if (!key) return info("Episodes", "Set your TMDB API key in Configure…");
  const r = J(await httpGet(TMDB + "/tv/" + showId + "/season/" + seasonNo + "?api_key=" + key));
  if (!r || !r.episodes) return info("Episodes", "Could not load episodes.");
  const items = [];
  for (const e of r.episodes) {
    items.push({
      id: "tmdb:episode:" + showId + ":" + seasonNo + ":" + e.episode_number,
      title: "E" + e.episode_number + " — " + e.name, subtitle: e.air_date || "",
      type: "episode", thumbnailUrl: e.still_path ? TMDB_IMG + e.still_path : "", expandable: false, url: "",
    });
  }
  return JSON.stringify({ title: r.name || "Episodes", items: items });
}

async function tmdbMovieMeta(id) {
  const key = getConfig("tmdbApiKey"); if (!key) return "{}";
  const r = J(await httpGet(TMDB + "/movie/" + id + "?api_key=" + key));
  if (!r || r.success === false) return "{}";
  const facts = [];
  if (r.release_date) facts.push(metaFact("Released", r.release_date));
  if (r.runtime) facts.push(metaFact("Runtime", r.runtime + " min"));
  if (r.vote_average) facts.push(metaFact("Rating", rating10(r.vote_average)));
  if (r.genres && r.genres.length) facts.push(metaFact("Genres", namesOf(r.genres, "name")));
  return metaResult({ title: r.title || r.name, subtitle: r.tagline || "", overview: r.overview || "",
    image: r.poster_path ? TMDB_IMG_LG + r.poster_path : "", facts: facts });
}

async function tmdbSeriesMeta(id) {
  const key = getConfig("tmdbApiKey"); if (!key) return "{}";
  const r = J(await httpGet(TMDB + "/tv/" + id + "?api_key=" + key));
  if (!r || r.success === false) return "{}";
  const facts = [];
  if (r.first_air_date) facts.push(metaFact("First aired", r.first_air_date));
  if (r.number_of_seasons) facts.push(metaFact("Seasons", r.number_of_seasons));
  if (r.number_of_episodes) facts.push(metaFact("Episodes", r.number_of_episodes));
  if (r.vote_average) facts.push(metaFact("Rating", rating10(r.vote_average)));
  if (r.genres && r.genres.length) facts.push(metaFact("Genres", namesOf(r.genres, "name")));
  if (r.status) facts.push(metaFact("Status", r.status));
  return metaResult({ title: r.name, subtitle: r.tagline || "", overview: r.overview || "",
    image: r.poster_path ? TMDB_IMG_LG + r.poster_path : "", facts: facts });
}

async function tmdbSeasonMeta(showId, n) {
  const key = getConfig("tmdbApiKey"); if (!key) return "{}";
  const r = J(await httpGet(TMDB + "/tv/" + showId + "/season/" + n + "?api_key=" + key));
  if (!r || r.success === false) return "{}";
  const facts = [];
  if (r.air_date) facts.push(metaFact("Air date", r.air_date));
  if (r.episodes) facts.push(metaFact("Episodes", r.episodes.length));
  return metaResult({ title: r.name || ("Season " + n), subtitle: "", overview: r.overview || "",
    image: r.poster_path ? TMDB_IMG_LG + r.poster_path : "", facts: facts });
}

async function tmdbEpisodeMeta(showId, season, ep) {
  const key = getConfig("tmdbApiKey"); if (!key) return "{}";
  const r = J(await httpGet(TMDB + "/tv/" + showId + "/season/" + season + "/episode/" + ep + "?api_key=" + key));
  if (!r || r.success === false) return "{}";
  const facts = [metaFact("Season / Episode", "S" + season + " · E" + ep)];
  if (r.air_date) facts.push(metaFact("Air date", r.air_date));
  if (r.runtime) facts.push(metaFact("Runtime", r.runtime + " min"));
  if (r.vote_average) facts.push(metaFact("Rating", rating10(r.vote_average)));
  return metaResult({ title: r.name || ("Episode " + ep), subtitle: "", overview: r.overview || "",
    image: r.still_path ? TMDB_IMG_LG + r.still_path : "", facts: facts });
}

// ---------------------------------------------------------------------------- IGDB
const tokenCache = new Map(); // clientId -> { value, exp } : keyed per user's client id (not a shared global)
async function igdbToken() {
  const id = getConfig("igdbClientId"), secret = getConfig("igdbClientSecret");
  if (!id || !secret) return "";
  const c = tokenCache.get(id);
  if (c && c.exp > Date.now()) return c.value;
  const resp = J(await httpRequest({
    method: "POST",
    url: "https://id.twitch.tv/oauth2/token?client_id=" + enc(id) + "&client_secret=" + enc(secret) + "&grant_type=client_credentials",
  }));
  if (!resp || !resp.access_token) return "";
  tokenCache.set(id, { value: resp.access_token, exp: Date.now() + ((resp.expires_in || 3600) - 60) * 1000 });
  return resp.access_token;
}

const CONSOLES = [
  { id: 130, name: "Nintendo Switch" }, { id: 41, name: "Wii U" }, { id: 5, name: "Wii" },
  { id: 21, name: "GameCube" }, { id: 4, name: "Nintendo 64" }, { id: 19, name: "SNES" },
  { id: 18, name: "NES" }, { id: 51, name: "Famicom Disk System" }, { id: 87, name: "Virtual Boy" },
  { id: 37, name: "Nintendo 3DS" }, { id: 20, name: "Nintendo DS" }, { id: 24, name: "Game Boy Advance" },
  { id: 22, name: "Game Boy Color" }, { id: 33, name: "Game Boy" },
  { id: 7, name: "PlayStation" }, { id: 8, name: "PlayStation 2" }, { id: 9, name: "PlayStation 3" },
  { id: 48, name: "PlayStation 4" }, { id: 38, name: "PSP" }, { id: 46, name: "PlayStation Vita" },
  { id: 11, name: "Xbox" }, { id: 12, name: "Xbox 360" },
  { id: 6, name: "PC (Windows)" }, { id: 13, name: "MS-DOS" },
  { id: 84, name: "SG-1000" }, { id: 64, name: "Master System" }, { id: 29, name: "Sega Genesis" },
  { id: 78, name: "Sega CD" }, { id: 30, name: "Sega 32X" }, { id: 32, name: "Sega Saturn" },
  { id: 23, name: "Dreamcast" }, { id: 35, name: "Game Gear" },
  { id: 59, name: "Atari 2600" }, { id: 66, name: "Atari 5200" }, { id: 60, name: "Atari 7800" },
  { id: 61, name: "Atari Lynx" }, { id: 62, name: "Atari Jaguar" }, { id: 63, name: "Atari ST" },
  { id: 86, name: "TurboGrafx-16" }, { id: 128, name: "PC Engine SuperGrafx" },
  { id: 80, name: "Neo Geo" }, { id: 119, name: "Neo Geo Pocket" }, { id: 120, name: "Neo Geo Pocket Color" },
  { id: 57, name: "WonderSwan" }, { id: 123, name: "WonderSwan Color" },
  { id: 50, name: "3DO" }, { id: 67, name: "Intellivision" }, { id: 68, name: "ColecoVision" },
  { id: 70, name: "Vectrex" }, { id: 117, name: "Philips CD-i" },
  { id: 25, name: "Amiga" }, { id: 27, name: "MSX" }, { id: 26, name: "ZX Spectrum" },
  { id: 52, name: "Arcade" },
];
function igdbCreds() { return getConfig("igdbClientId") && getConfig("igdbClientSecret"); }
async function igdbQuery(body) {
  const token = await igdbToken();
  if (!token) return null;
  return J(await httpRequest({
    method: "POST", url: "https://api.igdb.com/v4/games", body: body,
    headers: { "Client-ID": getConfig("igdbClientId"), "Authorization": "Bearer " + token, "Accept": "application/json" },
  }));
}
function igdbToItems(arr) {
  const items = [];
  for (const g of arr) {
    const cover = (g.cover && g.cover.image_id)
      ? "https://images.igdb.com/igdb/image/upload/t_cover_big/" + g.cover.image_id + ".jpg" : "";
    const y = g.first_release_date ? String(new Date(g.first_release_date * 1000).getFullYear()) : "";
    items.push({ id: "igdb:" + g.id, title: g.name, subtitle: y, type: "game", thumbnailUrl: cover, expandable: false, url: "" });
  }
  return items;
}
function consoleSlug(name) { return name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, ""); }
function igdbConsoles() {
  const items = [];
  for (const c of CONSOLES)
    items.push({ id: "igdbplatform:" + c.id, title: c.name, subtitle: "", type: "platform", expandable: true,
                 thumbnailUrl: CONSOLE_ICONS + consoleSlug(c.name) + ".svg", url: "" });
  return JSON.stringify({ title: "Games by Console", items: items });
}
async function gamesCatalog(query, page) {
  if (query) {
    page = page1(page);
    if (!igdbCreds()) return info("Games", "Set your IGDB client id + secret in Configure… to search games.");
    const r = await igdbQuery('search "' + query.replace(/"/g, "") + '"; fields name,cover.image_id,first_release_date; limit ' + PAGE + '; offset ' + ((page - 1) * PAGE) + ';');
    if (!r) return info("Games", "Could not authenticate with IGDB.");
    return result("Games: " + query, igdbToItems(r), r.length === PAGE);
  }
  return igdbConsoles();
}
async function igdbPlatformGames(platformId, page) {
  page = page1(page);
  if (!igdbCreds()) return info("Games", "Set your IGDB client id + secret in Configure… to load games.");
  const r = await igdbQuery('fields name,cover.image_id,first_release_date,rating; where platforms = (' + platformId + ') & cover != null; sort rating desc; limit ' + PAGE + '; offset ' + ((page - 1) * PAGE) + ';');
  if (!r) return info("Games", "Could not load games for this console.");
  if (!r.length && page === 1) return info("Games", "No games found for this console.");
  return result("Games", igdbToItems(r), r.length === PAGE);
}
async function igdbGameMeta(id) {
  if (!igdbCreds()) return "{}";
  const r = await igdbQuery('fields name,summary,storyline,rating,aggregated_rating,first_release_date,genres.name,platforms.name,cover.image_id,involved_companies.developer,involved_companies.company.name; where id = ' + id + ';');
  if (!r || !r.length) return "{}";
  const g = r[0], facts = [];
  if (g.first_release_date) facts.push(metaFact("Released", String(new Date(g.first_release_date * 1000).getFullYear())));
  const rt = g.rating || g.aggregated_rating;
  if (rt) facts.push(metaFact("Rating", Math.round(rt) + " / 100"));
  if (g.genres) facts.push(metaFact("Genres", namesOf(g.genres, "name")));
  if (g.platforms) facts.push(metaFact("Platforms", namesOf(g.platforms, "name", 6)));
  let dev = "";
  if (g.involved_companies)
    for (const ic of g.involved_companies)
      if (ic.developer && ic.company && ic.company.name) { dev = ic.company.name; break; }
  if (dev) facts.push(metaFact("Developer", dev));
  const cover = (g.cover && g.cover.image_id) ? "https://images.igdb.com/igdb/image/upload/t_cover_big/" + g.cover.image_id + ".jpg" : "";
  return metaResult({ title: g.name, subtitle: "", overview: g.summary || g.storyline || "", image: cover, facts: facts });
}

// ---------------------------------------------------------------------------- MusicBrainz
const MB = "https://musicbrainz.org/ws/2";
async function mbAlbums(query, page) {
  page = page1(page);
  const offset = (page - 1) * PAGE;
  const q = query ? query : "tag:rock AND primarytype:album";
  const r = J(await httpGet(MB + "/release-group/?query=" + enc(q) + "&fmt=json&limit=" + PAGE + "&offset=" + offset));
  if (!r || !r["release-groups"]) return info("Music", "Could not reach MusicBrainz.");
  const items = [];
  for (const rg of r["release-groups"]) {
    const artist = (rg["artist-credit"] && rg["artist-credit"][0]) ? rg["artist-credit"][0].name : "";
    items.push({
      id: "mb:rg:" + rg.id, title: rg.title,
      subtitle: artist + (rg["first-release-date"] ? " · " + year(rg["first-release-date"]) : ""),
      type: "album", thumbnailUrl: "https://coverartarchive.org/release-group/" + rg.id + "/front-250",
      expandable: true, url: "",
    });
  }
  return result("Music", items, (offset + PAGE) < (r.count || 0));
}
async function mbTracks(rgId) {
  const rel = J(await httpGet(MB + "/release?release-group=" + rgId + "&fmt=json&limit=1"));
  if (!rel || !rel.releases || !rel.releases.length) return info("Tracks", "No tracklist found.");
  const relId = rel.releases[0].id;
  const r = J(await httpGet(MB + "/release/" + relId + "?fmt=json&inc=recordings"));
  if (!r || !r.media) return info("Tracks", "Could not load tracks.");
  const items = [];
  for (const md of r.media) {
    const tracks = md.tracks || [];
    for (const tr of tracks) {
      const mins = tr.length ? msToClock(tr.length) : "";
      const recId = (tr.recording && tr.recording.id) ? tr.recording.id : tr.id;
      items.push({ id: "mb:track:" + recId, title: tr.position + ". " + tr.title, subtitle: mins, type: "track", expandable: false, url: "" });
    }
  }
  return JSON.stringify({ title: r.title || "Tracks", items: items });
}
async function mbAlbumMeta(rgId) {
  const r = J(await httpGet(MB + "/release-group/" + rgId + "?fmt=json&inc=artists+tags"));
  if (!r) return "{}";
  const artist = (r["artist-credit"] && r["artist-credit"][0]) ? r["artist-credit"][0].name : "";
  const facts = [];
  if (artist) facts.push(metaFact("Artist", artist));
  if (r["first-release-date"]) facts.push(metaFact("Released", r["first-release-date"]));
  if (r["primary-type"]) facts.push(metaFact("Type", r["primary-type"]));
  if (r.tags && r.tags.length) {
    r.tags.sort((a, b) => (b.count || 0) - (a.count || 0));
    facts.push(metaFact("Tags", namesOf(r.tags, "name", 5)));
  }
  return metaResult({ title: r.title, subtitle: artist, overview: r.disambiguation || "",
    image: "https://coverartarchive.org/release-group/" + rgId + "/front-250", facts: facts });
}
async function mbTrackMeta(recId) {
  const r = J(await httpGet(MB + "/recording/" + recId + "?fmt=json&inc=artists+releases"));
  if (!r) return "{}";
  const artist = (r["artist-credit"] && r["artist-credit"][0]) ? r["artist-credit"][0].name : "";
  const facts = [];
  if (artist) facts.push(metaFact("Artist", artist));
  if (r.length) facts.push(metaFact("Length", msToClock(r.length)));
  if (r["first-release-date"]) facts.push(metaFact("First released", r["first-release-date"]));
  const rel = (r.releases && r.releases.length) ? r.releases[0] : null;
  if (rel && rel.title) facts.push(metaFact("Appears on", rel.title));
  return metaResult({ title: r.title, subtitle: artist, overview: r.disambiguation || "",
    image: rel ? "https://coverartarchive.org/release/" + rel.id + "/front-250" : "", facts: facts });
}

// ---------------------------------------------------------------------------- Google Books / Open Library
const GBOOKS = "https://www.googleapis.com/books/v1/volumes";
async function gbooks(query, page, type) {
  type = type || "book";
  const label = (type === "audiobook") ? "Audiobooks" : "Books";
  page = page1(page);
  const start = (page - 1) * PAGE;
  const q = query ? query : "subject:fiction";
  const key = getConfig("googleBooksApiKey");
  const url = GBOOKS + "?q=" + enc(q) + "&maxResults=" + PAGE + "&startIndex=" + start + "&printType=books&country=US" + (key ? "&key=" + enc(key) : "");
  const r = J(await httpGet(url));
  if (!r) return info(label, "Could not reach Google Books.");
  if (r.error) {
    if ((r.error.code || 0) >= 500) return await olBooks(query, page, type);
    return info(label, "Google Books: " + (r.error.message || "request failed."));
  }
  const docs = r.items || [], items = [];
  for (const d of docs) {
    const vi = d.volumeInfo || {};
    let img = vi.imageLinks ? (vi.imageLinks.thumbnail || vi.imageLinks.smallThumbnail || "") : "";
    if (img) img = img.replace(/^http:/, "https:");
    items.push({
      id: "googlebooks:" + d.id, title: vi.title || "(untitled)",
      subtitle: ((vi.authors && vi.authors[0]) ? vi.authors[0] : "") + (vi.publishedDate ? " · " + year(vi.publishedDate) : ""),
      type: type, thumbnailUrl: img, expandable: false, url: "",
    });
  }
  return result(label, items, (start + items.length) < (r.totalItems || 0));
}
async function olBooks(query, page, type) {
  type = type || "book";
  const label = (type === "audiobook") ? "Audiobooks" : "Books";
  page = page1(page);
  const offset = (page - 1) * PAGE, items = [];
  let hasMore = false;
  const coverBase = "https://covers.openlibrary.org/b/id/";
  const push = (title, author, yr, coverId, key) => items.push({
    id: "openlibrary:" + (key || ""), title: title, subtitle: (author || "") + (yr ? " · " + yr : ""),
    type: type, thumbnailUrl: coverId ? coverBase + coverId + "-M.jpg" : "", expandable: false, url: "",
  });
  if (query) {
    const r = J(await httpGet("https://openlibrary.org/search.json?limit=" + PAGE + "&page=" + page + "&q=" + enc(query)));
    if (!r || !r.docs) return info(label, "Could not reach the book catalog.");
    for (const d of r.docs) push(d.title, d.author_name && d.author_name[0], d.first_publish_year, d.cover_i, d.key);
    hasMore = (offset + items.length) < (r.numFound || r.num_found || 0);
  } else {
    const r2 = J(await httpGet("https://openlibrary.org/subjects/fiction.json?limit=" + PAGE + "&offset=" + offset));
    if (!r2 || !r2.works) return info(label, "Could not reach the book catalog.");
    for (const w of r2.works) push(w.title, w.authors && w.authors[0] && w.authors[0].name, w.first_publish_year, w.cover_id, w.key);
    hasMore = (offset + PAGE) < (r2.work_count || 0);
  }
  return result(label, items, hasMore);
}
async function booksCatalog(query, page, type) {
  return getConfig("googleBooksApiKey") ? await gbooks(query, page, type) : await olBooks(query, page, type);
}
async function gbookMeta(volId) {
  const key = getConfig("googleBooksApiKey");
  const r = J(await httpGet(GBOOKS + "/" + enc(volId) + (key ? "?key=" + enc(key) : "")));
  if (!r || r.error) return "{}";
  const vi = r.volumeInfo || {}, facts = [];
  if (vi.authors) facts.push(metaFact("Author", vi.authors.join(", ")));
  if (vi.publishedDate) facts.push(metaFact("Published", vi.publishedDate));
  if (vi.publisher) facts.push(metaFact("Publisher", vi.publisher));
  if (vi.pageCount) facts.push(metaFact("Pages", vi.pageCount));
  if (vi.categories) facts.push(metaFact("Categories", vi.categories.join(", ")));
  if (vi.averageRating) facts.push(metaFact("Rating", vi.averageRating + " / 5"));
  let img = vi.imageLinks ? (vi.imageLinks.thumbnail || vi.imageLinks.smallThumbnail || "") : "";
  if (img) img = img.replace(/^http:/, "https:");
  return metaResult({ title: vi.title || "(untitled)", subtitle: (vi.authors && vi.authors[0]) || "",
    overview: stripHtml(vi.description), image: img, facts: facts });
}
async function olBookMeta(key) {
  if (key.charAt(0) !== "/") key = "/" + key;
  const r = J(await httpGet("https://openlibrary.org" + key + ".json"));
  if (!r) return "{}";
  let desc = "";
  if (typeof r.description === "string") desc = r.description;
  else if (r.description && r.description.value) desc = r.description.value;
  const facts = [];
  if (r.first_publish_date) facts.push(metaFact("Published", r.first_publish_date));
  if (r.subjects && r.subjects.length) facts.push(metaFact("Subjects", r.subjects.slice(0, 5).join(", ")));
  const img = (r.covers && r.covers.length && r.covers[0] > 0) ? "https://covers.openlibrary.org/b/id/" + r.covers[0] + "-L.jpg" : "";
  return metaResult({ title: r.title || "(untitled)", subtitle: "", overview: stripHtml(desc), image: img, facts: facts });
}

// ---------------------------------------------------------------------------- Comic Vine
const CV = "https://comicvine.gamespot.com/api";
function cvKey() { return getConfig("comicVineApiKey"); }
function cvImage(img) { return img ? (img.medium_url || img.small_url || img.original_url || "") : ""; }
function cvOk(r) { return r && (!r.error || r.error === "OK"); }
async function comicsCatalog(query, page) {
  page = page1(page);
  const key = cvKey();
  if (!key) return info("Comics", "Set your Comic Vine API key in Configure… to load comics.");
  const fields = "id,name,image,start_year,publisher,count_of_issues";
  const viaSearch = !!query;
  let url;
  if (viaSearch) url = CV + "/search/?api_key=" + enc(key) + "&format=json&resources=volume&limit=" + PAGE + "&page=" + page + "&query=" + enc(query) + "&field_list=" + fields;
  else { const offset = (page - 1) * PAGE; url = CV + "/volumes/?api_key=" + enc(key) + "&format=json&sort=date_last_updated:desc&limit=" + PAGE + "&offset=" + offset + "&field_list=" + fields; }
  const r = J(await httpGet(url));
  if (!r) return info("Comics", "Could not reach Comic Vine.");
  if (!cvOk(r)) return info("Comics", "Comic Vine: " + (r.error || "request failed."));
  const arr = r.results || [], items = [];
  for (const v of arr) {
    const pub = (v.publisher && v.publisher.name) ? v.publisher.name : "";
    items.push({ id: "comicvine:volume:" + v.id, title: v.name,
      subtitle: pub + (v.start_year ? (pub ? " · " : "") + v.start_year : ""),
      type: "comic", thumbnailUrl: cvImage(v.image), expandable: true, url: "" });
  }
  const total = r.number_of_total_results || 0;
  return result("Comics", items, viaSearch ? (arr.length === PAGE) : ((page * PAGE) < total));
}
async function cvIssues(volumeId, page) {
  page = page1(page);
  const key = cvKey(); if (!key) return info("Issues", "Set your Comic Vine API key in Configure…");
  const offset = (page - 1) * PAGE;
  const r = J(await httpGet(CV + "/issues/?api_key=" + enc(key) + "&format=json&filter=volume:" + volumeId + "&sort=issue_number:asc&limit=" + PAGE + "&offset=" + offset + "&field_list=id,name,issue_number,image,cover_date"));
  if (!r) return info("Issues", "Could not load issues.");
  if (!cvOk(r)) return info("Issues", "Comic Vine: " + (r.error || "request failed."));
  const arr = r.results || [], items = [];
  for (const is of arr)
    items.push({ id: "comicvine:issue:" + is.id, title: "#" + (is.issue_number || "?") + (is.name ? " — " + is.name : ""),
      subtitle: is.cover_date || "", type: "comic_issue", thumbnailUrl: cvImage(is.image), expandable: false, url: "" });
  const total = r.number_of_total_results || 0;
  return result("Issues", items, (page * PAGE) < total);
}
async function cvVolumeMeta(id) {
  const key = cvKey(); if (!key) return "{}";
  const r = J(await httpGet(CV + "/volume/4050-" + id + "/?api_key=" + enc(key) + "&format=json&field_list=name,deck,description,start_year,publisher,count_of_issues,image"));
  if (!cvOk(r) || !r.results) return "{}";
  const v = r.results, facts = [];
  if (v.publisher && v.publisher.name) facts.push(metaFact("Publisher", v.publisher.name));
  if (v.start_year) facts.push(metaFact("Started", v.start_year));
  if (v.count_of_issues) facts.push(metaFact("Issues", v.count_of_issues));
  return metaResult({ title: v.name, subtitle: "", overview: v.deck || stripHtml(v.description), image: cvImage(v.image), facts: facts });
}
async function cvIssueMeta(id) {
  const key = cvKey(); if (!key) return "{}";
  const r = J(await httpGet(CV + "/issue/4000-" + id + "/?api_key=" + enc(key) + "&format=json&field_list=name,issue_number,cover_date,store_date,deck,description,image,volume"));
  if (!cvOk(r) || !r.results) return "{}";
  const is = r.results, facts = [];
  if (is.issue_number) facts.push(metaFact("Issue", "#" + is.issue_number));
  if (is.volume && is.volume.name) facts.push(metaFact("Series", is.volume.name));
  if (is.cover_date) facts.push(metaFact("Cover date", is.cover_date));
  if (is.store_date) facts.push(metaFact("Store date", is.store_date));
  const title = is.name || ("#" + (is.issue_number || "") + (is.volume && is.volume.name ? " · " + is.volume.name : ""));
  return metaResult({ title: title, subtitle: (is.volume && is.volume.name) || "",
    overview: is.deck || stripHtml(is.description), image: cvImage(is.image), facts: facts });
}

// ---------------------------------------------------------------------------- MangaDex
const MDX = "https://api.mangadex.org";
const MDX_UP = "https://uploads.mangadex.org";
function firstVal(o) { if (!o) return ""; for (const k in o) if (o[k]) return o[k]; return ""; }
function mdxRatings() {
  let s = "&contentRating[]=safe&contentRating[]=suggestive";
  if (getConfig("mangaShowAdult") === "true") s += "&contentRating[]=erotica&contentRating[]=pornographic";
  return s;
}
function mdxTitle(attr) {
  if (!attr) return "(untitled)";
  const t = attr.title || {};
  if (t.en) return t.en;
  const v = firstVal(t); if (v) return v;
  const alts = attr.altTitles || [];
  for (const alt of alts) { const av = firstVal(alt); if (av) return av; }
  return "(untitled)";
}
function mdxCover(mangaId, rels) {
  rels = rels || [];
  for (const rel of rels)
    if (rel.type === "cover_art" && rel.attributes && rel.attributes.fileName)
      return MDX_UP + "/covers/" + mangaId + "/" + rel.attributes.fileName + ".256.jpg";
  return "";
}
async function mangaCatalog(query, page) {
  page = page1(page);
  const offset = (page - 1) * PAGE;
  let url = MDX + "/manga?limit=" + PAGE + "&offset=" + offset + "&includes[]=cover_art" + mdxRatings();
  url += query ? ("&title=" + enc(query)) : "&order[followedCount]=desc&hasAvailableChapters=true";
  const r = J(await httpGet(url));
  if (!r || !r.data) return info("Manga", "Could not reach MangaDex.");
  const items = [];
  for (const d of r.data) {
    const a = d.attributes || {};
    items.push({ id: "mangadex:" + d.id, title: mdxTitle(a),
      subtitle: (a.year ? String(a.year) : "") + (a.status ? (a.year ? " · " : "") + a.status : ""),
      type: "manga", thumbnailUrl: mdxCover(d.id, d.relationships), expandable: true, url: "" });
  }
  return result("Manga", items, (offset + PAGE) < (r.total || 0));
}
function numOr(v, dflt) { const n = parseFloat(v); return isNaN(n) ? dflt : n; }
async function mangaChapters(mangaId, page) {
  page = page1(page);
  const r = J(await httpGet(MDX + "/manga/" + mangaId + "/aggregate"));
  if (!r || !r.volumes) return info("Chapters", "Could not load chapters.");
  const all = [], vols = r.volumes;
  for (const vk in vols) {
    const vol = vols[vk] || {}, chs = vol.chapters || {};
    for (const ck in chs) {
      const c = chs[ck];
      let ids = [c.id];
      if (c.others && c.others.length) ids = ids.concat(c.others);
      all.push({ vol: vol.volume, chapter: c.chapter, ids: ids, count: c.count || 1 });
    }
  }
  if (!all.length) return info("Chapters", "No chapters available for this title.");
  all.sort((a, b) => {
    const av = numOr(a.vol, 1e9), bv = numOr(b.vol, 1e9);
    if (av !== bv) return av - bv;
    return numOr(a.chapter, 1e9) - numOr(b.chapter, 1e9);
  });
  const start = (page - 1) * PAGE, slice = all.slice(start, start + PAGE), items = [];
  for (const s of slice) {
    const vlabel = (s.vol && s.vol !== "none") ? ("Vol. " + s.vol + " · ") : "";
    items.push({ id: "mangadexch:" + s.ids.join(","), title: vlabel + (s.chapter ? ("Ch. " + s.chapter) : "Oneshot"),
      subtitle: s.count > 1 ? (s.count + " versions") : "1 version", type: "manga_chapter", thumbnailUrl: "", expandable: false, url: "" });
  }
  return result("Chapters", items, (start + PAGE) < all.length);
}
async function mangaMeta(mangaId) {
  const r = J(await httpGet(MDX + "/manga/" + mangaId + "?includes[]=cover_art"));
  if (!r || !r.data) return "{}";
  const d = r.data, a = d.attributes || {}, facts = [];
  if (a.status) facts.push(metaFact("Status", a.status));
  if (a.year) facts.push(metaFact("Year", a.year));
  if (a.publicationDemographic) facts.push(metaFact("Demographic", a.publicationDemographic));
  if (a.contentRating) facts.push(metaFact("Rating", a.contentRating));
  if (a.lastChapter) facts.push(metaFact("Last chapter", a.lastChapter));
  const genres = [], tags = a.tags || [];
  for (const tg of tags) { const ta = tg.attributes; if (ta && ta.group === "genre" && ta.name && ta.name.en) genres.push(ta.name.en); }
  if (genres.length) facts.push(metaFact("Genres", genres.slice(0, 6).join(", ")));
  return metaResult({ title: mdxTitle(a), subtitle: "",
    overview: stripHtml(a.description ? (a.description.en || firstVal(a.description)) : ""),
    image: mdxCover(d.id, d.relationships), facts: facts });
}
function chapterMetaFromObj(obj) {
  const a = obj.attributes || {}, rels = obj.relationships || [], facts = [];
  if (a.volume) facts.push(metaFact("Volume", a.volume));
  if (a.chapter) facts.push(metaFact("Chapter", a.chapter));
  if (a.pages) facts.push(metaFact("Pages", a.pages));
  if (a.translatedLanguage) facts.push(metaFact("Language", a.translatedLanguage));
  if (a.publishAt) facts.push(metaFact("Published", a.publishAt.substring(0, 10)));
  let group = "", series = "";
  for (const rel of rels) {
    if (rel.type === "scanlation_group" && rel.attributes) group = rel.attributes.name;
    if (rel.type === "manga" && rel.attributes) series = mdxTitle(rel.attributes);
  }
  if (group) facts.push(metaFact("Group", group));
  if (series) facts.push(metaFact("Series", series));
  const title = (a.chapter ? "Chapter " + a.chapter : "Oneshot") + (a.title ? " — " + a.title : "");
  return metaResult({ title: title, subtitle: series, overview: "", image: "", facts: facts });
}
async function mangaChapterMeta(idsCsv) {
  const ids = idsCsv.split(",");
  if (ids.length > 1) {
    let q = MDX + "/chapter?translatedLanguage[]=en&limit=1&includes[]=scanlation_group&includes[]=manga";
    for (const id of ids) q += "&ids[]=" + id;
    const pick = J(await httpGet(q));
    if (pick && pick.data && pick.data.length) return chapterMetaFromObj(pick.data[0]);
  }
  const r = J(await httpGet(MDX + "/chapter/" + ids[0] + "?includes[]=scanlation_group&includes[]=manga"));
  if (!r || !r.data) return "{}";
  return chapterMetaFromObj(r.data);
}

// ---------------------------------------------------------------------------- entry points
async function getCatalog(argJson) {
  const a = J(argJson) || {};
  const cat = a.catalog || "movies", q = a.query || "", p = a.page || 1;
  if (cat === "movies") return await tmdbList("movie", q, p);
  if (cat === "tv") return await tmdbList("tv", q, p);
  if (cat === "games") return await gamesCatalog(q, p);
  if (cat === "music") return await mbAlbums(q, p);
  if (cat === "books") return await booksCatalog(q, p);
  if (cat === "audiobooks") return await booksCatalog(q, p, "audiobook");
  if (cat === "comics") return await comicsCatalog(q, p);
  if (cat === "manga") return await mangaCatalog(q, p);
  return info("AIO Catalog", "Pick a media type.");
}
async function getDetail(argJson) {
  const a = J(argJson) || {};
  const p = a.page || 1, parts = (a.id || "").split(":");
  if (a.type === "series") return await tmdbSeasons(parts[2]);
  if (a.type === "season") return await tmdbEpisodes(parts[2], parts[3]);
  if (a.type === "album") return await mbTracks(parts[2]);
  if (a.type === "platform") return await igdbPlatformGames(parts[1], p);
  if (a.type === "comic") return await cvIssues(parts[2], p);
  if (a.type === "manga") return await mangaChapters(parts[1], p);
  return JSON.stringify({ title: "", items: [] });
}
async function getMeta(argJson) {
  const a = J(argJson) || {};
  const id = a.id || "", parts = id.split(":");
  if (a.type === "movie") return await tmdbMovieMeta(parts[2]);
  if (a.type === "series") return await tmdbSeriesMeta(parts[2]);
  if (a.type === "season") return await tmdbSeasonMeta(parts[2], parts[3]);
  if (a.type === "episode") return await tmdbEpisodeMeta(parts[2], parts[3], parts[4]);
  if (a.type === "game") return await igdbGameMeta(parts[1]);
  if (a.type === "album") return await mbAlbumMeta(parts[2]);
  if (a.type === "track") return await mbTrackMeta(parts[2]);
  if (a.type === "comic") return await cvVolumeMeta(parts[2]);
  if (a.type === "comic_issue") return await cvIssueMeta(parts[2]);
  if (a.type === "manga") return await mangaMeta(parts[1]);
  if (a.type === "manga_chapter") return await mangaChapterMeta(parts[1]);
  if (a.type === "book" || a.type === "audiobook") {
    if (parts[0] === "googlebooks") return await gbookMeta(id.substring("googlebooks:".length));
    if (parts[0] === "openlibrary") return await olBookMeta(id.substring("openlibrary:".length));
  }
  return "{}";
}

// ---- protocol routing -------------------------------------------------------------------------------
const HEADERS = { "content-type": "application/json; charset=utf-8", "cache-control": "public, max-age=600", "access-control-allow-origin": "*" };
const jsonObj = (o, status = 200) => new Response(JSON.stringify(o), { status, headers: HEADERS });
const jsonStr = (s) => new Response(s, { headers: HEADERS });
function parseExtras(seg) {
  const out = {};
  if (!seg) return out;
  for (const pair of seg.split("&")) { const i = pair.indexOf("="); if (i > 0) out[pair.slice(0, i)] = decodeURIComponent(pair.slice(i + 1)); }
  return out;
}
const dec = (s) => (s == null ? "" : decodeURIComponent(s));

// The app sends each user's config (their API keys etc.) as base64url(JSON) in the X-MMV-Config header.
function parseConfigHeader(h) {
  if (!h) return {};
  try { return JSON.parse(atob(h.replace(/-/g, "+").replace(/_/g, "/"))) || {}; } catch (e) { return {}; }
}

async function route(request) {
  const parts = new URL(request.url).pathname.replace(/^\/+|\/+$/g, "").split("/");
  const last = parts[parts.length - 1] || "";
  if (last.endsWith(".json")) parts[parts.length - 1] = last.slice(0, -5);

  if (parts[0] === "manifest") return jsonObj(MANIFEST);
  if (parts[0] === "catalog") {
    const ex = parseExtras(parts[2]);
    return jsonStr(await getCatalog(JSON.stringify({ catalog: dec(parts[1]), query: ex.search || "", page: Number(ex.page) || 1 })));
  }
  if (parts[0] === "meta") return jsonStr(await getMeta(JSON.stringify({ type: dec(parts[1]), id: dec(parts[2]) })));
  if (parts[0] === "detail") {
    const ex = parseExtras(parts[3]);
    return jsonStr(await getDetail(JSON.stringify({ type: dec(parts[1]), id: dec(parts[2]), page: Number(ex.page) || 1 })));
  }
  if (parts[0] === "stream") return jsonObj({ streams: [] }); // metadata source: no playable files
  return jsonObj({ error: "not found" }, 404);
}

export default {
  async fetch(request, env) {
    ENV = env;
    const cfg = parseConfigHeader(request.headers.get("X-MMV-Config"));
    // Run the whole request inside the per-user config scope so getConfig() resolves THIS user's keys.
    return cfgStore.run(cfg, async () => {
      try { return await route(request); }
      catch (e) { return jsonObj({ error: String((e && e.message) || e) }, 502); }
    });
  },
};
