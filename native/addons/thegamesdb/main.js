// TheGamesDB game-metadata provider for My Media Vault.
//
// Pure metadata/artwork provider: only implements getMeta() for type "game". No browse
// catalog (manifest "catalogs": []). The app fans out to every game-meta provider on hover
// and merges results.
//
// Auth: a free public API key configured in Settings under "apikey", passed as the ?apikey=
// GET query param. The key is never logged or returned.

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }
function metaFact(l, v) { return { label: l, value: (v == null) ? "" : String(v) }; }

var TGDB = "https://api.thegamesdb.net/v1";

// TheGamesDB image "type" -> our canonical role.
var ROLE_BY_TYPE = {
    "boxart": "box",
    "fanart": "fanart",
    "banner": "banner",
    "clearlogo": "logo",
    "screenshot": "screenshot"
};

// Push a url into images[role], creating the array as needed.
function addImage(images, role, url) {
    if (!role || !url) return;
    if (!images[role]) images[role] = [];
    images[role].push(url);
}

// A value we can safely render as a fact (string or number, not an array of ids or an object).
function scalarText(v) {
    if (v == null) return "";
    if (typeof v === "string") return v;
    if (typeof v === "number") return String(v);
    return "";
}

function getMeta(argJson) {
    var a = J(argJson) || {};
    if (a.type !== "game") return "{}";

    var key = getConfig("apikey");
    if (!key) return "{}";

    var title = a.title || "";
    if (!title) return "{}";

    // Step 1: search by name, requesting the info fields + inline boxart.
    var searchUrl = TGDB + "/Games/ByGameName?apikey=" + enc(key) + "&name=" + enc(title) +
        "&fields=players,publishers,genres,overview,rating&include=boxart";
    var sr = J(httpRequest({ url: searchUrl, method: "GET", headers: { "Accept": "application/json" } }));
    if (!sr || !sr.data || !sr.data.games || !sr.data.games.length) return "{}";

    var game = sr.data.games[0];
    if (!game || !game.id) return "{}";
    var gameId = game.id;

    var images = {};

    // Inline boxart from the search response's include block.
    if (sr.include && sr.include.boxart) {
        var box = sr.include.boxart;
        var boxBase = box.base_url && (box.base_url.original || box.base_url.large || box.base_url.medium) || "";
        var boxData = box.data && box.data[gameId];
        if (boxBase && boxData && boxData.length) {
            for (var b = 0; b < boxData.length; b++) {
                var bi = boxData[b];
                if (!bi || !bi.filename) continue;
                var role = ROLE_BY_TYPE[bi.type] || "";
                addImage(images, role, boxBase + bi.filename);
            }
        }
    }

    // Step 2: full image set for this game.
    var imgUrl = TGDB + "/Games/Images?apikey=" + enc(key) + "&games_id=" + gameId;
    var ir = J(httpRequest({ url: imgUrl, method: "GET", headers: { "Accept": "application/json" } }));
    if (ir && ir.data) {
        var base = ir.data.base_url && (ir.data.base_url.original || ir.data.base_url.large || ir.data.base_url.medium) || "";
        var list = ir.data.images && ir.data.images[gameId];
        if (base && list && list.length) {
            for (var i = 0; i < list.length; i++) {
                var im = list[i];
                if (!im || !im.filename) continue;
                var r = ROLE_BY_TYPE[im.type] || "";
                addImage(images, r, base + im.filename);
            }
        }
    }

    // Canonicalise a box -> poster mirror and single best cover.
    var image = "";
    if (images.box && images.box.length) {
        images.poster = images.box.slice(0);
        image = images.box[0];
    }

    // Facts / meta from the ByGameName response.
    var overview  = scalarText(game.overview);
    var players   = scalarText(game.players);
    var rating    = scalarText(game.rating);
    var publisher = scalarText(game.publisher || game.publishers); // string only; id-arrays skipped
    var genre     = scalarText(game.genre || game.genres);

    var facts = [];
    if (publisher) facts.push(metaFact("Publisher", publisher));
    if (players)   facts.push(metaFact("Players", players));
    if (genre)     facts.push(metaFact("Genre", genre));
    if (rating)    facts.push(metaFact("Rating", rating));

    var out = { title: game.game_title || title };
    if (game.release_date) out.subtitle = String(game.release_date).substring(0, 4);
    if (overview) out.overview = overview;
    if (image) out.image = image;
    if (facts.length) out.facts = facts;

    var hasImg = false, ik;
    for (ik in images) { if (images.hasOwnProperty(ik) && images[ik].length) { hasImg = true; break; } }
    if (hasImg) out.images = images;

    var meta = {};
    if (publisher) meta.publisher = publisher;
    if (players)   meta.players = players;
    var hasMeta = false, mk;
    for (mk in meta) { if (meta.hasOwnProperty(mk)) { hasMeta = true; break; } }
    if (hasMeta) out.meta = meta;

    // Nothing useful at all -> no data.
    if (!hasImg && !overview && !facts.length) return "{}";

    return JSON.stringify(out);
}
