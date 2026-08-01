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
    // -1 is the parser's "missing" sentinel, so the defaults must BE that sentinel: a
    // default-constructed entry that read season 0 would look like a valid special (Trakt's
    // specials really are season 0) carrying an invalid episode.
    int       season = -1;
    int       episode = -1;
    QString   episodeTitle;
    TraktIds  episodeIds;
    QString   posterUrl;   // "" when Trakt gave none
};

namespace trakt
{
    // The ONE reader for a Trakt timestamp string, shared by the calendar, the calendar cache and every
    // /sync/* payload TraktSync reads. It exists as a named, public thing for one reason: the hazard it
    // guards is invisible. Qt reads an ISO string with NO zone designator as LOCAL time, so converting it
    // to UTC shifts it by whatever this machine's offset happens to be — enough to move a late-evening
    // episode onto the wrong calendar day, differently in summer and winter, and (worse, for the backfill)
    // enough to move a `last_watched_at` across the watermark. Trakt states its times ARE UTC, so a bare
    // string is stamped UTC rather than converted; an EXPLICIT offset is real information and IS converted.
    // A second copy of that rule in another file would be a second chance to omit it.
    //
    // Returns an INVALID QDateTime for anything unparseable. Callers drop such an entry.
    QDateTime parseTraktTimeUtc(const QString& s);

    // TOTAL and TOLERANT by contract: a malformed entry is skipped, a missing `ids` object yields an
    // empty TraktIds, an unparseable date drops that entry, and non-array or non-JSON input returns
    // empty. One bad row must never cost the user their whole calendar.
    QVector<CalendarEntry> parseMyShowsCalendar(const QByteArray& json);

    // Is this body a calendar response AT ALL — i.e. does it parse as a JSON array?
    //
    // parseMyShowsCalendar deliberately collapses two very different bodies onto the same empty
    // vector: "a JSON array with zero rows" (a real, correct answer — nothing airs this week) and
    // "this was never a calendar" (an HTML captive-portal interstitial, a JSON error object, a
    // truncated body). A caller that only wants to READ a calendar is right not to care. A caller
    // that OVERWRITES the offline cache with the result must care, because the second body would
    // blank a good cache on a reply that carried HTTP 200 and no transport error at all.
    //
    // So the discriminator is the array-ness of the body, NOT the row count: an empty array must
    // still be able to replace a stale calendar, or the user keeps seeing last month's forever.
    // It lives here, with the rest of the wire-format knowledge, so TraktClient never has to
    // construct a QJsonDocument of its own and no second place has an opinion about the shape.
    bool looksLikeCalendarPayload(const QByteArray& json);

    // ---- the OAuth token reply (the one body the WRITE path has to read) -------------------------
    // What /oauth/token (refresh) and /oauth/device/token (device flow) hand back. Both endpoints
    // return the same shape, so it is modelled once.
    struct TokenReply
    {
        // false => the body was NOT a token reply. The caller must then leave the STORED tokens
        // exactly as they were: this is the one parse in the app whose failure is unrecoverable if
        // mishandled, because writing an empty pair over a good one unlinks the account for good and
        // no later reply can put it back. Compare looksLikeCalendarPayload, which guards the cache —
        // the same class of body, but a loss the next fetch repairs.
        bool    valid = false;
        QString accessToken;    // non-empty whenever valid
        // "" is a LEGITIMATE value here and means "the reply carried no refresh token", NOT "the
        // refresh token is empty". Trakt ROTATES refresh tokens, so storing "" would destroy the only
        // credential that can ever mint another access token. A caller must keep whatever it already
        // has when this is empty; it must never write it through.
        QString refreshToken;
        qint64  expiresInSec = 0;   // > 0 whenever valid
        qint64  createdAtUnix = 0;  // Trakt's issue time; 0 when absent (the device flow prefers it)
    };

    // Parse a token reply, TOTALLY: any body that is not one — an HTML captive-portal interstitial, a
    // TLS-intercepting proxy's error page, a JSON error object, a 200 that simply omits the token —
    // yields valid=false rather than a half-filled struct.
    //
    // "Is a token reply" means: a JSON object, carrying a NON-EMPTY string `access_token`, and a
    // POSITIVE `expires_in`. The expiry is part of the test and not a detail: a reply without one
    // parses to an already-expired token, so every later call would re-refresh immediately, and each
    // of those spends a rotated refresh token. Rejecting a valid-but-odd reply costs one failed
    // refresh that is retried; accepting an invalid one costs the account link.
    //
    // Lives here, beside looksLikeCalendarPayload, for the same reason: TraktClient.cpp is Qt Network
    // plus the app's ini, so a check written inside it could only ever be exercised by running the
    // app against a hostile network. Here it is pure QByteArray-in/struct-out and probe_trakt §13
    // pins every leg with no I/O.
    TokenReply parseTokenReply(const QByteArray& json);

    // "ttShow:season:episode" — keyed on the SHOW's imdb id, which is the form the scrobbler already
    // emits and the stream resolver already consumes.
    //
    // Returns "" when the show has no USABLE imdb id, or when season/episode are out of range. That
    // empty string is the DOCUMENTED SIGNAL for "show this entry but mark it not playable" — it is
    // not an error, and it must never be substituted with a tmdb/tvdb id, which nothing downstream
    // can use.
    //
    // "Usable" means the id both starts with "tt" and carries no ':' of its own. The colon is this
    // format's field separator, so an id containing one would emit more than three fields; and a
    // value that is not an IMDB title id at all (a bare number, a person's "nm…" id) would emit a
    // well-formed-looking id no resolver can ever satisfy. Both are the same failure the range
    // guard exists to prevent — an entry that LOOKS playable and is not.
    //
    // Season 0 is valid (Trakt uses it for specials). Episode numbers start at 1.
    QString imdbStreamIdFor(const TraktIds& showIds, int season, int episode);

    // The MOVIE half of the same mapping: a bare "tt123", which is the other form the scrobbler emits and
    // the stream resolver consumes (TraktClient.h). Returns "" — the same documented "not playable" signal —
    // for exactly the same reasons, applying exactly the same usability test as the episode form: it shares
    // one private predicate with imdbStreamIdFor rather than restating it, because two copies of "what
    // counts as a usable IMDB id" is two places to disagree about which rows are playable.
    //
    // It exists because the watchlist and the collection are MIXED lists — Trakt returns movies and shows
    // through the one endpoint — so the surface that renders them needs both mappings, and the alternative
    // was a second implementation of the tt-prefix/colon rule living in TraktSync.
    QString imdbMovieStreamIdFor(const TraktIds& ids);

    // ---- the disk-cache format (#23 task 2) ------------------------------------------------------
    // TraktClient persists the last good calendar so an offline launch still shows something, but the
    // FORMAT lives here rather than there: TraktClient.cpp pulls Qt Network and the app's ini, so a
    // serialiser written inside it could only ever be exercised by running the app against Trakt. Here
    // it is the same pure QJson-in/structs-out shape as the parser, and probe_trakt pins the round trip
    // and the corrupt-input behaviour with no I/O at all. TraktClient keeps only the store read/write.
    //
    // The document is {"v":1,"entries":[...]}, one object per entry with every CalendarEntry field named
    // explicitly — no implicit conversions, and adding a field later cannot silently reorder anything.
    QByteArray serializeCalendar(const QVector<CalendarEntry>& entries);

    // TOTAL, exactly like the parser, and for a stronger reason: this input is a file on disk that a
    // crash mid-write, a disk error or a hand-edited ini can truncate. Empty/garbage/truncated input, a
    // wrong top-level shape and an unrecognised version all yield an EMPTY vector — a caller that gets
    // nothing falls back to a live fetch, whereas a half-read entry would be rendered as if it were real.
    // Individual entries follow the parser's own rule: an entry whose air time will not parse is dropped
    // (it cannot be placed on a calendar), and a missing season/episode reads back as the -1 sentinel
    // rather than 0, so a round trip through the cache is value-identical to the parse that produced it.
    QVector<CalendarEntry> deserializeCalendar(const QByteArray& json);
}
