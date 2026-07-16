// SteamGridDB game-artwork provider for My Media Vault.
//
// Pure metadata/artwork provider: only implements getMeta() for type "game". It does NOT
// contribute a browse catalog (see manifest "catalogs": []). The app fans out to every
// game-meta provider on hover and merges the results, so this addon just returns whatever
// artwork it can find for a given title.
//
// Auth: an API key (free) configured in Settings under "apikey". Sent as an
// "Authorization: Bearer <key>" header on every request. The key is never logged or returned.
// SteamGridDB provides no overview/facts/videos — only images (grids/heroes/logos/icons).

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }

var SGDB = "https://www.steamgriddb.com/api/v2";

// Authenticated GET -> parsed JSON (or null). httpRequest returns the body string ("" on denial).
function sgdbGet(url, key) {
    var body = httpRequest({
        url: url,
        method: "GET",
        headers: { "Authorization": "Bearer " + key, "Accept": "application/json" }
    });
    return J(body);
}

// Collect data[].url into an array, guarding every hop.
function collectUrls(resp) {
    var out = [];
    if (!resp || resp.success === false || !resp.data || !resp.data.length) return out;
    for (var i = 0; i < resp.data.length; i++) {
        var d = resp.data[i];
        if (d && d.url) out.push(d.url);
    }
    return out;
}

function getMeta(argJson) {
    var a = J(argJson) || {};
    if (a.type !== "game") return "{}";

    var key = getConfig("apikey");
    if (!key) return "{}";

    var title = a.title || "";
    if (!title) return "{}";

    // Step 1: resolve the game id via autocomplete search.
    var search = sgdbGet(SGDB + "/search/autocomplete/" + enc(title), key);
    if (!search || search.success === false || !search.data || !search.data.length) return "{}";
    var gameId = search.data[0] && search.data[0].id;
    if (!gameId) return "{}";

    // Step 2: pull each artwork class.
    var grids  = collectUrls(sgdbGet(SGDB + "/grids/game/" + gameId + "?dimensions=600x900", key));
    var heroes = collectUrls(sgdbGet(SGDB + "/heroes/game/" + gameId, key));
    var logos  = collectUrls(sgdbGet(SGDB + "/logos/game/" + gameId, key));
    var icons  = collectUrls(sgdbGet(SGDB + "/icons/game/" + gameId, key));

    var images = {};
    if (grids.length)  { images.box = grids; images.poster = grids; }
    if (heroes.length) { images.hero = heroes; images.background = heroes; }
    if (logos.length)  { images.logo = logos; }
    if (icons.length)  { images.icon = icons; }

    var out = { title: title, images: images };
    if (grids.length) out.image = grids[0];

    // Nothing found at all -> behave as "no data".
    if (!grids.length && !heroes.length && !logos.length && !icons.length) return "{}";

    return JSON.stringify(out);
}
