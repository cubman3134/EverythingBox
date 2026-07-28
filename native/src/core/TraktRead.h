// The Trakt READ layer (issue #23). TraktClient is a scrobbler — it pushes what you watch and never
// reads anything back. This is the first read piece: a tolerant parser for the "my shows" calendar,
// and the identity mapper that turns Trakt's id bag into the app's own key.
//
// Shape confirmed against Trakt's current API reference for GET /calendars/{target}/shows/{start_date}/{days}
// (docs.trakt.tv/reference/getcalendarsshows): a FLAT array; each item carries a top-level `first_aired`
// string plus `show` and `episode` objects, each with an `ids` bag.
//
// Pure: QJson in, structs out. No network, no GUI, no ini — so probe_trakt links against Qt6::Core
// alone and pins the tables with no I/O.
#pragma once
#include <QDateTime>
#include <QString>
#include <QVector>

class QByteArray;

// Trakt returns an id bag per show and per episode. Per the reference, `trakt`, `tvdb` and `tmdb`
// arrive as JSON NUMBERS while `imdb` (and the show-only `slug`) are strings — and `tvdb`/`imdb`/`tmdb`
// are all NULLABLE, so an explicit JSON null is a shape the parser must expect, not an anomaly. All are
// kept as QString here so callers have one type.
struct TraktIds
{
    QString imdb;   // "tt1000001" — the only one the app can key on
    QString tmdb;
    QString tvdb;
    QString trakt;
};

struct CalendarEntry
{
    QDateTime airsAtUtc;
    QString   showTitle;
    TraktIds  showIds;
    int       season = 0;
    int       episode = 0;
    QString   episodeTitle;
    TraktIds  episodeIds;
    QString   posterUrl;   // "" when Trakt gave none
};

namespace trakt
{
    // TOTAL and TOLERANT by contract: a malformed entry is skipped, a missing `ids` object yields an
    // empty TraktIds, an unparseable date drops that entry, and non-array or non-JSON input returns
    // empty. One bad row must never cost the user their whole calendar.
    QVector<CalendarEntry> parseMyShowsCalendar(const QByteArray& json);

    // "ttShow:season:episode" — keyed on the SHOW's imdb id, which is the form the scrobbler already
    // emits and the stream resolver already consumes.
    //
    // Returns "" when the show has no imdb id, or when season/episode are out of range. That empty
    // string is the DOCUMENTED SIGNAL for "show this entry but mark it not playable" — it is not an
    // error, and it must never be substituted with a tmdb/tvdb id, which nothing downstream can use.
    //
    // Season 0 is valid (Trakt uses it for specials). Episode numbers start at 1.
    QString imdbStreamIdFor(const TraktIds& showIds, int season, int episode);
}
