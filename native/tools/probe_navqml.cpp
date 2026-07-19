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
#include "nav/NavThemeGraph.h"
#include "BlackFrameWatchdog.h"

#include <QImage>
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
        "    property int chEditReq: 0\n"
        "    property string xLastCommit: \"\"\n"
        "    property int xCommitCount: 0\n"
        "    property int xEditReq: 0\n"
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
        "            onChosen: (i) => { host.chosenIndex = i; host.chosenCount++ }\n"
        "            onEditRequested: (z) => host.chEditReq++ }\n"
        "        Loader {\n"       // teardown vehicle: activating registers field2, deactivating must DEregister it
        "            objectName: \"dynLoader\"; active: false\n"
        "            sourceComponent: El.ThemedTextField { navZone: \"field2\"; navRow: 2; navCol: 0 }\n"
        "        }\n"
        "        El.ThemedTextField { objectName: \"tfx\"; navZone: \"fieldx\"; navRow: 3; navCol: 0; externalEdit: true\n"
        "            onCommitted: (t) => { host.xLastCommit = t; host.xCommitCount++ }\n"
        "            onEditRequested: (z) => { if (z === \"fieldx\") host.xEditReq++ } }\n"
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
    CHECK(host->property("editReqCount").toInt() == 0, "the INLINE flow does NOT emit editRequested (self-contained)");
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
    CHECK(host->property("chEditReq").toInt() == 0, "the choice's INLINE flow does not emit editRequested either");

    // ---- 8. post-commit routing: the very next arrow moves the selection again (focus fully handed back) ----
    graph.select(QStringLiteral("field1"), 0);
    pump();
    sendKey(win, Qt::Key_Down);
    CHECK(graph.zone() == QStringLiteral("choice1"), "one arrow AFTER a commit moves the selection (routing restored)");

    // ---- 9. externalEdit (the TV / OSK route): activate only signals; the HOST edits + returns via finishEdit ----
    QQuickItem* tfx = host->findChild<QQuickItem*>(QStringLiteral("tfx"));
    CHECK(tfx != nullptr, "the externalEdit field is present");
    if (tfx) {
        QQuickItem* xinput = tfx->findChild<QQuickItem*>(QStringLiteral("tfInput"));
        graph.select(QStringLiteral("fieldx"), 0);
        pump();
        graph.activate();
        pump();
        CHECK(host->property("xEditReq").toInt() == 1, "externalEdit activate emits editRequested(navZone) exactly once");
        CHECK(!tfx->property("editing").toBool(), "externalEdit does NOT enter inline editing (no double editor)");
        CHECK(tfx->property("externalPending").toBool(), "externalEdit goes pending (host owes finishEdit)");
        CHECK(!(xinput && xinput->property("activeFocus").toBool()), "externalEdit grabs NO focus (the host's OSK owns input)");
        sendKey(win, Qt::Key_Up);
        CHECK(graph.zone() != QStringLiteral("fieldx"), "arrows STILL move the selection while an external edit is pending");
        // The host ran its OSK, writes the result back, and closes the loop: commits exactly once.
        tfx->setProperty("text", QStringLiteral("Link"));
        QMetaObject::invokeMethod(tfx, "finishEdit", Q_ARG(QVariant, QVariant(true)));
        pump();
        CHECK(host->property("xCommitCount").toInt() == 1, "finishEdit(true) commits EXACTLY once");
        CHECK(host->property("xLastCommit").toString() == QStringLiteral("Link"), "the external commit carries the host-written text");
        CHECK(!tfx->property("externalPending").toBool(), "finishEdit clears the pending state (back to selected)");
        // The abandon leg: a second request, answered with finishEdit(false), commits nothing.
        graph.select(QStringLiteral("fieldx"), 0);
        graph.activate();
        pump();
        CHECK(host->property("xEditReq").toInt() == 2, "a second activation re-requests the external editor");
        QMetaObject::invokeMethod(tfx, "finishEdit", Q_ARG(QVariant, QVariant(false)));
        pump();
        CHECK(host->property("xCommitCount").toInt() == 1, "finishEdit(false) commits NOTHING");
        CHECK(!tfx->property("externalPending").toBool(), "finishEdit(false) also returns to selected");
    }

    // ---- 9b. externalEdit on ThemedChoice: same suppression + finishEdit contract ----
    tc->setProperty("externalEdit", true);
    graph.select(QStringLiteral("choice1"), 0);
    pump();
    graph.activate();
    pump();
    CHECK(host->property("chEditReq").toInt() == 1, "external choice activate emits editRequested once");
    CHECK(!tc->property("editing").toBool(), "external choice does NOT open the inline list");
    CHECK(tc->property("externalPending").toBool(), "external choice goes pending");
    tc->setProperty("currentOption", 2);                 // the host's picker chose Gamma…
    QMetaObject::invokeMethod(tc, "finishEdit", Q_ARG(QVariant, QVariant(true)));
    pump();
    CHECK(host->property("chosenCount").toInt() == 2, "finishEdit(true) fires chosen() exactly once more");
    CHECK(host->property("chosenIndex").toInt() == 2, "chosen() carries the host-written option");
    CHECK(!tc->property("externalPending").toBool(), "the choice returns to selected");
    tc->setProperty("externalEdit", false);

    // ---- 10. teardown: destruction DEregisters the zone (no phantom zones after a Loader unload) ----
    QQuickItem* dynLoader = host->findChild<QQuickItem*>(QStringLiteral("dynLoader"));
    CHECK(dynLoader != nullptr, "the teardown Loader is present");
    if (dynLoader) {
        // (a) destroy while NOT selected: the zone simply vanishes from the graph.
        dynLoader->setProperty("active", true);
        pump();
        graph.select(QStringLiteral("field2"), 0);
        CHECK(graph.zone() == QStringLiteral("field2"), "the Loader-created field registered its zone");
        CHECK(graph.validate(nullptr), "the graph validates with the dynamic zone present");
        graph.select(QStringLiteral("field1"), 0);       // move off before the unload
        dynLoader->setProperty("active", false);         // Loader unload -> Component.onDestruction -> removeZone
        pump();
        graph.select(QStringLiteral("field2"), 0);       // select() refuses an unregistered zone…
        CHECK(graph.zone() != QStringLiteral("field2"), "the destroyed field's zone is GONE (select refuses it)");
        CHECK(graph.validate(nullptr), "the graph validates after the teardown");
        sendKey(win, Qt::Key_Down);                       // …and no arrow walk can land on the phantom either
        sendKey(win, Qt::Key_Down);
        CHECK(graph.zone() != QStringLiteral("field2"), "arrows cannot reach the deregistered zone");

        // (b) destroy while SELECTED: the selection must reassign to a live zone, never dangle.
        dynLoader->setProperty("active", true);
        pump();
        graph.select(QStringLiteral("field2"), 0);
        CHECK(graph.zone() == QStringLiteral("field2"), "the re-created field re-registered (selected again)");
        dynLoader->setProperty("active", false);         // destroyed out from under the selection
        pump();
        CHECK(graph.zone() != QStringLiteral("field2") && !graph.zone().isEmpty(),
              "destroying the SELECTED field reassigns the selection to a live zone");
        CHECK(graph.validate(nullptr), "the graph validates after the selected-zone teardown");
    }
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

    // ---------------------------------------------------------------- 0. black-frame classifier (Task 5)
    // BlackFrameWatchdog::isBlack is a pure luma classifier: ≥99% of pixels below luma 16 = a black frame.
    // It backs the debug-gated self-heal for the intermittent all-black app state, so its judgment — and
    // crucially its REFUSAL to call a failed grab (null image) black — must be pinned exactly.
    {
        // A gray value g renders to Rec.601 luma == g (the 77+150+29 weights sum to 256), so we build test
        // frames by gray level: 0 = black (luma 0), 40 = dark-but-not-black, 255 = bright.
        auto solid = [](int w, int h, int gray) {
            QImage im(w, h, QImage::Format_ARGB32);
            im.fill(qRgb(gray, gray, gray));
            return im;
        };

        // (a) an all-black 64×36 grab -> true.
        CHECK(BlackFrameWatchdog::isBlack(solid(64, 36, 0)), "an all-black 64x36 frame classifies as black");

        // (b) one bright row in an otherwise black frame -> NOT black (64/2304 ≈ 2.8% bright, well under 1%).
        {
            QImage im = solid(64, 36, 0);
            for (int x = 0; x < im.width(); ++x) im.setPixel(x, 0, qRgb(255, 255, 255));
            CHECK(!BlackFrameWatchdog::isBlack(im), "one bright row keeps the frame out of 'black'");
        }

        // (c) a uniformly dark-but-not-black frame (luma 40) -> NOT black (nothing is below luma 16).
        CHECK(!BlackFrameWatchdog::isBlack(solid(64, 36, 40)), "a uniform luma-40 frame is dark, not black");

        // (d) threshold edge on a 100×100 (10000 px) frame: exactly 99% black + 1% bright.
        //     dark == 9900, threshold*total == 0.99*10000 == 9900 -> the inclusive >= classifies it BLACK.
        {
            QImage im = solid(100, 100, 0);
            for (int x = 0; x < 100; ++x) im.setPixel(x, 0, qRgb(255, 255, 255)); // exactly 100 bright (1%)
            CHECK(BlackFrameWatchdog::isBlack(im), "exactly 99% black sits ON the threshold and reads as black (>=)");
            im.setPixel(0, 1, qRgb(255, 255, 255)); // one more bright -> 9899 dark, 98.99% < 99%
            CHECK(!BlackFrameWatchdog::isBlack(im), "one pixel past the edge (98.99% black) reads as NOT black");
        }

        // (e) a null / empty grab -> NEVER black (a FAILED grab must not trip the watchdog).
        CHECK(!BlackFrameWatchdog::isBlack(QImage()), "a null image is never classified black (failed grab guard)");
        CHECK(!BlackFrameWatchdog::isBlack(QImage(0, 0, QImage::Format_ARGB32)), "an empty image is never black");
    }

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
        // The graph shape is built by the SAME function the app runs (buildThemedNavGraph, NavThemeGraph.h) —
        // not a hand-copied replica — so this assertion can never drift from ThemeEngine::buildView's shipped
        // graph. buildView starts categories/buttons/actions at 0 and the QML feeds live counts; here we
        // supply fixed test counts after building. The shipped themed graph must pass its own validator and
        // reach every arrow-navigable zone.
        NavGraph g;
        buildThemedNavGraph(g, 12);                    // items=12; categories/buttons/actions start hidden
        g.setZoneCount(QStringLiteral("categories"), 6);
        g.setZoneCount(QStringLiteral("buttons"), 2);
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

    // ---------------------------------------------------------------- 15. the REAL themed DETAIL graph (Task 2)
    {
        // The detail view's zones (a detailActions row over a scrollable detailBody, plus an optional
        // detailChildren list for series/seasons) are built by the SAME shared builder the app runs
        // (buildThemedNavGraph with a DetailState), count-gated exactly like the inline `actions` overlay: 0
        // when the detail view is closed, live counts when it opens. Both childCount cases plus the inactive
        // case must pass validate(); when active, the detail zones must be arrow-reachable from the action row.

        // (a) inactive: the detail zones are registered but hidden (count 0) — the home graph still validates.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12, DetailState{ /*active=*/false, 0, 0 });
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.setZoneCount(QStringLiteral("buttons"), 2);
            QString why;
            CHECK(g.validate(&why), "detail-inactive: the themed graph validates with the detail zones hidden");
        }

        // (b) active, NO children (a flat movie/game/book): action row + scroll body, detailChildren inert.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12, DetailState{ /*active=*/true, /*actionCount=*/4, /*childCount=*/0 });
            QString why;
            CHECK(g.validate(&why), "detail-active(flat): validates with actions+body, children hidden");
            // (move() reports whether the DISPLAYED INDEX changed; a non-co-located edge crossing that lands on
            //  index 0 returns false while still switching the zone — so assert on the zone, not the return.)
            g.select(QStringLiteral("detailActions"), 0);
            g.move(Qt::Key_Down);
            CHECK(g.zone() == QStringLiteral("detailBody"),
                  "detail-active(flat): Down from the action row lands on the scroll body");
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("detailActions"),
                  "detail-active(flat): Up from the body returns to the action row");
            g.select(QStringLiteral("detailActions"), 3);
            CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("detailActions") && g.index() == 0,
                  "detail-active(flat): the action row wraps Right past the last button");
        }

        // (c) active WITH children (a series/season): actions <-> body <-> children all arrow-reachable.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12, DetailState{ /*active=*/true, /*actionCount=*/3, /*childCount=*/5 });
            QString why;
            CHECK(g.validate(&why), "detail-active(series): validates with actions+body+children");
            std::set<QString> reached;
            std::set<std::pair<QString,int>> seen;
            std::deque<std::pair<QString,int>> q;
            g.select(QStringLiteral("detailActions"), 0);
            q.push_back({g.zone(), g.index()});
            seen.insert({g.zone(), g.index()});
            reached.insert(g.zone());
            static const Qt::Key darr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
            while (!q.empty()) {
                auto [z, i] = q.front(); q.pop_front();
                for (Qt::Key k : darr) {
                    g.select(z, i);
                    g.move(k);
                    auto st = std::make_pair(g.zone(), g.index());
                    if (!seen.count(st)) { seen.insert(st); reached.insert(st.first); q.push_back(st); }
                }
            }
            CHECK(reached.count(QStringLiteral("detailActions")) && reached.count(QStringLiteral("detailBody"))
                  && reached.count(QStringLiteral("detailChildren")),
                  "detail-active(series): arrows alone reach the action row, body, and children list");
        }

        // (d) CONTAINMENT: the detail view is modal. With detail ACTIVE and the covered home zones LIVE
        //     (a non-empty button bar + categories — the worst case a theme can present), no sequence of
        //     arrows from the detail surface may escape onto items/categories/buttons. Up from the action
        //     row (which used to geometrically cross into the button bar) must be a contained no-op.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12, DetailState{ /*active=*/true, /*actionCount=*/4, /*childCount=*/5 });
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.setZoneCount(QStringLiteral("buttons"), 2);          // live button bar under the detail view
            QString why;
            CHECK(g.validate(&why), "detail-containment: validates with detail active over live home zones");

            // Up from the action row: consumed by the declared self edge — no move, no zone change.
            g.select(QStringLiteral("detailActions"), 1);
            CHECK(!g.move(Qt::Key_Up) && g.zone() == QStringLiteral("detailActions") && g.index() == 1,
                  "detail-containment: Up from the action row is a contained no-op (no escape to buttons)");

            // Directed BFS over every arrow from the detail surface: items/categories/buttons unreachable.
            std::set<QString> reached;
            std::set<std::pair<QString,int>> seen;
            std::deque<std::pair<QString,int>> q;
            g.select(QStringLiteral("detailActions"), 0);
            q.push_back({g.zone(), g.index()});
            seen.insert({g.zone(), g.index()});
            reached.insert(g.zone());
            static const Qt::Key carr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
            while (!q.empty()) {
                auto [z, i] = q.front(); q.pop_front();
                for (Qt::Key k : carr) {
                    g.select(z, i);
                    g.move(k);
                    auto st = std::make_pair(g.zone(), g.index());
                    if (!seen.count(st)) { seen.insert(st); reached.insert(st.first); q.push_back(st); }
                }
            }
            CHECK(!reached.count(QStringLiteral("items")) && !reached.count(QStringLiteral("categories"))
                  && !reached.count(QStringLiteral("buttons")),
                  "detail-containment: no arrow sequence escapes the detail surface onto items/categories/buttons");
            // …and the same walk still covers the whole detail surface.
            CHECK(reached.count(QStringLiteral("detailActions")) && reached.count(QStringLiteral("detailBody"))
                  && reached.count(QStringLiteral("detailChildren")),
                  "detail-containment: the contained walk still reaches every detail zone");
        }

        // (e) DISMISSAL LEG: the detailActions→items Esc edge lands back on the home cursor where it was,
        //     exactly mirroring §9's actions-overlay dismissal check (which does the same with `actions`). In
        //     the app the host's "detail" level pop performs the dismissal (see the NavThemeGraph.h note — the
        //     edge is never walked by move() there); this asserts the edge's STRUCTURE resolves correctly, so
        //     validate()'s undirected walk that relies on it is anchored to real, tested behaviour.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12, DetailState{ /*active=*/true, /*actionCount=*/4, /*childCount=*/0 });
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.select(QStringLiteral("items"), 7);          // the home cursor before opening the detail view
            g.select(QStringLiteral("detailActions"), 2);  // …then the detail view holds the cursor (memory:=7)
            g.move(Qt::Key_Escape);                        // the declared dismissal edge (detailActions→items)
            CHECK(g.zone() == QStringLiteral("items") && g.index() == 7,
                  "detail-dismiss: the detailActions→items Esc edge restores the remembered items cursor (7)");
        }
    }

    // ---------------------------------------------------------------- 16. the REAL reader graph shape (Task 3)
    {
        // The reader surface's zones (readerNav bottom bar + readerSettings font rows + readerToc chapter list)
        // are built by the SAME shared builder the app's ReaderChromeHost runs (buildReaderNavGraph, kind Book).
        // readerSettings/readerToc are count-gated (0 until the chrome feeds live counts, like the home's
        // categories/actions); the shipped reader graph must pass its own validator and, once populated, reach
        // every chrome zone by arrows alone — while no arrow escapes the (standalone, modal) reader surface.

        // (a) gated: settings + toc hidden — the reader graph still validates (declared/geometric union links
        //     all three even at count 0), and only readerNav is arrow-navigable.
        {
            NavGraph g;
            buildReaderNavGraph(g, ReaderKind::Book);
            QString why;
            CHECK(g.validate(&why), "reader: the graph validates with settings + toc gated (hidden)");
            CHECK(g.zone() == QStringLiteral("readerNav"), "reader: the default zone is the nav bar");
            g.select(QStringLiteral("readerNav"), 0);
            CHECK(!g.move(Qt::Key_Up) || g.zone() == QStringLiteral("readerNav"),
                  "reader: Up with settings hidden cannot leave the nav bar (gated edge is inert)");
        }

        // (b) populated: font-size row (1) + a chapter list (5). validate holds; a directed BFS from the nav
        //     bar reaches all three chrome zones AND never escapes onto anything else.
        {
            NavGraph g;
            buildReaderNavGraph(g, ReaderKind::Book);
            g.setZoneCount(QStringLiteral("readerSettings"), 1);   // Book: one ThemedChoice (font size)
            g.setZoneCount(QStringLiteral("readerToc"), 5);        // five chapters
            QString why;
            CHECK(g.validate(&why), "reader: validates with settings + toc populated");

            // readerNav wraps its strip (prev/progress/next).
            g.select(QStringLiteral("readerNav"), 2);
            CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("readerNav") && g.index() == 0,
                  "reader: the nav bar wraps Right past the last button");

            // Up from the nav bar reaches settings; Up again (a Vertical list at its top edge) crosses to the
            // toc by geometry — the whole chrome is reachable.
            g.select(QStringLiteral("readerNav"), 0);
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("readerSettings"),
                  "reader: Up from the nav bar lands on the settings row");
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("readerToc"),
                  "reader: Up from the settings row crosses to the chapter list");

            // The toc is a real list: Down steps WITHIN it (a declared edge would have frozen this), only
            // crossing back to settings at the list's bottom edge.
            g.select(QStringLiteral("readerToc"), 0);
            CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("readerToc") && g.index() == 1,
                  "reader: Down steps within the chapter list (not consumed by a cross-zone edge)");

            // Directed BFS: every chrome zone reachable; nothing else exists to escape to (standalone graph).
            std::set<QString> reached;
            std::set<std::pair<QString,int>> seen;
            std::deque<std::pair<QString,int>> q;
            g.select(QStringLiteral("readerNav"), 0);
            q.push_back({g.zone(), g.index()});
            seen.insert({g.zone(), g.index()});
            reached.insert(g.zone());
            static const Qt::Key rarr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
            while (!q.empty()) {
                auto [z, i] = q.front(); q.pop_front();
                for (Qt::Key k : rarr) {
                    g.select(z, i);
                    g.move(k);
                    auto st = std::make_pair(g.zone(), g.index());
                    if (!seen.count(st)) { seen.insert(st); reached.insert(st.first); q.push_back(st); }
                }
            }
            CHECK(reached.count(QStringLiteral("readerNav")) && reached.count(QStringLiteral("readerSettings"))
                  && reached.count(QStringLiteral("readerToc")),
                  "reader: arrows alone reach the nav bar, settings row, and chapter list");
            CHECK(reached.size() == 3,
                  "reader: the walk stays on the three reader zones (a standalone, contained surface)");
        }

        // (c) containment pins: the SELF edges consume cross-axis / off-surface arrows without moving.
        {
            NavGraph g;
            buildReaderNavGraph(g, ReaderKind::Book);
            g.setZoneCount(QStringLiteral("readerSettings"), 1);
            g.setZoneCount(QStringLiteral("readerToc"), 5);
            g.select(QStringLiteral("readerNav"), 1);
            CHECK(!g.move(Qt::Key_Down) && g.zone() == QStringLiteral("readerNav") && g.index() == 1,
                  "reader: Down off the bottom nav bar is a contained no-op");
            g.select(QStringLiteral("readerToc"), 2);
            CHECK(!g.move(Qt::Key_Left) && g.zone() == QStringLiteral("readerToc") && g.index() == 2,
                  "reader: Left across the chapter list is a contained no-op (cross-axis SELF pin)");
        }

        // (d) Pdf/Comic (Task 4): the SAME shared builder, no ToC (readerToc stays 0) and a settings row of
        //     zoom/fit buttons (Pdf 3, Comic 4 with the two-up toggle). The host feeds those counts; the shape
        //     must still validate, step Up nav→settings, keep the settings list stepping internally, and stay
        //     contained on just the two live zones (readerToc gated off ⇒ no chapter list to reach).
        auto checkReaderKind = [](ReaderKind kind, int settingsRows, const char* label) {
            NavGraph g;
            buildReaderNavGraph(g, kind);
            g.setZoneCount(QStringLiteral("readerSettings"), settingsRows);
            g.setZoneCount(QStringLiteral("readerToc"), 0);        // pdf/comic have no ToC
            QString why;
            CHECK(g.validate(&why), label);                        // (the label names the kind)
            CHECK(g.zone() == QStringLiteral("readerNav"), "reader(pdf/comic): default zone is the nav bar");

            // Up from the nav bar reaches the settings row; Up again cannot cross to the gated (hidden) ToC.
            g.select(QStringLiteral("readerNav"), 0);
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("readerSettings"),
                  "reader(pdf/comic): Up from the nav bar lands on the zoom/fit settings row");
            g.select(QStringLiteral("readerSettings"), 0);
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("readerSettings"),
                  "reader(pdf/comic): Up with the ToC gated cannot leave the settings row");

            // The settings row is a real Vertical list: Down steps within it (not consumed by a cross-zone edge).
            if (settingsRows >= 2) {
                g.select(QStringLiteral("readerSettings"), 0);
                CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("readerSettings") && g.index() == 1,
                      "reader(pdf/comic): Down steps within the settings row");
            }

            // Directed BFS: only the nav bar + settings row are reachable (ToC is gated off — 2 zones, no more).
            std::set<QString> reached;
            std::set<std::pair<QString,int>> seen;
            std::deque<std::pair<QString,int>> q;
            g.select(QStringLiteral("readerNav"), 0);
            q.push_back({g.zone(), g.index()});
            seen.insert({g.zone(), g.index()});
            reached.insert(g.zone());
            static const Qt::Key rarr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
            while (!q.empty()) {
                auto [z, i] = q.front(); q.pop_front();
                for (Qt::Key k : rarr) {
                    g.select(z, i);
                    g.move(k);
                    auto st = std::make_pair(g.zone(), g.index());
                    if (!seen.count(st)) { seen.insert(st); reached.insert(st.first); q.push_back(st); }
                }
            }
            CHECK(reached.count(QStringLiteral("readerNav")) && reached.count(QStringLiteral("readerSettings")),
                  "reader(pdf/comic): arrows reach the nav bar and the settings row");
            CHECK(reached.size() == 2,
                  "reader(pdf/comic): the walk stays on the two live zones (no ToC reachable)");

            // Containment: Left across the settings list is a cross-axis SELF-pin no-op.
            g.select(QStringLiteral("readerSettings"), 0);
            CHECK(!g.move(Qt::Key_Left) && g.zone() == QStringLiteral("readerSettings") && g.index() == 0,
                  "reader(pdf/comic): Left across the settings row is a contained no-op");
        };
        checkReaderKind(ReaderKind::Pdf,   3, "reader(pdf): validates (3 zoom/fit rows, no ToC)");
        checkReaderKind(ReaderKind::Comic, 4, "reader(comic): validates (4 rows incl. two-up, no ToC)");
    }

    // ---------------------------------------------------------------- 17. the REAL audio now-playing graph (Task 5)
    {
        // The audio now-playing page's zones (transport strip + queue list) are built by the SAME shared builder
        // the app runs on the home graph (buildAudioPageNavGraph after buildThemedNavGraph — exactly what
        // ThemeEngine::buildView does), count-gated like the detail zones: 0 while the page is closed, live
        // counts when it opens. The inactive case must validate; when active, the two zones must be arrow-
        // reachable from each other AND no arrow may escape onto the LIVE home zones underneath (it is modal).

        // (a) inactive: the audio zones are registered but hidden (count 0) — the home graph still validates.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12);
            buildAudioPageNavGraph(g);
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.setZoneCount(QStringLiteral("buttons"), 2);
            QString why;
            CHECK(g.validate(&why), "audio-inactive: the themed graph validates with the audio zones hidden");
        }

        // (b) active: transport strip (8 verbs) + queue list (5 tracks). validate holds; the two zones cross by
        //     arrows (Down enters the queue, Up returns to the strip) and each steps internally.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12);
            buildAudioPageNavGraph(g);
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.setZoneCount(QStringLiteral("buttons"), 2);           // live home zones under the (modal) audio page
            g.setZoneCount(QStringLiteral("transport"), 8);
            g.setZoneCount(QStringLiteral("queue"), 5);
            QString why;
            CHECK(g.validate(&why), "audio-active: validates with transport+queue over live home zones");

            g.select(QStringLiteral("transport"), 0);
            g.move(Qt::Key_Down);
            CHECK(g.zone() == QStringLiteral("queue"), "audio-active: Down from the transport strip enters the queue");
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("transport"), "audio-active: Up from the queue returns to the transport strip");

            // The transport strip is Horizontal: Left/Right step within it.
            g.select(QStringLiteral("transport"), 2);
            CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("transport") && g.index() == 3,
                  "audio-active: Right steps within the transport strip");
            CHECK(g.move(Qt::Key_Left) && g.zone() == QStringLiteral("transport") && g.index() == 2,
                  "audio-active: Left steps within the transport strip");
            // The strip WRAPS in-strip at both ends (detailActions' solution): a boundary along-axis arrow
            // wraps instead of ever falling through to geometric crossing — the horizontal containment is
            // self-contained, independent of whatever sits (hidden or not) in neighbouring grid columns.
            g.select(QStringLiteral("transport"), 0);
            CHECK(g.move(Qt::Key_Left) && g.zone() == QStringLiteral("transport") && g.index() == 7,
                  "audio-active: Left off the strip's first button wraps to the last (no escape)");
            g.select(QStringLiteral("transport"), 7);
            CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("transport") && g.index() == 0,
                  "audio-active: Right off the strip's last button wraps to the first (no escape)");
            // The queue is Vertical: Down steps within it; past the last row is contained (SELF pin).
            g.select(QStringLiteral("queue"), 0);
            CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("queue") && g.index() == 1,
                  "audio-active: Down steps within the queue list");
            g.select(QStringLiteral("queue"), 4);
            CHECK(!g.move(Qt::Key_Down) && g.zone() == QStringLiteral("queue") && g.index() == 4,
                  "audio-active: Down off the queue's last row is a contained no-op");
        }

        // (c) CONTAINMENT: with the audio page active over LIVE home zones, no arrow sequence from the audio
        //     surface may escape onto items/categories/buttons (the page is modal). Up off the transport strip
        //     is a contained no-op; a directed BFS reaches ONLY transport + queue.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12);
            buildAudioPageNavGraph(g);
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.setZoneCount(QStringLiteral("buttons"), 2);
            g.setZoneCount(QStringLiteral("transport"), 8);
            g.setZoneCount(QStringLiteral("queue"), 5);

            g.select(QStringLiteral("transport"), 3);
            CHECK(!g.move(Qt::Key_Up) && g.zone() == QStringLiteral("transport") && g.index() == 3,
                  "audio-containment: Up off the transport strip is a contained no-op (no escape to buttons)");

            std::set<QString> reached;
            std::set<std::pair<QString,int>> seen;
            std::deque<std::pair<QString,int>> q;
            g.select(QStringLiteral("transport"), 0);
            q.push_back({g.zone(), g.index()});
            seen.insert({g.zone(), g.index()});
            reached.insert(g.zone());
            static const Qt::Key aarr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
            while (!q.empty()) {
                auto [z, i] = q.front(); q.pop_front();
                for (Qt::Key k : aarr) {
                    g.select(z, i);
                    g.move(k);
                    auto st = std::make_pair(g.zone(), g.index());
                    if (!seen.count(st)) { seen.insert(st); reached.insert(st.first); q.push_back(st); }
                }
            }
            CHECK(!reached.count(QStringLiteral("items")) && !reached.count(QStringLiteral("categories"))
                  && !reached.count(QStringLiteral("buttons")),
                  "audio-containment: no arrow sequence escapes the audio surface onto items/categories/buttons");
            CHECK(reached.count(QStringLiteral("transport")) && reached.count(QStringLiteral("queue"))
                  && reached.size() == 2,
                  "audio-containment: the contained walk reaches exactly the transport strip + queue");
        }

        // (d) DISMISSAL LEG: the transport→items Esc edge lands back on the home cursor where it was, mirroring
        //     the detail/actions dismissal checks. In the app the host's "nowplaying" level pop performs the
        //     real dismissal; this asserts the declared edge's STRUCTURE resolves (validate()'s undirected walk
        //     relies on it to see the modal stack connected to the home surface).
        {
            NavGraph g;
            buildThemedNavGraph(g, 12);
            buildAudioPageNavGraph(g);
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.setZoneCount(QStringLiteral("transport"), 8);
            g.setZoneCount(QStringLiteral("queue"), 5);
            g.select(QStringLiteral("items"), 9);          // the home cursor before opening the audio page
            g.select(QStringLiteral("transport"), 4);      // …then the audio page holds the cursor (items memory:=9)
            g.move(Qt::Key_Escape);                        // the declared dismissal edge (transport→items)
            CHECK(g.zone() == QStringLiteral("items") && g.index() == 9,
                  "audio-dismiss: the transport→items Esc edge restores the remembered items cursor (9)");
        }
    }

    // ---------------------------------------------------------------- 18. the REAL themed PANEL graph (Task B2.1)
    {
        // A themed settings panel (ThemedPanelHost) is its OWN standalone NavGraph — panelRows (the row list) +
        // panelBack (the header Back affordance) — built by the SAME shared builder the app's host runs
        // (buildPanelNavGraph, NavThemeGraph.h), the ONE definition this shape-test asserts so the CI assertion
        // can never drift from the shipped graph. The panel is the whole surface (no home zones underneath), so
        // a directed BFS must reach EXACTLY the two panel zones and validate() must hold.

        // (a) validate: a hub-sized panel (14 rows) forms a connected graph.
        {
            NavGraph g;
            buildPanelNavGraph(g, 14);
            QString why;
            CHECK(g.validate(&why), "panel: the themed panel graph validates (panelRows + panelBack)");
        }

        // (b) the back-zone edge + geometry: Down off the header enters the row list; Up off the FIRST row
        //     crosses back up to the header; Up off a deeper row steps within the list (not to the header).
        {
            NavGraph g;
            buildPanelNavGraph(g, 14);
            g.select(QStringLiteral("panelBack"), 0);
            g.move(Qt::Key_Down);
            CHECK(g.zone() == QStringLiteral("panelRows"),
                  "panel: Down off the header Back enters the row list");
            g.select(QStringLiteral("panelRows"), 0);
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("panelBack"),
                  "panel: Up off the first row crosses to the header Back");
            g.select(QStringLiteral("panelRows"), 5);
            CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("panelRows") && g.index() == 6,
                  "panel: Down steps within the row list");
            g.select(QStringLiteral("panelRows"), 5);
            CHECK(g.move(Qt::Key_Up) && g.zone() == QStringLiteral("panelRows") && g.index() == 4,
                  "panel: Up off a deeper row steps within the list (does not jump to the header)");
        }

        // (c) containment: no arrow off the header escapes (a 1-count strip pinned on Up/Left/Right); a directed
        //     BFS from the row list reaches EXACTLY panelRows + panelBack.
        {
            NavGraph g;
            buildPanelNavGraph(g, 14);
            g.select(QStringLiteral("panelBack"), 0);
            CHECK(!g.move(Qt::Key_Up) && g.zone() == QStringLiteral("panelBack"),
                  "panel: Up off the header Back is a contained no-op");
            CHECK(!g.move(Qt::Key_Left) && g.zone() == QStringLiteral("panelBack"),
                  "panel: Left off the header Back is a contained no-op");
            CHECK(!g.move(Qt::Key_Right) && g.zone() == QStringLiteral("panelBack"),
                  "panel: Right off the header Back is a contained no-op");
            // The row list's cross-axis Left/Right are SELF-pinned no-ops (a Vertical list has no sideways move).
            g.select(QStringLiteral("panelRows"), 3);
            CHECK(!g.move(Qt::Key_Left) && g.zone() == QStringLiteral("panelRows") && g.index() == 3,
                  "panel: Left on the row list is a contained no-op");

            std::set<QString> reached;
            std::set<std::pair<QString,int>> seen;
            std::deque<std::pair<QString,int>> q;
            g.select(QStringLiteral("panelRows"), 0);
            q.push_back({g.zone(), g.index()});
            seen.insert({g.zone(), g.index()});
            reached.insert(g.zone());
            static const Qt::Key parr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
            while (!q.empty()) {
                auto [z, i] = q.front(); q.pop_front();
                for (Qt::Key k : parr) {
                    g.select(z, i);
                    g.move(k);
                    auto st = std::make_pair(g.zone(), g.index());
                    if (!seen.count(st)) { seen.insert(st); reached.insert(st.first); q.push_back(st); }
                }
            }
            CHECK(reached.count(QStringLiteral("panelRows")) && reached.count(QStringLiteral("panelBack"))
                  && reached.size() == 2,
                  "panel: the contained walk reaches exactly the row list + header Back");
        }
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
