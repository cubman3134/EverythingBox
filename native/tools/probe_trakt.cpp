// Headless check of the Trakt read layer (issue #23). TraktClient is a scrobbler — write-only — and
// this is the first read piece: a TOLERANT parser for the "my shows" calendar and the identity mapper
// the watchlist/collection and history-backfill follow-ups will both reuse.
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
// Prints TRAKT-OK on success; any failure prints TRAKT-FAIL <cond> and exits non-zero.
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

    if (failures == 0) { std::puts("TRAKT-OK"); return 0; }
    std::fprintf(stderr, "TRAKT: %d check(s) failed\n", failures);
    return 1;
}
