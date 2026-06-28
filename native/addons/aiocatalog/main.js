// AIO Catalog — an all-in-one metadata media-source addon.
//
//   Movies & TV  -> TMDB        (settings: tmdbApiKey)
//   Games        -> IGDB        (settings: igdbClientId + igdbClientSecret; optional steamGridDbKey)
//   Music        -> MusicBrainz (no key needed) + Cover Art Archive
//
// Contract:
//   getCatalog({ catalog, query })  -> { title, items:[ MediaItem ] }   (one of the manifest "catalogs")
//   getDetail({ id, type })         -> { title, items:[ MediaItem ] }   (children of a container item)
//   getMeta({ id, type })           -> { title, subtitle, overview, image, facts:[{label,value}] }
//                                       (metadata for the item's detail-page header; "{}" if unavailable)
//
// A MediaItem: { id, title, subtitle, type, thumbnailUrl, url, expandable }
//   - expandable:true marks a container (TV show / season / album) -> clicking calls getDetail.
//   - clicking any item opens a detail page; getMeta fills its header (show vs. episode, album vs. song).
//   - url is left empty for now; local files get associated with these items later.

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }
function info(title, msg) { return JSON.stringify({ title: title, items: [{ id: "_info", title: msg, type: "info" }] }); }
function year(d) { return (d || "").substring(0, 4); }
function page1(p) { return (p && p > 0) ? p : 1; }
var PAGE = 40; // items per page
// Wrap items with a paging flag the app uses for infinite scroll.
function result(title, items, hasMore) { return JSON.stringify({ title: title, items: items, hasMore: !!hasMore }); }

// ---- getMeta helpers (single-item detail header: title/subtitle/overview/image + labelled facts) ----
function metaFact(label, value) { return { label: label, value: (value == null) ? "" : String(value) }; }
function metaResult(o) { return JSON.stringify(o); }   // { title, subtitle, overview, image, facts:[] }
function rating10(v) { return (v || v === 0) ? (Math.round(v * 10) / 10) + " / 10" : ""; }
function stripHtml(s) { return (s || "").replace(/<[^>]*>/g, " ").replace(/\s+/g, " ").trim(); }
function namesOf(arr, field, max) { // join the .name (or [field]) of an array, capped at max
    if (!arr) return "";
    var out = [];
    for (var i = 0; i < arr.length && (!max || i < max); i++) out.push(field ? arr[i][field] : arr[i]);
    return out.join(", ");
}
function msToClock(ms) { return Math.floor(ms / 60000) + ":" + ("0" + Math.floor((ms % 60000) / 1000)).slice(-2); }

// ---------------------------------------------------------------------------- TMDB (movies + TV)

var TMDB = "https://api.themoviedb.org/3";
var TMDB_IMG = "https://image.tmdb.org/t/p/w342";
var TMDB_IMG_LG = "https://image.tmdb.org/t/p/w500"; // detail-header covers

// TMDB genre id -> name. Movies and TV use different genre sets.
var TMDB_MOVIE_GENRES = [["28","Action"],["12","Adventure"],["16","Animation"],["35","Comedy"],["80","Crime"],
    ["99","Documentary"],["18","Drama"],["10751","Family"],["14","Fantasy"],["36","History"],["27","Horror"],
    ["10402","Music"],["9648","Mystery"],["10749","Romance"],["878","Sci-Fi"],["53","Thriller"],["10752","War"],["37","Western"]];
var TMDB_TV_GENRES = [["10759","Action & Adventure"],["16","Animation"],["35","Comedy"],["80","Crime"],
    ["99","Documentary"],["18","Drama"],["10751","Family"],["10762","Kids"],["9648","Mystery"],
    ["10765","Sci-Fi & Fantasy"],["10768","War & Politics"],["37","Western"]];

function optList(pairs, anyLabel) {
    var o = [{ value: "", label: anyLabel }];
    for (var i = 0; i < pairs.length; i++) o.push({ value: pairs[i][0], label: pairs[i][1] });
    return o;
}
function yearOptions() {
    var cy = 2026; try { cy = new Date().getFullYear(); } catch (e) {}
    var o = [{ value: "", label: "Any year" }];
    for (var y = cy; y >= 1960; y--) o.push({ value: String(y), label: String(y) });
    return o;
}
function tmdbFilters(kind) {
    return [
        { key: "genre", label: "Genre", options: optList(kind === "tv" ? TMDB_TV_GENRES : TMDB_MOVIE_GENRES, "Any genre") },
        { key: "year",  label: "Year",  options: yearOptions() },
        { key: "rating", label: "Min rating", options: [
            { value: "", label: "Any" }, { value: "9", label: "9+" }, { value: "8", label: "8+" },
            { value: "7", label: "7+" }, { value: "6", label: "6+" }, { value: "5", label: "5+" }] },
        { key: "sort", label: "Sort", options: [
            { value: "", label: "Popular" }, { value: "top", label: "Top rated" }, { value: "new", label: "Newest" }] }
    ];
}
function resultF(title, items, hasMore, filters) {
    return JSON.stringify({ title: title, items: items, hasMore: !!hasMore, filters: filters });
}
function tmdbItems(results, kind, path) {
    var items = [];
    for (var i = 0; i < results.length; i++) {
        var m = results[i];
        items.push({
            id: "tmdb:" + path + ":" + m.id,
            title: m.title || m.name,
            subtitle: year(m.release_date || m.first_air_date),
            type: kind === "tv" ? "series" : "movie",
            thumbnailUrl: m.poster_path ? TMDB_IMG + m.poster_path : "",
            expandable: kind === "tv",
            url: ""
        });
    }
    return items;
}

function tmdbList(kind, query, page, f) {
    page = page1(page); f = f || {};
    var key = getConfig("tmdbApiKey");
    var label = kind === "tv" ? "TV Shows" : "Movies";
    if (!key) return info(label, "Set your TMDB API key in Configure… to load " + kind + ".");
    var path = kind === "tv" ? "tv" : "movie";
    var filters = tmdbFilters(kind);

    if (query) {
        // /search can't filter server-side, so filter + sort this page of results client-side.
        var sr = J(httpGet(TMDB + "/search/" + path + "?api_key=" + key + "&page=" + page + "&query=" + enc(query)));
        if (!sr || !sr.results) return info(label, "Could not reach TMDB.");
        var rows = sr.results;
        if (f.genre)  rows = rows.filter(function (m) { return (m.genre_ids || []).indexOf(parseInt(f.genre, 10)) >= 0; });
        if (f.rating) rows = rows.filter(function (m) { return (m.vote_average || 0) >= parseFloat(f.rating); });
        if (f.year)   rows = rows.filter(function (m) { return year(m.release_date || m.first_air_date) === f.year; });
        if (f.sort === "top") rows.sort(function (a, b) { return (b.vote_average || 0) - (a.vote_average || 0); });
        else if (f.sort === "new") rows.sort(function (a, b) {
            return (b.release_date || b.first_air_date || "").localeCompare(a.release_date || a.first_air_date || ""); });
        return resultF(label, tmdbItems(rows, kind, path), sr.page < sr.total_pages, filters);
    }

    // No query: TMDB /discover applies every filter server-side.
    var sort = f.sort === "top" ? "vote_average.desc"
             : f.sort === "new" ? (kind === "tv" ? "first_air_date.desc" : "primary_release_date.desc")
             : "popularity.desc";
    var url = TMDB + "/discover/" + path + "?api_key=" + key + "&page=" + page + "&sort_by=" + sort + "&vote_count.gte=50";
    if (f.genre)  url += "&with_genres=" + enc(f.genre);
    if (f.rating) url += "&vote_average.gte=" + enc(f.rating);
    if (f.year)   url += (kind === "tv" ? "&first_air_date_year=" : "&primary_release_year=") + enc(f.year);
    var r = J(httpGet(url));
    if (!r || !r.results) return info(label, "Could not reach TMDB (check your key / connection).");
    return resultF(label, tmdbItems(r.results, kind, path), r.page < r.total_pages, filters);
}

function tmdbSeasons(showId) {
    var key = getConfig("tmdbApiKey"); if (!key) return info("Seasons", "Set your TMDB API key.");
    var r = J(httpGet(TMDB + "/tv/" + showId + "?api_key=" + key));
    if (!r || !r.seasons) return info("Seasons", "Could not load seasons.");
    var items = [];
    for (var i = 0; i < r.seasons.length; i++) {
        var s = r.seasons[i];
        items.push({
            id: "tmdb:season:" + showId + ":" + s.season_number,
            title: s.name || ("Season " + s.season_number),
            subtitle: (s.episode_count || 0) + " episodes",
            type: "season",
            thumbnailUrl: s.poster_path ? TMDB_IMG + s.poster_path : "",
            expandable: true, url: ""
        });
    }
    return JSON.stringify({ title: r.name || "Seasons", items: items });
}

function tmdbEpisodes(showId, seasonNo) {
    var key = getConfig("tmdbApiKey"); if (!key) return info("Episodes", "Set your TMDB API key.");
    var r = J(httpGet(TMDB + "/tv/" + showId + "/season/" + seasonNo + "?api_key=" + key));
    if (!r || !r.episodes) return info("Episodes", "Could not load episodes.");
    var items = [];
    for (var i = 0; i < r.episodes.length; i++) {
        var e = r.episodes[i];
        items.push({
            id: "tmdb:episode:" + showId + ":" + seasonNo + ":" + e.episode_number,
            title: "E" + e.episode_number + " — " + e.name,
            subtitle: e.air_date || "",
            type: "episode",
            thumbnailUrl: e.still_path ? TMDB_IMG + e.still_path : "",
            expandable: false, url: ""
        });
    }
    return JSON.stringify({ title: r.name || "Episodes", items: items });
}

function tmdbMovieMeta(id) {
    var key = getConfig("tmdbApiKey"); if (!key) return "{}";
    // append external_ids so we can carry the IMDB id (lets IMDB-only stream addons resolve a TMDB title).
    var r = J(httpGet(TMDB + "/movie/" + id + "?api_key=" + key + "&append_to_response=external_ids"));
    if (!r || r.success === false) return "{}";
    var facts = [];
    if (r.release_date) facts.push(metaFact("Released", r.release_date));
    if (r.runtime)      facts.push(metaFact("Runtime", r.runtime + " min"));
    if (r.vote_average) facts.push(metaFact("Rating", rating10(r.vote_average)));
    if (r.genres && r.genres.length) facts.push(metaFact("Genres", namesOf(r.genres, "name")));
    var imdb = (r.external_ids && r.external_ids.imdb_id) || "";
    return metaResult({ title: r.title || r.name, subtitle: r.tagline || "", overview: r.overview || "",
        image: r.poster_path ? TMDB_IMG_LG + r.poster_path : "", facts: facts, imdbStreamId: imdb });
}

function tmdbSeriesMeta(id) {
    var key = getConfig("tmdbApiKey"); if (!key) return "{}";
    var r = J(httpGet(TMDB + "/tv/" + id + "?api_key=" + key));
    if (!r || r.success === false) return "{}";
    var facts = [];
    if (r.first_air_date)     facts.push(metaFact("First aired", r.first_air_date));
    if (r.number_of_seasons)  facts.push(metaFact("Seasons", r.number_of_seasons));
    if (r.number_of_episodes) facts.push(metaFact("Episodes", r.number_of_episodes));
    if (r.vote_average)       facts.push(metaFact("Rating", rating10(r.vote_average)));
    if (r.genres && r.genres.length) facts.push(metaFact("Genres", namesOf(r.genres, "name")));
    if (r.status)             facts.push(metaFact("Status", r.status));
    return metaResult({ title: r.name, subtitle: r.tagline || "", overview: r.overview || "",
        image: r.poster_path ? TMDB_IMG_LG + r.poster_path : "", facts: facts });
}

function tmdbSeasonMeta(showId, n) {
    var key = getConfig("tmdbApiKey"); if (!key) return "{}";
    var r = J(httpGet(TMDB + "/tv/" + showId + "/season/" + n + "?api_key=" + key));
    if (!r || r.success === false) return "{}";
    var facts = [];
    if (r.air_date) facts.push(metaFact("Air date", r.air_date));
    if (r.episodes) facts.push(metaFact("Episodes", r.episodes.length));
    return metaResult({ title: r.name || ("Season " + n), subtitle: "", overview: r.overview || "",
        image: r.poster_path ? TMDB_IMG_LG + r.poster_path : "", facts: facts });
}

function tmdbEpisodeMeta(showId, season, ep) {
    var key = getConfig("tmdbApiKey"); if (!key) return "{}";
    var r = J(httpGet(TMDB + "/tv/" + showId + "/season/" + season + "/episode/" + ep + "?api_key=" + key));
    if (!r || r.success === false) return "{}";
    var facts = [];
    facts.push(metaFact("Season / Episode", "S" + season + " · E" + ep));
    if (r.air_date)     facts.push(metaFact("Air date", r.air_date));
    if (r.runtime)      facts.push(metaFact("Runtime", r.runtime + " min"));
    if (r.vote_average) facts.push(metaFact("Rating", rating10(r.vote_average)));
    // Stremio series streams are keyed by the SHOW's IMDB id + season + episode ("ttShow:season:ep").
    var ids = J(httpGet(TMDB + "/tv/" + showId + "/external_ids?api_key=" + key));
    var showImdb = (ids && ids.imdb_id) || "";
    var streamId = showImdb ? (showImdb + ":" + season + ":" + ep) : "";
    return metaResult({ title: r.name || ("Episode " + ep), subtitle: "", overview: r.overview || "",
        image: r.still_path ? TMDB_IMG_LG + r.still_path : "", facts: facts, imdbStreamId: streamId });
}

// ---------------------------------------------------------------------------- IGDB (games)

function igdbToken() {
    var id = getConfig("igdbClientId"), secret = getConfig("igdbClientSecret");
    if (!id || !secret) return "";
    var cached = getStorage("igdbToken"), exp = parseInt(getStorage("igdbTokenExp") || "0", 10);
    if (cached && exp > Date.now()) return cached;
    var resp = J(httpRequest({
        method: "POST",
        url: "https://id.twitch.tv/oauth2/token?client_id=" + enc(id) + "&client_secret=" + enc(secret) + "&grant_type=client_credentials"
    }));
    if (!resp || !resp.access_token) return "";
    setStorage("igdbToken", resp.access_token);
    setStorage("igdbTokenExp", String(Date.now() + ((resp.expires_in || 3600) - 60) * 1000));
    return resp.access_token;
}

// Curated consoles (IGDB platform ids). The Games catalog lists these; drilling into one fetches its games.
var CONSOLES = [
    // Nintendo
    { id: 130, name: "Nintendo Switch" }, { id: 41, name: "Wii U" }, { id: 5, name: "Wii" },
    { id: 21, name: "GameCube" }, { id: 4, name: "Nintendo 64" }, { id: 19, name: "SNES" },
    { id: 18, name: "NES" }, { id: 51, name: "Famicom Disk System" }, { id: 87, name: "Virtual Boy" },
    { id: 37, name: "Nintendo 3DS" }, { id: 20, name: "Nintendo DS" }, { id: 24, name: "Game Boy Advance" },
    { id: 22, name: "Game Boy Color" }, { id: 33, name: "Game Boy" },
    // Sony
    { id: 7, name: "PlayStation" }, { id: 8, name: "PlayStation 2" }, { id: 9, name: "PlayStation 3" },
    { id: 48, name: "PlayStation 4" }, { id: 38, name: "PSP" }, { id: 46, name: "PlayStation Vita" },
    // Microsoft
    { id: 11, name: "Xbox" }, { id: 12, name: "Xbox 360" },
    { id: 6, name: "PC (Windows)" }, { id: 13, name: "MS-DOS" },
    // Sega
    { id: 84, name: "SG-1000" }, { id: 64, name: "Master System" }, { id: 29, name: "Sega Genesis" },
    { id: 78, name: "Sega CD" }, { id: 30, name: "Sega 32X" }, { id: 32, name: "Sega Saturn" },
    { id: 23, name: "Dreamcast" }, { id: 35, name: "Game Gear" },
    // Atari
    { id: 59, name: "Atari 2600" }, { id: 66, name: "Atari 5200" }, { id: 60, name: "Atari 7800" },
    { id: 61, name: "Atari Lynx" }, { id: 62, name: "Atari Jaguar" }, { id: 63, name: "Atari ST" },
    // NEC
    { id: 86, name: "TurboGrafx-16" }, { id: 128, name: "PC Engine SuperGrafx" },
    // SNK
    { id: 80, name: "Neo Geo" }, { id: 119, name: "Neo Geo Pocket" }, { id: 120, name: "Neo Geo Pocket Color" },
    // Bandai
    { id: 57, name: "WonderSwan" }, { id: 123, name: "WonderSwan Color" },
    // Other classic consoles
    { id: 50, name: "3DO" }, { id: 67, name: "Intellivision" }, { id: 68, name: "ColecoVision" },
    { id: 70, name: "Vectrex" }, { id: 117, name: "Philips CD-i" },
    // Home computers
    { id: 25, name: "Amiga" }, { id: 27, name: "MSX" }, { id: 26, name: "ZX Spectrum" },
    // Arcade
    { id: 52, name: "Arcade" }
];

function igdbCreds() { return getConfig("igdbClientId") && getConfig("igdbClientSecret"); }

// Run an IGDB /games query body; returns the parsed array (or null on auth/connection failure).
function igdbQuery(body) {
    var token = igdbToken();
    if (!token) return null;
    return J(httpRequest({
        method: "POST", url: "https://api.igdb.com/v4/games", body: body,
        headers: { "Client-ID": getConfig("igdbClientId"), "Authorization": "Bearer " + token, "Accept": "application/json" }
    }));
}

function igdbToItems(arr) {
    var items = [];
    for (var i = 0; i < arr.length; i++) {
        var g = arr[i];
        var cover = (g.cover && g.cover.image_id)
            ? "https://images.igdb.com/igdb/image/upload/t_cover_big/" + g.cover.image_id + ".jpg" : "";
        var y = g.first_release_date ? String(new Date(g.first_release_date * 1000).getFullYear()) : "";
        items.push({ id: "igdb:" + g.id, title: g.name, subtitle: y, type: "game",
                     thumbnailUrl: cover, expandable: false, url: "" });
    }
    return items;
}

function consoleSlug(name) {
    return name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "");
}

function igdbConsoles() {
    var items = [];
    for (var i = 0; i < CONSOLES.length; i++)
        items.push({ id: "igdbplatform:" + CONSOLES[i].id, title: CONSOLES[i].name,
                     subtitle: "", type: "platform", expandable: true,
                     thumbnailUrl: "consoles/" + consoleSlug(CONSOLES[i].name) + ".svg", url: "" });
    return JSON.stringify({ title: "Games by Console", items: items });
}

// IGDB genre id -> name (stable integer ids).
var IGDB_GENRES = [["2","Point & Click"],["4","Fighting"],["5","Shooter"],["7","Music"],["8","Platform"],
    ["9","Puzzle"],["10","Racing"],["11","RTS"],["12","RPG"],["13","Simulator"],["14","Sport"],["15","Strategy"],
    ["16","Turn-based"],["24","Tactical"],["25","Hack & Slash"],["31","Adventure"],["32","Indie"],["33","Arcade"],
    ["34","Visual Novel"],["35","Card & Board"]];
function gameFilters() {
    return [
        { key: "genre", label: "Genre", options: optList(IGDB_GENRES, "Any genre") },
        { key: "sort", label: "Sort", options: [
            { value: "", label: "Top rated" }, { value: "new", label: "Newest" }, { value: "name", label: "Name" }] }
    ];
}
function igdbSort(f) {
    return f.sort === "new" ? "first_release_date desc" : f.sort === "name" ? "name asc" : "rating desc";
}

// Games catalog: browse-by-console when there's no query; search across all platforms when there is.
function gamesCatalog(query, page, f) {
    f = f || {};
    if (query) {
        page = page1(page);
        if (!igdbCreds()) return info("Games", "Set your IGDB (Twitch) client id + secret in Configure… to search games.");
        var body = 'search "' + query.replace(/"/g, "") + '"; fields name,cover.image_id,first_release_date; ';
        if (f.genre) body += 'where genres = (' + parseInt(f.genre, 10) + '); ';
        body += 'limit ' + PAGE + '; offset ' + ((page - 1) * PAGE) + ';';
        var r = igdbQuery(body);
        if (!r) return info("Games", "Could not authenticate with IGDB (check client id/secret).");
        return resultF("Games: " + query, igdbToItems(r), r.length === PAGE, gameFilters());
    }
    return igdbConsoles(); // static list, no paging (filters appear once you drill into a console)
}

function igdbPlatformGames(platformId, page, f) {
    page = page1(page); f = f || {};
    if (!igdbCreds()) return info("Games", "Set your IGDB (Twitch) client id + secret in Configure… to load games.");
    var where = "where platforms = (" + platformId + ") & cover != null";
    if (f.genre) where += " & genres = (" + parseInt(f.genre, 10) + ")";
    var r = igdbQuery('fields name,cover.image_id,first_release_date,rating; ' + where + '; sort ' + igdbSort(f) + '; ' +
                      'limit ' + PAGE + '; offset ' + ((page - 1) * PAGE) + ';');
    if (!r) return info("Games", "Could not load games for this console.");
    if (!r.length && page === 1) return resultF("Games", [{ id: "_info", title: "No games match these filters.", type: "info" }], false, gameFilters());
    return resultF("Games", igdbToItems(r), r.length === PAGE, gameFilters());
}

function igdbGameMeta(id) {
    if (!igdbCreds()) return "{}";
    var r = igdbQuery('fields name,summary,storyline,rating,aggregated_rating,first_release_date,' +
        'genres.name,platforms.name,cover.image_id,involved_companies.developer,involved_companies.company.name; ' +
        'where id = ' + id + ';');
    if (!r || !r.length) return "{}";
    var g = r[0], facts = [];
    if (g.first_release_date)
        facts.push(metaFact("Released", String(new Date(g.first_release_date * 1000).getFullYear())));
    var rt = g.rating || g.aggregated_rating;
    if (rt) facts.push(metaFact("Rating", Math.round(rt) + " / 100"));
    if (g.genres)    facts.push(metaFact("Genres", namesOf(g.genres, "name")));
    if (g.platforms) facts.push(metaFact("Platforms", namesOf(g.platforms, "name", 6)));
    var dev = "";
    if (g.involved_companies)
        for (var i = 0; i < g.involved_companies.length; i++) {
            var ic = g.involved_companies[i];
            if (ic.developer && ic.company && ic.company.name) { dev = ic.company.name; break; }
        }
    if (dev) facts.push(metaFact("Developer", dev));
    var cover = (g.cover && g.cover.image_id)
        ? "https://images.igdb.com/igdb/image/upload/t_cover_big/" + g.cover.image_id + ".jpg" : "";
    return metaResult({ title: g.name, subtitle: "", overview: g.summary || g.storyline || "",
        image: cover, facts: facts });
}

// ---------------------------------------------------------------------------- MusicBrainz (music)

var MB = "https://musicbrainz.org/ws/2";

function mbAlbums(query, page) {
    page = page1(page);
    var offset = (page - 1) * PAGE;
    var q = query ? query : "tag:rock AND primarytype:album";
    var r = J(httpGet(MB + "/release-group/?query=" + enc(q) + "&fmt=json&limit=" + PAGE + "&offset=" + offset));
    if (!r || !r["release-groups"]) return info("Music", "Could not reach MusicBrainz.");
    var rgs = r["release-groups"], items = [];
    for (var i = 0; i < rgs.length; i++) {
        var rg = rgs[i];
        var artist = (rg["artist-credit"] && rg["artist-credit"][0]) ? rg["artist-credit"][0].name : "";
        items.push({
            id: "mb:rg:" + rg.id,
            title: rg.title,
            subtitle: artist + (rg["first-release-date"] ? " · " + year(rg["first-release-date"]) : ""),
            type: "album",
            thumbnailUrl: "https://coverartarchive.org/release-group/" + rg.id + "/front-250",
            expandable: true, url: ""
        });
    }
    return result("Music", items, (offset + PAGE) < (r.count || 0));
}

function mbTracks(rgId) {
    // Find a release in the group, then read its tracklist.
    var rel = J(httpGet(MB + "/release?release-group=" + rgId + "&fmt=json&limit=1"));
    if (!rel || !rel.releases || !rel.releases.length) return info("Tracks", "No tracklist found.");
    var relId = rel.releases[0].id;
    var r = J(httpGet(MB + "/release/" + relId + "?fmt=json&inc=recordings"));
    if (!r || !r.media) return info("Tracks", "Could not load tracks.");
    var items = [];
    for (var m = 0; m < r.media.length; m++) {
        var tracks = r.media[m].tracks || [];
        for (var t = 0; t < tracks.length; t++) {
            var tr = tracks[t];
            var mins = tr.length ? msToClock(tr.length) : "";
            // Key by the recording id (the real entity getMeta looks up), falling back to the track id.
            var recId = (tr.recording && tr.recording.id) ? tr.recording.id : tr.id;
            items.push({ id: "mb:track:" + recId, title: tr.position + ". " + tr.title,
                         subtitle: mins, type: "track", expandable: false, url: "" });
        }
    }
    return JSON.stringify({ title: r.title || "Tracks", items: items });
}

function mbAlbumMeta(rgId) {
    var r = J(httpGet(MB + "/release-group/" + rgId + "?fmt=json&inc=artists+tags"));
    if (!r) return "{}";
    var artist = (r["artist-credit"] && r["artist-credit"][0]) ? r["artist-credit"][0].name : "";
    var facts = [];
    if (artist)                  facts.push(metaFact("Artist", artist));
    if (r["first-release-date"]) facts.push(metaFact("Released", r["first-release-date"]));
    if (r["primary-type"])       facts.push(metaFact("Type", r["primary-type"]));
    if (r.tags && r.tags.length) {
        r.tags.sort(function (a, b) { return (b.count || 0) - (a.count || 0); });
        facts.push(metaFact("Tags", namesOf(r.tags, "name", 5)));
    }
    return metaResult({ title: r.title, subtitle: artist, overview: r.disambiguation || "",
        image: "https://coverartarchive.org/release-group/" + rgId + "/front-250", facts: facts });
}

function mbTrackMeta(recId) {
    var r = J(httpGet(MB + "/recording/" + recId + "?fmt=json&inc=artists+releases"));
    if (!r) return "{}";
    var artist = (r["artist-credit"] && r["artist-credit"][0]) ? r["artist-credit"][0].name : "";
    var facts = [];
    if (artist)                  facts.push(metaFact("Artist", artist));
    if (r.length)                facts.push(metaFact("Length", msToClock(r.length)));
    if (r["first-release-date"]) facts.push(metaFact("First released", r["first-release-date"]));
    var rel = (r.releases && r.releases.length) ? r.releases[0] : null;
    if (rel && rel.title) facts.push(metaFact("Appears on", rel.title));
    return metaResult({ title: r.title, subtitle: artist, overview: r.disambiguation || "",
        image: rel ? "https://coverartarchive.org/release/" + rel.id + "/front-250" : "", facts: facts });
}

// ---------------------------------------------------------------------------- Google Books (books)
// Fast + reliable; works without a key (rate-limited), better with the optional googleBooksApiKey.

var GBOOKS = "https://www.googleapis.com/books/v1/volumes";

// `type` lets the same data back two catalogs: "book" (Books) and "audiobook" (Audiobooks).
function gbooks(query, page, type) {
    type = type || "book";
    var label = (type === "audiobook") ? "Audiobooks" : "Books";
    page = page1(page);
    var start = (page - 1) * PAGE;
    var q = query ? query : "subject:fiction";
    var key = getConfig("googleBooksApiKey");
    // NB: no "projection=lite" - it intermittently triggers Google's 503 "backendFailed" for certain
    // startIndex offsets (and retrying the same offset doesn't help). Full projection is reliable and
    // still carries volumeInfo.imageLinks. "country" is required by the API in some regions.
    var url = GBOOKS + "?q=" + enc(q) + "&maxResults=" + PAGE + "&startIndex=" + start
            + "&printType=books&country=US" + (key ? "&key=" + enc(key) : "");
    var r = J(httpGet(url));
    if (!r) return info(label, "Could not reach Google Books.");
    // On a transient backend error, fall back to the keyless Open Library catalog so the list still loads.
    if (r.error) {
        if ((r.error.code || 0) >= 500) return olBooks(query, page, type);
        return info(label, "Google Books: " + (r.error.message || "request failed."));
    }
    var docs = r.items || [];
    var items = [];
    for (var i = 0; i < docs.length; i++) {
        var vi = docs[i].volumeInfo || {};
        var img = vi.imageLinks ? (vi.imageLinks.thumbnail || vi.imageLinks.smallThumbnail || "") : "";
        if (img) img = img.replace(/^http:/, "https:"); // serve cover over https
        items.push({
            id: "googlebooks:" + docs[i].id,
            title: vi.title || "(untitled)",
            subtitle: ((vi.authors && vi.authors[0]) ? vi.authors[0] : "") + (vi.publishedDate ? " · " + year(vi.publishedDate) : ""),
            type: type,
            thumbnailUrl: img,
            expandable: false, url: ""
        });
    }
    return result(label, items, (start + items.length) < (r.totalItems || 0));
}

// Keyless fallback (Open Library) - used when no Google Books key is configured.
function olBooks(query, page, type) {
    type = type || "book";
    var label = (type === "audiobook") ? "Audiobooks" : "Books";
    page = page1(page);
    var offset = (page - 1) * PAGE, items = [], hasMore = false;
    var cover = "https://covers.openlibrary.org/b/id/";
    function push(title, author, yr, coverId, key) {
        items.push({ id: "openlibrary:" + (key || ""), title: title,
            subtitle: (author || "") + (yr ? " · " + yr : ""), type: type,
            thumbnailUrl: coverId ? cover + coverId + "-M.jpg" : "", expandable: false, url: "" });
    }
    if (query) {
        var r = J(httpGet("https://openlibrary.org/search.json?limit=" + PAGE + "&page=" + page + "&q=" + enc(query)));
        if (!r || !r.docs) return info(label, "Could not reach the book catalog.");
        for (var i = 0; i < r.docs.length; i++) {
            var d = r.docs[i];
            push(d.title, d.author_name && d.author_name[0], d.first_publish_year, d.cover_i, d.key);
        }
        hasMore = (offset + items.length) < (r.numFound || r.num_found || 0);
    } else {
        var r2 = J(httpGet("https://openlibrary.org/subjects/fiction.json?limit=" + PAGE + "&offset=" + offset));
        if (!r2 || !r2.works) return info(label, "Could not reach the book catalog.");
        for (var j = 0; j < r2.works.length; j++) {
            var w = r2.works[j];
            push(w.title, w.authors && w.authors[0] && w.authors[0].name, w.first_publish_year, w.cover_id, w.key);
        }
        hasMore = (offset + PAGE) < (r2.work_count || 0);
    }
    return result(label, items, hasMore);
}

// Prefer Google Books when a key is set (fast + reliable); else fall back to keyless Open Library.
// `type` switches the item type so the Audiobooks catalog reuses the very same book data.
function booksCatalog(query, page, type) {
    return getConfig("googleBooksApiKey") ? gbooks(query, page, type) : olBooks(query, page, type);
}

function gbookMeta(volId) {
    var key = getConfig("googleBooksApiKey");
    var r = J(httpGet(GBOOKS + "/" + enc(volId) + (key ? "?key=" + enc(key) : "")));
    if (!r || r.error) return "{}";
    var vi = r.volumeInfo || {}, facts = [];
    if (vi.authors)       facts.push(metaFact("Author", vi.authors.join(", ")));
    if (vi.publishedDate) facts.push(metaFact("Published", vi.publishedDate));
    if (vi.publisher)     facts.push(metaFact("Publisher", vi.publisher));
    if (vi.pageCount)     facts.push(metaFact("Pages", vi.pageCount));
    if (vi.categories)    facts.push(metaFact("Categories", vi.categories.join(", ")));
    if (vi.averageRating) facts.push(metaFact("Rating", vi.averageRating + " / 5"));
    var img = vi.imageLinks ? (vi.imageLinks.thumbnail || vi.imageLinks.smallThumbnail || "") : "";
    if (img) img = img.replace(/^http:/, "https:");
    return metaResult({ title: vi.title || "(untitled)", subtitle: (vi.authors && vi.authors[0]) || "",
        overview: stripHtml(vi.description), image: img, facts: facts });
}

function olBookMeta(key) {
    if (key.charAt(0) !== "/") key = "/" + key;
    var r = J(httpGet("https://openlibrary.org" + key + ".json"));
    if (!r) return "{}";
    var desc = "";
    if (typeof r.description === "string") desc = r.description;
    else if (r.description && r.description.value) desc = r.description.value;
    var facts = [];
    if (r.first_publish_date)            facts.push(metaFact("Published", r.first_publish_date));
    if (r.subjects && r.subjects.length) facts.push(metaFact("Subjects", r.subjects.slice(0, 5).join(", ")));
    var img = (r.covers && r.covers.length && r.covers[0] > 0)
        ? "https://covers.openlibrary.org/b/id/" + r.covers[0] + "-L.jpg" : "";
    return metaResult({ title: r.title || "(untitled)", subtitle: "", overview: stripHtml(desc),
        image: img, facts: facts });
}

// ---------------------------------------------------------------------------- Comic Vine (comics)
// Western comics. A "volume" is a series; drilling into one lists its issues. Needs a free API key.

var CV = "https://comicvine.gamespot.com/api";
function cvKey() { return getConfig("comicVineApiKey"); }
function cvImage(img) { return img ? (img.medium_url || img.small_url || img.original_url || "") : ""; }
function cvOk(r) { return r && (!r.error || r.error === "OK"); }

function comicsCatalog(query, page) {
    page = page1(page);
    var key = cvKey();
    if (!key) return info("Comics", "Set your Comic Vine API key in Configure… to load comics.");
    var url, viaSearch = !!query;
    var fields = "id,name,image,start_year,publisher,count_of_issues";
    if (viaSearch) {
        url = CV + "/search/?api_key=" + enc(key) + "&format=json&resources=volume&limit=" + PAGE +
              "&page=" + page + "&query=" + enc(query) + "&field_list=" + fields;
    } else {
        var offset = (page - 1) * PAGE;
        url = CV + "/volumes/?api_key=" + enc(key) + "&format=json&sort=date_last_updated:desc&limit=" + PAGE +
              "&offset=" + offset + "&field_list=" + fields;
    }
    var r = J(httpGet(url));
    if (!r) return info("Comics", "Could not reach Comic Vine.");
    if (!cvOk(r)) return info("Comics", "Comic Vine: " + (r.error || "request failed."));
    var arr = r.results || [], items = [];
    for (var i = 0; i < arr.length; i++) {
        var v = arr[i];
        var pub = (v.publisher && v.publisher.name) ? v.publisher.name : "";
        items.push({
            id: "comicvine:volume:" + v.id, title: v.name,
            subtitle: pub + (v.start_year ? (pub ? " · " : "") + v.start_year : ""),
            type: "comic", thumbnailUrl: cvImage(v.image), expandable: true, url: ""
        });
    }
    var total = r.number_of_total_results || 0;
    return result("Comics", items, viaSearch ? (arr.length === PAGE) : ((page * PAGE) < total));
}

function cvIssues(volumeId, page) {
    page = page1(page);
    var key = cvKey(); if (!key) return info("Issues", "Set your Comic Vine API key.");
    var offset = (page - 1) * PAGE;
    var r = J(httpGet(CV + "/issues/?api_key=" + enc(key) + "&format=json&filter=volume:" + volumeId +
        "&sort=issue_number:asc&limit=" + PAGE + "&offset=" + offset +
        "&field_list=id,name,issue_number,image,cover_date"));
    if (!r) return info("Issues", "Could not load issues.");
    if (!cvOk(r)) return info("Issues", "Comic Vine: " + (r.error || "request failed."));
    var arr = r.results || [], items = [];
    for (var i = 0; i < arr.length; i++) {
        var is = arr[i];
        items.push({
            id: "comicvine:issue:" + is.id,
            title: "#" + (is.issue_number || "?") + (is.name ? " — " + is.name : ""),
            subtitle: is.cover_date || "", type: "comic_issue",
            thumbnailUrl: cvImage(is.image), expandable: false, url: ""
        });
    }
    var total = r.number_of_total_results || 0;
    return result("Issues", items, (page * PAGE) < total);
}

function cvVolumeMeta(id) {
    var key = cvKey(); if (!key) return "{}";
    var r = J(httpGet(CV + "/volume/4050-" + id + "/?api_key=" + enc(key) + "&format=json" +
        "&field_list=name,deck,description,start_year,publisher,count_of_issues,image"));
    if (!cvOk(r) || !r.results) return "{}";
    var v = r.results, facts = [];
    if (v.publisher && v.publisher.name) facts.push(metaFact("Publisher", v.publisher.name));
    if (v.start_year)      facts.push(metaFact("Started", v.start_year));
    if (v.count_of_issues) facts.push(metaFact("Issues", v.count_of_issues));
    return metaResult({ title: v.name, subtitle: "", overview: v.deck || stripHtml(v.description),
        image: cvImage(v.image), facts: facts });
}

function cvIssueMeta(id) {
    var key = cvKey(); if (!key) return "{}";
    var r = J(httpGet(CV + "/issue/4000-" + id + "/?api_key=" + enc(key) + "&format=json" +
        "&field_list=name,issue_number,cover_date,store_date,deck,description,image,volume"));
    if (!cvOk(r) || !r.results) return "{}";
    var is = r.results, facts = [];
    if (is.issue_number) facts.push(metaFact("Issue", "#" + is.issue_number));
    if (is.volume && is.volume.name) facts.push(metaFact("Series", is.volume.name));
    if (is.cover_date) facts.push(metaFact("Cover date", is.cover_date));
    if (is.store_date) facts.push(metaFact("Store date", is.store_date));
    var title = is.name || ("#" + (is.issue_number || "") +
        (is.volume && is.volume.name ? " · " + is.volume.name : ""));
    return metaResult({ title: title, subtitle: (is.volume && is.volume.name) || "",
        overview: is.deck || stripHtml(is.description), image: cvImage(is.image), facts: facts });
}

// ---------------------------------------------------------------------------- MangaDex (manga)
// Keyless. A manga is a container; drilling into one lists its (English) chapters - the only free
// source that exposes chapter-level data. getMeta fills the detail header for a manga or a chapter.

var MDX = "https://api.mangadex.org";
var MDX_UP = "https://uploads.mangadex.org";

function firstVal(o) { if (!o) return ""; for (var k in o) if (o[k]) return o[k]; return ""; }

// Content-rating filter: safe + suggestive by default, plus erotica + pornographic when the toggle is on.
function mdxRatings() {
    var s = "&contentRating[]=safe&contentRating[]=suggestive";
    if (getConfig("mangaShowAdult") === "true") s += "&contentRating[]=erotica&contentRating[]=pornographic";
    return s;
}

function mdxTitle(attr) {
    if (!attr) return "(untitled)";
    var t = attr.title || {};
    if (t.en) return t.en;
    var v = firstVal(t); if (v) return v;
    var alts = attr.altTitles || [];
    for (var i = 0; i < alts.length; i++) { var av = firstVal(alts[i]); if (av) return av; }
    return "(untitled)";
}

function mdxCover(mangaId, rels) {
    rels = rels || [];
    for (var i = 0; i < rels.length; i++)
        if (rels[i].type === "cover_art" && rels[i].attributes && rels[i].attributes.fileName)
            return MDX_UP + "/covers/" + mangaId + "/" + rels[i].attributes.fileName + ".256.jpg";
    return "";
}

// MangaDex genre tag UUIDs (stable). Used as includedTags[] to filter the catalog.
var MDX_GENRES = [["391b0423-d847-456f-aff0-8b0cfc03066b","Action"],["87cc87cd-a395-47af-b27a-93258283bbc6","Adventure"],
    ["4d32cc48-9f00-4cca-9b5a-a839f0764984","Comedy"],["b9af3a63-f058-46de-a9a0-e0c13906197a","Drama"],
    ["cdc58593-87dd-415e-bbc0-2ec27bf404cc","Fantasy"],["cdad7e68-1419-41dd-bdce-27753074a640","Horror"],
    ["ee968100-4191-4968-93d3-f82d72be7e46","Mystery"],["3b60b75c-a2d7-4860-ab56-05f391bb889c","Psychological"],
    ["423e2eae-a7a2-4a8b-ac03-a8351462d71d","Romance"],["256c8bd9-4904-4360-bf4f-508a76d67183","Sci-Fi"],
    ["e5301a23-ebd9-49dd-a0cb-2add944c7fe9","Slice of Life"],["69964a64-2f90-4d33-beeb-f3ed2875eb4c","Sports"],
    ["eabc5b4c-6aff-42f3-b657-3e90cbd00b75","Supernatural"]];
function mangaFilters() {
    return [
        { key: "genre", label: "Genre", options: optList(MDX_GENRES, "Any genre") },
        { key: "sort", label: "Sort", options: [
            { value: "", label: "Popular" }, { value: "top", label: "Top rated" },
            { value: "updated", label: "Recently updated" }, { value: "new", label: "Newest" }] }
    ];
}

function mangaCatalog(query, page, f) {
    page = page1(page); f = f || {};
    var offset = (page - 1) * PAGE;
    var url = MDX + "/manga?limit=" + PAGE + "&offset=" + offset + "&includes[]=cover_art" + mdxRatings();
    if (query)   url += "&title=" + enc(query);
    if (f.genre) url += "&includedTags[]=" + enc(f.genre);
    var order = f.sort === "top" ? "rating" : f.sort === "new" ? "year"
              : f.sort === "updated" ? "latestUploadedChapter" : "followedCount";
    url += "&order[" + order + "]=desc&hasAvailableChapters=true";
    var r = J(httpGet(url));
    if (!r || !r.data) return info("Manga", "Could not reach MangaDex.");
    var items = [];
    for (var i = 0; i < r.data.length; i++) {
        var d = r.data[i], a = d.attributes || {};
        items.push({
            id: "mangadex:" + d.id, title: mdxTitle(a),
            subtitle: (a.year ? String(a.year) : "") + (a.status ? (a.year ? " · " : "") + a.status : ""),
            type: "manga", thumbnailUrl: mdxCover(d.id, d.relationships),
            expandable: true, url: ""
        });
    }
    return resultF("Manga", items, (offset + PAGE) < (r.total || 0), mangaFilters());
}

function numOr(v, dflt) { var n = parseFloat(v); return isNaN(n) ? dflt : n; }

function mangaChapters(mangaId, page) {
    page = page1(page);
    // The aggregate endpoint collapses every language/scanlation of a chapter into ONE entry (with a
    // representative chapter id + a version count), grouped by volume - so we show one item per chapter.
    var r = J(httpGet(MDX + "/manga/" + mangaId + "/aggregate"));
    if (!r || !r.volumes) return info("Chapters", "Could not load chapters.");

    var all = [], vols = r.volumes;
    for (var vk in vols) {
        var vol = vols[vk] || {}, chs = vol.chapters || {};
        for (var ck in chs) {
            var c = chs[ck];
            // Keep every version id for this chapter (the representative + "others") so getMeta can pick
            // the English one when the chapter is opened.
            var ids = [c.id];
            if (c.others && c.others.length) ids = ids.concat(c.others);
            all.push({ vol: vol.volume, chapter: c.chapter, ids: ids, count: c.count || 1 });
        }
    }
    if (!all.length) return info("Chapters", "No chapters available for this title.");

    // Order by volume then chapter (numeric where possible; unnumbered/oneshots sort last).
    all.sort(function (a, b) {
        var av = numOr(a.vol, 1e9), bv = numOr(b.vol, 1e9);
        if (av !== bv) return av - bv;
        return numOr(a.chapter, 1e9) - numOr(b.chapter, 1e9);
    });

    // Page client-side so infinite scroll still works on long series.
    var start = (page - 1) * PAGE, slice = all.slice(start, start + PAGE), items = [];
    for (var i = 0; i < slice.length; i++) {
        var s = slice[i];
        var vlabel = (s.vol && s.vol !== "none") ? ("Vol. " + s.vol + " · ") : "";
        items.push({
            id: "mangadexch:" + s.ids.join(","), title: vlabel + (s.chapter ? ("Ch. " + s.chapter) : "Oneshot"),
            subtitle: s.count > 1 ? (s.count + " versions") : "1 version",
            type: "manga_chapter", thumbnailUrl: "", expandable: false, url: ""
        });
    }
    return result("Chapters", items, (start + PAGE) < all.length);
}

function mangaMeta(mangaId) {
    var r = J(httpGet(MDX + "/manga/" + mangaId + "?includes[]=cover_art"));
    if (!r || !r.data) return "{}";
    var d = r.data, a = d.attributes || {}, facts = [];
    if (a.status)                 facts.push(metaFact("Status", a.status));
    if (a.year)                   facts.push(metaFact("Year", a.year));
    if (a.publicationDemographic) facts.push(metaFact("Demographic", a.publicationDemographic));
    if (a.contentRating)          facts.push(metaFact("Rating", a.contentRating));
    if (a.lastChapter)            facts.push(metaFact("Last chapter", a.lastChapter));
    var genres = [], tags = a.tags || [];
    for (var t = 0; t < tags.length; t++) {
        var ta = tags[t].attributes;
        if (ta && ta.group === "genre" && ta.name && ta.name.en) genres.push(ta.name.en);
    }
    if (genres.length) facts.push(metaFact("Genres", genres.slice(0, 6).join(", ")));
    return metaResult({ title: mdxTitle(a), subtitle: "",
        overview: stripHtml(a.description ? (a.description.en || firstVal(a.description)) : ""),
        image: mdxCover(d.id, d.relationships), facts: facts });
}

function chapterMetaFromObj(obj) {
    var a = obj.attributes || {}, rels = obj.relationships || [], facts = [];
    if (a.volume)             facts.push(metaFact("Volume", a.volume));
    if (a.chapter)            facts.push(metaFact("Chapter", a.chapter));
    if (a.pages)              facts.push(metaFact("Pages", a.pages));
    if (a.translatedLanguage) facts.push(metaFact("Language", a.translatedLanguage));
    if (a.publishAt)          facts.push(metaFact("Published", a.publishAt.substring(0, 10)));
    var group = "", series = "";
    for (var i = 0; i < rels.length; i++) {
        if (rels[i].type === "scanlation_group" && rels[i].attributes) group = rels[i].attributes.name;
        if (rels[i].type === "manga" && rels[i].attributes) series = mdxTitle(rels[i].attributes);
    }
    if (group)  facts.push(metaFact("Group", group));
    if (series) facts.push(metaFact("Series", series));
    var title = (a.chapter ? "Chapter " + a.chapter : "Oneshot") + (a.title ? " — " + a.title : "");
    return metaResult({ title: title, subtitle: series, overview: "", image: "", facts: facts });
}

// idsCsv = all version ids for one chapter number. Default to the English version when one exists.
function mangaChapterMeta(idsCsv) {
    var ids = idsCsv.split(",");
    if (ids.length > 1) {
        var q = MDX + "/chapter?translatedLanguage[]=en&limit=1&includes[]=scanlation_group&includes[]=manga";
        for (var i = 0; i < ids.length; i++) q += "&ids[]=" + ids[i];
        var pick = J(httpGet(q));
        if (pick && pick.data && pick.data.length) return chapterMetaFromObj(pick.data[0]); // English found
    }
    // No English among them (or only one version): fall back to the representative chapter.
    var r = J(httpGet(MDX + "/chapter/" + ids[0] + "?includes[]=scanlation_group&includes[]=manga"));
    if (!r || !r.data) return "{}";
    return chapterMetaFromObj(r.data);
}

// ---------------------------------------------------------------------------- entry points

function getCatalog(argJson) {
    var a = J(argJson) || {};
    var cat = a.catalog || "movies";
    var q = a.query || "";
    var p = a.page || 1;
    var f = { genre: a.genre || "", year: a.year || "", rating: a.rating || "", sort: a.sort || "" };
    if (cat === "movies") return tmdbList("movie", q, p, f);
    if (cat === "tv")     return tmdbList("tv", q, p, f);
    if (cat === "games")  return gamesCatalog(q, p, f);
    if (cat === "music")  return mbAlbums(q, p);
    if (cat === "books")  return booksCatalog(q, p);
    if (cat === "audiobooks") return booksCatalog(q, p, "audiobook"); // same data, audio-typed
    if (cat === "comics") return comicsCatalog(q, p);
    if (cat === "manga")  return mangaCatalog(q, p, f);
    return info("AIO Catalog", "Pick a media type.");
}

function getDetail(argJson) {
    var a = J(argJson) || {};
    var p = a.page || 1;
    var f = { genre: a.genre || "", sort: a.sort || "" };
    var parts = (a.id || "").split(":");
    if (a.type === "series")   return tmdbSeasons(parts[2]);              // tmdb:tv:{id} (all seasons)
    if (a.type === "season")   return tmdbEpisodes(parts[2], parts[3]);   // tmdb:season:{show}:{n} (all episodes)
    if (a.type === "album")    return mbTracks(parts[2]);                 // mb:rg:{id} (all tracks)
    if (a.type === "platform") return igdbPlatformGames(parts[1], p, f);  // igdbplatform:{id} (paged, filterable)
    if (a.type === "comic")    return cvIssues(parts[2], p);              // comicvine:volume:{id} (issues, paged)
    if (a.type === "manga")    return mangaChapters(parts[1], p);         // mangadex:{id} (chapters, paged)
    return JSON.stringify({ title: "", items: [] });
}

// Map an IMDB id to TMDB metadata, so another addon's IMDB-keyed item (e.g. Allarr "mv:tt123") can be
// enriched with our TMDB synopsis/facts. id form: "imdb:movie:tt123" / "imdb:series:tt123" /
// "imdb:episode:tt123:S:E".
function imdbMeta(parts) {
    var key = getConfig("tmdbApiKey"); if (!key) return "{}";
    var sub = parts[1], imdb = parts[2];
    var f = J(httpGet(TMDB + "/find/" + imdb + "?api_key=" + key + "&external_source=imdb_id"));
    if (!f) return "{}";
    if (sub === "movie") {
        var mr = f.movie_results || []; if (!mr.length) return "{}";
        return tmdbMovieMeta(mr[0].id);
    }
    var tr = f.tv_results || []; if (!tr.length) return "{}";
    if (sub === "episode") return tmdbEpisodeMeta(tr[0].id, parts[3], parts[4]);
    return tmdbSeriesMeta(tr[0].id);
}

// Per-item metadata for the detail header. Returns "{}" when nothing useful is available.
function getMeta(argJson) {
    var a = J(argJson) || {};
    var id = a.id || "", parts = id.split(":");
    if (parts[0] === "imdb")  return imdbMeta(parts);                          // imdb:{movie|series|episode}:tt…
    if (a.type === "movie")   return tmdbMovieMeta(parts[2]);                  // tmdb:movie:{id}
    if (a.type === "series")  return tmdbSeriesMeta(parts[2]);                 // tmdb:tv:{id}
    if (a.type === "season")  return tmdbSeasonMeta(parts[2], parts[3]);       // tmdb:season:{show}:{n}
    if (a.type === "episode") return tmdbEpisodeMeta(parts[2], parts[3], parts[4]); // tmdb:episode:{show}:{s}:{e}
    if (a.type === "game")    return igdbGameMeta(parts[1]);                   // igdb:{id}
    if (a.type === "album")   return mbAlbumMeta(parts[2]);                    // mb:rg:{id}
    if (a.type === "track")   return mbTrackMeta(parts[2]);                    // mb:track:{recordingId}
    if (a.type === "comic")         return cvVolumeMeta(parts[2]);             // comicvine:volume:{id}
    if (a.type === "comic_issue")   return cvIssueMeta(parts[2]);              // comicvine:issue:{id}
    if (a.type === "manga")         return mangaMeta(parts[1]);                // mangadex:{id}
    if (a.type === "manga_chapter") return mangaChapterMeta(parts[1]);         // mangadexch:{id}
    if (a.type === "book" || a.type === "audiobook") { // audiobooks reuse the book metadata sources
        if (parts[0] === "googlebooks") return gbookMeta(id.substring("googlebooks:".length));
        if (parts[0] === "openlibrary") return olBookMeta(id.substring("openlibrary:".length));
    }
    return "{}";
}

function search(argJson) { return getCatalog(argJson); }
