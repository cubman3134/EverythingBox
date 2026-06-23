// Podcasts via the keyless iTunes Search API. Demonstrates an addon-defined media type ("podcast"),
// declared in manifest.json under "mediaTypes" with its own colour + icon. A podcast drills into its
// episodes; an episode carries an audio URL, so it plays when opened.

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }
function result(title, items, hasMore) { return JSON.stringify({ title: title, items: items, hasMore: !!hasMore }); }
function info(title, msg) { return JSON.stringify({ title: title, items: [{ id: "_info", title: msg, type: "info" }] }); }
function metaFact(l, v) { return { label: l, value: (v == null) ? "" : String(v) }; }

var ITUNES = "https://itunes.apple.com";

function getCatalog(argJson) {
    var a = J(argJson) || {};
    var term = a.query || "news"; // a default browse term when nothing is searched
    var r = J(httpGet(ITUNES + "/search?media=podcast&limit=40&term=" + enc(term)));
    if (!r || !r.results) return info("Podcasts", "Could not reach iTunes.");
    var items = [];
    for (var i = 0; i < r.results.length; i++) {
        var p = r.results[i];
        if (!p.collectionId) continue;
        items.push({
            id: "itpod:" + p.collectionId,
            title: p.collectionName,
            subtitle: p.artistName || "",
            type: "podcast",
            thumbnailUrl: p.artworkUrl600 || p.artworkUrl100 || "",
            expandable: true, url: ""
        });
    }
    return result("Podcasts", items, false);
}

function getDetail(argJson) {
    var a = J(argJson) || {};
    if (a.type !== "podcast") return JSON.stringify({ title: "", items: [] });
    var id = (a.id || "").split(":")[1];
    var r = J(httpGet(ITUNES + "/lookup?id=" + enc(id) + "&media=podcast&entity=podcastEpisode&limit=60"));
    if (!r || !r.results) return JSON.stringify({ title: "Episodes", items: [] });
    var items = [];
    for (var i = 0; i < r.results.length; i++) {
        var e = r.results[i];
        if (e.wrapperType !== "podcastEpisode" && e.kind !== "podcast-episode") continue;
        items.push({
            id: "itep:" + (e.trackId || i),
            title: e.trackName || "Episode",
            subtitle: (e.releaseDate || "").substring(0, 10),
            type: "podcast_episode",
            thumbnailUrl: e.artworkUrl160 || e.artworkUrl600 || "",
            url: e.episodeUrl || e.previewUrl || "", // audio stream -> plays when opened
            mime: "audio", expandable: false
        });
    }
    return JSON.stringify({ title: "Episodes", items: items });
}

function getMeta(argJson) {
    var a = J(argJson) || {};
    if (a.type !== "podcast") return "{}";
    var id = (a.id || "").split(":")[1];
    var r = J(httpGet(ITUNES + "/lookup?id=" + enc(id)));
    if (!r || !r.results || !r.results.length) return "{}";
    var p = r.results[0], facts = [];
    if (p.artistName)       facts.push(metaFact("Author", p.artistName));
    if (p.primaryGenreName) facts.push(metaFact("Genre", p.primaryGenreName));
    if (p.trackCount)       facts.push(metaFact("Episodes", p.trackCount));
    if (p.releaseDate)      facts.push(metaFact("Latest", p.releaseDate.substring(0, 10)));
    return JSON.stringify({ title: p.collectionName, subtitle: p.artistName || "",
        overview: "", image: p.artworkUrl600 || p.artworkUrl100 || "", facts: facts });
}

function search(argJson) { return getCatalog(argJson); }
