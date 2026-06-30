// Live TV via the public iptv-org playlists (https://iptv-org.github.io). Browse channels by country or by
// category, or search popular channels by name. Channel streams are HLS (.m3u8) and play through the
// built-in player. No API key needed.
//
// Why playlists and not the JSON API: channels.json is ~10 MB (too big to parse in the addon engine), while
// the per-country / per-category .m3u files are small and already pair each channel's name + logo + stream.

var API = "https://iptv-org.github.io/api";
var IPTV = "https://iptv-org.github.io/iptv";
var VER = "v1"; // cache namespace - bump to force a refresh of cached playlists

// Popular countries surfaced at the top level (the rest are under "All Countries").
var POPULAR = ["us", "uk", "ca", "au", "in", "de", "fr", "es", "it", "br", "mx", "jp", "kr"]; // iptv-org uses "uk"

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function trim(s) { return (s == null ? "" : String(s)).replace(/^\s+|\s+$/g, ""); }
function result(title, items, hasMore) { return JSON.stringify({ title: title, items: items, hasMore: !!hasMore }); }
function info(title, msg) { return JSON.stringify({ title: title, items: [{ id: "_info", title: msg, type: "info" }] }); }

// Fetch with a persistent cache. Playlists/metadata are stable enough; an individual stream that has gone
// dead simply fails at play time regardless of caching, so this only trades a little freshness for speed.
function fetchCached(key, url) {
    var cached = getStorage(VER + ":" + key);
    if (cached && cached.length > 0) return cached;
    var body = httpGet(url) || "";
    if (body.length > 0) setStorage(VER + ":" + key, body);
    return body;
}

function countries() {
    var arr = J(fetchCached("countries", API + "/countries.json")) || [];
    var map = {};
    for (var i = 0; i < arr.length; i++) map[trim(arr[i].code).toLowerCase()] = arr[i];
    return { list: arr, map: map };
}

function countryFolder(c) {
    return {
        id: "country/" + trim(c.code).toLowerCase(),
        title: (c.flag ? c.flag + " " : "") + c.name,
        subtitle: "Country", type: "livetv", thumbnailUrl: "", expandable: true, url: ""
    };
}

function folder(id, title, subtitle) {
    return { id: id, title: title, subtitle: subtitle, type: "livetv", thumbnailUrl: "", expandable: true, url: "" };
}

// Top level: a few popular countries, then "All Countries" and "Categories" to drill into. A query routes
// to channel search instead.
function getCatalog(argJson) {
    var a = J(argJson) || {};
    if (a.query) return searchChannels(a.query);

    var cc = countries();
    var items = [];
    for (var i = 0; i < POPULAR.length; i++)
        if (cc.map[POPULAR[i]]) items.push(countryFolder(cc.map[POPULAR[i]]));
    items.push(folder("countries", "🌍 All Countries (A–Z)", "Browse by country"));
    items.push(folder("categories", "🏷️ Categories", "Browse by genre"));
    return result("Live TV", items, false);
}

// Parse an iptv-org M3U into channel objects { name, logo, group, url }.
function parseM3U(text) {
    var lines = (text || "").split(/\r?\n/);
    var out = [], cur = null;
    for (var i = 0; i < lines.length; i++) {
        var line = lines[i];
        if (line.indexOf("#EXTINF") === 0) {
            var ci = line.lastIndexOf(",");
            var lm = line.match(/tvg-logo="([^"]*)"/);
            var gm = line.match(/group-title="([^"]*)"/);
            cur = { name: ci >= 0 ? trim(line.substring(ci + 1)) : "", logo: lm ? lm[1] : "", group: gm ? gm[1] : "" };
        } else if (line && line.charAt(0) !== "#") {
            var u = trim(line);
            if (cur && u) { cur.url = u; out.push(cur); }
            cur = null;
        }
    }
    return out;
}

function channelItem(c) {
    return {
        id: "ch:" + c.url, title: c.name || "Channel", subtitle: c.group || "",
        type: "livetv", thumbnailUrl: c.logo || "", url: c.url, mime: "video", expandable: false
    };
}

function channelsFromM3U(title, cacheKey, url) {
    var chans = parseM3U(fetchCached(cacheKey, url));
    var items = [];
    for (var i = 0; i < chans.length && items.length < 600; i++)
        if (chans[i].url) items.push(channelItem(chans[i]));
    if (!items.length) return info(title, "No channels available right now.");
    return result(title, items, items.length >= 600);
}

function getDetail(argJson) {
    var a = J(argJson) || {};
    var id = a.id || "";

    if (id === "countries") {
        var cc = countries(), items = [];
        for (var i = 0; i < cc.list.length; i++) items.push(countryFolder(cc.list[i]));
        return result("Countries", items, false);
    }
    if (id === "categories") {
        var cats = J(fetchCached("categories", API + "/categories.json")) || [];
        var out = [];
        for (var j = 0; j < cats.length; j++)
            out.push(folder("category/" + cats[j].id, cats[j].name, cats[j].description || ""));
        return result("Categories", out, false);
    }
    if (id.indexOf("country/") === 0) {
        var code = id.substring(8);
        var c = countries().map[code];
        var nm = c ? ((c.flag ? c.flag + " " : "") + c.name) : code.toUpperCase();
        return channelsFromM3U(nm, "m3u:country:" + code, IPTV + "/countries/" + code + ".m3u");
    }
    if (id.indexOf("category/") === 0) {
        var cat = id.substring(9);
        return channelsFromM3U("Live TV", "m3u:category:" + cat, IPTV + "/categories/" + cat + ".m3u");
    }
    return JSON.stringify({ title: "", items: [] });
}

// Search channel NAMES within a configurable set of countries (full channels.json is too big to scan here).
function searchChannels(query) {
    var q = trim(query).toLowerCase();
    if (!q) return info("Search", "Type a channel name to search.");
    var codes = (getConfig("searchCountries") || "us,uk,ca,au").split(",");
    var items = [], seen = {};
    for (var i = 0; i < codes.length && items.length < 80; i++) {
        var code = trim(codes[i]).toLowerCase();
        if (!code) continue;
        var chans = parseM3U(fetchCached("m3u:country:" + code, IPTV + "/countries/" + code + ".m3u"));
        for (var j = 0; j < chans.length && items.length < 80; j++) {
            var c = chans[j];
            if (!c.url || !c.name || seen[c.url]) continue;
            if (c.name.toLowerCase().indexOf(q) < 0) continue;
            seen[c.url] = 1;
            items.push(channelItem(c));
        }
    }
    if (!items.length) return info("Search", "No channels matched “" + query + "”.");
    return result("Results for “" + query + "”", items, false);
}

function search(argJson) { return searchChannels((J(argJson) || {}).query || ""); }

// Light detail header (the host only passes id + type here). Channels open and play directly, so this
// mainly dresses the folder detail views.
function getMeta(argJson) {
    var a = J(argJson) || {}, id = a.id || "";
    if (id.indexOf("country/") === 0) {
        var c = countries().map[id.substring(8)];
        if (c) return JSON.stringify({ title: (c.flag ? c.flag + " " : "") + c.name, subtitle: "Live TV by country",
            overview: "", image: "", facts: [] });
    }
    if (id === "countries")  return JSON.stringify({ title: "All Countries", subtitle: "Live TV", overview: "", image: "", facts: [] });
    if (id === "categories") return JSON.stringify({ title: "Categories", subtitle: "Live TV", overview: "", image: "", facts: [] });
    return JSON.stringify({ title: "", subtitle: "", overview: "", image: "", facts: [] });
}
