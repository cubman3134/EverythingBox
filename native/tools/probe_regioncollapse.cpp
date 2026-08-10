// Headless test for RegionCollapse (issue #50): region ranking against an ordered priority, revision
// tie-breaking, region-less fallback, same-title grouping (and NON-grouping of different games), the winner
// pick, the exact loser set, and the per-language default priority. Every expected value is HAND-AUTHORED
// here — never computed by calling the function under test — so a fixture cannot become a fixed point of the
// code it checks. Prints REGIONCOLLAPSE-OK when all hold.
#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>
#include "../src/core/RegionCollapse.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

// The one group whose chosen title matches (case-insensitive); nullptr if not exactly one.
static const RegionCollapse::RegionGroup* groupNamed(const QVector<RegionCollapse::RegionGroup>& gs, const QString& title)
{
    const RegionCollapse::RegionGroup* found = nullptr;
    for (const auto& g : gs)
        if (g.chosenTitle.compare(title, Qt::CaseInsensitive) == 0)
        {
            if (found) return nullptr;
            found = &g;
        }
    return found;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // A fixed priority for the ranking tests, so expected indices are hand-known and independent of any locale
    // default. USA=0, Europe=1, Japan=2; a region not in this list ranks 3 (after all listed).
    const QStringList prio = { QStringLiteral("USA"), QStringLiteral("Europe"), QStringLiteral("Japan") };

    // ---- 1. regionRank honours the priority ORDER --------------------------------------------------------
    CHECK(RegionCollapse::regionRank("Game (USA).sfc", prio)    == 0, "USA ranks first under USA>Europe>Japan");
    CHECK(RegionCollapse::regionRank("Game (Europe).sfc", prio) == 1, "Europe ranks second");
    CHECK(RegionCollapse::regionRank("Game (Japan).sfc", prio)  == 2, "Japan ranks third");
    // The single-letter GoodTools codes map to the same canonical regions.
    CHECK(RegionCollapse::regionRank("Game (U).nes", prio) == 0, "(U) is USA");
    CHECK(RegionCollapse::regionRank("Game (E).nes", prio) == 1, "(E) is Europe");
    CHECK(RegionCollapse::regionRank("Game (J).nes", prio) == 2, "(J) is Japan");
    // A reversed priority flips the ranking — proves the index is read from `priority`, not baked in.
    const QStringList rev = { QStringLiteral("Japan"), QStringLiteral("Europe"), QStringLiteral("USA") };
    CHECK(RegionCollapse::regionRank("Game (Japan).sfc", rev) == 0, "Japan ranks first under Japan>Europe>USA");
    CHECK(RegionCollapse::regionRank("Game (USA).sfc", rev)   == 2, "USA ranks last under Japan>Europe>USA");
    // A multi-region tag ranks as its BEST member (min index): "(USA, Europe)" -> USA -> 0.
    CHECK(RegionCollapse::regionRank("Game (USA, Europe).sfc", prio) == 0, "multi-region ranks as its best member");

    // ---- 2. a region-less name (or an unlisted region) ranks AFTER all listed regions --------------------
    CHECK(RegionCollapse::regionRank("Plain Game.sfc", prio) == 3, "no region tag ranks after all listed (== size)");
    CHECK(RegionCollapse::regionRank("Game (Korea).sfc", prio) == 3, "a region absent from priority ranks after all listed");
    // A revision tag is NOT a region: "(Rev A)" must not read as Asia/Australia and pull a rank in.
    CHECK(RegionCollapse::regionRank("Game (Rev A).sfc", prio) == 3, "(Rev A) is not mistaken for a region");

    // ---- 3. revisionOf: numeric, letter->ordinal, version, absent ----------------------------------------
    CHECK(RegionCollapse::revisionOf("Game (Rev 2).sfc") == 2, "(Rev 2) -> 2");
    CHECK(RegionCollapse::revisionOf("Game (Rev A).sfc") == 1, "(Rev A) -> 1 (ordinal)");
    CHECK(RegionCollapse::revisionOf("Game (Rev B).sfc") == 2, "(Rev B) -> 2 (ordinal)");
    CHECK(RegionCollapse::revisionOf("Game (v1.1).sfc")  == 1001, "(v1.1) -> 1001");
    CHECK(RegionCollapse::revisionOf("Game (v1.0).sfc")  == 1000, "(v1.0) -> 1000");
    CHECK(RegionCollapse::revisionOf("Game (USA).sfc")   == 0, "no revision -> 0");
    CHECK(RegionCollapse::revisionOf("Game (v1.1).sfc") > RegionCollapse::revisionOf("Game (v1.0).sfc"),
          "a higher version is a higher revision");

    // ---- 4. collapseByRegion: the highest-priority region WINS -------------------------------------------
    {
        const QVector<QString> in = {
            "C:/roms/snes/Chrono Trigger (Japan).sfc",
            "C:/roms/snes/Chrono Trigger (USA).sfc",
            "C:/roms/snes/Chrono Trigger (Europe).sfc",
        };
        const auto gs = RegionCollapse::collapseByRegion(in, prio);
        const auto* g = groupNamed(gs, "Chrono Trigger");
        CHECK(gs.size() == 1, "three region variants collapse to one group");
        // Winner is hand-known: USA is first in `prio`, so the USA file wins regardless of input order.
        CHECK(g && g->chosenPath == "C:/roms/snes/Chrono Trigger (USA).sfc", "USA variant wins under USA-first priority");
        // otherVersions holds EXACTLY the two losers, ranked (Europe before Japan under this priority).
        CHECK(g && g->otherVersions.size() == 2, "two losers recorded");
        CHECK(g && g->otherVersions.size() == 2
                 && g->otherVersions[0] == "C:/roms/snes/Chrono Trigger (Europe).sfc"
                 && g->otherVersions[1] == "C:/roms/snes/Chrono Trigger (Japan).sfc",
              "losers are Europe then Japan, in ranked order");
        // Flip the priority: Japan-first must change the winner to the Japan file.
        const auto gs2 = RegionCollapse::collapseByRegion(in, rev);
        const auto* g2 = groupNamed(gs2, "Chrono Trigger");
        CHECK(g2 && g2->chosenPath == "C:/roms/snes/Chrono Trigger (Japan).sfc", "Japan wins under Japan-first priority");
    }

    // ---- 5. revision breaks a tie WITHIN the same region -------------------------------------------------
    {
        const QVector<QString> in = {
            "C:/roms/nes/Zelda (USA).nes",
            "C:/roms/nes/Zelda (USA) (Rev 1).nes",
        };
        const auto gs = RegionCollapse::collapseByRegion(in, prio);
        const auto* g = groupNamed(gs, "Zelda");
        CHECK(gs.size() == 1, "same-region variants collapse to one group");
        CHECK(g && g->chosenPath == "C:/roms/nes/Zelda (USA) (Rev 1).nes", "highest revision wins a same-region tie");
        CHECK(g && g->otherVersions.size() == 1 && g->otherVersions[0] == "C:/roms/nes/Zelda (USA).nes",
              "the older revision is the sole loser");
    }

    // ---- 6. a region-less name is a CANDIDATE but loses to a listed region -------------------------------
    {
        const QVector<QString> in = {
            "C:/roms/gb/Tetris.gb",            // no region tag at all
            "C:/roms/gb/Tetris (USA).gb",
        };
        const auto gs = RegionCollapse::collapseByRegion(in, prio);
        const auto* g = groupNamed(gs, "Tetris");
        CHECK(gs.size() == 1, "a region-less file groups with its region-tagged sibling");
        CHECK(g && g->chosenPath == "C:/roms/gb/Tetris (USA).gb", "the USA variant beats the region-less one");
        CHECK(g && g->otherVersions.size() == 1 && g->otherVersions[0] == "C:/roms/gb/Tetris.gb",
              "the region-less file is kept as a loser, not dropped");
    }
    // A group of ONLY region-less files still yields one winner (deterministic path order) and no losers when single.
    {
        const QVector<QString> in = { "C:/roms/gb/Solo Game.gb" };
        const auto gs = RegionCollapse::collapseByRegion(in, prio);
        CHECK(gs.size() == 1 && gs[0].otherVersions.isEmpty(), "a single file is its own winner with no other versions");
    }

    // ---- 7. grouping matches ONLY same-title files — two different games never merge ---------------------
    {
        const QVector<QString> in = {
            "C:/roms/md/Sonic (USA).md",
            "C:/roms/md/Sonic (Europe).md",     // same game, different region -> ONE group
            "C:/roms/md/Streets of Rage (USA).md", // a different game entirely -> its OWN group
        };
        const auto gs = RegionCollapse::collapseByRegion(in, prio);
        CHECK(gs.size() == 2, "two distinct titles yield two groups, not one");
        const auto* sonic = groupNamed(gs, "Sonic");
        const auto* sor   = groupNamed(gs, "Streets of Rage");
        CHECK(sonic && sonic->otherVersions.size() == 1, "Sonic's two regions collapse to one entry + one other version");
        CHECK(sor && sor->otherVersions.isEmpty(), "the unrelated game stays a lone entry");
    }

    // ---- 8. defaultPriority returns the DOCUMENTED order per language ------------------------------------
    // Hand-authored expected orders (see RegionCollapse.h's documented mapping).
    {
        const QStringList en = RegionCollapse::defaultPriority("en_US");
        const QStringList enExpect = { "USA", "World", "Europe", "Japan" };
        CHECK(en == enExpect, "English default is USA,World,Europe,Japan");
        // A bare code and an unknown language both fall to the English default.
        CHECK(RegionCollapse::defaultPriority("en") == enExpect, "bare \"en\" uses the same default");
        CHECK(RegionCollapse::defaultPriority("xx") == enExpect, "an unknown language falls back to the US-first default");

        const QStringList ja = RegionCollapse::defaultPriority("ja");
        const QStringList jaExpect = { "Japan", "World", "USA", "Europe" };
        CHECK(ja == jaExpect, "Japanese default leads with Japan");

        const QStringList de = RegionCollapse::defaultPriority("de-DE");
        const QStringList deExpect = { "Europe", "Germany", "World", "USA", "Japan" };
        CHECK(de == deExpect, "German default leads with Europe then Germany");
    }

    if (fails == 0) printf("REGIONCOLLAPSE-OK\n");
    return fails == 0 ? 0 : 1;
}
