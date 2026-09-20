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

    // ---- 8. the TOSEC dialect (issue #190, item 4) ------------------------------------------------------
    // "(Disk 1 of 3)" and "(Side A)" are how TOSEC names a computer set. They group ONLY in the Computer
    // dialect, which a system opts into through its recipe — so every assertion here comes in pairs: what
    // the Computer dialect does, and the fact that the Console default still does exactly what it did.
    CHECK(DiscGroup::discNumber("Game (Disk 1 of 3).adf", DiscGroup::Dialect::Computer) == 1,
          "(Disk 1 of 3) detected in the Computer dialect");
    CHECK(DiscGroup::discNumber("Game (Disc 2 of 2).adf", DiscGroup::Dialect::Computer) == 2,
          "(Disc 2 of 2) detected in the Computer dialect");
    CHECK(DiscGroup::discNumber("Game (disk 3 OF 4).d64", DiscGroup::Dialect::Computer) == 3,
          "the 'of' form is case-insensitive");
    // THE GATE. Same names, default (Console) dialect: unchanged, which is why a console set cannot start
    // grouping differently because this feature exists.
    CHECK(DiscGroup::discNumber("Game (Disk 1 of 3).adf") == 0,
          "(Disk 1 of 3) is NOT a disc tag in the Console default");
    CHECK(DiscGroup::discNumber("Game (Disc 2 of 2).adf", DiscGroup::Dialect::Console) == 0,
          "the Console dialect refuses the 'of' form explicitly too");
    // The #49 forms still read identically in BOTH dialects.
    CHECK(DiscGroup::discNumber("Game (Disc 1).chd", DiscGroup::Dialect::Computer) == 1,
          "the console (Disc N) form still works in the Computer dialect");
    CHECK(DiscGroup::discNumber("Game (CD 3).chd", DiscGroup::Dialect::Computer) == 3,
          "(CD N) still works in the Computer dialect");

    // Side tags: letters map to their ordinal, and only in the Computer dialect.
    CHECK(DiscGroup::sideOrdinal("Game (Side A).adf", DiscGroup::Dialect::Computer) == 1, "(Side A) is side 1");
    CHECK(DiscGroup::sideOrdinal("Game (Side B).adf", DiscGroup::Dialect::Computer) == 2, "(Side B) is side 2");
    CHECK(DiscGroup::sideOrdinal("Game (side b).d64", DiscGroup::Dialect::Computer) == 2,
          "side detection is case-insensitive");
    CHECK(DiscGroup::sideOrdinal("Game (Side A).adf") == 0, "a side tag is nothing in the Console default");
    CHECK(DiscGroup::sideOrdinal("Sonic (USA).md", DiscGroup::Dialect::Computer) == 0,
          "a region tag is not a side tag");

    // ---- 9. a TOSEC two-disk set groups, in disk order ---------------------------------------------------
    {
        const QVector<QString> in = {
            "C:/roms/amiga/Monkey Island (1990) (Lucasfilm) (Disk 2 of 2).adf",
            "C:/roms/amiga/Monkey Island (1990) (Lucasfilm) (Disk 1 of 2).adf",
        };
        const auto sets = DiscGroup::groupDiscs(in, DiscGroup::Dialect::Computer);
        const auto* mi = setNamed(sets, "Monkey Island");
        CHECK(sets.size() == 1, "a TOSEC two-disk set collapses to one set");
        CHECK(mi && mi->isMultiDisc && mi->members.size() == 2, "the set is multi-disc with both members");
        // Hand-written expected order: disk 1 then disk 2, independent of the input order above.
        CHECK(mi && mi->members.size() == 2
                 && mi->members[0] == "C:/roms/amiga/Monkey Island (1990) (Lucasfilm) (Disk 1 of 2).adf"
                 && mi->members[1] == "C:/roms/amiga/Monkey Island (1990) (Lucasfilm) (Disk 2 of 2).adf",
              "TOSEC members ordered disk 1 then disk 2");
        CHECK(mi && mi->cleanTitle == "Monkey Island", "year/publisher/disk tags all stripped from the title");
        // THE GATE again, at the grouping level: the same files on a console system stay two entries.
        const auto consoleSets = DiscGroup::groupDiscs(in);
        CHECK(consoleSets.size() == 2, "the same names on a console system are still two separate entries");
        CHECK(consoleSets.size() == 2 && !consoleSets[0].isMultiDisc && !consoleSets[1].isMultiDisc,
              "and neither of them is a multi-disc set");
    }

    // ---- 10. two SIDES of one disk are ONE entry, ordered A then B --------------------------------------
    {
        const QVector<QString> in = {
            "C:/roms/c64/Maniac Mansion (1987) (Lucasfilm) (Side B).d64",
            "C:/roms/c64/Maniac Mansion (1987) (Lucasfilm) (Side A).d64",
        };
        const auto sets = DiscGroup::groupDiscs(in, DiscGroup::Dialect::Computer);
        const auto* mm = setNamed(sets, "Maniac Mansion");
        CHECK(sets.size() == 1 && mm && mm->isMultiDisc && mm->members.size() == 2,
              "Side A + Side B of one disk are one entry");
        CHECK(mm && mm->members.size() == 2
                 && mm->members[0].endsWith("(Side A).d64")
                 && mm->members[1].endsWith("(Side B).d64"),
              "sides ordered A then B, regardless of input order");
    }

    // ---- 11. disks AND sides order disk 1 side A, disk 1 side B, disk 2 side A … ------------------------
    {
        const QVector<QString> in = {
            "C:/roms/apple2/Ultima IV (Disk 2 of 2) (Side A).dsk",
            "C:/roms/apple2/Ultima IV (Disk 1 of 2) (Side B).dsk",
            "C:/roms/apple2/Ultima IV (Disk 2 of 2) (Side B).dsk",
            "C:/roms/apple2/Ultima IV (Disk 1 of 2) (Side A).dsk",
        };
        const auto sets = DiscGroup::groupDiscs(in, DiscGroup::Dialect::Computer);
        const auto* u4 = setNamed(sets, "Ultima IV");
        CHECK(sets.size() == 1 && u4 && u4->members.size() == 4, "four side-files are one four-member set");
        // The whole expected order, hand-written.
        CHECK(u4 && u4->members.size() == 4
                 && u4->members[0] == "C:/roms/apple2/Ultima IV (Disk 1 of 2) (Side A).dsk"
                 && u4->members[1] == "C:/roms/apple2/Ultima IV (Disk 1 of 2) (Side B).dsk"
                 && u4->members[2] == "C:/roms/apple2/Ultima IV (Disk 2 of 2) (Side A).dsk"
                 && u4->members[3] == "C:/roms/apple2/Ultima IV (Disk 2 of 2) (Side B).dsk",
              "ordered disk 1 side A, disk 1 side B, disk 2 side A, disk 2 side B");
    }

    // ---- 12. a lone computer title is still its own single, non-multi entry ------------------------------
    {
        const QVector<QString> in = {
            "C:/roms/amiga/Another World (1991) (Delphine).adf",
            "C:/roms/amiga/Shadow of the Beast (1989) (Psygnosis).adf",
        };
        const auto sets = DiscGroup::groupDiscs(in, DiscGroup::Dialect::Computer);
        CHECK(sets.size() == 2, "two untagged computer titles stay two entries in the Computer dialect");
        for (const auto& s : sets)
            CHECK(!s.isMultiDisc && s.members.size() == 1, "each is a single, non-multi set");
    }

    if (fails == 0) printf("DISCGROUP-OK\n");
    return fails == 0 ? 0 : 1;
}
