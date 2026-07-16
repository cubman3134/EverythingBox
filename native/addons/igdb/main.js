// IGDB game-metadata provider for My Media Vault.
//
// Pure metadata/artwork provider: only implements getMeta() for type "game". No browse
// catalog (manifest "catalogs": []). The app fans out to every game-meta provider on hover
// and merges results.
//
// Auth: Twitch developer credentials configured in Settings — "clientId" (Twitch Client ID)
// and "clientSecret" (Twitch Client Secret). We OAuth against Twitch for an app access token,
// cache it in storage across calls, and send it plus the Client-ID header to the IGDB API.
// Credentials/tokens are never logged or returned.

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }
function metaFact(l, v) { return { label: l, value: (v == null) ? "" : String(v) }; }

var IGDB_IMG = "https://images.igdb.com/igdb/image/upload/";

// Escape a title for safe embedding inside an apicalypse `search "..."` clause.
function apiStr(s) { return String(s || "").replace(/\\/g, "").replace(/"/g, ""); }

function creds() {
    var id = getConfig("clientId"), secret = getConfig("clientSecret");
    if (!id || !secret) return null;
    return { id: id, secret: secret };
}

// Obtain an app access token. Reuses the cached token unless force is true.
function igdbToken(c, force) {
    if (!force) {
        var cached = getStorage("igdb_token");
        if (cached) return cached;
    }
    var resp = J(httpRequest({
        method: "POST",
        url: "https://id.twitch.tv/oauth2/token?client_id=" + enc(c.id) +
             "&client_secret=" + enc(c.secret) + "&grant_type=client_credentials"
    }));
    if (!resp || !resp.access_token) return "";
    setStorage("igdb_token", resp.access_token);
    return resp.access_token;
}

// Run an apicalypse query body against /v4/games. Returns the parsed array (or null).
function igdbQuery(c, token, body) {
    if (!token) return null;
    return J(httpRequest({
        method: "POST",
        url: "https://api.igdb.com/v4/games",
        body: body,
        headers: {
            "Client-ID": c.id,
            "Authorization": "Bearer " + token,
            "Accept": "application/json"
        }
    }));
}

function imgUrl(size, imageId) { return IGDB_IMG + "t_" + size + "/" + imageId + ".jpg"; }

function getMeta(argJson) {
    var a = J(argJson) || {};
    if (a.type !== "game") return "{}";

    var c = creds();
    if (!c) return "{}";

    var title = a.title || "";
    if (!title) return "{}";

    var body = 'search "' + apiStr(title) + '"; fields name,summary,cover.image_id,' +
        'artworks.image_id,screenshots.image_id,videos.video_id,rating,first_release_date,' +
        'genres.name,involved_companies.company.name,involved_companies.developer,' +
        'involved_companies.publisher; limit 1;';

    var token = igdbToken(c, false);
    var arr = igdbQuery(c, token, body);
    // Empty/unauthorized (e.g. an expired cached token) -> re-auth once and retry.
    if (!arr || !arr.length) {
        token = igdbToken(c, true);
        arr = igdbQuery(c, token, body);
    }
    if (!arr || !arr.length) return "{}";

    var g = arr[0];
    if (!g) return "{}";

    var images = {};
    var image = "";

    if (g.cover && g.cover.image_id) {
        var cov = imgUrl("cover_big", g.cover.image_id);
        images.box = [cov];
        images.poster = [cov];
        image = cov;
    }
    if (g.artworks && g.artworks.length) {
        var art = [];
        for (var i = 0; i < g.artworks.length; i++) {
            if (g.artworks[i] && g.artworks[i].image_id) art.push(imgUrl("1080p", g.artworks[i].image_id));
        }
        if (art.length) { images.fanart = art; images.background = art; }
    }
    if (g.screenshots && g.screenshots.length) {
        var ss = [];
        for (var s = 0; s < g.screenshots.length; s++) {
            if (g.screenshots[s] && g.screenshots[s].image_id) ss.push(imgUrl("screenshot_big", g.screenshots[s].image_id));
        }
        if (ss.length) images.screenshot = ss;
    }

    var videos = [];
    if (g.videos && g.videos.length) {
        for (var v = 0; v < g.videos.length; v++) {
            if (g.videos[v] && g.videos[v].video_id) videos.push("https://www.youtube.com/watch?v=" + g.videos[v].video_id);
        }
    }

    // Developer / publisher from involved_companies flags.
    var developer = "", publisher = "";
    if (g.involved_companies && g.involved_companies.length) {
        for (var k = 0; k < g.involved_companies.length; k++) {
            var ic = g.involved_companies[k];
            if (!ic || !ic.company || !ic.company.name) continue;
            if (ic.developer && !developer) developer = ic.company.name;
            if (ic.publisher && !publisher) publisher = ic.company.name;
        }
    }

    var genres = "";
    if (g.genres && g.genres.length) {
        var gn = [];
        for (var gi = 0; gi < g.genres.length; gi++) {
            if (g.genres[gi] && g.genres[gi].name) gn.push(g.genres[gi].name);
        }
        genres = gn.join(", ");
    }

    var facts = [];
    if (g.rating != null) facts.push(metaFact("Rating", Math.round(g.rating)));
    if (genres)    facts.push(metaFact("Genres", genres));
    if (developer) facts.push(metaFact("Developer", developer));
    if (publisher) facts.push(metaFact("Publisher", publisher));

    var out = { title: g.name || title };
    if (g.summary) out.overview = g.summary;
    if (image) out.image = image;
    if (facts.length) out.facts = facts;
    // images object present only if it has any role.
    var hasImg = false, r;
    for (r in images) { if (images.hasOwnProperty(r)) { hasImg = true; break; } }
    if (hasImg) out.images = images;
    if (videos.length) out.videos = videos;

    var meta = {};
    if (developer) meta.developer = developer;
    if (publisher) meta.publisher = publisher;
    var hasMeta = false, m;
    for (m in meta) { if (meta.hasOwnProperty(m)) { hasMeta = true; break; } }
    if (hasMeta) out.meta = meta;

    return JSON.stringify(out);
}
