// Headless test for DiscGroup (issue #49): disc-tag detection across the four spellings, disc ordering,
// grouping mixed formats by normalised title, region+disc tags both stripped, and the generated .m3u body.
// Expected groupings are hand-written here — never computed by calling the function under test — so a
// fixture cannot become a fixed point of the code it is meant to check. Prints DISCGROUP-OK when all hold.
#include <QCoreApplication>
#include <QString>
#include <QVector>
#include "../src/core/DiscGroup.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

// Find the one set whose cleanTitle matches (case-insensitive); returns nullptr if not exactly one.
static const DiscGroup::DiscSet* setNamed(const QVector<DiscGroup::DiscSet>& sets, const QString& title)
{
    const DiscGroup::DiscSet* found = nullptr;
    for (const auto& s : sets)
        if (s.cleanTitle.compare(title, Qt::CaseInsensitive) == 0)
        {
            if (found) return nullptr; // ambiguous — two sets share the title
            found = &s;
        }
    return found;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. disc-tag detection: the four spellings, case-insensitive, either bracket ---------------------
    CHECK(DiscGroup::discNumber("Game (Disc 1).chd") == 1, "(Disc N) detected");
    CHECK(DiscGroup::discNumber("Game (Disk 2).cue") == 2, "(Disk N) detected");
    CHECK(DiscGroup::discNumber("Game (CD 3).chd")  == 3, "(CD N) detected");
    CHECK(DiscGroup::discNumber("Game [CD 4].chd")  == 4, "[CD N] detected");
    CHECK(DiscGroup::discNumber("Game (disc 5).chd") == 5, "detection is case-insensitive");
    CHECK(DiscGroup::discNumber("Game (DISC 12).chd") == 12, "multi-digit disc number");
    // Absence: a plain game and a non-disc parenthetical tag both read as 0 (not a disc member).
    CHECK(DiscGroup::discNumber("Sonic (USA).md") == 0, "a region tag is not a disc tag");
    CHECK(DiscGroup::discNumber("Plain Game.iso") == 0, "no tag at all is 0");

    // ---- 2. a 3-disc set groups into ONE set with 3 members, ordered by disc number ---------------------
    // Deliberately hand the input DISC 3, DISC 1, DISC 2 out of order to prove ordering isn't filesystem order.
    {
        const QVector<QString> in = {
            "C:/roms/psx/Final Fantasy VII (Disc 3) (USA).chd",
            "C:/roms/psx/Final Fantasy VII (Disc 1) (USA).chd",
            "C:/roms/psx/Final Fantasy VII (Disc 2) (USA).chd",
        };
        const auto sets = DiscGroup::groupDiscs(in);
        const auto* ff = setNamed(sets, "Final Fantasy VII");
        CHECK(sets.size() == 1, "3 discs collapse to exactly one set");
        CHECK(ff && ff->isMultiDisc, "the set is multi-disc");
        CHECK(ff && ff->members.size() == 3, "the set has 3 members");
        // The expected ORDER is hand-written (disc 1, then 2, then 3), independent of the input order above.
        CHECK(ff && ff->members.size() == 3
                 && ff->members[0] == "C:/roms/psx/Final Fantasy VII (Disc 1) (USA).chd"
                 && ff->members[1] == "C:/roms/psx/Final Fantasy VII (Disc 2) (USA).chd"
                 && ff->members[2] == "C:/roms/psx/Final Fantasy VII (Disc 3) (USA).chd",
              "members ordered disc 1,2,3 regardless of input order");
        // The region tag AND the disc tag are both stripped from the title.
        CHECK(ff && ff->cleanTitle == "Final Fantasy VII", "region+disc tags both stripped from cleanTitle");
    }

    // ---- 3. mixed formats within one set group by TITLE, not extension ----------------------------------
    {
        const QVector<QString> in = {
            "C:/roms/psx/Parasite Eve (Disc 1).chd",
            "C:/roms/psx/Parasite Eve (Disc 2).cue",   // different extension, same game
        };
        const auto sets = DiscGroup::groupDiscs(in);
        const auto* pe = setNamed(sets, "Parasite Eve");
        CHECK(sets.size() == 1, "mixed .chd + .cue collapse to one set");
        CHECK(pe && pe->isMultiDisc && pe->members.size() == 2, "both formats are members of the set");
        CHECK(pe && pe->members[0].endsWith(".chd") && pe->members[1].endsWith(".cue"),
              "mixed-format members ordered by disc number, keeping their own extensions");
    }

    // ---- 4. region + disc tag both stripped so (Disc 1)(USA)/(Disc 2)(USA) group ------------------------
    {
        const QVector<QString> in = {
            "Metal Gear Solid (Disc 1)(USA).chd",   // no space between the two tags
            "Metal Gear Solid (Disc 2)(USA).chd",
        };
        const auto sets = DiscGroup::groupDiscs(in);
        const auto* mgs = setNamed(sets, "Metal Gear Solid");
        CHECK(sets.size() == 1 && mgs && mgs->isMultiDisc && mgs->members.size() == 2,
              "adjacent (Disc N)(USA) tags group into one set");
    }

    // ---- 5. a lone game (no disc tag) stays a single-member, non-multi set ------------------------------
    // AND two region variants of the SAME base title must NOT merge — disc-number 0 never groups.
    {
        const QVector<QString> in = {
            "C:/roms/md/Sonic (USA).md",
            "C:/roms/md/Sonic (Europe).md",   // normalises to the same key, but is a different game
        };
        const auto sets = DiscGroup::groupDiscs(in);
        CHECK(sets.size() == 2, "two region variants stay two separate games, not one set");
        for (const auto& s : sets)
            CHECK(!s.isMultiDisc && s.members.size() == 1, "each lone game is a single non-multi set");
    }

    // ---- 6. a set with only ONE disc present is single-member / not multi-disc --------------------------
    {
        const QVector<QString> in = { "C:/roms/psx/Chrono Cross (Disc 1).chd" };
        const auto sets = DiscGroup::groupDiscs(in);
        CHECK(sets.size() == 1 && !sets[0].isMultiDisc && sets[0].members.size() == 1,
              "a single disc present is not treated as a multi-disc set");
    }

    // ---- 7. the generated .m3u body lists members in disc order, one absolute path per line -------------
    {
        DiscGroup::DiscSet s;
        s.cleanTitle = "Final Fantasy VII";
        s.isMultiDisc = true;
        s.members = {
            "C:/roms/psx/Final Fantasy VII (Disc 1) (USA).chd",
            "C:/roms/psx/Final Fantasy VII (Disc 2) (USA).chd",
        };
        const QString body = DiscGroup::m3uContentFor(s);
        // Expected body hand-written: the two paths, disc order, each on its own line, trailing newline.
        const QString expected =
            "C:/roms/psx/Final Fantasy VII (Disc 1) (USA).chd\n"
            "C:/roms/psx/Final Fantasy VII (Disc 2) (USA).chd\n";
        CHECK(body == expected, "m3u body is member paths in order, one per line, trailing newline");
        CHECK(body.count('\n') == 2, "m3u body has exactly one line per member");
    }

    if (fails == 0) printf("DISCGROUP-OK\n");
    return fails == 0 ? 0 : 1;
}
