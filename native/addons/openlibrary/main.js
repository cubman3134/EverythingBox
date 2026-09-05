// Open Library as an AUDIOBOOK metadata provider for EverythingBox (issue #198).
//
// Pure metadata provider: only getMeta(), only for type "audiobook", no browse catalog ("catalogs": []).
// The host fans a scanned book out across every addon that declared metaFor:["audiobook"] and merges the
// replies by provider precedence; this one leads, because Open Library is the only official public source
// in the pair that carries EDITION-level contributions — which is where an audiobook's narrator is.
//
// KEYLESS AND OFFICIAL. openlibrary.org publishes a documented public API with no account and no key. That
// is the whole reason it is one of the two providers the app ships. The audiobook catalogue the ecosystem
// actually leans on (Audible: narrator, series, ASIN) has NO official public API, so it is not here and not
// in the app — an unofficial endpoint belongs in an addon somebody chooses to install, and any addon that
// declares metaFor:["audiobook"] is an equal provider to this one.
//
// WHAT IT ANSWERS WITH, and the labels matter — the host reads audiobook fields out of the labelled facts
// list by NAME, so these spellings are the contract:
//   Narrator          - from an audiobook edition's `contributions` ("Martin Shaw (Narrator)"), which is the
//                       only place Open Library states one.
//   Series            - the work/edition `series` field, with its trailing number split off.
//   Series position   - THAT NUMBER, and nothing else. Never parsed out of a title: "Book 3 of ..." in a
//                       title is as often a boxed-set volume or a publisher's numbering as it is the place
//                       the shelf sorts by, and the host refuses to guess (see AudiobookMeta.h).
//   Published         - first publish year.
//   Runtime           - only when an audiobook edition states one; usually absent, and then omitted.
//
// THREE REQUESTS AT MOST, and only for a book whose tags left blanks: the search, the work (for its
// description), and the edition list (for a narrator). A book whose tags already say everything is never
// asked about at all — the host skips it before this file is reached.

function J(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function enc(s) { return encodeURIComponent(s || ""); }
function metaFact(l, v) { return { label: l, value: (v == null) ? "" : String(v) }; }

var OL = "https://openlibrary.org";
var COVERS = "https://covers.openlibrary.org";

function get(url) {
    return J(httpRequest({ url: url, method: "GET", headers: { "Accept": "application/json" } }));
}

// Open Library's description is either a plain string or { "type": "...", "value": "..." }.
function descriptionOf(o) {
    if (!o) return "";
    var d = o.description;
    if (!d) return "";
    if (typeof d === "string") return d;
    if (d.value) return String(d.value);
    return "";
}

// "Discworld ; 3" / "Discworld #3" / "Discworld, book 3" -> { name: "Discworld", index: "3" }. The index
// comes from THIS field, which is the provider's own statement about where the book sits — never from a
// title. A series string with no number yields an empty index and the host leaves the book where the scan
// put it.
function splitSeries(raw) {
    var s = String(raw || "").trim();
    if (!s) return { name: "", index: "" };
    var m = s.match(/^(.*?)[\s,;]*(?:#|bk\.?|book|vol\.?|volume|no\.?)?\s*([0-9]+(?:\.[0-9]+)?)\s*$/i);
    if (m && m[1]) return { name: m[1].replace(/[\s,;:#-]+$/, "").trim(), index: m[2] };
    return { name: s, index: "" };
}

// The narrator, out of an edition's `contributions`: Open Library writes them as "Name (Role)".
function narratorOf(contributions) {
    if (!contributions || !contributions.length) return "";
    for (var i = 0; i < contributions.length; i++) {
        var c = String(contributions[i] || "");
        if (/\b(narrator|narrated|read by|reader)\b/i.test(c))
            return c.replace(/\s*\([^)]*\)\s*$/, "").trim();
    }
    return "";
}

// Is this edition an AUDIO one? Open Library states the format inconsistently, so three fields are checked
// and none of them is required — a print edition's contributions are read too, because a "read by" credit on
// one is still a statement about who narrated the book.
function isAudioEdition(e) {
    var fmt = String((e && e.physical_format) || "");
    if (/audio|cd|cassette|mp3/i.test(fmt)) return true;
    var media = (e && e.media_type) || "";
    if (/audio/i.test(String(media))) return true;
    return false;
}

function getMeta(argJson) {
    var a = J(argJson) || {};
    if (a.type !== "audiobook") return "{}";

    var title = String(a.title || "").trim();
    if (!title) return "{}";
    var author = String(a.subtitle || "").trim();   // the host puts the book's author here

    var url = OL + "/search.json?limit=5&fields=key,title,author_name,first_publish_year,cover_i,series"
            + "&title=" + enc(title);
    if (author) url += "&author=" + enc(author);
    var sr = get(url);
    if (!sr || !sr.docs || !sr.docs.length) return "{}";

    var doc = sr.docs[0];
    if (!doc || !doc.key) return "{}";

    var workKey = String(doc.key);                       // "/works/OL27448W"
    var out = { title: doc.title || title };
    if (doc.author_name && doc.author_name.length) out.subtitle = doc.author_name[0];
    if (doc.cover_i) out.image = COVERS + "/b/id/" + doc.cover_i + "-L.jpg";

    var facts = [];
    if (doc.author_name && doc.author_name.length) facts.push(metaFact("Author", doc.author_name[0]));
    if (doc.first_publish_year) facts.push(metaFact("Published", doc.first_publish_year));

    var series = { name: "", index: "" };
    if (doc.series && doc.series.length) series = splitSeries(doc.series[0]);

    // The work, for its description.
    var work = get(OL + workKey + ".json");
    var overview = descriptionOf(work);
    if (overview) out.overview = overview;
    if (!series.name && work && work.series && work.series.length) series = splitSeries(work.series[0]);

    // The editions, for a NARRATOR — the field that earns this feature, and the only place Open Library
    // states one. An audiobook edition is preferred; any edition carrying a "read by" credit will do.
    var narrator = "", runtime = "";
    var eds = get(OL + workKey + "/editions.json?limit=50");
    if (eds && eds.entries && eds.entries.length) {
        var fallback = "";
        for (var i = 0; i < eds.entries.length; i++) {
            var e = eds.entries[i];
            var n = narratorOf(e && e.contributions);
            if (!n) continue;
            if (isAudioEdition(e)) {
                narrator = n;
                if (e.number_of_pages) { /* a page count is not a runtime; deliberately ignored */ }
                if (e.physical_dimensions) { /* nor is a box size */ }
                break;
            }
            if (!fallback) fallback = n;
        }
        if (!narrator) narrator = fallback;
        if (!series.name) {
            for (var k = 0; k < eds.entries.length; k++) {
                var se = eds.entries[k];
                if (se && se.series && se.series.length) { series = splitSeries(se.series[0]); break; }
            }
        }
    }

    if (narrator) facts.push(metaFact("Narrator", narrator));
    if (series.name) facts.push(metaFact("Series", series.name));
    if (series.index) facts.push(metaFact("Series position", series.index));
    if (runtime) facts.push(metaFact("Runtime", runtime));
    if (facts.length) out.facts = facts;

    // The provider's own id for what it matched, in the reply's extra-metadata bag. The host stores it so
    // the book's level can name what was matched and the user can reject it.
    out.meta = { matchId: workKey };

    if (!out.image && !out.overview && !facts.length) return "{}";   // nothing useful: say nothing
    return JSON.stringify(out);
}
