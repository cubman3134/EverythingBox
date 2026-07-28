// Headless check of the Trakt read layer (issue #23). TraktClient scrobbles AND now reads; TraktRead is
// where every wire format it touches lives — a TOLERANT parser for the "my shows" calendar, the identity
// mapper the watchlist/collection and history-backfill follow-ups will both reuse, the on-disk cache
// format, and the OAuth token reply.
//
// The fixtures below are written from the shape Trakt's current API reference documents for
// GET /calendars/{target}/shows/{start_date}/{days} (docs.trakt.tv/reference/getcalendarsshows):
// a flat array of objects, each with a top-level `first_aired` string and `show` / `episode` objects
// carrying an `ids` bag whose `trakt`/`tvdb`/`tmdb` are integers and `imdb`/`slug` are strings.
//
// Two properties matter most and are pinned hardest:
//   * The parser is TOTAL. One malformed entry must never cost the user their whole calendar.
//   * imdbStreamIdFor returns "" — not an error, not a guess — when Trakt gave no show IMDB id.
//     That empty string is the documented signal for "show it, but it is not playable".
//
// §11-§13 pin the three properties the NETWORK paths depend on and could not otherwise reach without a
// socket: that "an empty calendar" and "not a calendar at all" are distinguishable before the offline
// cache is overwritten (§11); that the token-refresh queue issues one refresh for any number of callers
// (§12); and — the one whose failure is unrecoverable — that a 200 which is not a token reply can never
// blank the stored access AND refresh tokens, which permanently unlinks the account (§13).
//
// Prints TRAKT-OK on success; any failure prints TRAKT-FAIL <cond> and exits non-zero.
#include "SingleFlight.h"
#include "TraktRead.h"

#include <QByteArray>
#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "TRAKT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A normal two-entry batch. The SECOND show deliberately has NO imdb id — the unjoinable case.
static const char* kNormal = R"([
  { "first_aired": "2026-08-04T01:00:00.000Z",
    "episode": { "season": 1, "number": 4, "title": "Fourth",
                 "ids": { "trakt": 11, "tvdb": 12, "imdb": "tt2000004", "tmdb": 13 } },
    "show": { "title": "Alpha Show",
              "ids": { "trakt": 1, "slug": "alpha", "tvdb": 2, "imdb": "tt1000001", "tmdb": 3 } } },
  { "first_aired": "2026-08-05T02:30:00.000Z",
    "episode": { "season": 2, "number": 7, "title": "Seventh",
                 "ids": { "trakt": 21, "tvdb": 22, "tmdb": 23 } },
    "show": { "title": "Beta Show",
              "ids": { "trakt": 4, "slug": "beta", "tvdb": 5, "tmdb": 6 } } }
])";

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. the normal batch ----------------------------------------------------------------
    {
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(kNormal));
        CHECK(e.size() == 2);
        CHECK(e.value(0).showTitle == QStringLiteral("Alpha Show"));
        CHECK(e.value(0).season == 1);
        CHECK(e.value(0).episode == 4);
        CHECK(e.value(0).episodeTitle == QStringLiteral("Fourth"));
        CHECK(e.value(0).showIds.imdb == QStringLiteral("tt1000001"));
        CHECK(e.value(0).airsAtUtc.isValid());
        // Numeric ids must survive as strings — a number read with toString() yields empty and the
        // id is silently lost, which is the bug this pins.
        CHECK(e.value(0).showIds.tmdb == QStringLiteral("3"));
        CHECK(e.value(0).showIds.tvdb == QStringLiteral("2"));
        // The unjoinable second entry is PRESENT, with an empty imdb.
        CHECK(e.value(1).showTitle == QStringLiteral("Beta Show"));
        CHECK(e.value(1).showIds.imdb.isEmpty());
        CHECK(e.value(1).showIds.tmdb == QStringLiteral("6"));
    }

    // ---- 2. TOTALITY: a malformed entry must not cost the surrounding ones --------------------
    {
        const char* mixed = R"([
          { "first_aired": "2026-08-04T01:00:00.000Z",
            "episode": { "season": 1, "number": 1, "title": "Keep me",
                         "ids": { "imdb": "tt9" } },
            "show": { "title": "Good", "ids": { "imdb": "tt1" } } },
          { "first_aired": "not-a-date",
            "episode": { "season": 1, "number": 2 }, "show": { "title": "Bad date" } },
          "a bare string where an object belongs",
          { "episode": { "season": 1, "number": 3 }, "show": { "title": "No date at all" } },
          { "first_aired": "2026-08-06T01:00:00.000Z",
            "episode": { "season": 3, "number": 9, "title": "Keep me too",
                         "ids": { "imdb": "tt8" } },
            "show": { "title": "Also good", "ids": { "imdb": "tt2" } } }
        ])";
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(mixed));
        // Exactly the two well-formed entries survive; the three broken ones are dropped.
        CHECK(e.size() == 2);
        CHECK(e.value(0).showTitle == QStringLiteral("Good"));
        CHECK(e.value(1).showTitle == QStringLiteral("Also good"));
    }

    // ---- 3. degenerate inputs must return empty, never crash ---------------------------------
    CHECK(trakt::parseMyShowsCalendar(QByteArray()).isEmpty());
    CHECK(trakt::parseMyShowsCalendar(QByteArray("not json at all")).isEmpty());
    CHECK(trakt::parseMyShowsCalendar(QByteArray("[]")).isEmpty());
    CHECK(trakt::parseMyShowsCalendar(QByteArray("{}")).isEmpty());          // object, not array
    CHECK(trakt::parseMyShowsCalendar(QByteArray("null")).isEmpty());
    // A single WELL-FORMED entry that is not wrapped in an array must not be accepted as a one-row
    // calendar. The endpoint returns a flat array; a naked object is an error body, not a row. (The
    // bare "{}" above cannot pin this on its own — it has no parseable content either way.)
    CHECK(trakt::parseMyShowsCalendar(QByteArray(
        R"({ "first_aired": "2026-08-04T01:00:00.000Z",
             "episode": { "season": 1, "number": 1, "title": "Naked" },
             "show": { "title": "Naked", "ids": { "imdb": "tt7" } } })")).isEmpty());

    // ---- 4. a missing ids OBJECT is not a crash ----------------------------------------------
    {
        const char* noIds = R"([
          { "first_aired": "2026-08-04T01:00:00.000Z",
            "episode": { "season": 1, "number": 1 }, "show": { "title": "Idless" } }
        ])";
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(noIds));
        CHECK(e.size() == 1);
        CHECK(e.value(0).showIds.imdb.isEmpty());
        CHECK(e.value(0).showIds.tmdb.isEmpty());
        CHECK(e.value(0).showTitle == QStringLiteral("Idless"));
    }

    // ---- 5. imdbStreamIdFor: the identity mapper ---------------------------------------------
    {
        TraktIds full; full.imdb = QStringLiteral("tt1000001"); full.tmdb = QStringLiteral("3");
        CHECK(trakt::imdbStreamIdFor(full, 1, 4) == QStringLiteral("tt1000001:1:4"));
        CHECK(trakt::imdbStreamIdFor(full, 12, 205) == QStringLiteral("tt1000001:12:205"));
        // Season 0 is a REAL season on Trakt (specials) and must map, not be rejected.
        CHECK(trakt::imdbStreamIdFor(full, 0, 1) == QStringLiteral("tt1000001:0:1"));

        // No show imdb id -> empty. NOT an error, NOT a guess from tmdb/tvdb: the empty string is
        // the documented "not playable" signal the catalog builder keys on.
        TraktIds noImdb; noImdb.tmdb = QStringLiteral("6"); noImdb.tvdb = QStringLiteral("5");
        CHECK(trakt::imdbStreamIdFor(noImdb, 2, 7).isEmpty());
        CHECK(trakt::imdbStreamIdFor(TraktIds{}, 1, 1).isEmpty());

        // Nonsense episode numbers must not produce a malformed id that the resolver would choke on.
        CHECK(trakt::imdbStreamIdFor(full, -1, 4).isEmpty());
        CHECK(trakt::imdbStreamIdFor(full, 1, 0).isEmpty());
        CHECK(trakt::imdbStreamIdFor(full, 1, -3).isEmpty());

        // …and neither must a nonsense ID. The range guard above exists to stop a stream id that
        // LOOKS playable and is not; an unvalidated imdb field is the same failure through another
        // door. A "tt" prefix and NO embedded ':' are both required — the first rejects an id that
        // is not an IMDB id at all, the second rejects one that would silently add extra fields.
        TraktIds numeric;   numeric.imdb   = QStringLiteral("123");        // a number that reached idString()
        TraktIds colons;    colons.imdb    = QStringLiteral("tt1:9:9");    // would yield FIVE fields
        TraktIds blank;     blank.imdb     = QStringLiteral("   ");        // present but empty after trim
        TraktIds wrongKind; wrongKind.imdb = QStringLiteral("nm0000001");  // a plausible IMDB *person* id
        CHECK(trakt::imdbStreamIdFor(numeric, 1, 1).isEmpty());
        CHECK(trakt::imdbStreamIdFor(colons, 1, 1).isEmpty());
        CHECK(trakt::imdbStreamIdFor(blank, 1, 1).isEmpty());
        CHECK(trakt::imdbStreamIdFor(wrongKind, 1, 1).isEmpty());
    }

    // ---- 5b. the struct's own defaults must agree with the parser's sentinels ------------------
    // The parser writes -1 for a missing season/episode. If the struct defaulted to 0, a
    // default-constructed entry would read as a VALID special (season 0) with an invalid episode.
    {
        CalendarEntry fresh;
        CHECK(fresh.season == -1);
        CHECK(fresh.episode == -1);
    }

    // ---- 6. shapes the API reference documents that the sketch above did not ------------------
    // The reference marks ids.tvdb / ids.imdb / ids.tmdb NULLABLE, so an explicit JSON null is a
    // documented shape, not an anomaly — it must read as an empty id, never the literal "null".
    // It also lists an ids.plex OBJECT, which must not derail the sibling ids around it. And it
    // pins first_aired only as a UTC ISO 8601 string, not its fractional-second part, so the
    // no-milliseconds form must parse too.
    {
        const char* docShapes = R"([
          { "first_aired": "2026-08-07T04:00:00.000Z",
            "episode": { "season": 4, "number": 2, "title": null,
                         "ids": { "trakt": 31, "tvdb": null, "imdb": null, "tmdb": 33 } },
            "show": { "title": "Nullable Show", "year": null,
                      "ids": { "trakt": 7, "slug": "nullable", "tvdb": null,
                               "imdb": null, "tmdb": 9, "plex": { "guid": "plex://show/x" } } } },
          { "first_aired": "2026-08-08T05:00:00Z",
            "episode": { "season": 1, "number": 1, "title": "No millis",
                         "ids": { "trakt": 41 } },
            "show": { "title": "No Millis Show", "ids": { "trakt": 10, "imdb": "tt5" } } }
        ])";
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(docShapes));
        CHECK(e.size() == 2);
        // A null id is empty, not the four characters "null".
        CHECK(e.value(0).showIds.imdb.isEmpty());
        CHECK(e.value(0).showIds.tvdb.isEmpty());
        CHECK(e.value(0).episodeIds.imdb.isEmpty());
        // …and the non-null siblings on either side of the nulls and the plex object still land.
        CHECK(e.value(0).showIds.trakt == QStringLiteral("7"));
        CHECK(e.value(0).showIds.tmdb == QStringLiteral("9"));
        CHECK(e.value(0).episodeIds.tmdb == QStringLiteral("33"));
        CHECK(e.value(0).episodeTitle.isEmpty());          // title is nullable too
        // A nulled show imdb is exactly the unjoinable case: present, but not playable.
        CHECK(trakt::imdbStreamIdFor(e.value(0).showIds, e.value(0).season, e.value(0).episode).isEmpty());
        // The seconds-precision air time parses rather than dropping the entry.
        CHECK(e.value(1).showTitle == QStringLiteral("No Millis Show"));
        CHECK(e.value(1).airsAtUtc.isValid());
        CHECK(e.value(1).airsAtUtc.toString(Qt::ISODate) == QStringLiteral("2026-08-08T05:00:00Z"));
    }

    // ---- 7. first_aired zone handling: the wall-clock must never move ------------------------
    // Trakt states every calendar time is UTC, but the API reference pins first_aired only as
    // {"type":"string"} with NO format annotation — so a response that drops the trailing Z, or a
    // proxy that reformats the field, is a shape we must survive. Qt parses a zone-less ISO string
    // as LOCAL time; converting that to UTC shifts it by the machine's offset, which puts a late
    // evening episode on the wrong calendar DAY for every non-UTC user (and moves with DST).
    //
    // So: a designator-free string is read as the UTC the API promises, while an EXPLICIT offset is
    // honoured rather than overridden. This section runs on whatever zone the machine is in — that
    // is the point; on a UTC machine it is a tautology, everywhere else it is the regression.
    {
        const char* zones = R"([
          { "first_aired": "2026-08-08T23:30:00Z",
            "episode": { "season": 1, "number": 1 }, "show": { "title": "Zulu" } },
          { "first_aired": "2026-08-08T23:30:00.250Z",
            "episode": { "season": 1, "number": 2 }, "show": { "title": "Zulu millis" } },
          { "first_aired": "2026-08-09T01:30:00+02:00",
            "episode": { "season": 1, "number": 3 }, "show": { "title": "Offset" } },
          { "first_aired": "2026-08-08T23:30:00",
            "episode": { "season": 1, "number": 4 }, "show": { "title": "Naked time" } },
          { "first_aired": "2026-08-08",
            "episode": { "season": 1, "number": 5 }, "show": { "title": "Naked date" } }
        ])";
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(zones));
        CHECK(e.size() == 5);
        // Every one of these denotes the SAME instant, 2026-08-08T23:30Z — except the bare date,
        // which is midnight UTC on the 8th.
        CHECK(e.value(0).airsAtUtc.toString(Qt::ISODate) == QStringLiteral("2026-08-08T23:30:00Z"));
        CHECK(e.value(1).airsAtUtc.toString(Qt::ISODateWithMs) == QStringLiteral("2026-08-08T23:30:00.250Z"));
        // An explicit offset is REAL information: 01:30+02:00 is 23:30Z. It must be converted, not
        // stamped over with UTC — this is the assertion an over-eager "force everything to UTC" fix
        // trips on.
        CHECK(e.value(2).airsAtUtc.toString(Qt::ISODate) == QStringLiteral("2026-08-08T23:30:00Z"));
        // No designator at all: the wall clock must survive untouched.
        CHECK(e.value(3).airsAtUtc.toString(Qt::ISODate) == QStringLiteral("2026-08-08T23:30:00Z"));
        CHECK(e.value(4).airsAtUtc.toString(Qt::ISODate) == QStringLiteral("2026-08-08T00:00:00Z"));
        // And the day itself — the thing a shifted time actually costs the user.
        CHECK(e.value(3).airsAtUtc.date().toString(Qt::ISODate) == QStringLiteral("2026-08-08"));
    }

    // ---- 8. a numeric id that cannot be a qint64 is dropped, not wrapped ----------------------
    // qint64(double) is UNDEFINED for an out-of-range value; on MSVC 1e300 lands on
    // -9223372036854775808, a bogus id later joins could match on. An unrepresentable number is
    // no id at all.
    {
        const char* huge = R"([
          { "first_aired": "2026-08-04T01:00:00.000Z",
            "episode": { "season": 1, "number": 1 },
            "show": { "title": "Huge", "ids": { "imdb": "tt6", "tmdb": 1e300, "tvdb": -1e300, "trakt": 42 } } }
        ])";
        const QVector<CalendarEntry> e = trakt::parseMyShowsCalendar(QByteArray(huge));
        CHECK(e.size() == 1);
        CHECK(e.value(0).showIds.tmdb.isEmpty());
        CHECK(e.value(0).showIds.tvdb.isEmpty());
        CHECK(e.value(0).showIds.trakt == QStringLiteral("42"));   // the sane sibling still lands
    }

    // ---- 9. the disk cache ROUND-TRIPS every field -------------------------------------------
    // TraktClient persists the last good calendar so an offline launch still shows something. That is
    // only worth doing if what comes back is what went in: a field silently dropped here is a title or
    // an id that is present on a live launch and missing on an offline one — the hardest kind of bug to
    // see, because the offline path is the one nobody looks at. So every field is asserted, on an entry
    // built to exercise the awkward ones rather than on a parse result alone.
    {
        QVector<CalendarEntry> in = trakt::parseMyShowsCalendar(QByteArray(kNormal));
        CHECK(in.size() == 2);

        // posterUrl is never set by the parser (Trakt's calendar carries no image), so a round trip
        // over parse output alone would never touch it. Set it here, or the cache could drop the field
        // and every test would still pass.
        in[0].posterUrl = QStringLiteral("https://example.invalid/a.jpg");
        // Non-ASCII, an apostrophe and a '=' — the characters an ini-backed store is most likely to
        // mangle, since QSettings has to escape them to survive the file format.
        in[1].showTitle = QStringLiteral("Bêta Show: “quoted” = ünïcodé");

        // A third entry pinning the SENTINELS. An episode whose season/number Trakt omitted reads back
        // as -1, not 0 — 0 is a real season (specials), so a cache that defaulted to it would resurrect
        // unknown episodes as valid-looking specials.
        CalendarEntry sentinel;
        sentinel.airsAtUtc = QDateTime::fromString(QStringLiteral("2026-08-09T20:00:00.000Z"),
                                                   Qt::ISODateWithMs).toUTC();
        sentinel.showTitle = QStringLiteral("Sentinel");
        in.push_back(sentinel);

        const QVector<CalendarEntry> out = trakt::deserializeCalendar(trakt::serializeCalendar(in));
        CHECK(out.size() == in.size());
        for (int i = 0; i < qMin(out.size(), in.size()); ++i)
        {
            CHECK(out[i].airsAtUtc == in[i].airsAtUtc);
            CHECK(out[i].showTitle == in[i].showTitle);
            CHECK(out[i].season == in[i].season);
            CHECK(out[i].episode == in[i].episode);
            CHECK(out[i].episodeTitle == in[i].episodeTitle);
            CHECK(out[i].posterUrl == in[i].posterUrl);
            CHECK(out[i].showIds.imdb == in[i].showIds.imdb);
            CHECK(out[i].showIds.tmdb == in[i].showIds.tmdb);
            CHECK(out[i].showIds.tvdb == in[i].showIds.tvdb);
            CHECK(out[i].showIds.trakt == in[i].showIds.trakt);
            CHECK(out[i].episodeIds.imdb == in[i].episodeIds.imdb);
            CHECK(out[i].episodeIds.tmdb == in[i].episodeIds.tmdb);
            CHECK(out[i].episodeIds.tvdb == in[i].episodeIds.tvdb);
            CHECK(out[i].episodeIds.trakt == in[i].episodeIds.trakt);
        }
        // Spot-check the two that carry the most meaning downstream, so a broken loop above cannot
        // pass by comparing an empty vector against itself.
        CHECK(out.value(2).season == -1);
        CHECK(out.value(2).episode == -1);
        CHECK(trakt::imdbStreamIdFor(out.value(0).showIds, out.value(0).season, out.value(0).episode)
              == QStringLiteral("tt1000001:1:4"));
        // ...and the entry that had no show imdb id is STILL not playable after a round trip. A cache
        // that invented an id here would make an unplayable row look playable only on offline launches.
        CHECK(trakt::imdbStreamIdFor(out.value(1).showIds, out.value(1).season, out.value(1).episode)
              .isEmpty());
        // An empty calendar is a legitimate answer ("nothing airs this week") and must survive the
        // round trip as empty rather than as a parse failure the caller would retry forever.
        CHECK(trakt::serializeCalendar({}).isEmpty() == false);
        CHECK(trakt::deserializeCalendar(trakt::serializeCalendar({})).isEmpty() == true);
    }

    // ---- 10. a CORRUPT cache yields empty, never a crash or a half-read entry -----------------
    // This input is a file on disk: a crash mid-write, a full disk or a hand-edited ini can truncate it
    // arbitrarily. Every shape below must land on the same empty vector the absent-cache case does, so
    // the caller simply re-fetches. A half-populated entry would instead be RENDERED as if it were real.
    {
        for (const char* bad : {
                 "",                                        // absent / never written
                 "   ",                                     // whitespace
                 "not json at all",                         // not JSON
                 "{",                                       // truncated at the first byte
                 R"({"v":1,"entries":[{"airsAtUtc":"2026-)", // truncated mid-entry
                 "null", "42", R"("a string")",             // valid JSON, wrong top-level type
                 R"([{"airsAtUtc":"2026-08-04T01:00:00.000Z"}])",       // a BARE array: not our shape
                 R"({"entries":[]})",                                    // version missing entirely
                 R"({"v":0,"entries":[]})",                              // version 0
                 R"({"v":2,"entries":[{"airsAtUtc":"2026-08-04T01:00:00.000Z"}]})", // a FUTURE version
                 R"({"v":"1","entries":[]})",                            // version as a string
                 R"({"v":1})",                                           // no entries key
                 R"({"v":1,"entries":{}})",                              // entries as an object
                 R"({"v":1,"entries":"nope"})",                          // entries as a string
             })
            CHECK(trakt::deserializeCalendar(QByteArray(bad)).isEmpty() == true);

        // A well-formed document whose ENTRIES are individually broken: each bad row is dropped and the
        // one good row survives. Same totality the wire parser has — one corrupt row must not cost the
        // user the rest of a cache that is otherwise fine.
        const char* mixed = R"({"v":1,"entries":[
          "a bare string",
          123,
          {},
          {"showTitle":"No date at all"},
          {"airsAtUtc":"","showTitle":"Empty date"},
          {"airsAtUtc":"not a date","showTitle":"Junk date"},
          {"airsAtUtc":"2026-08-04T01:00:00.000Z","showTitle":"Good",
           "showIds":{"imdb":"tt9"},"season":3,"episode":2}
        ]})";
        const QVector<CalendarEntry> e = trakt::deserializeCalendar(QByteArray(mixed));
        CHECK(e.size() == 1);
        CHECK(e.value(0).showTitle == QStringLiteral("Good"));
        CHECK(e.value(0).showIds.imdb == QStringLiteral("tt9"));
        CHECK(e.value(0).season == 3);
        CHECK(e.value(0).episode == 2);
        // Fields the row never carried come back as the documented absences, not as junk.
        CHECK(e.value(0).episodeTitle.isEmpty());
        CHECK(e.value(0).posterUrl.isEmpty());
        CHECK(e.value(0).episodeIds.imdb.isEmpty());

        // A row whose ids bag is the WRONG TYPE (a number where the cache always writes a string, or a
        // scalar where an object belongs) still yields an entry — with no ids, which imdbStreamIdFor
        // already reads as "not playable". This is the one place a lenient read could manufacture a
        // bogus id, so it is pinned.
        const char* wrongTypes = R"({"v":1,"entries":[
          {"airsAtUtc":"2026-08-04T01:00:00.000Z","showTitle":42,"showIds":7,
           "episodeIds":{"imdb":1234},"season":"3","episode":null}
        ]})";
        const QVector<CalendarEntry> w = trakt::deserializeCalendar(QByteArray(wrongTypes));
        CHECK(w.size() == 1);
        CHECK(w.value(0).showTitle.isEmpty());
        CHECK(w.value(0).showIds.imdb.isEmpty());
        CHECK(w.value(0).episodeIds.imdb.isEmpty());   // a NUMBER is not an id the cache ever wrote
        CHECK(w.value(0).season == -1);                // a string season is absent, not 3
        CHECK(w.value(0).episode == -1);
        CHECK(trakt::imdbStreamIdFor(w.value(0).showIds, w.value(0).season, w.value(0).episode).isEmpty());
    }

    // ---- 11. "is this a calendar at all" vs "is this calendar empty" -------------------------
    // parseMyShowsCalendar returns the same empty vector for a zero-row array and for a body that was
    // never a calendar. The fetch path OVERWRITES the offline cache with its result, so for it those
    // two must not be the same thing: a captive portal or corporate proxy answering with an HTTP 200
    // HTML interstitial produces no transport error, parses to nothing, and would blank a good cache
    // — the cache whose entire job is the offline launch. The discriminator is the ARRAY-NESS of the
    // body, never the row count, because a genuinely empty calendar must still be able to replace a
    // stale one or the user sees last month's forever.
    {
        // Rejected: not a JSON array, whatever else it may be.
        CHECK(trakt::looksLikeCalendarPayload(QByteArray(
            "<!DOCTYPE html><html><head><title>Sign in to the network</title></head>"
            "<body>Please accept the terms to continue.</body></html>")) == false);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray(R"({"error":"invalid_grant"})")) == false);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray(R"("just a string")")) == false);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray("{}")) == false);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray("null")) == false);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray("42")) == false);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray()) == false);          // empty body
        CHECK(trakt::looksLikeCalendarPayload(QByteArray("not json at all")) == false);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray("[{\"first_aired\":")) == false); // truncated

        // Accepted: a VALID EMPTY ARRAY is a real answer ("nothing airs this week"), and a populated
        // one obviously is. Both must be cacheable.
        CHECK(trakt::looksLikeCalendarPayload(QByteArray("[]")) == true);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray("  [ ]  ")) == true);
        CHECK(trakt::looksLikeCalendarPayload(QByteArray(kNormal)) == true);
        // An array of junk is still an array: the parser's totality drops the rows, and caching the
        // (empty) result of a body Trakt really did send is correct.
        CHECK(trakt::looksLikeCalendarPayload(QByteArray(R"(["nope", 1, null])")) == true);
        CHECK(trakt::parseMyShowsCalendar(QByteArray(R"(["nope", 1, null])")).isEmpty());

        // The property that makes it a discriminator at all: accepted-and-empty and rejected are
        // different states for the same empty parse result.
        CHECK(trakt::parseMyShowsCalendar(QByteArray("[]")).isEmpty()
              && trakt::parseMyShowsCalendar(QByteArray("{}")).isEmpty());
        CHECK(trakt::looksLikeCalendarPayload(QByteArray("[]"))
              != trakt::looksLikeCalendarPayload(QByteArray("{}")));
    }

    // ---- 12. the token-refresh single-flight queue --------------------------------------------
    // TraktClient::ensureValidToken must issue ONE /oauth/token refresh no matter how many callers
    // arrive while it is in flight: Trakt rotates the refresh token, so a second POST presents a token
    // the first already consumed, and interleaved replies can write the older pair over the newer one
    // — a permanently broken account link. The queue that guarantees it is SingleFlight, kept Qt-free
    // precisely so its awkward cases are reachable here with no socket.
    {
        // One starter, many joiners; everyone is answered exactly once with the same result.
        {
            SingleFlight sf;
            int calls = 0, trues = 0, starts = 0;
            auto w = [&](bool ok) { ++calls; if (ok) ++trues; };
            if (sf.join(w)) ++starts;
            if (sf.join(w)) ++starts;
            if (sf.join(w)) ++starts;
            CHECK(starts == 1);              // exactly one caller was told to make the request
            CHECK(sf.inFlight());
            CHECK(sf.waiting() == 3);
            CHECK(calls == 0);               // nobody is answered before the reply lands
            sf.settle(true);
            CHECK(calls == 3 && trues == 3); // ...and then everybody is, exactly once
            CHECK(!sf.inFlight());
            CHECK(sf.waiting() == 0);
        }

        // The failure path drains identically — a queue that only empties on success would strand
        // every joiner forever, which is worse than the duplicate request it replaced.
        {
            SingleFlight sf;
            int calls = 0, falses = 0;
            auto w = [&](bool ok) { ++calls; if (!ok) ++falses; };
            CHECK(sf.join(w) == true);
            CHECK(sf.join(w) == false);
            sf.settle(false);
            CHECK(calls == 2 && falses == 2);
            CHECK(!sf.inFlight() && sf.waiting() == 0);
        }

        // After settling, the NEXT caller starts a fresh operation rather than being told one is
        // already running — the flag must not latch.
        {
            SingleFlight sf;
            CHECK(sf.join([](bool) {}) == true);
            sf.settle(true);
            CHECK(sf.join([](bool) {}) == true);
            sf.settle(true);
            CHECK(!sf.inFlight());
        }

        // A waiter enqueued from INSIDE the fan-out — the case a naive drain gets wrong in both
        // directions at once. It must be told to start a fresh operation (not silently queued behind
        // one that already settled, where nothing would ever call it), and this drain must not also
        // call it. Exactly once, on the second settle.
        {
            SingleFlight sf;
            int lateCalls = 0, lateStarts = 0;
            auto late = [&](bool) { ++lateCalls; };
            sf.join([&](bool) { if (sf.join(late)) ++lateStarts; });
            sf.settle(true);
            CHECK(lateStarts == 1);     // told to start its own refresh
            CHECK(lateCalls == 0);      // NOT called by the drain it arrived during
            CHECK(sf.inFlight());       // the fresh operation is running
            CHECK(sf.waiting() == 1);
            sf.settle(true);
            CHECK(lateCalls == 1);      // called exactly once, by its own operation
            CHECK(!sf.inFlight() && sf.waiting() == 0);
        }

        // Degenerate calls must not crash or double-call: settling an idle queue, and a null waiter.
        {
            SingleFlight sf;
            sf.settle(true);            // nobody waiting
            CHECK(!sf.inFlight() && sf.waiting() == 0);
            int calls = 0;
            sf.join(SingleFlight::Waiter{});          // a default-constructed std::function
            sf.join([&](bool) { ++calls; });
            sf.settle(true);
            CHECK(calls == 1);
            sf.settle(true);            // a second settle on the same reply calls nobody again
            CHECK(calls == 1);
        }
    }

    // ---- 13. the OAuth token reply: never write an empty pair over a good one -----------------
    // The token-refresh handler used to parse its 200 straight into setTraktTokens(). A NoError reply
    // whose body is not a token reply — a TLS-intercepting proxy's page, a captive-portal
    // interstitial, a 200 that omits the token — parses to an EMPTY object, so that wrote
    // setTraktTokens("", "", exp): the access token AND the refresh token blanked, and the refresh
    // token is the only credential that can mint another. The account is unlinked permanently and the
    // user must re-link by hand.
    //
    // It is the same hazard §11 guards the CACHE against, one severity up: the cache's loss is
    // repaired by the next fetch and this one is not repairable at all. And the branch made it
    // unattended — a 30-minute top-up timer plus a startup fetch now drive refreshes at every expiry,
    // on whatever network the box is on, which is exactly the population that serves these bodies.
    //
    // valid=false is the "leave the store alone" answer, so it is what every hostile body must yield.
    {
        // --- bodies that are NOT a token reply. Each must be valid=false, and — the property that
        // actually protects the account — must carry no access token to write.
        const char* kNotTokens[] = {
            // The captive portal / intercepting proxy, verbatim in shape: HTTP 200, no transport error.
            "<!DOCTYPE html><html><head><title>Sign in to the network</title></head>"
            "<body>Please accept the terms to continue.</body></html>",
            R"({"error":"invalid_grant","error_description":"refresh token revoked"})", // no token key
            R"({"expires_in":7776000,"refresh_token":"rrr","created_at":1750000000})",  // omits access_token
            R"({"access_token":"","refresh_token":"rrr","expires_in":7776000})",        // PRESENT but EMPTY
            R"({"access_token":"   ","refresh_token":"rrr","expires_in":7776000})",     // whitespace only
            R"({"access_token":null,"expires_in":7776000})",                            // JSON null
            R"({"access_token":12345,"expires_in":7776000})",     // a NUMBER: toString() would give ""
            R"([{"access_token":"aaa","expires_in":7776000}])",   // right object, wrong top-level shape
            R"("just a string")",
            "null",
            "42",
            "[]",
            "",                                                  // empty body
            "not json at all",
            R"({"access_token":"aaa","expires_in":)",             // truncated mid-value
        };
        for (const char* body : kNotTokens)
        {
            const trakt::TokenReply t = trakt::parseTokenReply(QByteArray(body));
            CHECK(t.valid == false);
            CHECK(t.accessToken.isEmpty());     // nothing a caller could be tempted to store
            CHECK(t.refreshToken.isEmpty());
            CHECK(t.expiresInSec == 0);
        }

        // --- the expiry is part of the test. A reply without a usable one parses to an ALREADY
        // EXPIRED token, so the very next call re-refreshes — and each of those spends a rotated
        // refresh token. Rejecting costs one retried refresh; accepting costs the link.
        const char* kBadExpiry[] = {
            R"({"access_token":"aaa","refresh_token":"rrr"})",                    // absent
            R"({"access_token":"aaa","refresh_token":"rrr","expires_in":0})",     // zero
            R"({"access_token":"aaa","refresh_token":"rrr","expires_in":-1})",    // negative
            R"({"access_token":"aaa","refresh_token":"rrr","expires_in":null})",
            R"({"access_token":"aaa","refresh_token":"rrr","expires_in":"soon"})",// unparseable string
            // 1e18 seconds would OVERFLOW the `now + expires_in` the caller computes, which is
            // undefined — the range has to close here, while the value is still a double.
            R"({"access_token":"aaa","refresh_token":"rrr","expires_in":1e18})",
        };
        for (const char* body : kBadExpiry)
        {
            const trakt::TokenReply t = trakt::parseTokenReply(QByteArray(body));
            CHECK(t.valid == false);
            CHECK(t.accessToken.isEmpty());
        }

        // --- a well-formed reply: accepted, every field carried through intact.
        {
            const trakt::TokenReply t = trakt::parseTokenReply(QByteArray(
                R"({"access_token":"acc-1","token_type":"bearer","expires_in":7776000,)"
                R"("refresh_token":"ref-2","scope":"public","created_at":1750000000})"));
            CHECK(t.valid == true);
            CHECK(t.accessToken == QStringLiteral("acc-1"));
            CHECK(t.refreshToken == QStringLiteral("ref-2"));
            CHECK(t.expiresInSec == 7776000);
            CHECK(t.createdAtUnix == 1750000000);
        }
        // created_at is optional — its absence is not a rejection, it just means the caller falls
        // back to the local clock for the issue time.
        {
            const trakt::TokenReply t = trakt::parseTokenReply(
                QByteArray(R"({"access_token":"acc-1","refresh_token":"ref-2","expires_in":90})"));
            CHECK(t.valid == true);
            CHECK(t.createdAtUnix == 0);
            CHECK(t.expiresInSec == 90);
        }
        // created_at is an ABSOLUTE unix timestamp, expires_in is a DURATION, and they must not share
        // a bound — an earlier draft of this parser capped both at ten years and silently dropped
        // created_at from EVERY real reply (1.75e9 seconds), which the device flow uses as its issue
        // time. Pinned with a present-day timestamp so the two bounds can never be merged again.
        {
            const trakt::TokenReply t = trakt::parseTokenReply(
                QByteArray(R"({"access_token":"acc-1","expires_in":7776000,"created_at":1753574400})"));
            CHECK(t.valid == true);
            CHECK(t.createdAtUnix == 1753574400);          // NOT rejected as an out-of-range duration
            CHECK(t.expiresInSec == 7776000);
            // ...while a timestamp-sized value in expires_in IS still rejected: 1.75e9 seconds is 55
            // years, not an access-token lifetime, so the duration bound must still bite.
            CHECK(trakt::parseTokenReply(
                      QByteArray(R"({"access_token":"acc-1","expires_in":1753574400})")).valid == false);
        }

        // expires_in as a STRING is tolerated — strictness is about the TOKEN, not the encoding.
        {
            const trakt::TokenReply t = trakt::parseTokenReply(
                QByteArray(R"({"access_token":"acc-1","expires_in":"7776000"})"));
            CHECK(t.valid == true);
            CHECK(t.expiresInSec == 7776000);
        }

        // --- the refresh token: "" is REPORTED, never a rejection, and never something to store.
        // Trakt rotates refresh tokens, so writing "" is as fatal as writing an empty access token.
        // The parser's job is to say the reply carried none; the CALLER keeps the one it already has
        // (TraktClient.cpp), which is strictly better than rejecting — rejecting would also discard a
        // perfectly good access token and leave the link no less broken.
        {
            const char* kNoRefresh[] = {
                R"({"access_token":"acc-1","expires_in":7776000})",                     // absent
                R"({"access_token":"acc-1","refresh_token":"","expires_in":7776000})",  // empty string
                R"({"access_token":"acc-1","refresh_token":null,"expires_in":7776000})",
                R"({"access_token":"acc-1","refresh_token":99,"expires_in":7776000})",  // a number
            };
            for (const char* body : kNoRefresh)
            {
                const trakt::TokenReply t = trakt::parseTokenReply(QByteArray(body));
                CHECK(t.valid == true);                    // the ACCESS token is what makes it valid
                CHECK(t.accessToken == QStringLiteral("acc-1"));
                CHECK(t.refreshToken.isEmpty());           // "carried none" — the caller preserves
            }
        }

        // --- the discriminator property, stated directly: the empty-token body and the good one are
        // the SAME HTTP 200 with the same transport result, and only this predicate separates them.
        CHECK(trakt::parseTokenReply(QByteArray(R"({"access_token":"","expires_in":1})")).valid
              != trakt::parseTokenReply(QByteArray(R"({"access_token":"a","expires_in":1})")).valid);
        // Tokens are trimmed, not rejected, for incidental whitespace — a value that is ONLY
        // whitespace is empty and was rejected above.
        {
            const trakt::TokenReply t = trakt::parseTokenReply(
                QByteArray("{\"access_token\":\" acc-1 \",\"refresh_token\":\"\\tref-2\\n\",\"expires_in\":5}"));
            CHECK(t.valid == true);
            CHECK(t.accessToken == QStringLiteral("acc-1"));
            CHECK(t.refreshToken == QStringLiteral("ref-2"));
        }
    }

    if (failures == 0) { std::puts("TRAKT-OK"); return 0; }
    std::fprintf(stderr, "TRAKT: %d check(s) failed\n", failures);
    return 1;
}
