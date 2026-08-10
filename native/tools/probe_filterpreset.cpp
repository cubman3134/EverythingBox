// Headless check of the saved-filter-preset feature (issue #63): the pure filter model/evaluator
// (src/core/GameFilter) and the per-profile preset store (src/core/FilterPresetStore). QtCore-only — the
// evaluator takes a plain GameFacts snapshot and the store is a QSettings wrapper — so it runs under the
// offscreen QPA in CI with no window, no addons and no ini of its own.
//
// The evaluator is pinned against HAND-BUILT fixtures with HAND-WRITTEN expected results: every assertion
// names the games a filter should match, computed by a human reading the fixtures, NOT by calling matches()
// a second time to derive what matches() should say (that would pass no matter what the function did). It
// covers: the empty filter matches all; AND across dimensions; OR within a dimension; each dimension
// (system, favourite, played, tag, genre, min-players, decade, completion, hidden); an unscraped game never
// matching a scraped-field dimension; the scraped-field parsers; and the JSON round-trip.
//
// The store is pinned for save / load / upsert / rename / delete round-trips and per-profile isolation.
//
// Prints FILTERPRESET-OK on success; any failure prints FILTERPRESET-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// the store reads/writes starts empty and is removed at exit. The probe still SEEDS a profile id via
// ProfileStore::setCurrent, because currentId() otherwise resolves to "default" rather than a named profile.
#include "GameFilter.h"
#include "FilterPresetStore.h"
#include "ProfileStore.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSet>
#include <cstdio>

using namespace gamefilter;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FILTERPRESET-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- Fixture library ------------------------------------------------------------------------------------
// Six games with known facts. Read them once; every expected-match set below is derived by eye from THIS
// table, never from the evaluator.
//   idx system    fav played  tags          genres              players year comp   hidden
//   0   snes      Y   0(no)    {rpg}         {RPG}               1       1994 2(Fin) N
//   1   snes      N   3600(Y)  {}            {Action}            2       1991 0      N
//   2   nes       Y   60(Y)    {rpg,hard}    {RPG,Adventure}     2       1988 1(InP) N
//   3   pc        N   0(no)    {}            {Strategy}          4       2005 0      N
//   4   genesis   N   0(no)    {}            {}(unscraped)       0       0    0      N
//   5   snes      N   0(no)    {}            {Puzzle}            1       1999 0      Y(hidden)
static QVector<GameFacts> makeLibrary()
{
    QVector<GameFacts> lib(6);
    auto& g = lib;
    g[0].systems = {QStringLiteral("snes")};    g[0].favorite = true;  g[0].playSeconds = 0;
    g[0].tags = {QStringLiteral("rpg")};        g[0].genres = {QStringLiteral("RPG")};
    g[0].maxPlayers = 1; g[0].releaseYear = 1994; g[0].completion = 2;

    g[1].systems = {QStringLiteral("snes")};    g[1].favorite = false; g[1].playSeconds = 3600;
    g[1].genres = {QStringLiteral("Action")};   g[1].maxPlayers = 2; g[1].releaseYear = 1991; g[1].completion = 0;

    g[2].systems = {QStringLiteral("nes")};     g[2].favorite = true;  g[2].playSeconds = 60;
    g[2].tags = {QStringLiteral("rpg"), QStringLiteral("hard")};
    g[2].genres = {QStringLiteral("RPG"), QStringLiteral("Adventure")};
    g[2].maxPlayers = 2; g[2].releaseYear = 1988; g[2].completion = 1;

    g[3].systems = {QStringLiteral("pc")};      g[3].favorite = false; g[3].playSeconds = 0;
    g[3].genres = {QStringLiteral("Strategy")}; g[3].maxPlayers = 4; g[3].releaseYear = 2005; g[3].completion = 0;

    g[4].systems = {QStringLiteral("genesis")}; g[4].favorite = false; g[4].playSeconds = 0;
    // g[4] is fully unscraped: no genre, no player count, no year.

    g[5].systems = {QStringLiteral("snes")};    g[5].favorite = false; g[5].playSeconds = 0;
    g[5].genres = {QStringLiteral("Puzzle")};   g[5].maxPlayers = 1; g[5].releaseYear = 1999; g[5].completion = 0;
    g[5].hidden = true;
    return lib;
}

// The set of fixture indices a filter matches.
static QSet<int> matchSet(const Filter& f, const QVector<GameFacts>& lib)
{
    QSet<int> out;
    for (int i = 0; i < lib.size(); ++i)
        if (matches(f, lib[i])) out.insert(i);
    return out;
}

// Compare a filter's match set to a hand-written expected set of indices.
#define EXPECT_MATCH(f, ...) do { \
    const QSet<int> got = matchSet((f), lib); \
    const QSet<int> want = QSet<int>(std::initializer_list<int>{__VA_ARGS__}); \
    if (got != want) { \
        std::fprintf(stderr, "FILTERPRESET-FAIL match mismatch (line %d): got %d want %d\n", \
                     __LINE__, int(got.size()), int(want.size())); \
        ++failures; \
    } \
} while (0)

static void testEvaluator()
{
    const QVector<GameFacts> lib = makeLibrary();

    // Empty filter matches every game.
    CHECK(Filter{}.isEmpty());
    EXPECT_MATCH(Filter{}, 0, 1, 2, 3, 4, 5);

    // Single dimension: system.
    { Filter f; f.systems = {QStringLiteral("snes")};                 EXPECT_MATCH(f, 0, 1, 5); }
    // OR within the system dimension.
    { Filter f; f.systems = {QStringLiteral("snes"), QStringLiteral("nes")}; EXPECT_MATCH(f, 0, 1, 2, 5); }
    // System match is case-insensitive.
    { Filter f; f.systems = {QStringLiteral("SNES")};                 EXPECT_MATCH(f, 0, 1, 5); }

    // Favourite (tri-state).
    { Filter f; f.favorite = Tri::Yes;                                EXPECT_MATCH(f, 0, 2); }
    { Filter f; f.favorite = Tri::No;                                 EXPECT_MATCH(f, 1, 3, 4, 5); }

    // Played / unplayed (playSeconds > 0).
    { Filter f; f.played = Tri::Yes;                                  EXPECT_MATCH(f, 1, 2); }
    { Filter f; f.played = Tri::No;                                   EXPECT_MATCH(f, 0, 3, 4, 5); }

    // Tag (OR within), case-sensitive to the stored tag.
    { Filter f; f.tags = {QStringLiteral("rpg")};                     EXPECT_MATCH(f, 0, 2); }
    { Filter f; f.tags = {QStringLiteral("hard")};                    EXPECT_MATCH(f, 2); }
    { Filter f; f.tags = {QStringLiteral("rpg"), QStringLiteral("hard")}; EXPECT_MATCH(f, 0, 2); }

    // Genre, case-insensitive; an unscraped game (idx 4) never matches.
    { Filter f; f.genres = {QStringLiteral("rpg")};                   EXPECT_MATCH(f, 0, 2); }
    { Filter f; f.genres = {QStringLiteral("Action")};               EXPECT_MATCH(f, 1); }
    { Filter f; f.genres = {QStringLiteral("Adventure"), QStringLiteral("Strategy")}; EXPECT_MATCH(f, 2, 3); }

    // Min-players: ">= N", OR of thresholds. Unknown player count (idx 4) matches nothing.
    { Filter f; f.minPlayers = {2};                                   EXPECT_MATCH(f, 1, 2, 3); }
    { Filter f; f.minPlayers = {4};                                   EXPECT_MATCH(f, 3); }
    { Filter f; f.minPlayers = {2, 4};                                EXPECT_MATCH(f, 1, 2, 3); }

    // Decade: value is the decade start; unscraped year (idx 4) is in no decade.
    { Filter f; f.decades = {1990};                                   EXPECT_MATCH(f, 0, 1, 5); }
    { Filter f; f.decades = {1980, 2000};                             EXPECT_MATCH(f, 2, 3); }

    // Completion (OR).
    { Filter f; f.completions = {2};                                  EXPECT_MATCH(f, 0); }
    { Filter f; f.completions = {0};                                  EXPECT_MATCH(f, 1, 3, 4, 5); }
    { Filter f; f.completions = {1, 2};                               EXPECT_MATCH(f, 0, 2); }

    // Hidden (tri-state): only idx 5 is hidden.
    { Filter f; f.hidden = Tri::Yes;                                  EXPECT_MATCH(f, 5); }
    { Filter f; f.hidden = Tri::No;                                   EXPECT_MATCH(f, 0, 1, 2, 3, 4); }

    // AND across dimensions: SNES AND unplayed -> idx 0 and 5 (idx 1 is played).
    { Filter f; f.systems = {QStringLiteral("snes")}; f.played = Tri::No; EXPECT_MATCH(f, 0, 5); }
    // Three dimensions: SNES AND favourite AND unplayed -> only idx 0.
    { Filter f; f.systems = {QStringLiteral("snes")}; f.favorite = Tri::Yes; f.played = Tri::No;
      EXPECT_MATCH(f, 0); }
    // AND with an OR dimension: (SNES or NES) AND favourite -> idx 0, 2.
    { Filter f; f.systems = {QStringLiteral("snes"), QStringLiteral("nes")}; f.favorite = Tri::Yes;
      EXPECT_MATCH(f, 0, 2); }
    // A contradictory AND matches nothing: NES AND SNES-only game -> the system dim already ORs, so use
    // played=Yes AND unplayed-only genre. SNES(system) AND played=Yes AND favourite=No -> idx 1 only.
    { Filter f; f.systems = {QStringLiteral("snes")}; f.played = Tri::Yes; f.favorite = Tri::No;
      EXPECT_MATCH(f, 1); }
    // A genre filter over an unscraped-only result set is empty: genesis(system, idx4) AND any genre.
    { Filter f; f.systems = {QStringLiteral("genesis")}; f.genres = {QStringLiteral("Action")};
      EXPECT_MATCH(f /* nothing */); }
}

static void testParsers()
{
    CHECK(parseMaxPlayers(QStringLiteral("1")) == 1);
    CHECK(parseMaxPlayers(QStringLiteral("1-4")) == 4);
    CHECK(parseMaxPlayers(QStringLiteral("Up to 8")) == 8);
    CHECK(parseMaxPlayers(QStringLiteral("2 Players")) == 2);
    CHECK(parseMaxPlayers(QStringLiteral("Single player")) == 0);
    CHECK(parseMaxPlayers(QString()) == 0);

    const QStringList g1 = splitGenres(QStringLiteral("Action / Adventure"));
    CHECK(g1.size() == 2 && g1[0] == QStringLiteral("Action") && g1[1] == QStringLiteral("Adventure"));
    const QStringList g2 = splitGenres(QStringLiteral("RPG, Strategy; Sim"));
    CHECK(g2.size() == 3);
    CHECK(splitGenres(QString()).isEmpty());

    CHECK(parseYear(QStringLiteral("1996")) == 1996);
    CHECK(parseYear(QStringLiteral("19960101T000000")) == 1996);
    CHECK(parseYear(QStringLiteral("1996-03-21")) == 1996);
    CHECK(parseYear(QStringLiteral("12")) == 0);
    CHECK(parseYear(QStringLiteral("3021")) == 0);   // out of plausible range
    CHECK(parseYear(QString()) == 0);
}

static void testJsonRoundTrip()
{
    const QVector<GameFacts> lib = makeLibrary();
    Filter f;
    f.systems = {QStringLiteral("snes"), QStringLiteral("nes")};
    f.tags = {QStringLiteral("rpg")};
    f.genres = {QStringLiteral("RPG")};
    f.minPlayers = {2};
    f.decades = {1990};
    f.completions = {1, 2};
    f.favorite = Tri::Yes;
    f.played = Tri::No;
    f.hidden = Tri::No;

    const Filter r = Filter::fromJson(f.toJson());
    // Field-level equality after the round-trip.
    CHECK(r.systems == f.systems);
    CHECK(r.tags == f.tags);
    CHECK(r.genres == f.genres);
    CHECK(r.minPlayers == f.minPlayers);
    CHECK(r.decades == f.decades);
    CHECK(r.completions == f.completions);
    CHECK(r.favorite == f.favorite);
    CHECK(r.played == f.played);
    CHECK(r.hidden == f.hidden);
    // And behavioural equality: the reconstructed filter matches the same games (independent cross-check).
    CHECK(matchSet(f, lib) == matchSet(r, lib));

    // The empty filter round-trips to an empty object and back to empty.
    CHECK(Filter{}.toJson().isEmpty());
    CHECK(Filter::fromJson(QJsonObject{}).isEmpty());
}

static void testStore()
{
    ProfileStore::setCurrent(QStringLiteral("alpha"));
    CHECK(FilterPresetStore::list().isEmpty());     // isolated data dir: starts empty

    Filter fa; fa.systems = {QStringLiteral("snes")}; fa.played = Tri::No; // "Unplayed SNES"
    Filter fb; fb.minPlayers = {2};                                        // "2-player games"

    FilterPresetStore::save({QString(), QStringLiteral("Unplayed SNES"), fa, 0});
    FilterPresetStore::save({QString(), QStringLiteral("Two player"), fb, 0});

    QVector<FilterPreset> ps = FilterPresetStore::list();
    CHECK(ps.size() == 2);
    CHECK(ps.first().name == QStringLiteral("Two player"));  // newest first
    CHECK(FilterPresetStore::exists(QStringLiteral("Unplayed SNES")));
    CHECK(!FilterPresetStore::exists(QStringLiteral("nope")));

    // Loaded filter survives the round-trip through the store.
    const FilterPreset got = FilterPresetStore::get(QStringLiteral("Unplayed SNES"));
    CHECK(got.filter.systems == QStringList{QStringLiteral("snes")});
    CHECK(got.filter.played == Tri::No);
    CHECK(got.ts > 0);   // save() stamps a time

    // Upsert by name: re-saving "Two player" with a different filter replaces, does not duplicate.
    Filter fb2; fb2.minPlayers = {4};
    FilterPresetStore::save({QString(), QStringLiteral("Two player"), fb2, 0});
    CHECK(FilterPresetStore::list().size() == 2);
    CHECK(FilterPresetStore::get(QStringLiteral("Two player")).filter.minPlayers == QVector<int>{4});

    // Rename round-trips; a rename to a taken name or from a missing name fails and changes nothing.
    CHECK(FilterPresetStore::rename(QStringLiteral("Unplayed SNES"), QStringLiteral("SNES backlog")));
    CHECK(FilterPresetStore::exists(QStringLiteral("SNES backlog")));
    CHECK(!FilterPresetStore::exists(QStringLiteral("Unplayed SNES")));
    CHECK(!FilterPresetStore::rename(QStringLiteral("SNES backlog"), QStringLiteral("Two player"))); // taken
    CHECK(!FilterPresetStore::rename(QStringLiteral("ghost"), QStringLiteral("whatever")));          // missing
    CHECK(FilterPresetStore::list().size() == 2);   // both failed renames left the store untouched

    // Delete round-trips; deleting a missing name is a no-op.
    FilterPresetStore::remove(QStringLiteral("Two player"));
    CHECK(FilterPresetStore::list().size() == 1);
    CHECK(!FilterPresetStore::exists(QStringLiteral("Two player")));
    FilterPresetStore::remove(QStringLiteral("Two player")); // already gone
    CHECK(FilterPresetStore::list().size() == 1);

    // Per-profile isolation: profile beta cannot see alpha's presets, and its own writes don't leak back.
    ProfileStore::setCurrent(QStringLiteral("beta"));
    CHECK(FilterPresetStore::list().isEmpty());
    FilterPresetStore::save({QString(), QStringLiteral("Beta only"), fb, 0});
    CHECK(FilterPresetStore::list().size() == 1);
    ProfileStore::setCurrent(QStringLiteral("alpha"));
    CHECK(FilterPresetStore::list().size() == 1);   // still just "SNES backlog"
    CHECK(!FilterPresetStore::exists(QStringLiteral("Beta only")));
    CHECK(FilterPresetStore::exists(QStringLiteral("SNES backlog")));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testEvaluator();
    testParsers();
    testJsonRoundTrip();
    testStore();
    if (failures == 0) std::printf("FILTERPRESET-OK\n");
    return failures == 0 ? 0 : 1;
}
