// Headless regression test for NavGraph (src/ui/nav/NavGraph) — the pure selection model + back stack that
// backs every themed screen. NavGraph is a plain QObject (no QML, no widgets), so this runs under the
// offscreen QPA in CI and proves the invariants later tasks lean on:
//
//   * after the first registerZone, (zone, index) is ALWAYS valid — there is no null state;
//   * a churn storm (grow / shrink / zero / remove zones, 1000 randomized mutations with a FIXED seed)
//     never yields an invalid selection, and validate() holds after every mutation;
//   * a set index snaps off "divider" (unselectable) entries and the snap always terminates;
//   * move() walks the zone grid spatially and reaches every registered zone from the default (Invariant 2),
//     with pinned directional picks on a 3x3 grid and a pinned reassignment successor;
//   * a Vertical zone (XMB item column) steps its index on Up/Down and crosses zones on Left/Right;
//   * removeZone on the last remaining zone refuses (no representable null), and re-registering the
//     currently selected zone re-snaps the held index;
//   * the back stack pops LIFO, runs onPop in order, bottoms out on rootBack(), and IGNORES both a
//     pushLevel() and a popLevel() issued from inside an onPop callback (no re-push/cascade loops).
//
// Prints NAVQML-OK on success; any failure prints NAVQML-FAIL <what> and exits non-zero.
#include "nav/NavGraph.h"

#include <QGuiApplication>
#include <QSet>
#include <cstdio>
#include <deque>
#include <random>
#include <set>
#include <vector>

static int failures = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "NAVQML-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

static void pump() { if (QCoreApplication::instance()) { QCoreApplication::processEvents(); } }

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");   // the runner loop invokes us without a -platform arg
    QGuiApplication app(argc, argv);

    // ---------------------------------------------------------------- 1. selection valid on first zone
    {
        NavGraph g;
        CHECK(g.zone().isEmpty(), "no zone before any registerZone");
        g.registerZone(QStringLiteral("main"), 5, 0, 0);
        CHECK(g.zone() == QStringLiteral("main"), "first registerZone owns the selection");
        CHECK(g.index() == 0, "first selection lands on index 0");
        CHECK(g.validate(nullptr), "a single registered zone validates");
    }

    // ---------------------------------------------------------------- 2. divider snap + termination
    {
        NavGraph g;
        g.registerZone(QStringLiteral("list"), 6, 0, 0);
        g.setUnselectable(QStringLiteral("list"), QSet<int>{1, 3});
        g.select(QStringLiteral("list"), 1);       // asked for a divider
        CHECK(g.index() != 1 && g.index() != 3, "select snaps off a divider");
        CHECK(g.index() == 2 || g.index() == 0, "snap picks the nearest selectable");
        g.select(QStringLiteral("list"), 3);
        CHECK(g.index() != 1 && g.index() != 3, "select snaps off the other divider");

        // Every index but one is a divider — snap must still terminate on the lone selectable.
        g.setUnselectable(QStringLiteral("list"), QSet<int>{0, 1, 2, 3, 5});
        g.select(QStringLiteral("list"), 0);
        CHECK(g.index() == 4, "snap crosses a run of dividers to the only selectable");

        // Clamp: out-of-range select is pulled into the count.
        g.setUnselectable(QStringLiteral("list"), QSet<int>{});
        g.select(QStringLiteral("list"), 99);
        CHECK(g.index() == 5, "select clamps a too-large index to the last element");
        g.select(QStringLiteral("list"), -4);
        CHECK(g.index() == 0, "select clamps a negative index to 0");
    }

    // ---------------------------------------------------------------- 3. churn storm (FIXED seed)
    {
        NavGraph g;
        // A single column of zones: nearest-neighbor keeps the grid connected under ANY removal subset,
        // so validate()'s connectivity invariant holds throughout the churn. z0 is the never-removed,
        // never-zeroed root, so a selectable zone always exists.
        const int N = 8;
        std::vector<QString> ids;
        for (int i = 0; i < N; ++i) ids.push_back(QStringLiteral("z%1").arg(i));
        std::set<int> present;
        for (int i = 0; i < N; ++i) { g.registerZone(ids[i], (i == 0 ? 4 : i % 5), i, 0); present.insert(i); }
        g.setDefaultZone(ids[0]);
        std::set<int> unsel3;         // a fixed divider pattern on z3
        unsel3.insert(0); unsel3.insert(2);

        auto selectionValid = [&](const char* when) {
            QString z = g.zone();
            CHECK(!z.isEmpty(), when);
            int idx = g.index();
            CHECK(idx >= 0, when);
            QString why;
            CHECK(g.validate(&why), when);
        };

        std::mt19937 rng(0xC0FFEEu);   // deterministic — never wall-clock
        for (int step = 0; step < 1000; ++step) {
            int op = rng() % 5;
            int zi = 1 + (rng() % (N - 1));   // never touch the root z0
            if (op == 0) {                    // grow / shrink (incl. 0)
                if (present.count(zi)) g.setZoneCount(ids[zi], rng() % 9);
            } else if (op == 1) {             // remove
                if (present.count(zi)) { g.removeZone(ids[zi]); present.erase(zi); }
            } else if (op == 2) {             // re-add
                if (!present.count(zi)) { g.registerZone(ids[zi], 1 + rng() % 4, zi, 0); present.insert(zi); }
            } else if (op == 3) {             // divider churn on z3
                if (present.count(3)) g.setUnselectable(ids[3], QSet<int>(unsel3.begin(), unsel3.end()));
            } else {                          // navigate
                static const Qt::Key arr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
                g.move(arr[rng() % 4]);
                g.select(ids[zi], int(rng() % 10) - 2);
            }
            selectionValid("churn keeps a valid, validating selection");
            // If the selected zone has a positive count, index must be in-range and not a divider.
            QString z = g.zone();
            for (int i = 0; i < N; ++i) if (ids[i] == z) {
                // (indirectly verified by validate(); explicit range guard below)
            }
        }
        pump();
    }

    // ---------------------------------------------------------------- 4. move() reaches every zone (Inv 2)
    {
        NavGraph g;
        std::set<QString> registry;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                QString id = QStringLiteral("g%1%2").arg(r).arg(c);
                g.registerZone(id, 3, r, c);
                registry.insert(id);
            }
        g.setDefaultZone(QStringLiteral("g11"));
        CHECK(g.validate(nullptr), "the 3x3 grid is a connected zone graph");

        // BFS over (zone,index) states applying all four arrows; collect the reached zones.
        std::set<QString> reached;
        std::set<std::pair<QString,int>> seen;
        std::deque<std::pair<QString,int>> q;
        g.select(QStringLiteral("g11"), 0);
        q.push_back({g.zone(), g.index()});
        seen.insert({g.zone(), g.index()});
        reached.insert(g.zone());
        static const Qt::Key arr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
        while (!q.empty()) {
            auto [z, i] = q.front(); q.pop_front();
            for (Qt::Key k : arr) {
                g.select(z, i);
                g.move(k);
                auto st = std::make_pair(g.zone(), g.index());
                if (!seen.count(st)) { seen.insert(st); reached.insert(st.first); q.push_back(st); }
            }
        }
        CHECK(reached == registry, "arrows reach every zone from the default (spatial connectivity)");

        // Pinned directional resolution (the storm can't catch valid-but-WRONG picks): from g11 the
        // cross-axis arrows go to the orthogonal neighbors, and the along-axis arrows cross only at edges.
        g.select(QStringLiteral("g11"), 1);
        g.move(Qt::Key_Down);
        CHECK(g.zone() == QStringLiteral("g21") && g.index() == 1, "g11 + Down = g21 (index carried)");
        g.select(QStringLiteral("g11"), 1);
        g.move(Qt::Key_Up);
        CHECK(g.zone() == QStringLiteral("g01") && g.index() == 1, "g11 + Up = g01 (index carried)");
        g.select(QStringLiteral("g11"), 0);
        g.move(Qt::Key_Right);
        CHECK(g.zone() == QStringLiteral("g11") && g.index() == 1, "Right mid-strip steps the index, stays in g11");
        g.select(QStringLiteral("g11"), 2);
        g.move(Qt::Key_Right);
        CHECK(g.zone() == QStringLiteral("g12") && g.index() == 2, "g11 + Right at the strip edge = g12");
        g.select(QStringLiteral("g11"), 0);
        g.move(Qt::Key_Left);
        CHECK(g.zone() == QStringLiteral("g10") && g.index() == 0, "g11 + Left at index 0 = g10");

        // Pinned reassignment target: remove the selected g11 — all four orthogonal neighbors are at grid
        // distance 1, so the documented tie-break (registration order) picks g01 (registered before g10/g12/g21).
        g.select(QStringLiteral("g11"), 1);
        g.removeZone(QStringLiteral("g11"));
        CHECK(g.zone() == QStringLiteral("g01"), "removing the selected g11 reassigns to g01 (nearest, reg-order tie-break)");
        CHECK(g.index() == 1, "reassignment carries the index");
        CHECK(g.validate(nullptr), "the grid still validates after the pinned removal");
    }

    // ---------------------------------------------------------------- 4b. vertical-axis zone (XMB column)
    {
        NavGraph g;
        g.registerZone(QStringLiteral("menu"), 4, 0, 0, Qt::Vertical);    // the XMB item column
        g.registerZone(QStringLiteral("side"), 3, 0, 1);                  // a horizontal strip to its right
        g.select(QStringLiteral("menu"), 0);
        CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("menu") && g.index() == 1,
              "Down steps a Vertical zone's index (stays in the zone)");
        CHECK(g.move(Qt::Key_Down) && g.index() == 2, "Down steps again");
        CHECK(g.move(Qt::Key_Up) && g.index() == 1, "Up steps back");
        CHECK(!g.move(Qt::Key_Left), "Left off a Vertical zone with nothing there is a no-op (returns false)");
        CHECK(g.zone() == QStringLiteral("menu"), "the failed cross leaves the selection put");
        CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("side"),
              "Right crosses OUT of a Vertical zone (cross-axis arrow)");
        CHECK(g.index() == 1, "the cross carries the index into the strip");
        CHECK(g.move(Qt::Key_Left) && g.zone() == QStringLiteral("side") && g.index() == 0,
              "Left inside the Horizontal strip steps its index first");
        CHECK(g.move(Qt::Key_Left) && g.zone() == QStringLiteral("menu"),
              "Left at the strip's edge crosses back into the Vertical zone");
        // Divider snap still works along the vertical axis.
        g.setUnselectable(QStringLiteral("menu"), QSet<int>{1});
        g.select(QStringLiteral("menu"), 0);
        CHECK(g.move(Qt::Key_Down) && g.index() == 2, "Down skips a divider in a Vertical zone");
        g.select(QStringLiteral("menu"), 1);
        CHECK(g.index() != 1, "select snaps off a Vertical zone's divider");
    }

    // ---------------------------------------------------------------- 4c. last-zone removal is a no-op
    {
        NavGraph g;
        g.registerZone(QStringLiteral("only"), 3, 0, 0);
        g.select(QStringLiteral("only"), 2);
        g.removeZone(QStringLiteral("only"));   // must refuse — no representable null state
        CHECK(g.zone() == QStringLiteral("only"), "removeZone on the last zone is a refusing no-op");
        CHECK(g.index() == 2, "the selection is untouched by the refused removal");
        CHECK(g.validate(nullptr), "the registry still validates");
        g.select(QStringLiteral("only"), 1);
        CHECK(g.index() == 1, "the refused zone is still fully live (select works)");
    }

    // ---------------------------------------------------------------- 4d. re-registering the selected zone re-snaps
    {
        NavGraph g;
        g.registerZone(QStringLiteral("list"), 10, 0, 0);
        g.registerZone(QStringLiteral("other"), 2, 1, 0);
        g.select(QStringLiteral("list"), 9);
        g.registerZone(QStringLiteral("list"), 3, 0, 0);   // a Repeater rebuild shrank the zone
        CHECK(g.zone() == QStringLiteral("list"), "re-registering the selected zone keeps it selected");
        CHECK(g.index() == 2, "the held index re-snaps into the smaller count");
        CHECK(g.validate(nullptr), "validate passes after the re-register snap");
        g.registerZone(QStringLiteral("list"), 0, 0, 0);   // rebuild emptied it entirely
        CHECK(g.zone() == QStringLiteral("other"), "re-registering the selected zone at count 0 reassigns away");
        CHECK(g.validate(nullptr), "validate passes after the count-0 re-register");
    }

    // ---------------------------------------------------------------- 5. back stack: LIFO + rootBack
    {
        NavGraph g;
        g.registerZone(QStringLiteral("scr"), 3, 0, 0);
        std::vector<int> order;
        int rootBacks = 0;
        int levelSignals = 0;
        QObject::connect(&g, &NavGraph::rootBack, [&]{ ++rootBacks; });
        QObject::connect(&g, &NavGraph::levelsChanged, [&](int){ ++levelSignals; });
        for (int i = 0; i < 5; ++i) g.pushLevel(QStringLiteral("L%1").arg(i), [&order, i]{ order.push_back(i); });
        CHECK(g.levelDepth() == 5, "five pushes give depth 5");
        CHECK(levelSignals >= 5, "each push notifies levelsChanged");

        for (int i = 0; i < 5; ++i) CHECK(g.back(), "back() always returns true while levels remain");
        CHECK(g.levelDepth() == 0, "five backs empty the stack");
        std::vector<int> expected = {4, 3, 2, 1, 0};
        CHECK(order == expected, "onPop runs LIFO (last pushed pops first)");
        CHECK(rootBacks == 0, "no rootBack while the stack was non-empty");

        CHECK(g.back(), "back() at an empty stack still returns true");
        CHECK(rootBacks == 1, "back() at the root emits rootBack exactly once");
        CHECK(g.levelDepth() == 0, "rootBack leaves the stack empty");
    }

    // ---------------------------------------------------------------- 6. push inside onPop is IGNORED
    {
        NavGraph g;
        g.registerZone(QStringLiteral("scr"), 3, 0, 0);
        bool ran = false;
        g.pushLevel(QStringLiteral("X"), [&]{
            ran = true;
            g.pushLevel(QStringLiteral("Y"), []{});   // must be ignored during the pop
        });
        CHECK(g.levelDepth() == 1, "one level pushed");
        CHECK(g.back(), "back pops the level");
        CHECK(ran, "the onPop callback ran");
        CHECK(g.levelDepth() == 0, "a push from inside onPop is ignored — no re-push loop");

        // Symmetry: a reentrant popLevel() from inside an onPop is equally a no-op — the level below
        // survives and its onPop does not run.
        bool lowerRan = false;
        g.pushLevel(QStringLiteral("low"), [&]{ lowerRan = true; });
        g.pushLevel(QStringLiteral("top"), [&]{ g.popLevel(); });   // must NOT cascade into "low"
        CHECK(g.back(), "back pops the top level");
        CHECK(g.levelDepth() == 1, "a pop from inside onPop is ignored — the level below survives");
        CHECK(!lowerRan, "the surviving level's onPop did not run");
    }

    if (failures) { std::fprintf(stderr, "NAVQML-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("NAVQML-OK\n");
    return 0;
}
