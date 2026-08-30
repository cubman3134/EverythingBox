// DISCORD RICH PRESENCE — the rules that decide what a presence card SAYS, and nothing that touches the
// world.
//
// WHY THIS IS A SEPARATE FILE FROM THE TRANSPORT. Everything below is arithmetic and string handling over
// plain structs: which Discord activity type a kind maps to, whether a countdown is meaningful, what happens
// to the timestamp when playback pauses, how a title is cut to fit a byte budget. None of it is a property
// of Discord's socket, and every part of it is wrong in a way nothing in the app would notice — a card that
// counts a paused film down to zero, a truncation that splits a codepoint and makes Discord discard the
// update whole. So it lives here, as pure functions, and probe_presence drives every arm of it with no
// Discord running, no socket and no clock.
//
// NOTHING HERE READS THE CLOCK. `nowUnix` is a PARAMETER, for the same reason trakt::planMissed takes its
// own: a rule that reads the clock itself can only be tested by waiting.
#pragma once
#include <QPair>
#include <QString>
#include <QVector>

namespace Presence
{
    // Discord's own activity type numbers. SET_ACTIVITY accepts only these three (plus Competing, which
    // nothing here is).
    inline constexpr int kPlaying   = 0;
    inline constexpr int kListening = 2;
    inline constexpr int kWatching  = 3;

    // Discord's limit on `details` and `state` is 128 BYTES, not 128 characters, and an update that exceeds
    // it is discarded whole rather than truncated — so an over-long CJK title makes presence silently stop
    // updating rather than showing a shortened name. See clampUtf8.
    inline constexpr int kMaxFieldBytes = 128;
    inline constexpr int kMaxButtons    = 2;

    enum class Kind { None, Movie, Episode, LiveTv, Music, Audiobook, Game, PcGame, Reading };

    // What the app knows about the thing that is open. Filled in at the six hook points from the same locals
    // that already build the RecentItem there — this struct deliberately holds nothing that would need a new
    // lookup, a scrape or a network call.
    struct Item
    {
        Kind    kind = Kind::None;
        QString title;      // "The Bear"
        QString subtitle;   // "S3E4 · Violet" / "Sigur Rós — Ágætis byrjun" / "SNES" / "Reading · p. 3 of 40"
        QString artUrl;     // used ONLY when it starts with https:// (Discord's CDN cannot fetch a local
                            // path); anything else falls back to the uploaded key for the kind
        QString imdbId;     // "tt0083658" or "ttShow:3:4" -> the IMDb button. Empty for everything else.
        QString system;     // console id / storefront, for the caller's own use when building `subtitle`
    };

    // Exactly what goes on the wire. Compared field-for-field by the orchestrator: two builds that are equal
    // produce no frame at all, which is what makes Discord's 5-per-20-seconds limit a non-issue rather than
    // something to engineer around.
    struct Activity
    {
        bool    valid = false;   // false = there is nothing to show; the orchestrator clears instead
        int     type  = kPlaying;
        QString details;
        QString state;
        QString largeImage, largeText;
        QString smallImage, smallText;
        qint64  startUnix = 0;   // set -> Discord counts UP from here
        qint64  endUnix   = 0;   // set -> Discord counts DOWN to here. Never both.
        QVector<QPair<QString, QString>> buttons;   // label, url — at most kMaxButtons

        bool operator==(const Activity& o) const;
        bool operator!=(const Activity& o) const { return !(*this == o); }
    };

    int     typeFor(Kind k);        // Watching / Listening / Playing
    QString fallbackAsset(Kind k);  // the uploaded asset key: "movie", "tv", "livetv", …
    QString kindLabel(Kind k);      // the small badge's hover text: "Movie", "Live TV", …

    // Whether a countdown is meaningful for this kind. A film and a track end; a live channel, a game and a
    // book do not, and a countdown on one of those would be a fabricated number.
    bool hasCountdown(Kind k);

    // Cut `s` to at most `maxBytes` UTF-8 bytes WITHOUT splitting a codepoint. A naive left(128) does both
    // halves of this wrong: it counts the wrong unit, and it can leave a truncated multi-byte sequence that
    // makes the whole payload invalid.
    QString clampUtf8(const QString& s, int maxBytes);

    // The card for whatever is open. An empty title or Kind::None yields an invalid Activity (send nothing).
    Activity build(const Item& item, double positionSec, double durationSec, bool paused, qint64 nowUnix);

    // The card for "the app is open but nothing is playing".
    Activity idle(qint64 sessionStartUnix);
}
