// Hello Source - a sample media-source addon for the native Goliath shell.
//
// A media-source addon implements getCatalog() and, optionally, search(). Both receive a JSON string
// argument and must return a JSON string of the shape:
//   { "title": "...", "items": [ { "id", "title", "subtitle", "type", "url", "thumbnailUrl" }, ... ] }
//
// item.type routes how the app opens it: "ebook" / "pdf" / "audio" / "video" (or "link" / a stream URL).
// item.url may be an http(s) URL or a path relative to this addon's own folder.
//
// Host functions available (sandboxed, gated by manifest "permissions"):
//   log(message)                -> void
//   httpGet(url)                -> string   (requires "network" permission)
//   getStorage(key)             -> string
//   setStorage(key, value)      -> void

function getCatalog(argJson) {
    log("Hello Source: building catalog");

    var items = [
        {
            id: "pride-and-prejudice",
            title: "Pride and Prejudice",
            subtitle: "Jane Austen (via Hello Source addon)",
            type: "ebook",
            url: "pride-and-prejudice.epub"   // resolved relative to this addon's folder
        }
    ];

    return JSON.stringify({ title: "Hello Source", items: items });
}

function search(argJson) {
    var query = (JSON.parse(argJson || "{}").query || "").toLowerCase();
    log("Hello Source: search for '" + query + "'");

    var all = JSON.parse(getCatalog("{}")).items;
    var hits = [];
    for (var i = 0; i < all.length; i++) {
        if (all[i].title.toLowerCase().indexOf(query) >= 0) hits.push(all[i]);
    }
    return JSON.stringify({ title: "Search: " + query, items: hits });
}
