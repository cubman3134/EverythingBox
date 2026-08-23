// A pure, testable filter model over the game library (issue #63). It answers ONE question — does a game
// match a saved filter — and it answers it from a plain snapshot of the game's facts, never by reaching back
// into any store. That is deliberate: the extraction of a game's facts (favourite from FavoritesStore, tags/
// hidden/completion from ItemMarks, playtime from PlayStats, genre/players/year from the scrape) lives in the
// UI where those stores live; this header stays QtCore-only so a headless probe can exercise the semantics
// against hand-built fixtures with no window, no ini and no addons.
//
// SEMANTICS: AND across dimensions, OR within a dimension. A dimension the filter leaves empty imposes no
// constraint, so the empty filter matches every game. Within one dimension the accepted values are ORed: a
// systems filter of {snes,nes} matches a game on EITHER console; a minPlayers filter of {2,4} matches a game
// that supports at least 2 OR at least 4 players (i.e. >= 2). The three booleans (favourite / played / hidden)
// are tri-state: Any imposes nothing, Yes/No require the fact to be exactly that.
//
// Genre / player-count / release-decade are only as good as the scrape (issue #63's scope note): a game with
// no scraped genre simply carries an empty genres list and never matches a genre dimension. That is by design,
// not a bug — the filter does not fabricate facts it was not given.
//
// IT IS NO LONGER ONLY GAMES (issue #196, part 2), and the names here are older than that. #196 asks for
// composer and conductor to be exposed as #63 filter fields — "all Bach conducted by Gardiner" — and the
// cheapest honest way to give it is two more OR-set dimensions on THIS model, evaluated by THIS function,
// saved by the same preset store and rendered as the same shelf. A bespoke music-query path would have been
// a second evaluator with a second set of semantics to keep in step. The cost is that `GameFacts` now
// carries facts about a piece of music; renaming the namespace and the struct across the store, the cloud
// merge and their probes was judged worse churn than this comment, but it is a debt and it is written down.
// Everything below still holds: a dimension nobody fills constrains nothing, and a track with no COMPOSER
// tag simply carries an empty composers list and never matches a composer dimension.
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace gamefilter
{
// A snapshot of one game's queryable facts, extracted by the caller from the per-game stores. Every field
// defaults to "unknown / no constraint satisfied": an empty list, a false flag, a zero count/year. The
// evaluator treats all of these as "this game asserts nothing on that dimension", so an unscraped game never
// matches a scraped-field dimension.
struct GameFacts
{
    QStringList systems;   // SystemCatalog ids this game belongs to ("snes", "pc", …); usually exactly one
    bool        favorite = false;
    bool        hidden    = false;
    QStringList tags;      // ItemMarks tags carried by this game
    int         completion = 0;   // ItemMarks::Completion cast to int (0 None … 4 Planned)
    qint64      playSeconds = 0;  // PlayStats total across sessions; 0 == unplayed
    QStringList genres;    // scraped genres, already split into individual genre names
    int         maxPlayers = 0;   // scraped max simultaneous players (0 == unknown)
    int         releaseYear = 0;  // scraped release year (0 == unknown)
    // The classical credits of a MUSIC track (issue #196, part 2), already split by the scan — never
    // re-derived from a display string here, because the split depended on a user setting and on what the
    // container held structurally, which only the scan was in a position to know. Empty for every game and
    // for every track with no such tag, which is the whole point: an empty fact matches no dimension.
    QStringList composers;
    QStringList conductors;
};

enum class Tri { Any, Yes, No };

// A saved filter. Each list-typed dimension is an OR-set of accepted values; an empty list means the
// dimension is absent (no constraint). The evaluator ANDs every present dimension together.
struct Filter
{
    QStringList systems;      // OR: game is on one of these SystemCatalog ids
    QStringList tags;         // OR: game carries one of these tags
    QStringList genres;       // OR: game's scraped genres contain one of these (case-insensitive)
    QStringList composers;    // OR: track's composers contain one of these (case-insensitive) — #196
    QStringList conductors;   // OR: track's conductors contain one of these (case-insensitive) — #196
    QVector<int> minPlayers;  // OR of ">= N" thresholds: game supports at least one of these player counts
    QVector<int> decades;     // OR: game's release year falls in one of these decades (value = decade start, e.g. 1990)
    QVector<int> completions; // OR: game's completion mark equals one of these
    Tri favorite = Tri::Any;
    Tri played   = Tri::Any;  // Yes == playSeconds > 0
    Tri hidden   = Tri::Any;

    bool isEmpty() const;

    QJsonObject toJson() const;
    static Filter fromJson(const QJsonObject& o);

    // A short human summary of the constraints, for a shelf/preset label ("SNES · Unplayed · 2P+"). Empty
    // filter -> "All games". Deterministic dimension order so the same filter always reads the same.
    QString describe() const;
};

// The evaluator. AND across present dimensions, OR within each. An empty filter matches every game.
bool matches(const Filter& f, const GameFacts& g);

// Parse a scraped "players" string ("1", "2", "1-4", "Up to 8", "4 Players") to a max player count, or 0
// when nothing numeric is present. Pure so the extraction path is itself testable.
int parseMaxPlayers(const QString& raw);

// Split a scraped "genre" string ("Action / Adventure", "RPG, Strategy") into individual trimmed genres.
QStringList splitGenres(const QString& raw);

// The four-digit year out of a scraped release value ("1996", "19960101T000000", "1996-03-21"), or 0.
int parseYear(const QString& raw);
} // namespace gamefilter
