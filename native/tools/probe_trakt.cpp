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
#include "TraktSync.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QMap>
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

    // =============================================================================================
    // §14-§21 — the SECOND read slice (#23): the watchlist/collection lists, and the watched-history
    // backfill. Everything below is TraktSync, and every one of these cases is reachable ONLY here:
    // a rate-limited page, a run that dies halfway, and "the user and Trakt disagree about whether
    // this was watched" cannot be produced against a live account on demand.
    // =============================================================================================

    // ---- 14. the list parser (/sync/watchlist and /sync/collection share one) -------------------
    {
        // A WATCHLIST batch: rows carry an explicit `type`. The third row is an EPISODE entry — which
        // Trakt really does return from this endpoint, and which carries a `show` object of its own.
        const char* watchlist = R"([
          { "rank": 1, "listed_at": "2026-01-02T00:00:00.000Z", "type": "movie",
            "movie": { "title": "Movie One", "year": 2019,
                       "ids": { "trakt": 7, "slug": "m1", "imdb": "tt3000001", "tmdb": 8 } } },
          { "rank": 2, "listed_at": "2026-01-03T00:00:00.000Z", "type": "show",
            "show": { "title": "Show One", "year": 2021,
                      "ids": { "trakt": 9, "slug": "s1", "imdb": "tt4000001", "tvdb": 10 } } },
          { "rank": 3, "listed_at": "2026-01-04T00:00:00.000Z", "type": "episode",
            "episode": { "season": 2, "number": 5, "ids": { "imdb": "tt5000001" } },
            "show": { "title": "Show Two", "ids": { "imdb": "tt6000001" } } }
        ])";
        const QVector<TraktListEntry> e = trakt::parseListPayload(QByteArray(watchlist));
        // The episode row is DROPPED. It is the precedence rule made visible: it carries a perfectly
        // good `show` object, so a parser that inferred the kind from the nested objects instead of
        // reading `type` would silently turn "I want to watch season 2, episode 5" into "the whole of
        // Show Two is on my watchlist" — under the show's own ids, indistinguishable from a real row.
        CHECK(e.size() == 2);
        CHECK(e.value(0).type == QStringLiteral("movie"));
        CHECK(e.value(0).title == QStringLiteral("Movie One"));
        CHECK(e.value(0).year == 2019);
        CHECK(e.value(0).ids.imdb == QStringLiteral("tt3000001"));
        CHECK(e.value(0).ids.tmdb == QStringLiteral("8"));      // a NUMBER on the wire, kept as a string
        CHECK(e.value(1).type == QStringLiteral("show"));
        CHECK(e.value(1).title == QStringLiteral("Show One"));
        CHECK(e.value(1).ids.imdb == QStringLiteral("tt4000001"));
        // Nothing in the result came from the episode row — stated directly rather than inferred from
        // the count, so a parser that dropped a DIFFERENT row and kept this one still fails.
        for (const TraktListEntry& x : e) CHECK(x.ids.imdb != QStringLiteral("tt6000001"));
        // listed_at is read, and read as UTC seconds.
        CHECK(e.value(0).addedAt == 1767312000);   // 2026-01-02T00:00:00Z
        CHECK(e.value(1).addedAt == 1767398400);   // 2026-01-03T00:00:00Z
    }
    {
        // A COLLECTION batch: /sync/collection/{movies,shows} sends NO `type` at all, so the kind has
        // to be inferred — the other half of the precedence rule. Also pins the two other timestamp
        // field names, which is the only per-endpoint difference in the shape.
        const char* collection = R"([
          { "collected_at": "2026-02-01T00:00:00.000Z",
            "movie": { "title": "Owned Movie", "year": 2001, "ids": { "imdb": "tt7000001" } } },
          { "last_collected_at": "2026-02-02T00:00:00.000Z",
            "show": { "title": "Owned Show", "ids": { "imdb": "tt8000001" } },
            "seasons": [ { "number": 1, "episodes": [ { "number": 1 } ] } ] }
        ])";
        const QVector<TraktListEntry> e = trakt::parseListPayload(QByteArray(collection));
        CHECK(e.size() == 2);
        CHECK(e.value(0).type == QStringLiteral("movie"));
        CHECK(e.value(0).addedAt == 1769904000);   // collected_at
        CHECK(e.value(1).type == QStringLiteral("show"));
        CHECK(e.value(1).addedAt == 1769990400);   // last_collected_at
    }
    {
        // The fallback CHAIN and the pre-epoch squash, together — they interact, and only together do
        // they discriminate. A timestamp before 1970 is "no timestamp", not a negative one: if it were
        // passed through as a negative number, the `== 0` fallback below would never fire, the row
        // would keep a nonsense sort key, and the perfectly good collected_at beside it would be
        // ignored. Squashing at the one place that reads a Trakt time is what makes the chain work.
        const char* preEpoch = R"([
          { "listed_at": "1960-01-01T00:00:00.000Z",
            "collected_at": "2026-02-01T00:00:00.000Z",
            "movie": { "title": "Old stamp", "ids": { "imdb": "tt7100001" } } }
        ])";
        const QVector<TraktListEntry> e = trakt::parseListPayload(QByteArray(preEpoch));
        CHECK(e.size() == 1);
        CHECK(e.value(0).addedAt == 1769904000);   // the collected_at won
        CHECK(e.value(0).addedAt > 0);
    }
    {
        // TOTALITY, on the same contract as the calendar parser: a malformed row costs only itself.
        const char* mixed = R"([
          { "type": "movie", "movie": { "title": "Keep", "ids": { "imdb": "tt1" } } },
          "a bare string where an object belongs",
          { "type": "movie" },
          { "type": "person", "person": { "name": "Someone" } },
          { "type": "show", "show": { "title": "", "ids": {} } },
          { "type": "movie", "movie": { "title": "", "ids": { "imdb": "tt2" } } },
          { "type": "show", "show": { "title": "Also keep", "ids": {} } }
        ])";
        const QVector<TraktListEntry> e = trakt::parseListPayload(QByteArray(mixed));
        // Kept: "Keep", the titleless-but-identifiable tt2, and "Also keep".
        // Dropped: the bare string; the `type:"movie"` with no `movie` object; the person row; and the
        // row with NEITHER a title NOR an id, which is a tile a user could not recognise or open.
        CHECK(e.size() == 3);
        CHECK(e.value(0).title == QStringLiteral("Keep"));
        // The titleless row survives BECAUSE it has an id. This is the half that discriminates the drop
        // rule: a rule that dropped a row missing EITHER (rather than BOTH) would lose it, and losing a
        // watchlist entry that the app can actually resolve is the expensive direction of that mistake.
        CHECK(e.value(1).title.isEmpty());
        CHECK(e.value(1).ids.imdb == QStringLiteral("tt2"));
        CHECK(e.value(2).title == QStringLiteral("Also keep"));
        // ...and the mirror: an id-less, title-less row is NOT among them.
        for (const TraktListEntry& x : e)
            CHECK(!(x.title.isEmpty() && x.ids.imdb.isEmpty()));
    }
    {
        // Non-array / non-JSON input is empty, never a half-read.
        CHECK(trakt::parseListPayload(QByteArray("{\"error\":\"nope\"}")).isEmpty());
        CHECK(trakt::parseListPayload(QByteArray("<html>captive portal</html>")).isEmpty());
        CHECK(trakt::parseListPayload(QByteArray()).isEmpty());

        // The cache-overwrite discriminator, exactly as for the calendar: array-ness, NOT row count.
        // An EMPTIED watchlist is a real answer and must be able to replace a stale cache.
        CHECK(trakt::looksLikeListPayload(QByteArray("[]")) == true);
        CHECK(trakt::looksLikeListPayload(QByteArray("[{\"type\":\"movie\"}]")) == true);
        CHECK(trakt::looksLikeListPayload(QByteArray("{\"error\":\"nope\"}")) == false);
        CHECK(trakt::looksLikeListPayload(QByteArray("<html>captive portal</html>")) == false);
        CHECK(trakt::looksLikeListPayload(QByteArray()) == false);
        // Stated as the property rather than as four unrelated facts: the empty list and the HTML page
        // are the SAME HTTP 200 with the same transport result, and only this predicate separates them.
        CHECK(trakt::looksLikeListPayload(QByteArray("[]"))
              != trakt::looksLikeListPayload(QByteArray("<html>captive portal</html>")));
    }

    // ---- 15. the list cache round trip ----------------------------------------------------------
    {
        QVector<TraktListEntry> in;
        TraktListEntry a; a.type = QStringLiteral("movie"); a.title = QStringLiteral("Cached Movie");
        a.year = 1999; a.addedAt = 1700000000;
        a.ids.imdb = QStringLiteral("tt9000001"); a.ids.tmdb = QStringLiteral("41");
        a.ids.tvdb = QStringLiteral("42");        a.ids.trakt = QStringLiteral("43");
        TraktListEntry b; b.type = QStringLiteral("show"); b.title = QStringLiteral("Cached Show");
        b.year = 2024; b.addedAt = 1700000001; b.ids.imdb = QStringLiteral("tt9000002");
        in << a << b;

        const QVector<TraktListEntry> out = trakt::deserializeList(trakt::serializeList(in));
        CHECK(out.size() == 2);
        // Every field, and every field set to something that is NOT its default — a round trip over
        // default-valued fields is a fixed point and would pass however badly the writer was mangled.
        CHECK(out.value(0).type == QStringLiteral("movie"));
        CHECK(out.value(0).title == QStringLiteral("Cached Movie"));
        CHECK(out.value(0).year == 1999);
        CHECK(out.value(0).addedAt == 1700000000);
        CHECK(out.value(0).ids.imdb == QStringLiteral("tt9000001"));
        CHECK(out.value(0).ids.tmdb == QStringLiteral("41"));
        CHECK(out.value(0).ids.tvdb == QStringLiteral("42"));
        CHECK(out.value(0).ids.trakt == QStringLiteral("43"));
        CHECK(out.value(1).type == QStringLiteral("show"));
        CHECK(out.value(1).title == QStringLiteral("Cached Show"));
        CHECK(out.value(1).year == 2024);
        CHECK(out.value(1).addedAt == 1700000001);

        // TOTAL on read: a file a crash truncated, a hand edit, or a format from a future build.
        CHECK(trakt::deserializeList(QByteArray()).isEmpty());
        CHECK(trakt::deserializeList(QByteArray("{\"v\":1,\"entries\":[")).isEmpty());
        CHECK(trakt::deserializeList(QByteArray("[]")).isEmpty());                       // a bare array
        // NO version at all. The fixture carries a REAL entry on purpose: with an empty entries array
        // the assertion holds however the version is defaulted, which would make it inert against the
        // one mistake it exists to catch — reading a missing version as the current one.
        CHECK(trakt::deserializeList(
                  QByteArray(R"({"entries":[{"type":"movie","title":"X"}]})")).isEmpty());
        CHECK(trakt::deserializeList(QByteArray("{\"v\":2,\"entries\":[]}")).isEmpty()); // a later one
        CHECK(trakt::deserializeList(QByteArray("{\"v\":1,\"entries\":{}}")).isEmpty()); // wrong shape
        // A cache carrying a row type no surface renders is filtered on READ too, not only on parse —
        // otherwise a hand-edited or future-written file smuggles one past the wire parser's rule.
        CHECK(trakt::deserializeList(
                  QByteArray(R"({"v":1,"entries":[{"type":"person","title":"Someone"}]})")).isEmpty());
        CHECK(trakt::deserializeList(
                  QByteArray(R"({"v":1,"entries":[{"type":"movie","title":"Fine"}]})")).size() == 1);
    }

    // ---- 16. the watched-history parser ---------------------------------------------------------
    {
        // /sync/watched/movies: one mark per row, keyed on the movie's own IMDB id.
        const char* movies = R"([
          { "plays": 4, "last_watched_at": "2026-03-01T12:00:00.000Z",
            "movie": { "title": "Seen", "ids": { "imdb": "tt100", "tmdb": 5 } } },
          { "plays": 1, "last_watched_at": "2026-03-02T12:00:00.000Z",
            "movie": { "title": "No imdb", "ids": { "tmdb": 6 } } },
          { "plays": 1, "movie": { "title": "No timestamp", "ids": { "imdb": "tt101" } } },
          { "plays": 1, "last_watched_at": "1960-01-01T00:00:00.000Z",
            "movie": { "title": "Before the epoch", "ids": { "imdb": "tt102" } } }
        ])";
        const trakt::WatchedParse w = trakt::parseWatchedPayload(QByteArray(movies));
        CHECK(w.marks.size() == 1);
        CHECK(w.marks.value(0).streamId == QStringLiteral("tt100"));
        CHECK(w.marks.value(0).lastWatchedAt == 1772366400);   // 2026-03-01T12:00:00Z
        // The two drop causes are counted SEPARATELY, because they mean different things to a user:
        // one is "Trakt has no IMDB id for this", the other is "Trakt sent no watch time".
        CHECK(w.droppedNoKey == 1);
        // A pre-1970 stamp is "no watch time", not a negative one. It has to be squashed HERE, at the
        // one place that reads a Trakt timestamp: a negative value flowing onward compares as older
        // than every watermark for ever, so it would be a mark that silently never imports and never
        // reports itself either.
        CHECK(w.droppedNoTimestamp == 2);
        for (const trakt::WatchedMark& m : w.marks) CHECK(m.lastWatchedAt > 0);
    }
    {
        // /sync/watched/shows: one mark per EPISODE, and the rule that matters most in this file —
        // an episode with no `last_watched_at` does NOT inherit the show's.
        //
        // The show-level stamp here (2026-06-01) is deliberately NEWER than either episode's. If it
        // were inherited, S1E2 would arrive carrying it, and it would come back over the watermark on
        // every later run — re-marking itself for ever, reverting whatever the user did to it. That is
        // the #58 failure mode with a different name, so the fixture is built to expose it: the show
        // stamp is a value no correct result may contain.
        const char* shows = R"([
          { "plays": 9, "last_watched_at": "2026-06-01T00:00:00.000Z",
            "show": { "title": "Watched Show", "ids": { "imdb": "tt200", "tvdb": 77 } },
            "seasons": [
              { "number": 1, "episodes": [
                  { "number": 1, "plays": 1, "last_watched_at": "2026-05-01T00:00:00.000Z" },
                  { "number": 2, "plays": 1 } ] },
              { "number": 0, "episodes": [
                  { "number": 1, "plays": 1, "last_watched_at": "2026-05-02T00:00:00.000Z" } ] } ] },
          { "plays": 3, "last_watched_at": "2026-06-01T00:00:00.000Z",
            "show": { "title": "No imdb show", "ids": { "tvdb": 78 } },
            "seasons": [ { "number": 1, "episodes": [
                  { "number": 1, "last_watched_at": "2026-05-03T00:00:00.000Z" } ] } ] }
        ])";
        const trakt::WatchedParse w = trakt::parseWatchedPayload(QByteArray(shows));
        CHECK(w.marks.size() == 2);
        CHECK(w.marks.value(0).streamId == QStringLiteral("tt200:1:1"));
        CHECK(w.marks.value(0).lastWatchedAt == 1777593600);   // the EPISODE's 2026-05-01, not the show's
        // Season 0 is Trakt's specials season and is a VALID key — the sentinel for "missing" is -1
        // precisely so that a real special is not confused with one.
        CHECK(w.marks.value(1).streamId == QStringLiteral("tt200:0:1"));
        CHECK(w.marks.value(1).lastWatchedAt == 1777680000);   // 2026-05-02
        // S1E2 is dropped and COUNTED, and the show-level stamp appears nowhere in the result.
        CHECK(w.droppedNoTimestamp == 1);
        for (const trakt::WatchedMark& m : w.marks)
        {
            CHECK(m.streamId != QStringLiteral("tt200:1:2"));
            CHECK(m.lastWatchedAt != 1780272000);   // 2026-06-01T00:00:00Z — the show-level stamp
        }
        // The id-less show contributed one unmappable EPISODE, not one unmappable show: the count is
        // per mark that could have existed, which is what the run reports to the user.
        CHECK(w.droppedNoKey == 1);
    }
    {
        // Structural totality, and the season/episode range rules, which reach droppedNoKey through
        // the SAME mapping the calendar uses rather than a second copy of it.
        const char* odd = R"([
          "not an object",
          { "plays": 1 },
          { "last_watched_at": "2026-05-01T00:00:00.000Z",
            "show": { "ids": { "imdb": "tt300" } },
            "seasons": [
              { "episodes": [ { "number": 1, "last_watched_at": "2026-05-01T00:00:00.000Z" } ] },
              { "number": 1, "episodes": [ { "last_watched_at": "2026-05-01T00:00:00.000Z" },
                                           { "number": 0, "last_watched_at": "2026-05-01T00:00:00.000Z" },
                                           { "number": 3, "last_watched_at": "2026-05-01T00:00:00.000Z" } ] } ] }
        ])";
        const trakt::WatchedParse w = trakt::parseWatchedPayload(QByteArray(odd));
        // Only S1E3 maps: a season with no `number` reads as the -1 sentinel; an episode with no
        // `number` likewise; and episode 0 is out of range because episodes are 1-based.
        CHECK(w.marks.size() == 1);
        CHECK(w.marks.value(0).streamId == QStringLiteral("tt300:1:3"));
        CHECK(w.droppedNoKey == 3);
        CHECK(w.droppedNoTimestamp == 0);
        CHECK(trakt::parseWatchedPayload(QByteArray("{\"error\":1}")).marks.isEmpty());
        CHECK(trakt::parseWatchedPayload(QByteArray("<html>")).marks.isEmpty());
    }

    // ---- 17. the reconciliation: which bucket every mark lands in -------------------------------
    {
        QVector<trakt::WatchedMark> marks;
        marks << trakt::WatchedMark{ QStringLiteral("tt-new"),   500 }   // -> toMark
              << trakt::WatchedMark{ QStringLiteral("tt-seen"),  500 }   // local already watched
              << trakt::WatchedMark{ QStringLiteral("tt-mine"),  500 }   // local says something else
              << trakt::WatchedMark{ QStringLiteral("tt-old"),   100 }   // under the watermark
              << trakt::WatchedMark{ QStringLiteral("tt-new"),   400 }   // a duplicate id
              << trakt::WatchedMark{ QString(),                  500 }   // no id
              << trakt::WatchedMark{ QStringLiteral("tt-zero"),    0 };  // no timestamp

        int localCalls = 0;
        const auto local = [&localCalls](const QString& id) {
            ++localCalls;
            if (id == QStringLiteral("tt-seen")) return trakt::LocalState::Watched;
            if (id == QStringLiteral("tt-mine")) return trakt::LocalState::OtherExplicit;
            return trakt::LocalState::Unmarked;
        };
        const trakt::BackfillPlan p = trakt::planWatchedBackfill(marks, /*watermark*/ 200, local);

        CHECK(p.toMark.size() == 1);
        CHECK(p.toMark.value(0) == QStringLiteral("tt-new"));
        CHECK(p.alreadyWatched == 1);       // no write at all: a redundant write re-arms the Drive push
        CHECK(p.keptLocal == 1);            // "in progress"/"abandoned"/"planned" is the user talking
        CHECK(p.skippedByWatermark == 1);
        CHECK(p.duplicates == 1);
        CHECK(p.unusable == 2);
        // Every mark handed in is in exactly one bucket. A bucket quietly missing an entry is how an
        // import comes to report that it did more than it did.
        CHECK(p.toMark.size() + p.alreadyWatched + p.keptLocal + p.skippedByWatermark
              + p.duplicates + p.unusable == marks.size());
        // The store is asked only about ELIGIBLE, first-seen marks — never about one the watermark
        // already excluded, and never twice about the same id.
        CHECK(localCalls == 3);
        // The watermark advances over what the run OBSERVED, not over what it MARKED. tt-old at 100 was
        // observed and skipped; the max is still 500. (This is the step the convergence argument in
        // TraktSync.h turns on, so it is asserted on its own before §18 exercises it.)
        CHECK(p.newWatermark == 500);
        // ...and it is NOT the max of the marked set, which would be the same 500 here — so the case
        // that separates them is stated directly: a run whose ONLY newest entry is one it did not mark.
        {
            QVector<trakt::WatchedMark> obs;
            obs << trakt::WatchedMark{ QStringLiteral("tt-a"), 300 }
                << trakt::WatchedMark{ QStringLiteral("tt-b"), 900 };
            const trakt::BackfillPlan q = trakt::planWatchedBackfill(
                obs, 0, [](const QString& id) {
                    return id == QStringLiteral("tt-b") ? trakt::LocalState::Watched
                                                        : trakt::LocalState::Unmarked; });
            CHECK(q.toMark.size() == 1);                 // only tt-a is marked
            CHECK(q.newWatermark == 900);                // but the watermark still reaches tt-b
        }
        // `complete` is the CALLER's word, never the planner's: the planner cannot see a missed page.
        // Its default is the safe one, so a caller that forgets to set it reports an incomplete run.
        CHECK(p.complete == false);
    }

    // ---- 18. repeated runs CONVERGE: the strict comparison, and the unmark that sticks ----------
    {
        // A tiny stand-in for the marks store, so a run can be applied and the next one can see it.
        struct FakeStore
        {
            QMap<QString, trakt::LocalState> st;
            trakt::LocalState get(const QString& k) const
            { return st.value(k, trakt::LocalState::Unmarked); }
            void apply(const trakt::BackfillPlan& p)
            { for (const QString& id : p.toMark) st[id] = trakt::LocalState::Watched; }
        };

        QVector<trakt::WatchedMark> history;
        history << trakt::WatchedMark{ QStringLiteral("ttA"), 100 }
                << trakt::WatchedMark{ QStringLiteral("ttB"), 200 };   // the NEWEST entry

        FakeStore store;
        qint64 watermark = 0;
        const auto lookup = [&store](const QString& id) { return store.get(id); };

        // Run 1: a fresh link. Everything is imported and the watermark reaches the newest entry.
        {
            trakt::BackfillPlan p = trakt::planWatchedBackfill(history, watermark, lookup);
            p.complete = true;
            store.apply(p);
            watermark = p.newWatermark;
            CHECK(p.toMark.size() == 2);
            CHECK(watermark == 200);
        }

        // The user now says "no, I have NOT watched ttB" — and ttB is the NEWEST entry, i.e. exactly
        // the one whose last_watched_at equals the watermark. That is not a corner case: it is
        // whichever episode they watched most recently, which is the one they are most likely to be
        // correcting.
        store.st[QStringLiteral("ttB")] = trakt::LocalState::Unmarked;

        // Run 2 and Run 3: the unmark STICKS, and keeps sticking. With a non-strict comparison ttB
        // would be eligible on every single run and be re-marked every single time — the user's edit
        // reverted for ever, silently. Nothing else about the input changed.
        for (int run = 0; run < 2; ++run)
        {
            trakt::BackfillPlan p = trakt::planWatchedBackfill(history, watermark, lookup);
            p.complete = true;
            CHECK(p.toMark.isEmpty());
            CHECK(p.skippedByWatermark == 2);
            store.apply(p);
            watermark = p.newWatermark;
            CHECK(watermark == 200);                                     // stable, run after run
            CHECK(store.get(QStringLiteral("ttB")) == trakt::LocalState::Unmarked);
            CHECK(store.get(QStringLiteral("ttA")) == trakt::LocalState::Watched);
        }

        // ...and the mirror, so this is convergence rather than paralysis: a genuine RE-WATCH moves
        // last_watched_at past the watermark, and the import picks it up again.
        history[1].lastWatchedAt = 300;
        {
            const trakt::BackfillPlan p = trakt::planWatchedBackfill(history, watermark, lookup);
            CHECK(p.toMark.size() == 1);
            CHECK(p.toMark.value(0) == QStringLiteral("ttB"));
            CHECK(p.newWatermark == 300);
        }
    }

    // ---- 19. a PARTIAL run loses nothing --------------------------------------------------------
    {
        // Two pages. The second one fails, so the run is incomplete and the watermark is NOT stored.
        // Both entries share a last_watched_at ON PURPOSE: if a partial run were allowed to advance
        // the watermark to what it managed to see, the entry on the page it never read would be
        // exactly equal to it, would fail the strict comparison for ever, and would be lost silently —
        // the user would simply never learn that half their history did not import.
        QVector<trakt::WatchedMark> page1, page2;
        page1 << trakt::WatchedMark{ QStringLiteral("ttP1"), 100 };
        page2 << trakt::WatchedMark{ QStringLiteral("ttP2"), 100 };

        QMap<QString, trakt::LocalState> store;
        const auto lookup = [&store](const QString& id)
        { return store.value(id, trakt::LocalState::Unmarked); };
        qint64 watermark = 0;

        // The failed run: page 1 arrived, page 2 did not. Whatever it managed to apply STAYS applied —
        // each write is idempotent, so there is nothing to roll back — but complete is false.
        {
            trakt::BackfillPlan p = trakt::planWatchedBackfill(page1, watermark, lookup);
            p.complete = false;                       // the fetch loop's verdict, not the planner's
            for (const QString& id : p.toMark) store[id] = trakt::LocalState::Watched;
            CHECK(p.toMark.size() == 1);
            CHECK(p.newWatermark == 100);             // it computed one...
            if (p.complete) watermark = p.newWatermark;
            CHECK(watermark == 0);                    // ...and the caller did NOT store it
        }

        // The retry replays BOTH pages from the start. Page 1 costs nothing (already watched, so no
        // write and no sync churn); page 2 is imported, which is the whole point.
        {
            QVector<trakt::WatchedMark> both = page1 + page2;
            trakt::BackfillPlan p = trakt::planWatchedBackfill(both, watermark, lookup);
            p.complete = true;
            for (const QString& id : p.toMark) store[id] = trakt::LocalState::Watched;
            if (p.complete) watermark = p.newWatermark;
            CHECK(p.alreadyWatched == 1);
            CHECK(p.toMark.size() == 1);
            CHECK(p.toMark.value(0) == QStringLiteral("ttP2"));
            CHECK(watermark == 100);
        }
        CHECK(store.value(QStringLiteral("ttP1")) == trakt::LocalState::Watched);
        CHECK(store.value(QStringLiteral("ttP2")) == trakt::LocalState::Watched);
    }

    // ---- 20. paging -----------------------------------------------------------------------------
    {
        QMap<QString, QString> h;
        h.insert(QStringLiteral("x-pagination-page"), QStringLiteral("2"));
        h.insert(QStringLiteral("x-pagination-page-count"), QStringLiteral("5"));
        h.insert(QStringLiteral("x-pagination-item-count"), QStringLiteral("437"));
        const trakt::PageInfo i = trakt::parsePageInfo(h);
        CHECK(i.page == 2);
        CHECK(i.pageCount == 5);
        CHECK(i.itemCount == 437);

        // Absent headers: counts read as 0, and itemCount stays at its "unknown" -1 rather than
        // collapsing onto 0, which would let a run report "0 items" when it simply was not told.
        const trakt::PageInfo none = trakt::parsePageInfo({});
        CHECK(none.page == 0);
        CHECK(none.pageCount == 0);
        CHECK(none.itemCount == -1);

        // A value that is not a non-negative integer is not a count.
        QMap<QString, QString> bad;
        bad.insert(QStringLiteral("x-pagination-page-count"), QStringLiteral("many"));
        bad.insert(QStringLiteral("x-pagination-item-count"), QStringLiteral("-3"));
        CHECK(trakt::parsePageInfo(bad).pageCount == 0);
        CHECK(trakt::parsePageInfo(bad).itemCount == -1);
        // ...but incidental whitespace from a proxy is tolerated.
        QMap<QString, QString> pad;
        pad.insert(QStringLiteral("x-pagination-page-count"), QStringLiteral(" 3 "));
        CHECK(trakt::parsePageInfo(pad).pageCount == 3);
    }
    {
        trakt::PageInfo i; i.page = 1; i.pageCount = 3;
        CHECK(trakt::nextPageAfter(i, 1) == 2);
        // THE echo test. The server says it sent page 1; we know we asked for page 2. The decision
        // uses OUR number: a server that echoes "1" for every page would otherwise hold the loop on
        // page 1 until the outright bound, re-importing it and never reaching page 3.
        CHECK(trakt::nextPageAfter(i, 2) == 3);
        CHECK(trakt::nextPageAfter(i, 3) == 0);          // the last page: the run is DONE
        CHECK(trakt::nextPageAfter(i, 4) == 0);          // past the end, defensively

        // No pagination headers => the endpoint answered in one body (the /sync/watched shape). That
        // is a COMPLETE run, not a broken one — asking for page 2 would restart the whole import.
        trakt::PageInfo unpaged;
        CHECK(trakt::nextPageAfter(unpaged, 1) == 0);

        // The outright bound. A `page_count` of a billion — hostile, or a bug at the other end — costs
        // kMaxPages requests, not an unbounded run that rate-limits the account into the ground.
        trakt::PageInfo huge; huge.pageCount = 1000000000;
        CHECK(trakt::nextPageAfter(huge, trakt::kMaxPages - 1) == trakt::kMaxPages);
        CHECK(trakt::nextPageAfter(huge, trakt::kMaxPages) == 0);

        // Nonsense in: stop, rather than invent a page 1 and start a run nobody asked for.
        CHECK(trakt::nextPageAfter(i, 0) == 0);
        CHECK(trakt::nextPageAfter(i, -7) == 0);
    }

    // ---- 21. rate limits, failures, and backoff --------------------------------------------------
    {
        const QByteArray arr("[]");
        const QByteArray html("<html>Sign in to the hotel wifi</html>");

        CHECK(trakt::classifyPage(200, {}, arr).outcome == trakt::PageOutcome::Ok);
        // A 200 that is not the payload — a captive portal, a TLS-intercepting proxy's error page.
        // Malformed, and deliberately NOT Retryable: the transport SUCCEEDED, so asking again returns
        // the same page and would burn the whole attempt budget before failing anyway.
        CHECK(trakt::classifyPage(200, {}, html).outcome == trakt::PageOutcome::Malformed);
        CHECK(trakt::classifyPage(200, {}, QByteArray()).outcome == trakt::PageOutcome::Malformed);

        // 429 with a server hint, honoured...
        QMap<QString, QString> ra;
        ra.insert(QStringLiteral("retry-after"), QStringLiteral("30"));
        CHECK(trakt::classifyPage(429, ra, arr).outcome == trakt::PageOutcome::Retryable);
        CHECK(trakt::classifyPage(429, ra, arr).retryAfterSec == 30);
        // ...but CLAMPED. A proxy answering "Retry-After: 86400" must not park a background import on
        // a day-long timer inside a running app.
        QMap<QString, QString> huge;
        huge.insert(QStringLiteral("retry-after"), QStringLiteral("86400"));
        CHECK(trakt::classifyPage(429, huge, arr).retryAfterSec == trakt::kMaxBackoffSec);
        // A "0" is no hint at all, not a wait of zero — a zero would spin the loop.
        QMap<QString, QString> zero;
        zero.insert(QStringLiteral("retry-after"), QStringLiteral("0"));
        CHECK(trakt::classifyPage(429, zero, arr).retryAfterSec == 0);
        // A date-form Retry-After (HTTP allows it) is not an integer; it reads as no hint rather than
        // as some accidental number.
        QMap<QString, QString> dated;
        dated.insert(QStringLiteral("retry-after"), QStringLiteral("Wed, 21 Oct 2026 07:28:00 GMT"));
        CHECK(trakt::classifyPage(429, dated, arr).retryAfterSec == 0);
        CHECK(trakt::classifyPage(429, {}, arr).retryAfterSec == 0);
        // A 429 is Retryable WHATEVER the body is: a rate-limit reply is an error page, not an array,
        // and classifying it on the body would turn every rate limit into an unretryable Malformed.
        CHECK(trakt::classifyPage(429, ra, html).outcome == trakt::PageOutcome::Retryable);

        // The token gate's business, not the retry loop's: hammering an expired token cannot help.
        CHECK(trakt::classifyPage(401, {}, html).outcome == trakt::PageOutcome::AuthFailed);
        CHECK(trakt::classifyPage(403, {}, html).outcome == trakt::PageOutcome::AuthFailed);
        // Server-side, so worth another go.
        CHECK(trakt::classifyPage(500, {}, html).outcome == trakt::PageOutcome::Retryable);
        CHECK(trakt::classifyPage(503, {}, html).outcome == trakt::PageOutcome::Retryable);
        // No HTTP response at all — a dropped connection, DNS, TLS.
        CHECK(trakt::classifyPage(0, {}, QByteArray()).outcome == trakt::PageOutcome::Retryable);
        CHECK(trakt::classifyPage(-1, {}, QByteArray()).outcome == trakt::PageOutcome::Retryable);
        // A bad request stays bad; an unfollowed redirect is a misconfiguration, not a wait.
        CHECK(trakt::classifyPage(404, {}, html).outcome == trakt::PageOutcome::Fatal);
        CHECK(trakt::classifyPage(302, {}, html).outcome == trakt::PageOutcome::Fatal);

        // The attempt budget. 1-based, so kMaxPageAttempts tries happen in total and the last one is
        // not followed by a wait nobody will use.
        CHECK(trakt::shouldRetryAttempt(1) == true);
        CHECK(trakt::shouldRetryAttempt(trakt::kMaxPageAttempts - 1) == true);
        CHECK(trakt::shouldRetryAttempt(trakt::kMaxPageAttempts) == false);
        CHECK(trakt::shouldRetryAttempt(0) == false);

        // Backoff: doubling from the base, capped, and never zero.
        CHECK(trakt::backoffSecFor(1, 0) == trakt::kBaseBackoffSec);
        CHECK(trakt::backoffSecFor(2, 0) == trakt::kBaseBackoffSec * 2);
        CHECK(trakt::backoffSecFor(3, 0) == trakt::kBaseBackoffSec * 4);
        CHECK(trakt::backoffSecFor(99, 0) == trakt::kMaxBackoffSec);      // the cap, not an overflow
        CHECK(trakt::backoffSecFor(0, 0) >= 1);
        // A server hint wins outright — it is the only party that knows when the window reopens —
        // but inside the same bound, because this is reachable from a caller that never saw a header.
        CHECK(trakt::backoffSecFor(1, 45) == 45);
        CHECK(trakt::backoffSecFor(3, 45) == 45);
        CHECK(trakt::backoffSecFor(1, 999999) == trakt::kMaxBackoffSec);
    }

    // ---- 22. the movie id mapping, and that it shares ONE rule with the episode one --------------
    {
        TraktIds ids;
        ids.imdb = QStringLiteral("tt400"); ids.tmdb = QStringLiteral("9");
        CHECK(trakt::imdbMovieStreamIdFor(ids) == QStringLiteral("tt400"));
        // Trimmed at the mapping, not at each call site: a stray space makes an id no resolver matches.
        ids.imdb = QStringLiteral("  tt401\n");
        CHECK(trakt::imdbMovieStreamIdFor(ids) == QStringLiteral("tt401"));

        // The whole point of the shared predicate: a value that is not an IMDB TITLE id is rejected by
        // BOTH mappings identically. Asserting the property rather than eight separate facts is what
        // makes a change to one of them — the exact way two copies of a rule drift apart — a failure.
        const char* kIds[] = { "tt1", "  tt2 ", "tt3:4", "nm5", "6", "", "t7", "TT8" };
        for (const char* raw : kIds)
        {
            TraktIds t; t.imdb = QString::fromLatin1(raw);
            CHECK(trakt::imdbMovieStreamIdFor(t).isEmpty()
                  == trakt::imdbStreamIdFor(t, 1, 1).isEmpty());
        }
        // ...and the property is not vacuous: the table really does contain both answers.
        {
            TraktIds ok;  ok.imdb  = QStringLiteral("tt1");
            TraktIds bad; bad.imdb = QStringLiteral("nm5");
            CHECK(trakt::imdbMovieStreamIdFor(ok).isEmpty() == false);
            CHECK(trakt::imdbMovieStreamIdFor(bad).isEmpty() == true);
            // A colon would emit more fields than the episode format has; it is rejected for the movie
            // form too, so the two can never disagree about which rows look playable.
            TraktIds colon; colon.imdb = QStringLiteral("tt1:9:9");
            CHECK(trakt::imdbMovieStreamIdFor(colon).isEmpty() == true);
        }
    }

    if (failures == 0) { std::puts("TRAKT-OK"); return 0; }
    std::fprintf(stderr, "TRAKT: %d check(s) failed\n", failures);
    return 1;
}
