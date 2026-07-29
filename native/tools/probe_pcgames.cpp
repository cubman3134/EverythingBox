// Headless check of PC-game identity and source selection. The PC folders were per-launcher; this is
// the unit that lets ONE entry carry Steam, GOG, Epic, Battle.net and a downloaded copy as sources.
//
// Two properties matter more than everything else here and are pinned hardest:
//   * normalizeTitle must NOT strip sequel numerals. Merging "Hades" with "Hades II" LOSES a game from
//     the user's library; failing to merge two editions merely shows it twice. The asymmetry is the
//     whole reason this is a probe and not a hand-wave.
//   * pickAutoSource must never return a NOT-ready source. Play is one keypress; silently starting a
//     multi-gigabyte download from it is the failure this function exists to prevent.
//
// Prints PCGAMES-OK on success; any failure prints PCGAMES-FAIL <cond> and exits non-zero.
#include "PcGameId.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PCGAMES-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace pcgame;

static PcGameSource src(PcGameSource::Kind k, const QString& launcher, bool ready)
{
    PcGameSource s; s.kind = k; s.launcher = launcher; s.ready = ready; return s;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The override store PERSISTS. Point it at a scratch ini and delete any leftover first, so a run is
    // hermetic: without this the probe writes into the app ini next to the exe and the NEXT run reads
    // what this one wrote. That is not hypothetical — it fired during the mutation pass here. A build
    // whose normalizeTitle wrongly ate "II" wrote an override key under a title no correct build ever
    // produces; the reverted, CORRECT build then read that key back and failed §4, which reads exactly
    // like "the revert did not take". Test isolation is what tells the two apart.
    const QString ini = QDir::temp().filePath(QStringLiteral("eb-probe-pcgames.ini"));
    QFile::remove(ini);
    setIniPathForTesting(ini);

    // ---- 1. normalizeTitle: noise that SHOULD collapse -----------------------------------------
    CHECK(normalizeTitle(QStringLiteral("Hades")) == normalizeTitle(QStringLiteral("HADES")));
    CHECK(normalizeTitle(QStringLiteral("Prey")) == normalizeTitle(QStringLiteral("Prey (2017)")));
    CHECK(normalizeTitle(QStringLiteral("Tomb Raider"))
          == normalizeTitle(QStringLiteral("Tomb Raider: Game of the Year Edition")));
    CHECK(normalizeTitle(QStringLiteral("Dishonored"))
          == normalizeTitle(QStringLiteral("Dishonored - Definitive Edition")));
    CHECK(normalizeTitle(QStringLiteral("BioShock"))
          == normalizeTitle(QStringLiteral("BioShock™ Remastered")));
    CHECK(normalizeTitle(QStringLiteral("Deus Ex"))
          == normalizeTitle(QStringLiteral("Deus Ex:  Director's Cut ")));

    // ---- 2. normalizeTitle: SEQUELS MUST NOT COLLAPSE ------------------------------------------
    // Each pair below is a DIFFERENT game. A merge here removes one of them from the library.
    CHECK(normalizeTitle(QStringLiteral("Hades")) != normalizeTitle(QStringLiteral("Hades II")));
    CHECK(normalizeTitle(QStringLiteral("Portal")) != normalizeTitle(QStringLiteral("Portal 2")));
    CHECK(normalizeTitle(QStringLiteral("Diablo II")) != normalizeTitle(QStringLiteral("Diablo III")));
    CHECK(normalizeTitle(QStringLiteral("Fallout 3")) != normalizeTitle(QStringLiteral("Fallout 4")));
    CHECK(normalizeTitle(QStringLiteral("The Witcher 2")) != normalizeTitle(QStringLiteral("The Witcher 3")));
    CHECK(normalizeTitle(QStringLiteral("Civilization V")) != normalizeTitle(QStringLiteral("Civilization VI")));
    // A numeral is NOT edition noise even next to edition noise.
    CHECK(normalizeTitle(QStringLiteral("Diablo II: Resurrected"))
          != normalizeTitle(QStringLiteral("Diablo III: Reaper of Souls")));
    // ...but the SAME sequel across two editions still collapses.
    CHECK(normalizeTitle(QStringLiteral("Portal 2"))
          == normalizeTitle(QStringLiteral("Portal 2 - Game of the Year Edition")));

    // ---- 3. degenerate input ------------------------------------------------------------------
    CHECK(normalizeTitle(QString()).isEmpty());
    CHECK(normalizeTitle(QStringLiteral("   ")).isEmpty());
    CHECK(normalizeTitle(QStringLiteral("!!!")).isEmpty());

    // ---- 4. sameGame: IGDB wins when BOTH sides have one ---------------------------------------
    // Same igdb id, wildly different titles -> same game (a regional rename).
    CHECK(sameGame(QStringLiteral("Rockman"), QStringLiteral("igdb:100"),
                   QStringLiteral("Mega Man"), QStringLiteral("igdb:100")) == true);
    // DIFFERENT igdb ids -> NOT the same game, even when the titles normalise identically. This is the
    // case where trusting the title would merge two genuinely distinct releases.
    CHECK(sameGame(QStringLiteral("Prey"), QStringLiteral("igdb:1"),
                   QStringLiteral("Prey"), QStringLiteral("igdb:2")) == false);
    // Only ONE side has an id -> fall back to the title, do not treat the missing id as a mismatch.
    CHECK(sameGame(QStringLiteral("Hades"), QStringLiteral("igdb:7"),
                   QStringLiteral("Hades"), QString()) == true);
    CHECK(sameGame(QStringLiteral("Hades"), QString(),
                   QStringLiteral("Hades II"), QString()) == false);

    // ---- 5. the override store beats BOTH heuristics -------------------------------------------
    {
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), true);
        CHECK(overrideSaysSame(QStringLiteral("hades"), QStringLiteral("hades ii")) == true);
        // symmetric — the user said these two are the same, in either order
        CHECK(overrideSaysSame(QStringLiteral("hades ii"), QStringLiteral("hades")) == true);
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), false);
        CHECK(overrideSaysSame(QStringLiteral("hades"), QStringLiteral("hades ii")) == false);
    }

    // ---- 5b. ...and sameGame consults it FIRST, ahead of both heuristics -----------------------
    // Added beyond the brief's table: the §5 block above exercises the store directly, so it cannot
    // observe WHERE sameGame consults it. Without these four checks, moving the override lookup to the
    // END of sameGame (mutation #6) changes no observable behaviour and the precedence rule that makes
    // the escape hatch usable is untested. Both directions are pinned: an override that says SAME must
    // beat disagreeing titles, and one that says NOT-SAME must beat two matching igdb ids.
    {
        // The titles say "different game"; the user says otherwise, and the user wins.
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), true);
        CHECK(sameGame(QStringLiteral("Hades"), QString(),
                       QStringLiteral("Hades II"), QString()) == true);
        // Symmetric through sameGame too, not just through the raw store.
        CHECK(sameGame(QStringLiteral("Hades II"), QString(),
                       QStringLiteral("Hades"), QString()) == true);
        // And a negative verdict beats even two IDENTICAL igdb ids — this is the user undoing a merge
        // the metadata provider itself got wrong, which is the case the ids can never fix themselves.
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), false);
        CHECK(sameGame(QStringLiteral("Hades"), QStringLiteral("igdb:7"),
                       QStringLiteral("Hades II"), QStringLiteral("igdb:7")) == false);
        // The stored NO also has to survive the title heuristic agreeing with it (belt and braces:
        // proves the lookup ran, not that the fallback happened to return the same answer).
        CHECK(sameGame(QStringLiteral("Hades"), QString(),
                       QStringLiteral("Hades II"), QString()) == false);
    }

    // ---- 5c. mergeKey: the igdb id when there is one, else the normalised title -----------------
    {
        CHECK(mergeKey(QStringLiteral("Hades"), QStringLiteral("igdb:7")) == QStringLiteral("igdb:7"));
        CHECK(mergeKey(QStringLiteral("Hades II"), QString()) == normalizeTitle(QStringLiteral("Hades II")));
        // Two editions of one game group together; a sequel does NOT join them.
        CHECK(mergeKey(QStringLiteral("Portal 2"), QString())
              == mergeKey(QStringLiteral("Portal 2 - Game of the Year Edition"), QString()));
        CHECK(mergeKey(QStringLiteral("Portal"), QString()) != mergeKey(QStringLiteral("Portal 2"), QString()));
    }

    // ---- 6. pickAutoSource: NEVER returns a not-ready source ------------------------------------
    {
        // exactly one ready -> launch it
        QVector<PcGameSource> one{ src(PcGameSource::LauncherOwned, QStringLiteral("steam"), false),
                                   src(PcGameSource::Downloaded, QString(), true),
                                   src(PcGameSource::AddonAvailable, QString(), false) };
        CHECK(pickAutoSource(one) == 1);
        CHECK(one.value(pickAutoSource(one)).ready == true);

        // several ready -> ask
        QVector<PcGameSource> many{ src(PcGameSource::LauncherInstalled, QStringLiteral("steam"), true),
                                    src(PcGameSource::Downloaded, QString(), true) };
        CHECK(pickAutoSource(many) == -1);

        // none ready -> ask, NEVER auto-start a download
        QVector<PcGameSource> none{ src(PcGameSource::LauncherOwned, QStringLiteral("steam"), false),
                                    src(PcGameSource::AddonAvailable, QString(), false) };
        CHECK(pickAutoSource(none) == -1);

        // empty -> ask (and must not index out of range)
        CHECK(pickAutoSource({}) == -1);
    }

    if (failures == 0) { std::puts("PCGAMES-OK"); return 0; }
    std::fprintf(stderr, "PCGAMES: %d check(s) failed\n", failures);
    return 1;
}
