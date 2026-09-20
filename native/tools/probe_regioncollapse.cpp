// Headless test for RegionCollapse (issue #50): region ranking against an ordered priority, revision
// tie-breaking, region-less fallback, same-title grouping (and NON-grouping of different games), the winner
// pick, the exact loser set, and the per-language default priority. Every expected value is HAND-AUTHORED
// here — never computed by calling the function under test — so a fixture cannot become a fixed point of the
// code it checks. Prints REGIONCOLLAPSE-OK when all hold.
#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>
#include "../src/core/FormatCollapse.h"
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

    // ---- 9. FORMAT collapse (issue #190, item 2) --------------------------------------------------------
    // The same title present in several interchangeable formats is ONE entry on the best-ranked format, with
    // the others carried as its alternates. Expected winners/alternates are hand-authored here, never
    // computed by calling the function under test. The ranking is the shipped Amiga one, written out in full
    // so the expectations below do not depend on what the recipe file happens to say.
    const QStringList amigaFmt = { QStringLiteral("lha"), QStringLiteral("hdf"), QStringLiteral("adf"),
                                   QStringLiteral("adz"), QStringLiteral("dms") };

    CHECK(FormatCollapse::formatOf("C:/roms/amiga/Lemmings.LHA") == "lha", "formatOf lowercases the extension");
    CHECK(FormatCollapse::formatRank("Lemmings.lha", amigaFmt) == 0, "lha is the top-ranked Amiga format");
    CHECK(FormatCollapse::formatRank("Lemmings.adf", amigaFmt) == 2, "adf ranks third under lha>hdf>adf>adz>dms");
    CHECK(FormatCollapse::formatRank("Lemmings.ipf", amigaFmt) == -1, "an unlisted format ranks -1, not last");

    // 9a — two formats of one title: one entry, on the .lha, with the .adf as an alternate.
    {
        const QVector<QString> in = {
            QStringLiteral("C:/roms/amiga/Lemmings.adf"),
            QStringLiteral("C:/roms/amiga/Lemmings.lha"),
        };
        const auto gs = FormatCollapse::collapseByFormat(in, amigaFmt);
        CHECK(gs.size() == 1, "Game.lha + Game.adf collapse to exactly one entry");
        CHECK(gs.size() == 1 && gs[0].chosenPath == "C:/roms/amiga/Lemmings.lha", "the .lha wins");
        CHECK(gs.size() == 1 && gs[0].chosenFormat == "lha", "the entry reports the format it chose");
        CHECK(gs.size() == 1 && gs[0].alternates.size() == 1
                 && gs[0].alternates[0] == "C:/roms/amiga/Lemmings.adf",
              "the .adf is remembered as an alternate, not listed as its own entry");
        CHECK(gs.size() == 1 && gs[0].chosenTitle == "Lemmings", "the entry keeps the readable title");
    }

    // 9b — the RANKING is respected across three formats, and the losers come back best-ranked first.
    {
        const QVector<QString> in = {
            QStringLiteral("C:/roms/amiga/Turrican.dms"),
            QStringLiteral("C:/roms/amiga/Turrican.adf"),
            QStringLiteral("C:/roms/amiga/Turrican.hdf"),
        };
        const auto gs = FormatCollapse::collapseByFormat(in, amigaFmt);
        CHECK(gs.size() == 1 && gs[0].chosenPath == "C:/roms/amiga/Turrican.hdf",
              "with no .lha present the .hdf wins over .adf and .dms");
        const QVector<QString> expectedAlts = { QStringLiteral("C:/roms/amiga/Turrican.adf"),
                                                QStringLiteral("C:/roms/amiga/Turrican.dms") };
        CHECK(gs.size() == 1 && gs[0].alternates == expectedAlts,
              "alternates are ordered by the ranking (adf before dms), hand-authored");
    }

    // 9c — a format the ranking does not list keeps TODAY's behaviour: its own entry, untouched.
    {
        const QVector<QString> in = {
            QStringLiteral("C:/roms/amiga/Rick Dangerous.lha"),
            QStringLiteral("C:/roms/amiga/Rick Dangerous.ipf"),   // preservation dump: not in the ranking
        };
        const auto gs = FormatCollapse::collapseByFormat(in, amigaFmt);
        CHECK(gs.size() == 2, "an unlisted format stays a separate entry");
        bool ipfAlone = false;
        for (const auto& g : gs)
            if (g.chosenPath.endsWith(".ipf")) ipfAlone = g.alternates.isEmpty();
        CHECK(ipfAlone, "the unlisted file is its own entry with no alternates");
    }

    // 9d — two genuinely different titles never merge, however they are ranked.
    {
        const QVector<QString> in = {
            QStringLiteral("C:/roms/amiga/Lemmings.lha"),
            QStringLiteral("C:/roms/amiga/Worms.adf"),
        };
        const auto gs = FormatCollapse::collapseByFormat(in, amigaFmt);
        CHECK(gs.size() == 2, "two different games stay two entries");
        CHECK(gs.size() == 2 && gs[0].alternates.isEmpty() && gs[1].alternates.isEmpty(),
              "neither game claims the other as an alternate");
    }

    // 9e — two files of the SAME listed format are a REGION question, not a format one: they stay separate
    // here and are left to collapseByRegion (which is off by default and asks the user's language).
    {
        const QVector<QString> in = {
            QStringLiteral("C:/roms/amiga/Zool (Europe).adf"),
            QStringLiteral("C:/roms/amiga/Zool (USA).adf"),
        };
        const auto gs = FormatCollapse::collapseByFormat(in, amigaFmt);
        CHECK(gs.size() == 2, "same-format region variants are not collapsed by the FORMAT pass");
    }

    // 9f — an EMPTY ranking (every console, and any computer whose recipe says nothing about formats) passes
    // the list through unchanged. This is the safety property that lets the pass run for every system.
    {
        const QVector<QString> in = {
            QStringLiteral("C:/roms/snes/Zelda.sfc"),
            QStringLiteral("C:/roms/snes/Zelda.smc"),
        };
        const auto gs = FormatCollapse::collapseByFormat(in, {});
        CHECK(gs.size() == 2, "an empty ranking collapses nothing");
        CHECK(gs.size() == 2 && gs[0].alternates.isEmpty() && gs[1].alternates.isEmpty(),
              "an empty ranking produces no alternates either");
    }

    // 9g — region tags still collapse as they do today: the FORMAT pass leaves #50's answer alone. The same
    // three files, through collapseByRegion with a hand-written priority, still yield one winner.
    {
        const QVector<QString> in = {
            QStringLiteral("C:/roms/amiga/Zool (Japan).adf"),
            QStringLiteral("C:/roms/amiga/Zool (USA).adf"),
            QStringLiteral("C:/roms/amiga/Zool (Europe).adf"),
        };
        const auto gs = RegionCollapse::collapseByRegion(in, prio);
        CHECK(gs.size() == 1 && gs[0].chosenPath == "C:/roms/amiga/Zool (USA).adf",
              "region collapse is unchanged by the format pass");
    }

    // 9h — the two passes COMPOSE in the order the scan runs them: format first (the .lha beats the disk
    // set), then whatever is left. A TOSEC two-disk .adf set plus the same title as one .lha is one entry.
    {
        const QVector<QString> in = {
            QStringLiteral("C:/roms/amiga/Lemmings (1991) (Psygnosis) (Disk 1 of 2).adf"),
            QStringLiteral("C:/roms/amiga/Lemmings (1991) (Psygnosis) (Disk 2 of 2).adf"),
            QStringLiteral("C:/roms/amiga/Lemmings (1991) (Psygnosis).lha"),
        };
        const auto gs = FormatCollapse::collapseByFormat(in, amigaFmt);
        CHECK(gs.size() == 1 && gs[0].chosenFormat == "lha",
              "a WHDLoad .lha beats the same title's two-disk .adf set");
        CHECK(gs.size() == 1 && gs[0].alternates.size() == 2,
              "both disks of the beaten set are remembered as alternates");
    }

    if (fails == 0) printf("REGIONCOLLAPSE-OK\n");
    return fails == 0 ? 0 : 1;
}
