// A My Media Vault remote addon, as a Cloudflare Worker. It speaks the app's remote-addon protocol
// (the same JSON the in-app JS addons return - MediaCatalog / MediaDetail) and is backed here by TMDB.
//
// Routes (all GET, all end in .json so the same layout also works as static files):
//   /manifest.json
//   /catalog/{id}.json                 (+ "/search={q}" and/or "/page={n}" path segments)
//   /meta/{type}/{id}.json
//   /detail/{type}/{id}.json           (+ "/page={n}")  -> a series' episodes
//
// Deploy: set a TMDB_API_KEY secret (`wrangler secret put TMDB_API_KEY`) and `wrangler deploy`.
// Then add the Worker's URL in the app: Add-ons -> Add addon by URL.

const TMDB = "https://api.themoviedb.org/3";
const IMG = "https://image.tmdb.org/t/p/w342";

const MANIFEST = {
  id: "com.mymediavault.tmdb-worker",
  name: "TMDB (Worker)",
  version: "1.0.0",
  type: "media-source",
  author: "My Media Vault",
  description: "Movies & series from TMDB, served through the My Media Vault remote-addon protocol on a Cloudflare Worker.",
  catalogs: [
    { id: "movies", name: "Movies", type: "movie" },
    { id: "series", name: "Series", type: "series" },
  ],
};

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "public, max-age=600" },
  });

// Parse the trailing "/key=value&key=value" extras segment that precedes ".json".
function parseExtras(seg) {
  const out = {};
  if (!seg) return out;
  for (const pair of seg.split("&")) {
    const i = pair.indexOf("=");
    if (i > 0) out[pair.slice(0, i)] = decodeURIComponent(pair.slice(i + 1));
  }
  return out;
}

async function tmdb(env, path, params = {}) {
  const u = new URL(TMDB + path);
  u.searchParams.set("api_key", env.TMDB_API_KEY);
  for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v);
  const r = await fetch(u, { cf: { cacheTtl: 600 } });
  if (!r.ok) throw new Error(`tmdb ${r.status}`);
  return r.json();
}

const movieItem = (m) => ({
  id: String(m.id), type: "movie", title: m.title,
  subtitle: (m.release_date || "").slice(0, 4),
  thumbnailUrl: m.poster_path ? IMG + m.poster_path : "",
});
const seriesItem = (s) => ({
  id: String(s.id), type: "series", title: s.name,
  subtitle: (s.first_air_date || "").slice(0, 4),
  thumbnailUrl: s.poster_path ? IMG + s.poster_path : "",
  expandable: true,
});

async function catalog(env, id, extras) {
  const page = Number(extras.page) || 1;
  const query = extras.search;
  if (id === "movies") {
    const data = query
      ? await tmdb(env, "/search/movie", { query, page })
      : await tmdb(env, "/movie/popular", { page });
    return { title: "Movies", hasMore: page < data.total_pages, items: (data.results || []).map(movieItem) };
  }
  if (id === "series") {
    const data = query
      ? await tmdb(env, "/search/tv", { query, page })
      : await tmdb(env, "/tv/popular", { page });
    return { title: "Series", hasMore: page < data.total_pages, items: (data.results || []).map(seriesItem) };
  }
  return { title: "", items: [] };
}

async function meta(env, type, id) {
  if (type === "movie") {
    const m = await tmdb(env, `/movie/${id}`);
    return {
      title: m.title,
      subtitle: [(m.release_date || "").slice(0, 4), m.runtime ? m.runtime + " min" : ""].filter(Boolean).join(" · "),
      overview: m.overview || "",
      image: m.poster_path ? IMG + m.poster_path : "",
      facts: [
        m.release_date && { label: "Released", value: m.release_date },
        m.vote_average && { label: "Rating", value: m.vote_average.toFixed(1) + " / 10" },
        m.genres?.length && { label: "Genres", value: m.genres.map((g) => g.name).join(", ") },
      ].filter(Boolean),
    };
  }
  if (type === "series") {
    const s = await tmdb(env, `/tv/${id}`);
    return {
      title: s.name,
      subtitle: (s.first_air_date || "").slice(0, 4),
      overview: s.overview || "",
      image: s.poster_path ? IMG + s.poster_path : "",
      facts: [
        s.number_of_seasons && { label: "Seasons", value: String(s.number_of_seasons) },
        s.vote_average && { label: "Rating", value: s.vote_average.toFixed(1) + " / 10" },
        s.genres?.length && { label: "Genres", value: s.genres.map((g) => g.name).join(", ") },
      ].filter(Boolean),
    };
  }
  return { title: "" };
}

// A series' children: season 1's episodes (a fuller addon would expose seasons as a middle layer).
async function detail(env, type, id) {
  if (type !== "series") return { title: "", items: [] };
  const season = await tmdb(env, `/tv/${id}/season/1`);
  return {
    title: season.name || "Season 1",
    hasMore: false,
    items: (season.episodes || []).map((e) => ({
      id: `${id}:1:${e.episode_number}`, type: "episode",
      title: e.name || `Episode ${e.episode_number}`,
      subtitle: `S1 E${e.episode_number}`,
      thumbnailUrl: e.still_path ? IMG + e.still_path : "",
    })),
  };
}

export default {
  async fetch(request, env) {
    try {
      if (!env.TMDB_API_KEY) return json({ error: "Set the TMDB_API_KEY secret on this Worker." }, 500);
      const parts = new URL(request.url).pathname.replace(/^\/+|\/+$/g, "").split("/");
      // strip the trailing ".json" off the last segment
      const last = parts[parts.length - 1] || "";
      if (last.endsWith(".json")) parts[parts.length - 1] = last.slice(0, -5);

      if (parts[0] === "manifest") return json(MANIFEST);
      if (parts[0] === "catalog") return json(await catalog(env, parts[1], parseExtras(parts[2])));
      if (parts[0] === "meta") return json(await meta(env, parts[1], parts[2]));
      if (parts[0] === "detail") return json(await detail(env, parts[1], parts[2]));
      return json({ error: "not found" }, 404);
    } catch (e) {
      return json({ error: String(e && e.message || e) }, 502);
    }
  },
};
