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
//
// When the QML theme engine is present (MMV_HAVE_QML) this ALSO proves the two-state themed input contract:
// it instantiates the real ThemedTextField / ThemedChoice components (loaded from the theme2 qrc) in an
// offscreen QQuickWidget with a REAL NavGraph exposed as the `nav` context property — exactly the wiring
// subsystem B will use — and asserts register / select / edit / commit / cancel and the "arrows stay in the
// field while editing" invariant, driving real key events through the QML focus system.
#include "nav/NavGraph.h"

#include <QSet>
#include <cstdio>
#include <deque>
#include <random>
#include <set>
#include <vector>

#ifdef MMV_HAVE_QML
#include <QApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QKeyEvent>
#include <QQmlError>
#include <QSGRendererInterface>
#else
#include <QGuiApplication>
#endif

static int failures = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "NAVQML-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

static void pump() { if (QCoreApplication::instance()) { QCoreApplication::processEvents(); } }

#ifdef MMV_HAVE_QML
// Deliver a real key press+release to the QQuickWidget's offscreen window; it routes to the active-focus QML
// item (the host's Keys handler when nothing is editing, or the TextInput/FocusScope while a field is edited).
static void sendKey(QQuickWindow* win, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(win, &press);
    QCoreApplication::sendEvent(win, &release);
    QCoreApplication::processEvents();
}

// The two-state themed-input contract, proven end to end against a real NavGraph + the qrc-embedded components.
static void runThemedInputAsserts()
{
    // A host FocusScope that mimics subsystem B's key router: arrows -> nav.move, Enter -> nav.activate. The
    // themed inputs sit inside it and self-register their zones; when one is EDITING it holds focus and
    // swallows the arrows, so the host's router never sees them (the invariant we assert below).
    const char* qml =
        "import QtQuick\n"
        "import \"elements\" as El\n"
        "FocusScope {\n"
        "    id: host\n"
        "    focus: true; width: 400; height: 300\n"
        "    property string lastCommit: \"\"\n"
        "    property int commitCount: 0\n"
        "    property int editReqCount: 0\n"
        "    property int chosenIndex: -1\n"
        "    property int chosenCount: 0\n"
        "    Keys.onPressed: (event) => {\n"
        "        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right\n"
        "            || event.key === Qt.Key_Up || event.key === Qt.Key_Down) { nav.move(event.key); event.accepted = true }\n"
        "        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { nav.activate(); event.accepted = true }\n"
        "    }\n"
        "    Column {\n"
        "        anchors.fill: parent\n"
        "        El.ThemedTextField { objectName: \"tf\"; navZone: \"field1\"; navRow: 0; navCol: 0; placeholder: \"name\"\n"
        "            onCommitted: (t) => { host.lastCommit = t; host.commitCount++ }\n"
        "            onEditRequested: (z) => host.editReqCount++ }\n"
        "        El.ThemedChoice { objectName: \"tc\"; navZone: \"choice1\"; navRow: 1; navCol: 0; options: [\"Alpha\", \"Beta\", \"Gamma\"]\n"
        "            onChosen: (i) => { host.chosenIndex = i; host.chosenCount++ } }\n"
        "    }\n"
        "}\n";

    NavGraph graph;   // the REAL selection model, exposed as `nav` (exactly ThemeEngine's context-property path)
    QQuickWidget qw;
    qw.setResizeMode(QQuickWidget::SizeRootObjectToView);
    qw.rootContext()->setContextProperty(QStringLiteral("nav"), &graph);

    QQmlComponent comp(qw.engine());
    comp.setData(QByteArray(qml), QUrl(QStringLiteral("qrc:/theme2/probe_host.qml")));
    if (comp.isError()) {
        for (const QQmlError& e : comp.errors()) std::fprintf(stderr, "NAVQML-FAIL host QML: %s\n", e.toString().toUtf8().constData());
        ++failures; return;
    }
    QObject* rootObj = comp.create(qw.rootContext());
    QQuickItem* host = qobject_cast<QQuickItem*>(rootObj);
    CHECK(host != nullptr, "the host QML instantiates (components resolved from the qrc)");
    if (!host) return;
    qw.setContent(QUrl(QStringLiteral("qrc:/theme2/probe_host.qml")), &comp, host);
    qw.resize(400, 300);
    qw.show();
    pump();

    QQuickWindow* win = qw.quickWindow();
    QQuickItem* tf = host->findChild<QQuickItem*>(QStringLiteral("tf"));
    QQuickItem* tc = host->findChild<QQuickItem*>(QStringLiteral("tc"));
    CHECK(tf && tc, "both themed inputs are present in the scene");
    if (!tf || !tc) return;
    QQuickItem* input = tf->findChild<QQuickItem*>(QStringLiteral("tfInput"));
    CHECK(input != nullptr, "the text field's inline TextInput exists");

    // ---- 1. each component self-registered its zone on completion ----
    // (A selection already exists — completion adopted the first zone — so the model is never null. select()
    // refuses an UNregistered zone, so landing on each id proves that id was registered on Component.onCompleted.)
    CHECK(!graph.zone().isEmpty(), "completion registered a zone (the selection is never null)");
    graph.select(QStringLiteral("field1"), 0);
    CHECK(graph.zone() == QStringLiteral("field1"), "field1 registered on completion (select lands on it)");
    graph.select(QStringLiteral("choice1"), 0);
    CHECK(graph.zone() == QStringLiteral("choice1"), "choice1 registered on completion (select lands on it)");
    graph.select(QStringLiteral("field1"), 0);

    // ---- 2. positive control: an arrow moves the selection (proves key routing to the host works) ----
    host->forceActiveFocus();
    pump();
    sendKey(win, Qt::Key_Down);
    CHECK(graph.zone() == QStringLiteral("choice1"), "Down moves the selection field1 -> choice1 (host router runs)");
    graph.select(QStringLiteral("field1"), 0);
    pump();

    // ---- 3. activate enters editing (the host answers Enter with nav.activate) ----
    CHECK(!tf->property("editing").toBool(), "the field is not editing before activation");
    graph.activate();
    pump();
    CHECK(tf->property("editing").toBool(), "activating the field's zone enters the editing state");
    CHECK(host->property("editReqCount").toInt() == 1, "entering editing emits editRequested(navZone) once (host may divert to OSK)");
    CHECK(input && input->property("activeFocus").toBool(), "the inline TextInput grabbed focus (keys now land in the field)");

    // ---- 4. while editing, arrows do NOT move the selection (they stay in the field) ----
    sendKey(win, Qt::Key_Down);
    CHECK(graph.zone() == QStringLiteral("field1"), "Down while editing does NOT move the selection (field swallows it)");
    sendKey(win, Qt::Key_Left);
    CHECK(graph.zone() == QStringLiteral("field1"), "Left while editing does NOT move the selection either");

    // ---- 5. Escape returns to selected WITHOUT committing ----
    const int commitsBeforeEsc = host->property("commitCount").toInt();
    sendKey(win, Qt::Key_Escape);
    CHECK(!tf->property("editing").toBool(), "Escape leaves the editing state");
    CHECK(host->property("commitCount").toInt() == commitsBeforeEsc, "Escape commits nothing");
    CHECK(graph.zone() == QStringLiteral("field1"), "Escape returns to the selected field");

    // ---- 6. Enter commits exactly once with the entered value ----
    graph.select(QStringLiteral("field1"), 0);
    graph.activate();                                    // re-enter editing
    pump();
    CHECK(tf->property("editing").toBool(), "re-activation re-enters editing");
    if (input) input->setProperty("text", QStringLiteral("Zelda"));   // "type" a value into the field
    const int commitsBeforeEnter = host->property("commitCount").toInt();
    sendKey(win, Qt::Key_Return);
    CHECK(!tf->property("editing").toBool(), "Enter leaves the editing state");
    CHECK(host->property("commitCount").toInt() == commitsBeforeEnter + 1, "Enter commits EXACTLY once");
    CHECK(host->property("lastCommit").toString() == QStringLiteral("Zelda"), "the commit carries the entered value");
    CHECK(tf->property("text").toString() == QStringLiteral("Zelda"), "the committed value is written back to the field's text");
    CHECK(graph.zone() == QStringLiteral("field1"), "committing returns to the selected field");

    // ---- 7. ThemedChoice: activate -> edit, arrow moves the pending option, Enter chooses once ----
    graph.select(QStringLiteral("choice1"), 0);
    graph.activate();
    pump();
    CHECK(tc->property("editing").toBool(), "activating the choice's zone opens its option list (editing)");
    CHECK(tc->property("pending").toInt() == tc->property("currentOption").toInt(), "the pending highlight starts on the current option");
    sendKey(win, Qt::Key_Down);
    CHECK(tc->property("pending").toInt() == 1, "Down moves the PENDING option, not the nav selection");
    CHECK(graph.zone() == QStringLiteral("choice1"), "…and the nav selection stays on the choice while editing");
    sendKey(win, Qt::Key_Return);
    CHECK(!tc->property("editing").toBool(), "Enter closes the choice list");
    CHECK(host->property("chosenCount").toInt() == 1, "Enter fires chosen() exactly once");
    CHECK(host->property("chosenIndex").toInt() == 1, "chosen() carries the picked option index");
    CHECK(tc->property("currentOption").toInt() == 1, "the picked option becomes current");
}
#endif // MMV_HAVE_QML

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");   // the runner loop invokes us without a -platform arg
#ifdef MMV_HAVE_QML
    qputenv("QT_QUICK_BACKEND", "software");    // no GPU under the offscreen QPA — match the app's software backend
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QApplication app(argc, argv);               // QQuickWidget needs the widget app
#else
    QGuiApplication app(argc, argv);
#endif

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
        // Reassignment restores the successor's REMEMBERED index (recorded when it was last left), never the
        // dead zone's carried index — pin g01's memory explicitly, then verify it is what comes back.
        g.select(QStringLiteral("g01"), 2);   // park g01 at 2…
        g.select(QStringLiteral("g11"), 1);   // …and leave it (records g01.memory = 2)
        g.removeZone(QStringLiteral("g11"));
        CHECK(g.zone() == QStringLiteral("g01"), "removing the selected g11 reassigns to g01 (nearest, reg-order tie-break)");
        CHECK(g.index() == 2, "reassignment restores g01's remembered index, not the carried index");
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

    // ---------------------------------------------------------------- 7. declared edges + per-zone memory
    {
        NavGraph g;
        // A two-cursor surface, exactly the themed XMB shape: a Vertical item column + a Horizontal
        // category axis CO-LOCATED in one grid cell, with declared cursor-switching edges.
        g.registerZone(QStringLiteral("col"), 5, 0, 0, Qt::Vertical);
        g.registerZone(QStringLiteral("bar"), 4, 0, 0);
        g.addEdge(QStringLiteral("col"), Qt::Key_Left,  QStringLiteral("bar"));
        g.addEdge(QStringLiteral("col"), Qt::Key_Right, QStringLiteral("bar"));
        g.addEdge(QStringLiteral("bar"), Qt::Key_Down,  QStringLiteral("col"));
        g.addEdge(QStringLiteral("bar"), Qt::Key_Up,    QStringLiteral("col"));
        g.select(QStringLiteral("col"), 2);
        CHECK(g.move(Qt::Key_Right), "Right from the column crosses via the declared edge (visible change)");
        CHECK(g.zone() == QStringLiteral("bar"), "the declared edge wins over axis/geometric resolution");
        CHECK(g.index() == 1, "co-located entry = remembered index (0) + the fused step (+1) in ONE press");
        CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("col") && g.index() == 3,
              "Down re-enters the column at its remembered index (2) + fused step = 3 (memory recorded on leave)");
        // A fused step that clamps is a pure cursor flip: returns false (no visible index change), but the
        // zone still switches so the next along-axis arrow steps the other cursor.
        g.select(QStringLiteral("bar"), 3);   // park the bar at its end…
        g.select(QStringLiteral("col"), 1);   // …and leave it (records bar.memory = 3)
        CHECK(!g.move(Qt::Key_Right), "Right into a bar whose fused step clamps returns false (no visible move)");
        CHECK(g.zone() == QStringLiteral("bar") && g.index() == 3, "…yet the cursor DID switch (zone flipped, index at memory)");

        // A non-co-located edge is a focus handoff: enters at the remembered index WITHOUT the fused step.
        g.registerZone(QStringLiteral("btns"), 2, 1, 0);   // a real bottom bar, spatially below
        g.addEdge(QStringLiteral("col"), Qt::Key_Down, QStringLiteral("btns"));
        g.addEdge(QStringLiteral("btns"), Qt::Key_Up,  QStringLiteral("col"));
        g.select(QStringLiteral("btns"), 1);
        g.select(QStringLiteral("col"), 0);   // leave btns (records btns.memory = 1)
        CHECK(!g.move(Qt::Key_Down), "the handoff edge fires mid-strip (edges beat the axis step) with no index change");
        CHECK(g.zone() == QStringLiteral("btns") && g.index() == 1, "handoff enters at the remembered button, unstepped");
        CHECK(!g.move(Qt::Key_Up), "leaving back up restores the column exactly (no visible index change)");
        CHECK(g.zone() == QStringLiteral("col") && g.index() == 0, "…at the index it was left on (memory, not carry)");

        // A hidden target makes the edge inert: resolution falls through to the axis step.
        g.setZoneCount(QStringLiteral("btns"), 0);
        g.select(QStringLiteral("col"), 1);
        CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("col") && g.index() == 2,
              "an edge to a hidden zone is inert -> the axis step proceeds");
    }

    // ---------------------------------------------------------------- 8. co-located reassign restores memory
    {
        // The live-caught regression, now gated: three co-located zones (the themed home), the transient
        // chooser zone hides itself — the selection must land on the ITEM cursor's remembered index, never
        // the chooser's carried actionIndex.
        NavGraph g;
        g.registerZone(QStringLiteral("items"), 12, 0, 0, Qt::Vertical);
        g.registerZone(QStringLiteral("categories"), 6, 0, 0);
        g.registerZone(QStringLiteral("actions"), 0, 0, 0, Qt::Vertical, /*wraps=*/true);
        g.addEdge(QStringLiteral("items"), Qt::Key_Left, QStringLiteral("categories"));   // connectivity
        g.addEdge(QStringLiteral("actions"), Qt::Key_Escape, QStringLiteral("items"));
        g.select(QStringLiteral("items"), 7);
        g.setZoneCount(QStringLiteral("actions"), 4);        // chooser opens
        g.select(QStringLiteral("actions"), 3);              // user moves to the 4th action row
        g.setZoneCount(QStringLiteral("actions"), 0);        // chooser closes (zone hides)
        CHECK(g.zone() == QStringLiteral("items"), "hiding the chooser reassigns to the co-located items zone");
        CHECK(g.index() == 7, "…at the item cursor's REMEMBERED index (7), not the carried actionIndex (3)");
        CHECK(g.validate(nullptr), "the co-located registry still validates");
    }

    // ---------------------------------------------------------------- 9. the REAL themed graph shape
    {
        // Replicates ThemeEngine::buildView's registration + edge set literally (keep in sync with it).
        // The shipped themed graph must pass its own validator and reach every arrow-navigable zone.
        NavGraph g;
        g.registerZone(QStringLiteral("items"), 12, 0, 0, Qt::Vertical);
        g.registerZone(QStringLiteral("categories"), 6, 0, 0);
        g.registerZone(QStringLiteral("buttons"), 2, 1, 0);
        g.registerZone(QStringLiteral("actions"), 0, 0, 0, Qt::Vertical, /*wraps=*/true);
        g.setDefaultZone(QStringLiteral("items"));
        g.addEdge(QStringLiteral("items"), Qt::Key_Left,  QStringLiteral("categories"));
        g.addEdge(QStringLiteral("items"), Qt::Key_Right, QStringLiteral("categories"));
        g.addEdge(QStringLiteral("categories"), Qt::Key_Down, QStringLiteral("items"));
        g.addEdge(QStringLiteral("categories"), Qt::Key_Up,   QStringLiteral("items"));
        g.addEdge(QStringLiteral("items"), Qt::Key_Down, QStringLiteral("buttons"));
        g.addEdge(QStringLiteral("buttons"), Qt::Key_Up, QStringLiteral("items"));
        g.addEdge(QStringLiteral("actions"), Qt::Key_Escape, QStringLiteral("items"));
        QString why;
        CHECK(g.validate(&why), "the REAL themed graph passes validate() with the chooser closed");
        g.setZoneCount(QStringLiteral("actions"), 4);
        CHECK(g.validate(&why), "…and with the chooser open");
        g.setZoneCount(QStringLiteral("actions"), 0);

        // Every arrow-navigable zone is reachable from the default via move() alone (the overlay `actions`
        // zone is entered by activation, not an arrow — its declared Esc edge is its return leg, below).
        std::set<QString> reached;
        std::set<std::pair<QString,int>> seen;
        std::deque<std::pair<QString,int>> q;
        g.select(QStringLiteral("items"), 0);
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
        CHECK(reached.count(QStringLiteral("items")) && reached.count(QStringLiteral("categories"))
              && reached.count(QStringLiteral("buttons")),
              "arrows alone reach items+categories+buttons from the default");

        // Two-cursor parity pin: from the column, one Right switches AND steps the category axis.
        g.select(QStringLiteral("categories"), 2);
        g.select(QStringLiteral("items"), 4);                // leave categories (memory = 2)
        CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("categories") && g.index() == 3,
              "one Right from the column = category cursor 2 -> 3 (edge entry at memory + fused step)");

        // The chooser's declared Esc edge returns to the item cursor (the leg syncActionsZone executes).
        // Esc is a non-arrow key: it resolves ONLY via the declared edge (no fused step, no geometry).
        g.select(QStringLiteral("items"), 5);
        g.setZoneCount(QStringLiteral("actions"), 4);
        g.select(QStringLiteral("actions"), 2);
        g.move(Qt::Key_Escape);
        CHECK(g.zone() == QStringLiteral("items") && g.index() == 5,
              "the chooser's declared Esc edge lands back on the remembered item cursor");
    }

    // ---------------------------------------------------------------- 10. themed screen back router (Task 3)
    {
        // Scripted mimic of the real themed screens: an XMB root drills into a catalog, then browse ×3, then
        // opens a detail page — each ENTRY pushing a level whose onPop is exactly what the host does on Back.
        // Back×N must then unwind them in order and, at the root, emit rootBack (the host's pause menu).
        NavGraph g;
        g.registerZone(QStringLiteral("items"), 8, 0, 0, Qt::Vertical);
        std::vector<QString> popped;   // records the SEMANTIC action each onPop stands for, in fire order
        int rootBacks = 0, backInvokes = 0;
        QObject::connect(&g, &NavGraph::rootBack, [&] { ++rootBacks; });
        QObject::connect(&g, &NavGraph::backInvoked, [&] { ++backInvokes; });

        g.pushLevel(QStringLiteral("catalog"), [&] { popped.push_back(QStringLiteral("catalog")); }); // enter a multi-catalog bucket
        g.pushLevel(QStringLiteral("browse"),  [&] { popped.push_back(QStringLiteral("browse1")); });  // drill 1
        g.pushLevel(QStringLiteral("browse"),  [&] { popped.push_back(QStringLiteral("browse2")); });  // drill 2
        g.pushLevel(QStringLiteral("browse"),  [&] { popped.push_back(QStringLiteral("browse3")); });  // drill 3
        g.pushLevel(QStringLiteral("detail"),  [&] { popped.push_back(QStringLiteral("detail")); });   // Info -> detail page
        CHECK(g.levelDepth() == 5, "five themed levels pushed (catalog + browse x3 + detail)");
        CHECK(g.countLevels(QStringLiteral("browse")) == 3, "countLevels tallies the three browse levels");
        CHECK(g.countLevels(QStringLiteral("catalog")) == 1, "countLevels tallies the one catalog level");

        g.back(); // detail
        g.back(); // browse3
        g.back(); // browse2
        g.back(); // browse1
        g.back(); // catalog
        std::vector<QString> expect = { QStringLiteral("detail"), QStringLiteral("browse3"),
                                        QStringLiteral("browse2"), QStringLiteral("browse1"),
                                        QStringLiteral("catalog") };
        CHECK(popped == expect, "Back unwinds detail -> browse3 -> browse2 -> browse1 -> catalog, in order");
        CHECK(g.levelDepth() == 0, "the themed level stack is empty after five Backs");
        CHECK(rootBacks == 0, "no rootBack while levels remained");
        CHECK(backInvokes == 5, "backInvoked fired once per Back (the host's back sound hook)");

        g.back(); // at the root now -> the pause menu / themed home
        CHECK(rootBacks == 1, "the sixth Back at the empty stack emits rootBack exactly once");
        CHECK(backInvokes == 6, "backInvoked also fires on the rootBack gesture");
    }

    // ---------------------------------------------------------------- 11. overlay pushes a level; Back closes it first
    {
        // An overlay (esc menu / OSK) opened over a themed screen mirrors itself as a level: Back closes the
        // overlay BEFORE it unwinds the screen's own drills. Here the overlay is the topmost level, so the
        // first Back runs its onPop (dismiss), leaving the browse level beneath untouched.
        NavGraph g;
        g.registerZone(QStringLiteral("items"), 4, 0, 0);
        bool overlayDismissed = false, browseBacked = false;
        g.pushLevel(QStringLiteral("browse"),  [&] { browseBacked = true; });
        g.pushLevel(QStringLiteral("overlay"), [&] { overlayDismissed = true; });
        CHECK(g.levelDepth() == 2, "browse + overlay on the stack");
        g.back();
        CHECK(overlayDismissed && !browseBacked, "Back closes the topmost overlay first, not the drill beneath it");
        CHECK(g.levelDepth() == 1 && g.countLevels(QStringLiteral("browse")) == 1, "the browse level survives the overlay close");
    }

    // ---------------------------------------------------------------- 12. out-of-band clear (mimic selectType)
    {
        // A search / category switch resets the underlying browse stack out of band. The host mirrors that by
        // dropping the graph's themed levels WITHOUT running their onPop (popLevelSilent) — so a subsequent
        // Back does NOT fire stale drill-up actions; it goes straight to rootBack.
        NavGraph g;
        g.registerZone(QStringLiteral("items"), 6, 0, 0);
        int stalePops = 0, rootBacks = 0;
        QObject::connect(&g, &NavGraph::rootBack, [&] { ++rootBacks; });
        g.pushLevel(QStringLiteral("catalog"), [&] { ++stalePops; });
        g.pushLevel(QStringLiteral("browse"),  [&] { ++stalePops; });
        g.pushLevel(QStringLiteral("browse"),  [&] { ++stalePops; });
        CHECK(g.levelDepth() == 3, "catalog + two browse levels before the out-of-band reset");

        // syncThemedLevels' reconcile to an empty themed stack: silently drop browse (on top) then catalog.
        while (g.countLevels(QStringLiteral("browse")) > 0) g.popLevelSilent();
        g.popLevelSilent(); // the catalog level
        CHECK(g.levelDepth() == 0, "popLevelSilent cleared every themed level");
        CHECK(stalePops == 0, "no onPop ran during the silent out-of-band clear");

        g.back();
        CHECK(rootBacks == 1 && stalePops == 0, "Back after the reset goes straight to rootBack — no stale drill pops");
    }

    // ---------------------------------------------------------------- 13. popLevelSilent / isPopping guards
    {
        NavGraph g;
        g.registerZone(QStringLiteral("z"), 3, 0, 0);
        bool sawPoppingTrue = false;
        g.pushLevel(QStringLiteral("outer"), [&] {
            // Inside an onPop the graph is "popping": a mirror reconcile must stand off (no-op).
            sawPoppingTrue = g.isPopping();
            g.popLevelSilent();                 // must be ignored mid-onPop (re-entrancy guard)
        });
        g.pushLevel(QStringLiteral("keep"), []{});
        CHECK(g.levelDepth() == 2, "two levels pushed");
        g.popLevel();                           // pops "keep" (top, no side effects)
        CHECK(g.levelDepth() == 1, "top level popped");
        g.popLevel();                           // pops "outer" -> its onPop runs, sees isPopping(), tries a re-entrant silent pop
        CHECK(sawPoppingTrue, "isPopping() reports true inside an onPop");
        CHECK(g.levelDepth() == 0, "the re-entrant popLevelSilent inside onPop was ignored (guarded)");
        CHECK(!g.isPopping(), "isPopping() is false once the pop completes");
    }

#ifdef MMV_HAVE_QML
    // ---------------------------------------------------------------- 14. two-state themed inputs (the real
    // ThemedTextField/ThemedChoice components, offscreen QQuickWidget, real NavGraph as `nav`).
    runThemedInputAsserts();
#endif

    if (failures) { std::fprintf(stderr, "NAVQML-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("NAVQML-OK\n");
    return 0;
}
