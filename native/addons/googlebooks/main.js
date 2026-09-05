// Google Books as an AUDIOBOOK metadata provider for EverythingBox (issue #198).
//
// Pure metadata provider: only getMeta(), only for type "audiobook", no browse catalog. The SECOND of the
// two providers the app ships, and it sorts second on purpose: it has the better descriptions and covers
// and it states no narrator at all, so it backfills the fields Open Library left empty and never overrides
// one Open Library filled (AudiobookMeta::mergeLowerPriority does the folding, field by field).
//
// KEYLESS AND OFFICIAL: the volumes endpoint answers unauthenticated. No account, no key, nothing stored.
//
// IT DOES NOT SUPPLY A NARRATOR, and that is worth stating rather than leaving as a gap somebody tries to
// close. Google Books describes the WORK, not an audiobook edition of it. The narrator comes from Open
// Library's edition contributions, from an audiobook's own tags, or from an addon the user installed. It
// does not come from an unofficial Audible endpoint compiled into this app — see AudiobookMeta.h.
//
// The fact LABELS below are the host's contract (it reads audiobook fields by name), so they match the
// other provider's exactly.

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }
function metaFact(l, v) { return { label: l, value: (v == null) ? "" : String(v) }; }

var GB = "https://www.googleapis.com/books/v1/volumes";

// Google's thumbnails come back on http and with a curl parameter that crops the cover; both are fixed here
// so the cached image is the whole jacket over TLS.
function coverOf(links) {
    if (!links) return "";
    var u = links.extraLarge || links.large || links.medium || links.thumbnail || links.smallThumbnail || "";
    if (!u) return "";
    return String(u).replace(/^http:/, "https:").replace(/&edge=curl/g, "");
}

function getMeta(argJson) {
    var a = J(argJson) || {};
    if (a.type !== "audiobook") return "{}";

    var title = String(a.title || "").trim();
    if (!title) return "{}";
    var author = String(a.subtitle || "").trim();   // the host puts the book's author here

    var q = 'intitle:"' + title.replace(/"/g, "") + '"';
    if (author) q += ' inauthor:"' + author.replace(/"/g, "") + '"';
    var r = J(httpRequest({
        url: GB + "?maxResults=5&printType=books&q=" + enc(q),
        method: "GET",
        headers: { "Accept": "application/json" }
    }));
    if (!r || !r.items || !r.items.length) return "{}";

    var vol = r.items[0];
    var v = vol && vol.volumeInfo;
    if (!v) return "{}";

    var out = { title: v.title || title };
    if (v.subtitle) out.title = v.title + ": " + v.subtitle;
    if (v.authors && v.authors.length) out.subtitle = v.authors[0];
    if (v.description) out.overview = String(v.description);
    var cover = coverOf(v.imageLinks);
    if (cover) out.image = cover;

    var facts = [];
    if (v.authors && v.authors.length) facts.push(metaFact("Author", v.authors[0]));
    if (v.publishedDate) facts.push(metaFact("Published", String(v.publishedDate)));
    // seriesInfo is present on a minority of volumes and is the volume's OWN statement of its place, which
    // is the only kind of position this app accepts. Absent -> nothing is said, and the book keeps the place
    // the scan gave it.
    if (v.seriesInfo && v.seriesInfo.volumeSeries && v.seriesInfo.volumeSeries.length) {
        var vs = v.seriesInfo.volumeSeries[0];
        if (vs && vs.orderNumber) facts.push(metaFact("Series position", vs.orderNumber));
    }
    if (facts.length) out.facts = facts;

    if (vol.id) out.meta = { matchId: String(vol.id) };

    if (!out.image && !out.overview && !facts.length) return "{}";
    return JSON.stringify(out);
}
