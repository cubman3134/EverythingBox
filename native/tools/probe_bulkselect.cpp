// Headless check of BulkSelect (src/core/BulkSelect.h) — the pure heart of bulk edit (issue #65): the
// multi-select set and the collision-safe reassign-target-path resolver. QtCore-only, header-only, so it runs
// under the offscreen QPA and links nothing but Qt6::Core. It pins:
//
//   * Selection invariants a grid overlay leans on — toggle is its own inverse; selectAll then clear is
//     empty; selectAll selects exactly the universe; invert is the complement within the universe; invert
//     twice is identity; invert drops any stray index not in the universe; selected() is ascending;
//   * reassignTargetPath EXHAUSTIVELY — the fresh-name destination is <root>/<folder>/<file>; a taken
//     destination yields a "(2)" name, NEVER the taken candidate (the ROM-safety assertion); a second taken
//     name walks to "(3)"; a game already in the target folder is a no-op (returns its own path); a name with
//     spaces and parentheses keeps its extension and its punctuation; a dotless name gets "(2)" with no dot;
//     an empty source is a skip; a fully-saturated destination is a skip (never an overwrite).
//
// Every expected value is an INDEPENDENT ORACLE — a hand-written literal or a hand-built QSet — never the
// output of the function under test, so a fixture cannot be a fixed point of the code it checks. The
// "destination taken" predicate is a QSet<QString> the probe fills, so the branches are driven with no disk.
//
// Prints BULKSELECT-OK on success; any failure prints BULKSELECT-FAIL <cond> (line) and exits non-zero.
#include "BulkSelect.h"

#include <QCoreApplication>
#include <QSet>
#include <QString>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "BULKSELECT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using BulkSelect::Selection;

// A "destination taken" predicate backed by a fixed set of absolute paths the probe declares by hand. Passed
// by value into reassignTargetPath as std::function<bool(const QString&)>.
static std::function<bool(const QString&)> takenBy(const QSet<QString>& set)
{
    return [set](const QString& p) { return set.contains(p); };
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Selection: the multi-select set invariants ------------------------------------------------------
    {
        Selection s;
        CHECK(s.count() == 0);
        CHECK(s.isEmpty());
        CHECK(!s.isSelected(3));

        // toggle adds, and is its own inverse.
        s.toggle(3);
        CHECK(s.isSelected(3));
        CHECK(s.count() == 1);
        s.toggle(3);
        CHECK(!s.isSelected(3));   // toggled back off — its own inverse
        CHECK(s.count() == 0);

        s.toggle(1);
        s.toggle(4);
        s.toggle(1);               // 1 off again
        CHECK(!s.isSelected(1));
        CHECK(s.isSelected(4));
        CHECK(s.count() == 1);
    }

    const QVector<int> universe{0, 1, 2, 3, 4};

    {
        // selectAll picks exactly the universe; clear empties it.
        Selection s;
        s.selectAll(universe);
        CHECK(s.count() == 5);
        for (int i : universe) CHECK(s.isSelected(i));
        CHECK(!s.isSelected(5));   // nothing outside the universe
        s.clear();
        CHECK(s.count() == 0);
        CHECK(s.isEmpty());
    }

    {
        // invert is the complement within the universe.
        Selection s;
        s.toggle(1);
        s.toggle(3);
        s.invert(universe);        // {1,3} -> {0,2,4}
        CHECK(s.isSelected(0));
        CHECK(!s.isSelected(1));
        CHECK(s.isSelected(2));
        CHECK(!s.isSelected(3));
        CHECK(s.isSelected(4));
        CHECK(s.count() == 3);

        // invert twice is identity.
        s.invert(universe);        // back to {1,3}
        CHECK(s.isSelected(1));
        CHECK(s.isSelected(3));
        CHECK(!s.isSelected(0));
        CHECK(!s.isSelected(2));
        CHECK(!s.isSelected(4));
        CHECK(s.count() == 2);
    }

    {
        // invert restricted to the universe drops a stray index the set holds but the universe does not, and
        // never introduces one outside it. (Guards the "always a subset of the universe" property that makes
        // the double-invert identity hold in the overlay after a level with fewer items.)
        Selection s;
        s.toggle(99);              // not in `universe`
        s.toggle(2);               // in `universe`
        s.invert(universe);        // complement of {2} within {0..4} == {0,1,3,4}; 99 dropped
        CHECK(s.count() == 4);
        CHECK(!s.isSelected(99));
        CHECK(!s.isSelected(2));
        CHECK(s.isSelected(0) && s.isSelected(1) && s.isSelected(3) && s.isSelected(4));
    }

    {
        // selected() is ascending regardless of toggle order.
        Selection s;
        s.toggle(4);
        s.toggle(0);
        s.toggle(2);
        const QVector<int> got = s.selected();
        const QVector<int> want{0, 2, 4};   // independent oracle
        CHECK(got == want);
    }

    // ---- reassignTargetPath: exhaustive, disk-free --------------------------------------------------------
    const QString root = QStringLiteral("C:/roms");

    {
        // Fresh destination: <root>/<folder>/<file>. Nothing taken.
        const QString cur = QStringLiteral("C:/roms/genesis/Sonic.zip");
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("megadrive"), cur, takenBy({}));
        CHECK(got == QStringLiteral("C:/roms/megadrive/Sonic.zip"));
    }

    {
        // Collision: the obvious destination is taken by a DIFFERENT file. Must yield "(2)", and MUST NOT be
        // the taken candidate. This is the ROM-safety assertion.
        const QString cur   = QStringLiteral("C:/roms/genesis/Sonic.zip");
        const QString taken = QStringLiteral("C:/roms/megadrive/Sonic.zip");
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("megadrive"), cur, takenBy({taken}));
        CHECK(got == QStringLiteral("C:/roms/megadrive/Sonic (2).zip"));
        CHECK(got != taken);   // never the file we would clobber
    }

    {
        // Two taken names walk to "(3)".
        const QString cur = QStringLiteral("C:/roms/genesis/Sonic.zip");
        const QSet<QString> taken{
            QStringLiteral("C:/roms/megadrive/Sonic.zip"),
            QStringLiteral("C:/roms/megadrive/Sonic (2).zip")};
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("megadrive"), cur, takenBy(taken));
        CHECK(got == QStringLiteral("C:/roms/megadrive/Sonic (3).zip"));
    }

    {
        // Already in the target folder: no-op, returns its own (cleaned) path.
        const QString cur = QStringLiteral("C:/roms/megadrive/Sonic.zip");
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("megadrive"), cur, takenBy({}));
        CHECK(got == cur);   // == currentPath signals no-op
    }

    {
        // Already-in-folder is a no-op even though a same-named file "exists" there (it IS this file). Must
        // not be mistaken for a collision and bumped to "(2)".
        const QString cur   = QStringLiteral("C:/roms/megadrive/Sonic.zip");
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("megadrive"), cur, takenBy({cur}));
        CHECK(got == cur);
    }

    {
        // A name with spaces and parentheses keeps BOTH its punctuation and its extension across the "(2)"
        // derivation (QFileInfo splits on the last dot only).
        const QString cur   = QStringLiteral("C:/roms/genesis/Sonic The Hedgehog (USA, Europe).md");
        const QString taken = QStringLiteral("C:/roms/megadrive/Sonic The Hedgehog (USA, Europe).md");
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("megadrive"), cur, takenBy({taken}));
        CHECK(got == QStringLiteral("C:/roms/megadrive/Sonic The Hedgehog (USA, Europe) (2).md"));
    }

    {
        // A dotless name gets "(2)" with no trailing dot.
        const QString cur   = QStringLiteral("C:/roms/arcade/mslug");
        const QString taken = QStringLiteral("C:/roms/fbneo/mslug");
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("fbneo"), cur, takenBy({taken}));
        CHECK(got == QStringLiteral("C:/roms/fbneo/mslug (2)"));
    }

    {
        // Empty source -> skip.
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("megadrive"), QString(), takenBy({}));
        CHECK(got.isEmpty());
    }

    {
        // Fully saturated destination -> skip (never overwrite). A predicate that reports EVERYTHING taken
        // must never return a taken path; it returns empty.
        const QString cur = QStringLiteral("C:/roms/genesis/Sonic.zip");
        const QString got = BulkSelect::reassignTargetPath(root, QStringLiteral("megadrive"), cur,
                                                           [](const QString&) { return true; });
        CHECK(got.isEmpty());
    }

    if (failures == 0) { std::printf("BULKSELECT-OK\n"); return 0; }
    std::fprintf(stderr, "BULKSELECT had %d failure(s)\n", failures);
    return 1;
}
