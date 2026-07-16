// ScreenScraper game-metadata provider for My Media Vault.
//
// Pure metadata/artwork provider: only implements getMeta() for type "game". No browse
// catalog (manifest "catalogs": []). The app fans out to every game-meta provider on hover
// and merges results.
//
// Auth: a free ScreenScraper account configured in Settings — "ssid" (username) and
// "sspassword" (password); optional "devid"/"devpassword". Credentials are passed as GET
// query params to the jeuInfos.php endpoint and are never logged or returned.

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }
function metaFact(l, v) { return { label: l, value: (v == null) ? "" : String(v) }; }

var SS = "https://www.screenscraper.fr/api2/jeuInfos.php";

// Common console name -> ScreenScraper systemeid. Unknown -> 0 (omit the param).
var SS_SYSTEMS = {
    "snes": 4, "super nintendo": 4, "super famicom": 4,
    "nes": 3, "famicom": 3, "nintendo entertainment system": 3,
    "genesis": 1, "mega drive": 1, "megadrive": 1, "sega genesis": 1,
    "gba": 12, "game boy advance": 12, "gameboy advance": 12,
    "gb": 9, "game boy": 9, "gameboy": 9,
    "gbc": 10, "game boy color": 10, "gameboy color": 10,
    "n64": 14, "nintendo 64": 14,
    "ps1": 57, "psx": 57, "playstation": 57, "playstation 1": 57,
    "ps2": 58, "playstation 2": 58,
    "gamecube": 13, "gc": 13, "nintendo gamecube": 13,
    "wii": 16, "nintendo wii": 16,
    "dreamcast": 23, "sega dreamcast": 23,
    "saturn": 22, "sega saturn": 22,
    "master system": 2, "sega master system": 2,
    "psp": 61, "playstation portable": 61,
    "arcade": 75, "mame": 75,
    "atari 2600": 26
};

function systemId(hint) {
    if (!hint) return 0;
    var key = String(hint).toLowerCase();
    if (SS_SYSTEMS.hasOwnProperty(key)) return SS_SYSTEMS[key];
    return 0;
}

// From a localised-text array [{langue|region, text}], pick the `en`/`us` entry then fall back
// to the first available text.
function pickText(arr) {
    if (!arr) return "";
    if (typeof arr === "string") return arr;
    if (!arr.length) return "";
    var i;
    for (i = 0; i < arr.length; i++) {
        var e = arr[i];
        if (!e) continue;
        var lang = (e.langue || e.region || "").toLowerCase();
        if ((lang === "en" || lang === "us" || lang === "wor") && e.text) return e.text;
    }
    for (i = 0; i < arr.length; i++) { if (arr[i] && arr[i].text) return arr[i].text; }
    return "";
}

// jeu.developpeur / editeur / joueurs may be an object {text:...} or a plain string.
function fieldText(f) {
    if (!f) return "";
    if (typeof f === "string") return f;
    if (f.text) return f.text;
    return "";
}

// Collect media urls whose `type` is in `types`, preferring en/us/wor region first.
function collectMedia(medias, types) {
    var preferred = [], other = [];
    if (!medias || !medias.length) return preferred;
    for (var i = 0; i < medias.length; i++) {
        var m = medias[i];
        if (!m || !m.url || !m.type) continue;
        var match = false;
        for (var t = 0; t < types.length; t++) { if (m.type === types[t]) { match = true; break; } }
        if (!match) continue;
        var region = (m.region || "").toLowerCase();
        if (region === "en" || region === "us" || region === "wor" || region === "eu") preferred.push(m.url);
        else other.push(m.url);
    }
    return preferred.concat(other);
}

function getMeta(argJson) {
    var a = J(argJson) || {};
    if (a.type !== "game") return "{}";

    var ssid = getConfig("ssid");
    var sspassword = getConfig("sspassword");
    if (!ssid) return "{}";

    var title = a.title || "";
    if (!title) return "{}";

    var devid = getConfig("devid") || "";
    var devpassword = getConfig("devpassword") || "";

    var url = SS + "?devid=" + enc(devid) + "&devpassword=" + enc(devpassword) +
        "&softname=mymediavault&output=json&ssid=" + enc(ssid) +
        "&sspassword=" + enc(sspassword) + "&romnom=" + enc(title);

    var sid = systemId(a.systemHint);
    if (sid) url += "&systemeid=" + sid;

    var r = J(httpRequest({ url: url, method: "GET", headers: { "Accept": "application/json" } }));
    if (!r || !r.response || !r.response.jeu) return "{}";

    var jeu = r.response.jeu;
    var medias = jeu.medias || [];

    var box    = collectMedia(medias, ["box-2D", "box-3D"]);
    var logo   = collectMedia(medias, ["wheel"]);
    var shots  = collectMedia(medias, ["ss", "sstitle"]);
    var fanart = collectMedia(medias, ["fanart"]);
    var banner = collectMedia(medias, ["screenmarquee"]);
    var videos = collectMedia(medias, ["video", "video-normalized"]);

    var images = {};
    if (box.length)    { images.box = box; images.poster = box; }
    if (logo.length)   { images.logo = logo; }
    if (shots.length)  { images.screenshot = shots; }
    if (fanart.length) { images.fanart = fanart; images.background = fanart; }
    if (banner.length) { images.banner = banner; }

    var overview = "";
    if (jeu.synopsis) overview = pickText(jeu.synopsis);

    var developer = fieldText(jeu.developpeur);
    var publisher = fieldText(jeu.editeur);
    var players   = fieldText(jeu.joueurs);
    var genre     = "";
    if (jeu.genres) {
        // genres is typically an array of {noms:[{langue,text}]} entries.
        var names = [];
        if (jeu.genres.length) {
            for (var gi = 0; gi < jeu.genres.length; gi++) {
                var g = jeu.genres[gi];
                if (!g) continue;
                var nm = pickText(g.noms) || fieldText(g);
                if (nm) names.push(nm);
            }
        }
        genre = names.join(", ");
    }

    var facts = [];
    if (developer) facts.push(metaFact("Developer", developer));
    if (publisher) facts.push(metaFact("Publisher", publisher));
    if (players)   facts.push(metaFact("Players", players));
    if (genre)     facts.push(metaFact("Genre", genre));

    var title2 = pickText(jeu.noms) || title;

    var out = { title: title2 };
    if (overview) out.overview = overview;
    if (box.length) out.image = box[0];
    if (facts.length) out.facts = facts;

    var hasImg = false, ik;
    for (ik in images) { if (images.hasOwnProperty(ik)) { hasImg = true; break; } }
    if (hasImg) out.images = images;
    if (videos.length) out.videos = videos;

    var meta = {};
    if (developer) meta.developer = developer;
    if (publisher) meta.publisher = publisher;
    if (players)   meta.players = players;
    var hasMeta = false, mk;
    for (mk in meta) { if (meta.hasOwnProperty(mk)) { hasMeta = true; break; } }
    if (hasMeta) out.meta = meta;

    return JSON.stringify(out);
}
