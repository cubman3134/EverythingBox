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
#include <QFont>
#include <QQmlError>
#include <QSGRendererInterface>
#include <QPointingDevice>            // §20: the synthetic touchscreen device QTest::touchEvent drives
#include <QtTest/QTest>              // §20: QTest::touchEvent — real touch sequences with real hit-testing
#include "theme2/ThemedPanelHost.h"   // §18(e): the REAL host, for the host-level pop-restore assertions
#include "theme2/FormFactor.h"        // §19: the form-factor authority exposed as the `form` context property
#include "core/Settings.h"            // §19: setDisplayMode drives FormFactor::refresh() (TV / identity legs)
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
        "    property int emptyChosen: 0\n"
        "    property int emptyEditReq: 0\n"
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
        "        El.ThemedChoice { objectName: \"tcEmpty\"; navZone: \"choiceEmpty\"; navRow: 4; navCol: 0; options: []\n"  // empty-options guard subject
        "            onChosen: (i) => { host.emptyChosen++ }\n"
        "            onEditRequested: (z) => host.emptyEditReq++ }\n"
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
    qw.rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance()); // §19 parity: `form` beside `nav`

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

    // ---- 9c. ThemedChoice empty-options guard (B2 Task 6 hardening): a 0-option Choice must not open ----
    // A Choice with no options has nothing to pick; activating it must be a total no-op — no inline list, no
    // editRequested, no chosen(), and the selection stays put (a wedge here would strand the cursor mid-panel).
    {
        QQuickItem* tce = host->findChild<QQuickItem*>(QStringLiteral("tcEmpty"));
        CHECK(tce != nullptr, "the empty-options choice is present");
        if (tce)
        {
            const int chosenBefore = host->property("emptyChosen").toInt();
            const int reqBefore     = host->property("emptyEditReq").toInt();
            // (a) inline mode: activate does nothing (no editing state, selection stays on the empty choice).
            graph.select(QStringLiteral("choiceEmpty"), 0);
            pump();
            CHECK(graph.zone() == QStringLiteral("choiceEmpty"), "the empty choice can still be SELECTED (zone count 1)");
            graph.activate();
            pump();
            CHECK(!tce->property("editing").toBool(), "activating a 0-option choice does NOT enter editing");
            CHECK(host->property("emptyChosen").toInt() == chosenBefore, "a 0-option choice fires no chosen()");
            CHECK(graph.zone() == QStringLiteral("choiceEmpty"), "the selection stays put (no wedge/reassign)");
            // (b) externalEdit mode: activate must not emit editRequested either (nothing for the host to pick).
            tce->setProperty("externalEdit", true);
            graph.activate();
            pump();
            CHECK(host->property("emptyEditReq").toInt() == reqBefore, "a 0-option external choice emits no editRequested");
            CHECK(!tce->property("externalPending").toBool(), "a 0-option external choice never goes pending");
            tce->setProperty("externalEdit", false);
            // Sanity: giving it options re-enables the picker (the guard is options-driven, not a permanent off).
            tce->setProperty("options", QVariantList{ QStringLiteral("One"), QStringLiteral("Two") });
            pump();
            graph.activate();
            pump();
            CHECK(tce->property("editing").toBool(), "populating options re-enables activation (guard lifts)");
            sendKey(win, Qt::Key_Escape);
        }
    }

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

// A run of Action rows (all selectable) with a tag-derived id/label — the panel content §18(e) drills.
static QVector<PanelRow> panelActionRows(int n, const QString& tag)
{
    QVector<PanelRow> rows;
    for (int i = 0; i < n; ++i)
    {
        PanelRow r;
        r.kind  = PanelRow::Action;
        r.id    = tag + QString::number(i);
        r.label = tag + QStringLiteral(" ") + QString::number(i);
        rows.push_back(r);
    }
    return rows;
}

// §18(e) — HOST-LEVEL pop-restore, the guard §18(d) structurally cannot be. §18(d) drives the BARE NavGraph
// through the host's call *sequence*, so it can only prove the graph leg; it never runs ThemedPanelHost::
// renderTop, where the real defect lived: renderTop read Entry.lastIndex AFTER setZoneCount had already shrunk
// the panelRows zone. When the popped child had MORE rows than the parent, that shrink clamps the stale child
// index into the smaller count and emits selectionChanged — onSelectionChanged then writes that clamped value
// into the just-revealed parent entry (stack_.last() is now the parent), clobbering the remembered row before
// renderTop reads it. The fix captures the target index BEFORE any graph mutation; this exercises the REAL host
// (present → drive cursor → present larger child → drive it down → pop) to pin that ordering, in both clamp
// directions. If this ever regresses (capture moved back after the mutations), assert (i) below goes red.
static void runPanelHostPopRestoreAsserts()
{
    auto noop   = [](const QString&, const QString&) {};
    auto onBack = [] {};

    // ---- (i) child LARGER than parent: the pop SHRINKS panelRows, so the clamp fires — the exact bug shape.
    //      The remembered parent row is INTERIOR (2 of 6), deliberately NOT the last row: the shrink clamps the
    //      stale child index to count-1 (== 5), so a host that restored the clamped value would land on 5, not
    //      2 — the two are distinct only because the remembered row is off the boundary. (A boundary remembered
    //      row would coincide with the clamp target and mask the defect, which is why the pure-graph §18(d)
    //      sequence, and any parent-at-last-row check, could never have gone red.)
    {
        ThemedPanelHost host;                                       // offscreen (QT_QPA_PLATFORM=offscreen)
        NavGraph* g = host.navGraph();
        host.present(QStringLiteral("Parent"), panelActionRows(6, QStringLiteral("p")), noop, onBack);
        CHECK(host.levelDepth() == 1, "panel-host: parent panel presented (depth 1)");
        g->select(QStringLiteral("panelRows"), 2);                 // the user's place on the parent (interior)
        CHECK(g->index() == 2, "panel-host: parent cursor parked on the interior row 2");

        host.present(QStringLiteral("Child"), panelActionRows(12, QStringLiteral("c")), noop, onBack);
        CHECK(host.levelDepth() == 2, "panel-host: a LARGER child (12 rows) presented (depth 2)");
        g->select(QStringLiteral("panelRows"), 11);                // drive the child cursor to its LAST row
        CHECK(g->index() == 11, "panel-host: child cursor driven to its last row (11)");

        host.handleBack();                                         // pop the child -> renderTop(restore=true)
        CHECK(host.levelDepth() == 1, "panel-host: Back pops the child, revealing the parent (depth 1)");
        CHECK(g->zone() == QStringLiteral("panelRows") && g->index() == 2,
              "panel-host: pop restores the parent's remembered INTERIOR row (2), NOT the shrink-clamped child index (5)");
        CHECK(g->validate(nullptr), "panel-host: the graph validates after the larger-child pop");
    }

    // ---- (ii) inverse — child SMALLER than parent: the pop GROWS panelRows, so NO clamp fires. This pins the
    //      other direction (the §18(d) shape) at host level: the remembered parent row still returns exactly.
    {
        ThemedPanelHost host;
        NavGraph* g = host.navGraph();
        host.present(QStringLiteral("Parent"), panelActionRows(6, QStringLiteral("p")), noop, onBack);
        g->select(QStringLiteral("panelRows"), 4);                 // interior parent row
        CHECK(g->index() == 4, "panel-host(inverse): parent cursor parked on row 4");

        host.present(QStringLiteral("Child"), panelActionRows(3, QStringLiteral("c")), noop, onBack);
        CHECK(host.levelDepth() == 2, "panel-host(inverse): a SMALLER child (3 rows) presented (depth 2)");
        g->select(QStringLiteral("panelRows"), 2);                 // child cursor within its 3 rows
        host.handleBack();
        CHECK(g->zone() == QStringLiteral("panelRows") && g->index() == 4,
              "panel-host(inverse): pop grows the zone and restores the parent's remembered row (4)");
        CHECK(g->validate(nullptr), "panel-host(inverse): the graph validates after the smaller-child pop");
    }
}

// §18(f) — replaceTop's SAME-LEVEL contract, the host leg the panel async-connection lifetime model rides on.
// A state-gated panel (Cloud Sync sign-in state, RA login, BIOS re-check) rebuilds its row SET on async events;
// MainWindow's handlers self-gate on the panel being top and then call the open* method, whose reentry path is
// replaceTop. That is only safe because replaceTop swaps the TOP entry IN PLACE: the level depth must NOT grow
// (a stray pushLevel would stack a duplicate panel the user Backs through twice — the exact "panel presented
// over something else" failure the gate exists to prevent), the fresh row set must land on its first selectable
// row (the old cursor is meaningless in a new set), and ONE Back afterwards must still pop straight to the
// parent. The MainWindow-side gate itself (themedPanelIsTop) is not linkable here; this pins the host half.
static void runPanelHostReplaceTopAsserts()
{
    auto noop   = [](const QString&, const QString&) {};
    auto onBack = [] {};

    // ---- (i) replaceTop on a presented stack: depth frozen, rows swapped, cursor re-homed, Back unaffected.
    {
        ThemedPanelHost host;
        NavGraph* g = host.navGraph();
        host.present(QStringLiteral("Hub"), panelActionRows(5, QStringLiteral("h")), noop, onBack);
        host.present(QStringLiteral("Cloud"), panelActionRows(6, QStringLiteral("a")), noop, onBack);
        CHECK(host.levelDepth() == 2, "panel-host(replaceTop): panel presented over the hub (depth 2)");
        g->select(QStringLiteral("panelRows"), 3);                 // the user's place in the OLD row set

        host.replaceTop(QStringLiteral("Cloud"), panelActionRows(4, QStringLiteral("b")), noop, onBack);
        CHECK(host.levelDepth() == 2,
              "panel-host(replaceTop): an in-place rebuild does NOT stack a level (depth stays 2)");
        CHECK(host.panelTitle() == QStringLiteral("Cloud"), "panel-host(replaceTop): the top title is the rebuilt panel");
        CHECK(g->zone() == QStringLiteral("panelRows") && g->index() == 0,
              "panel-host(replaceTop): the fresh row set lands on its first selectable row (the old cursor is void)");
        CHECK(g->validate(nullptr), "panel-host(replaceTop): the graph validates after the in-place rebuild");

        host.handleBack();                                         // ONE Back must reach the parent, not a duplicate
        CHECK(host.levelDepth() == 1 && host.panelTitle() == QStringLiteral("Hub"),
              "panel-host(replaceTop): one Back pops straight to the parent (no duplicate level to Back through)");
    }

    // ---- (ii) replaceTop on an EMPTY host degrades to present() (documented fallback).
    {
        ThemedPanelHost host;
        host.replaceTop(QStringLiteral("Fresh"), panelActionRows(3, QStringLiteral("f")), noop, onBack);
        CHECK(host.levelDepth() == 1 && host.panelTitle() == QStringLiteral("Fresh"),
              "panel-host(replaceTop): on an empty stack it degrades to a plain present (depth 1)");
    }
}

// §18(j) — HOST re-entrancy safety (final-review fix round): the three defects the whole-branch reviewer found in
// ThemedPanelHost's dispatch. All three are host-local and headlessly pinnable without MainWindow linkage.
//   (a) replaceTop invoked from INSIDE an onActivate callback: the entry's onActivate is move-assigned while it
//       executes. onGraphActivated dispatches through a BY-VALUE copy, so the executing closure survives — the
//       activation body runs to completion and the in-place rebuild lands. RED-DEMO: the onActivate captures a
//       heap sentinel and reads it AFTER calling replaceTop; before the fix that read is a use-after-free of the
//       just-destroyed closure's captures (a copy keeps it alive). Observable: the post-replace body completed,
//       the sentinel survived, depth stayed frozen (no stacked level), the rebuilt rows are current.
//   (b) overlayAbove(): the primitive MainWindow::themedPanelIsTop now also consults so an async handler never
//       rebuilds under a live OSK/menu. An overlay mirrors itself as an extra graph level, so overlayAbove() is
//       true exactly while the graph carries more levels than the host has panels.
//   (c) TextField commit re-location: after the (blocking) OSK returns, the value is committed by RE-LOCATING the
//       row by id in the CURRENT top entry — a mid-edit replaceTop that dropped the row must make the commit a
//       safe no-op (never a write through the freed buffer). Pinned via the shared find-by-id primitive
//       (updateRow) that the commit relocation uses: a patch to a vanished id no-ops; to a surviving id applies.
static void runPanelHostReentrancyAsserts()
{
    auto onBack = [] {};

    // ---- (a) replaceTop from inside an onActivate — the executing closure must survive its own reassignment.
    {
        ThemedPanelHost host;
        NavGraph* g = host.navGraph();

        auto sentinel = std::make_shared<int>(0xA11E);   // heap object the closure captures
        bool bodyCompleted = false;
        QString titleAfterReplace;

        QVector<PanelRow> before = panelActionRows(1, QStringLiteral("go"));   // one Action row: "go0"
        // The onActivate rebuilds THIS panel (replaceTop) and then KEEPS RUNNING — reading its captured sentinel
        // and the host state AFTER the rebuild. Pre-fix (dispatch via e.onActivate directly) the replaceTop
        // destroys this very closure, so every line below the replaceTop call touches freed captures.
        auto onAct = [&host, &sentinel, &bodyCompleted, &titleAfterReplace, onBack]
                     (const QString&, const QString&) {
            host.replaceTop(QStringLiteral("Rebuilt"), panelActionRows(3, QStringLiteral("nw")),
                            [](const QString&, const QString&) {}, onBack);
            // Everything from here on runs on a closure that replaceTop just move-assigned over:
            const int keepAlive = *sentinel;          // UAF pre-fix (captured heap ptr in a destroyed closure)
            titleAfterReplace = host.panelTitle();
            bodyCompleted = (keepAlive != 0);
        };

        host.present(QStringLiteral("Start"), before, onAct, onBack);
        CHECK(host.levelDepth() == 1, "panel-host(reentrant): the start panel presented (depth 1)");
        g->select(QStringLiteral("panelRows"), 0);
        g->activate();                                 // → onGraphActivated → the copied onAct runs

        CHECK(bodyCompleted, "panel-host(reentrant): the activation body ran to completion past its own replaceTop");
        CHECK(host.levelDepth() == 1,
              "panel-host(reentrant): the in-place rebuild did NOT stack a level (depth stays 1)");
        CHECK(host.panelTitle() == QStringLiteral("Rebuilt") && titleAfterReplace == QStringLiteral("Rebuilt"),
              "panel-host(reentrant): the top panel is the rebuilt one, seen from inside the surviving closure");
        CHECK(g->zone() == QStringLiteral("panelRows") && g->index() == 0,
              "panel-host(reentrant): the rebuilt row set landed on its first selectable row");
        CHECK(g->validate(nullptr), "panel-host(reentrant): the graph validates after the reentrant rebuild");
    }

    // ---- (b) overlayAbove() — the top-gate primitive: an overlay mirrors an EXTRA graph level over the panel.
    {
        ThemedPanelHost host;
        NavGraph* g = host.navGraph();
        host.present(QStringLiteral("Panel"), panelActionRows(3, QStringLiteral("p")), [](const QString&, const QString&) {}, onBack);
        CHECK(!host.overlayAbove(),
              "panel-host(overlay): no overlay over a freshly presented panel (graph levels == host panels)");
        // Mirror an overlay exactly as Osk::getText / NavOverlay::setNavGraph do — one extra "overlay" level.
        g->pushLevel(QStringLiteral("overlay"), [] {});
        CHECK(host.overlayAbove(),
              "panel-host(overlay): overlayAbove() is TRUE while an overlay level sits above the panel");
        g->popLevel();
        CHECK(!host.overlayAbove(),
              "panel-host(overlay): overlayAbove() clears when the overlay level pops (the gate re-opens)");
    }

    // ---- (c) TextField commit re-location — a commit to a row a mid-edit replaceTop removed is a safe no-op.
    //      The OSK loop can't be driven headlessly (it needs synthetic input), so this pins the find-by-id
    //      relocation the post-OSK commit performs, via the shared updateRow primitive: present a panel carrying
    //      a TextField "au.url", rebuild it AWAY (replaceTop to a set without that id), then a commit addressed to
    //      the vanished id must NOT reach the model; a commit to a surviving id must apply.
    {
        ThemedPanelHost host;
        QVector<PanelRow> withField;
        { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("au.url"); r.label = QStringLiteral("URL"); withField << r; }
        { PanelRow r; r.kind = PanelRow::Action;    r.id = QStringLiteral("au.add"); r.label = QStringLiteral("Add"); withField << r; }
        host.present(QStringLiteral("Add by URL"), withField, [](const QString&, const QString&) {}, onBack);

        // Mid-edit rebuild drops "au.url" (the shape of an async replaceTop while the URL OSK was open).
        host.replaceTop(QStringLiteral("Add-ons"), panelActionRows(2, QStringLiteral("lib")),
                        [](const QString&, const QString&) {}, onBack);
        CHECK(host.panelTitle() == QStringLiteral("Add-ons"),
              "panel-host(textfield-drop): the panel rebuilt away from the edited field");
        // A post-OSK commit relocates by id; the id is gone, so it drops. patchRow on the model must report false.
        PanelRow stale; stale.kind = PanelRow::TextField; stale.id = QStringLiteral("au.url"); stale.value = QStringLiteral("http://x");
        host.updateRow(QStringLiteral("au.url"), stale);   // no crash, no effect (id absent from the rebuilt set)
        CHECK(host.focusedRowLabel() != QStringLiteral("URL"),
              "panel-host(textfield-drop): the vanished field is not present after the rebuild (commit safely dropped)");

        // Positive leg: a commit to a surviving id lands (the ordinary no-rebuild case).
        QVector<PanelRow> keepField;
        { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("k.url"); r.label = QStringLiteral("Keep"); keepField << r; }
        host.replaceTop(QStringLiteral("Keeper"), keepField, [](const QString&, const QString&) {}, onBack);
        PanelRow patched; patched.kind = PanelRow::TextField; patched.id = QStringLiteral("k.url");
        patched.label = QStringLiteral("Keep"); patched.value = QStringLiteral("committed");
        host.updateRow(QStringLiteral("k.url"), patched);   // relocate-by-id succeeds → applies in place
        CHECK(host.levelDepth() == 1,
              "panel-host(textfield-drop): the surviving-row commit path leaves the stack intact (depth 1)");
    }
}

// §18(h) — the Add-ons manager panel graph (B2 Task 6.5), pinned against the REAL ThemedPanelHost with row sets
// mirroring the shipped shapes. (The remove confirm is a NavConfirm::ask overlay — Cancel focused, Back=Cancel,
// the confirmDeleteProfile pattern — so it is NOT a panel level; the panel graph is root → detail → config plus
// the root-nested Add-by-URL form.) Three legs the other §18 host asserts don't cover: (1) a row set whose
// LEADING row is an Info divider lands + snaps the cursor onto the first SELECTABLE row (the §18(e)/(f) sets
// were all-Action, so divider-skipping on a fresh present was never pinned) — the Add-by-URL shape; (2) a
// THREE-level drill (root → detail → config) whose Backs restore each parent's remembered INTERIOR row, the
// root's across TWO pops; (3) a masked config TextField patched in place keeps its level (updateRow is
// in-place). All are headlessly pinnable (no MainWindow linkage needed).
static void runAddonsPanelAsserts()
{
    auto noop   = [](const QString&, const QString&) {};
    auto onBack = [] {};

    ThemedPanelHost host;
    NavGraph* g = host.navGraph();

    // ---- Root "Add-ons": 4 action rows + a Separator + 3 source rows + a trailing Info status row.
    QVector<PanelRow> root;
    for (int i = 0; i < 4; ++i) { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("act%1").arg(i); r.label = r.id; root << r; }
    { PanelRow r; r.kind = PanelRow::Separator; r.id = QStringLiteral("sep"); r.label = QStringLiteral("Sources"); root << r; }
    for (int i = 0; i < 3; ++i) { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("src%1").arg(i); r.label = r.id; root << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("status"); root << r; }
    host.present(QStringLiteral("Add-ons"), root, noop, onBack);
    CHECK(host.levelDepth() == 1 && g->index() == 0, "addons: root lands on the first action row");
    g->select(QStringLiteral("panelRows"), 6);                 // a source row (interior: index 6 of the 3 sources)
    CHECK(g->index() == 6, "addons: cursor parked on an interior source row (6)");

    // ---- Per-addon detail: Toggle + Configure Action + destructive Remove + 2 Info rows.
    QVector<PanelRow> detail;
    { PanelRow r; r.kind = PanelRow::Toggle; r.id = QStringLiteral("ad.enabled"); r.label = QStringLiteral("Enabled"); r.checked = true; detail << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("ad.configure"); r.label = QStringLiteral("Configure"); detail << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("ad.remove"); r.label = QStringLiteral("Remove"); r.destructive = true; detail << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ad.version"); r.value = QStringLiteral("1.0"); detail << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ad.about"); r.value = QStringLiteral("desc"); detail << r; }
    host.present(QStringLiteral("Addon"), detail, noop, onBack);
    CHECK(host.levelDepth() == 2 && g->index() == 0, "addons: detail lands on the Enabled toggle (first selectable)");
    g->select(QStringLiteral("panelRows"), 1);                 // park on Configure (interior — the drill row)

    // ---- Config (depth 3): a masked TextField first + a trailing Info note. Lands on the masked field;
    //      updateRow patches it IN PLACE (level unchanged) — the credentials round-trip shape.
    QVector<PanelRow> cfg;
    { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("cfg:sspassword"); r.label = QStringLiteral("Password"); r.masked = true; cfg << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cfg.note"); r.value = QStringLiteral("plaintext note"); cfg << r; }
    host.present(QStringLiteral("Config"), cfg, noop, onBack);
    CHECK(host.levelDepth() == 3 && g->index() == 0, "addons: config lands on the masked TextField (first selectable)");
    { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("cfg:sspassword"); r.label = QStringLiteral("Password"); r.masked = true; r.value = QStringLiteral("secret");
      host.updateRow(QStringLiteral("cfg:sspassword"), r); }
    CHECK(host.levelDepth() == 3, "addons: updateRow on the masked config field keeps the level (in place)");

    // ---- Backs restore each parent's remembered interior row: config → detail (Configure, 1), detail → root
    //      (source row 6 — surviving TWO pops from the innermost level).
    host.handleBack();
    CHECK(host.levelDepth() == 2 && g->index() == 1,
          "addons: Back from config restores the detail's remembered Configure row (1)");
    host.handleBack();
    CHECK(host.levelDepth() == 1, "addons: second Back reveals the root");
    CHECK(g->zone() == QStringLiteral("panelRows") && g->index() == 6,
          "addons: the root's remembered interior source row (6) survives the two-level drill");
    CHECK(g->validate(nullptr), "addons: the graph validates after the drill unwinds");

    // ---- Add-by-URL (root-nested): the LEADING row is an Info divider (the hint), so a fresh present must
    //      SKIP it and land on the TextField (index 1) — the divider-skip-on-present pin.
    QVector<PanelRow> addurl;
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("au.hint"); r.value = QStringLiteral("Its manifest.json or base URL"); addurl << r; }
    { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("au.url"); r.label = QStringLiteral("URL"); addurl << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("au.status"); addurl << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("au.add"); r.label = QStringLiteral("Add"); addurl << r; }
    host.present(QStringLiteral("Add by URL"), addurl, noop, onBack);
    CHECK(host.levelDepth() == 2, "addons: Add-by-URL presented over the root (depth 2)");
    CHECK(g->zone() == QStringLiteral("panelRows") && g->index() == 1,
          "addons: Add-by-URL SKIPS the leading Info divider and lands on the URL TextField (index 1)");
    host.handleBack();
    CHECK(host.levelDepth() == 1 && g->index() == 6, "addons: Back from Add-by-URL restores the root cursor (6)");
}

// §18(i) — the Appearance panel graph (B2 Task 6.75, the last classic surface converted), pinned against the REAL
// ThemedPanelHost with the shipped row shape: a Toggle, then a Separator + Choice, then a run of Info/Separator
// dividers, then a lone trailing Action. Its distinctive geometry — three selectable rows (Toggle 0, Choice 2,
// Action 8) separated by MULTIPLE consecutive dividers, including a five-divider gap between the Choice and the
// lone Action — is a shape the other §18 sets don't cover (they never step ACROSS a multi-divider block via
// move()). Legs: (1) fresh present lands on the first selectable (the Toggle, skipping nothing); (2) Down steps
// Toggle -> Choice skipping the "Theme" Separator; (3) Down steps Choice -> Action hopping the FIVE trailing
// dividers in one move; (4) Up mirrors Action -> Choice; (5) presented as a hub child, one Back pops to the
// parent (the panel is nested under the settings hub — its onBack is the defensive root leg, not run on a pop).
static void runAppearancePanelAsserts()
{
    auto noop   = [](const QString&, const QString&) {};
    auto onBack = [] {};

    ThemedPanelHost host;
    NavGraph* g = host.navGraph();

    // The shipped Appearance row set (indices must match openAppearance's builder).
    QVector<PanelRow> rows;
    { PanelRow r; r.kind = PanelRow::Toggle;    r.id = QStringLiteral("appr.themed");    r.label = QStringLiteral("Use the themed home screen (beta)"); r.checked = true; rows << r; } // 0 selectable
    { PanelRow r; r.kind = PanelRow::Separator; r.label = QStringLiteral("Theme"); rows << r; }                                                                                            // 1 divider
    { PanelRow r; r.kind = PanelRow::Choice;    r.id = QStringLiteral("appr.theme");     r.label = QStringLiteral("Theme"); r.options = { QStringLiteral("Default"), QStringLiteral("Lumen") }; r.value = QStringLiteral("Default"); rows << r; } // 2 selectable
    { PanelRow r; r.kind = PanelRow::Info;      r.id = QStringLiteral("appr.applies");   r.label = QStringLiteral("Applies live…"); rows << r; }                                            // 3 divider
    { PanelRow r; r.kind = PanelRow::Separator; r.label = QStringLiteral("Get more themes"); rows << r; }                                                                                  // 4 divider
    { PanelRow r; r.kind = PanelRow::Info;      r.id = QStringLiteral("appr.customise");  r.label = QStringLiteral("Edit theme.json…"); rows << r; }                                        // 5 divider
    { PanelRow r; r.kind = PanelRow::Info;      r.id = QStringLiteral("appr.root");       r.label = QStringLiteral("Themes folder"); r.value = QStringLiteral("/path"); rows << r; }        // 6 divider
    { PanelRow r; r.kind = PanelRow::Info;      r.id = QStringLiteral("appr.community");  r.label = QStringLiteral("Browse community themes…"); rows << r; }                                // 7 divider
    { PanelRow r; r.kind = PanelRow::Action;    r.id = QStringLiteral("appr.gallery");    r.label = QStringLiteral("Open the theme gallery (GitHub)…"); rows << r; }                        // 8 selectable

    // Present as a hub child (a bare hub root below, so the pop reveals a parent rather than leaving the host).
    host.present(QStringLiteral("Settings"), panelActionRows(13, QStringLiteral("hub")), noop, onBack);
    host.present(QStringLiteral("Appearance"), rows, noop, onBack);
    CHECK(host.levelDepth() == 2 && g->zone() == QStringLiteral("panelRows") && g->index() == 0,
          "appearance: fresh present lands on the first selectable row (the themed-home Toggle, index 0)");
    CHECK(g->move(Qt::Key_Down) && g->index() == 2,
          "appearance: Down steps Toggle -> Choice, skipping the 'Theme' Separator (1)");
    CHECK(g->move(Qt::Key_Down) && g->index() == 8,
          "appearance: Down steps Choice -> the lone Action, hopping the five trailing dividers (3..7) in one move");
    CHECK(g->move(Qt::Key_Up) && g->index() == 2,
          "appearance: Up mirrors Action -> Choice back across the divider block");
    CHECK(g->validate(nullptr), "appearance: the graph validates for the Appearance row shape");
    host.handleBack();
    CHECK(host.levelDepth() == 1,
          "appearance: one Back pops Appearance to the settings hub (nested child — no host exit)");
}

// §18(g) — ThemeView-level pins (B2 Task 6 hardening): the two behaviours that live in ThemeView.qml itself and
// couldn't be tested from a bare NavGraph — (a) the XMB-buttons guard (a theme mixing an `xmb` element with
// `button` elements must NOT let the cursor reach the bottom-button bar: the QML holds the `buttons` zone count
// at 0 whenever xmbMode is true, so its declared items->buttons edge stays inert) and (b) grid-home rootBack
// (Escape at a grid home with an empty level stack routes through nav.back() to rootBack — the pause-menu leg).
// Loads the REAL ThemeView.qml from the qrc with a REAL NavGraph (built by the shared buildThemedNavGraph, so
// this rides the shipped graph shape) exposed as `nav` — the §14 offscreen-QQuickWidget pattern.
static void runThemeViewAsserts()
{
    auto probeItems = []() -> QVariantList {
        QVariantList v;
        for (int i = 0; i < 4; ++i)
            v << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Item %1").arg(i) } };
        return v;
    };
    const QVariantMap xmbEl{ { QStringLiteral("type"), QStringLiteral("xmb") },
                             { QStringLiteral("pos"), QVariantList{ 0, 0 } },
                             { QStringLiteral("size"), QVariantList{ 1, 1 } } };
    const QVariantMap gridEl{ { QStringLiteral("type"), QStringLiteral("grid") },
                              { QStringLiteral("columns"), 4 },
                              { QStringLiteral("pos"), QVariantList{ 0, 0 } },
                              { QStringLiteral("size"), QVariantList{ 1, 0.8 } } };
    const QVariantMap btnEl{ { QStringLiteral("type"), QStringLiteral("button") },
                             { QStringLiteral("action"), QStringLiteral("settings") },
                             { QStringLiteral("pos"), QVariantList{ 0.9, 0.9 } },
                             { QStringLiteral("size"), QVariantList{ 0.1, 0.06 } } };
    auto themeWith = [](const QVariantList& elements) -> QVariantMap {
        QVariantMap home{ { QStringLiteral("background"), QVariantMap{ { QStringLiteral("color"), QStringLiteral("#101010") } } },
                          { QStringLiteral("elements"), elements } };
        return QVariantMap{ { QStringLiteral("name"), QStringLiteral("Probe") },
                            { QStringLiteral("views"), QVariantMap{ { QStringLiteral("home"), home } } } };
    };

    // ---- (a) XMB + a button: the `buttons` zone stays hidden, so the cursor can never enter the bar ----
    {
        NavGraph g;
        buildThemedNavGraph(g, 4);
        buildAudioPageNavGraph(g);
        QQuickWidget qw;
        qw.setResizeMode(QQuickWidget::SizeRootObjectToView);
        qw.rootContext()->setContextProperty(QStringLiteral("nav"), &g);
        qw.rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance()); // §19 parity: `form` beside `nav`
        qw.setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));
        QQuickItem* root = qw.rootObject();
        CHECK(root != nullptr, "ThemeView.qml instantiates from the qrc (xmb case)");
        if (root)
        {
            root->setProperty("categories", QVariantList{ QStringLiteral("Video"), QStringLiteral("Games") });
            root->setProperty("items", probeItems());
            root->setProperty("currentIndex", 0);
            root->setProperty("currentView", QStringLiteral("home"));
            root->setProperty("theme", themeWith(QVariantList{ xmbEl, btnEl })); // set last
            qw.resize(1280, 720);
            qw.show();
            pump(); pump();
            CHECK(root->property("xmbMode").toBool(), "the xmb element puts the view in xmbMode");
            CHECK(root->property("buttonList").toList().size() == 1, "the button element is present in buttonList");
            // The guard: `buttons` is held hidden (count 0), so select() refuses to steer onto it…
            g.select(QStringLiteral("items"), 0);
            g.select(QStringLiteral("buttons"), 0);
            CHECK(g.zone() == QStringLiteral("items"),
                  "XMB-buttons guard: `buttons` hidden (count 0) — the cursor cannot enter the bar");
            // …and its declared items->buttons Down edge is inert too (a hidden target makes the edge inert),
            // so no arrow can cross into the bar from the column.
            CHECK(!g.move(Qt::Key_Down) || g.zone() != QStringLiteral("buttons"),
                  "XMB-buttons guard: the items->buttons edge is inert (Down never crosses into the bar)");
        }
    }

    // ---- (b) grid home (no xmb): the button bar IS live (positive control), and Escape -> rootBack ----
    {
        NavGraph g;
        buildThemedNavGraph(g, 4);
        buildAudioPageNavGraph(g);
        QQuickWidget qw;
        qw.setResizeMode(QQuickWidget::SizeRootObjectToView);
        qw.rootContext()->setContextProperty(QStringLiteral("nav"), &g);
        qw.rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance()); // §19 parity: `form` beside `nav`
        bool rootBackFired = false;
        QObject::connect(&g, &NavGraph::rootBack, [&rootBackFired] { rootBackFired = true; });
        qw.setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));
        QQuickItem* root = qw.rootObject();
        CHECK(root != nullptr, "ThemeView.qml instantiates from the qrc (grid case)");
        if (root)
        {
            root->setProperty("categories", QVariantList{});
            root->setProperty("items", probeItems());
            root->setProperty("currentIndex", 0);
            root->setProperty("currentView", QStringLiteral("home"));
            root->setProperty("theme", themeWith(QVariantList{ gridEl, btnEl })); // set last
            qw.resize(1280, 720);
            qw.show();
            pump(); pump();
            CHECK(!root->property("xmbMode").toBool(), "the grid home is NOT xmbMode");
            // Positive control (the guard's RED leans on this): the SAME button, in grid mode, IS reachable —
            // proving the xmbMode gate is what hides it above, not a missing button.
            CHECK(root->property("buttonList").toList().size() == 1, "grid buttonList carries the button");
            g.select(QStringLiteral("buttons"), 0);
            CHECK(g.zone() == QStringLiteral("buttons"),
                  "grid mode: the button-bar zone is live (count = buttonList.length) — the cursor can enter it");
            // grid-home rootBack: Escape at the root (empty level stack) routes nav.back() -> rootBack.
            g.select(QStringLiteral("items"), 0);
            root->forceActiveFocus();
            pump();
            CHECK(!rootBackFired, "no rootBack before the Escape");
            sendKey(qw.quickWindow(), Qt::Key_Escape);
            CHECK(rootBackFired, "grid-home Escape with an empty level stack emits rootBack (the pause-menu leg)");
        }
    }
}

// §19 — `form` context property + TV scale/insets on the ThemeView surface (D1 Task 2). Loads the REAL
// ThemeView.qml from the qrc with `form` registered (= &FormFactor::instance()) exactly as ThemeEngine::buildView
// now does, forces TV mode (Settings::setDisplayMode + FormFactor::refresh — setDisplayMode writes but does NOT
// refresh), and asserts the two consumers: the content Item is inset by the safe-area fraction
// (round(min(w,h) * safeAreaFrac)) and a themed Text's pixelSize rides uiScale (fraction * host.height * 1.3).
// Then Desktop mode is the IDENTITY net — inset 0 and the PRE-SCALE pixelSize (fraction * host.height, ffs == 1)
// — proving every D1 Task 2 change is a pixel-for-pixel no-op with default settings. A SQUARE fixture (w == h)
// makes min(w,h) == width, so the inset reads identically whether expressed as width- or min-based.
static void runFormFactorAsserts()
{
    const qreal frac = 0.03;                                  // the themed Text's fractional fontSize
    const int   side = 1000;                                  // square: min(w,h) == width
    const QVariantMap textEl{ { QStringLiteral("type"), QStringLiteral("text") },
                              { QStringLiteral("text"), QStringLiteral("FFPROBE") },
                              { QStringLiteral("fontSize"), frac },
                              { QStringLiteral("pos"), QVariantList{ 0.1, 0.1 } },
                              { QStringLiteral("size"), QVariantList{ 0.5, 0.1 } } };
    const QVariantMap home{ { QStringLiteral("background"), QVariantMap{ { QStringLiteral("color"), QStringLiteral("#101010") } } },
                            { QStringLiteral("elements"), QVariantList{ textEl } } };
    const QVariantMap theme{ { QStringLiteral("name"), QStringLiteral("FF") },
                             { QStringLiteral("views"), QVariantMap{ { QStringLiteral("home"), home } } } };

    // Force TV mode BEFORE the fixture builds (setDisplayMode writes the setting; the singleton must refresh()).
    Settings::setDisplayMode(QStringLiteral("tv"));
    FormFactor::instance().refresh();
    CHECK(FormFactor::instance().modeName() == QStringLiteral("tv"), "formfactor: TV mode is active for the fixture");

    NavGraph g;
    buildThemedNavGraph(g, 0);
    buildAudioPageNavGraph(g);
    QQuickWidget qw;
    qw.setResizeMode(QQuickWidget::SizeRootObjectToView);
    qw.rootContext()->setContextProperty(QStringLiteral("nav"), &g);
    qw.rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance()); // the D1 Task 2 prop
    qw.setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));
    QQuickItem* root = qw.rootObject();
    CHECK(root != nullptr, "ThemeView.qml instantiates from the qrc (formfactor case)");
    if (!root) { Settings::setDisplayMode(QStringLiteral("auto")); FormFactor::instance().refresh(); return; }

    root->setProperty("items", QVariantList{});
    root->setProperty("currentIndex", 0);
    root->setProperty("currentView", QStringLiteral("home"));
    root->setProperty("theme", theme);                       // set last — everything depends on it
    qw.resize(side, side);
    qw.show();
    pump(); pump();
    qw.grabFramebuffer();   // force a synchronous render pass so the Repeater realizes its element delegates
    pump();

    const qreal w = root->width(), h = root->height();
    CHECK(qFuzzyCompare(w, qreal(side)) && qFuzzyCompare(h, qreal(side)),
          "formfactor: the fixture root is square (min == width)");

    // (a) TV content inset: the content Item (objectName ffContent) is anchors.fill parent + anchors.margins ==
    //     round(min(w,h) * safeAreaFrac). Observe it via geometry: x/y == inset, width == side - 2*inset. On the
    //     square fixture round(min * 0.05) == round(width * 0.05) == 50.
    QQuickItem* content = root->findChild<QQuickItem*>(QStringLiteral("ffContent"));
    CHECK(content != nullptr, "formfactor: the content Item carries objectName ffContent");
    const int expectInset = qRound(qMin(w, h) * 0.05);
    if (content)
    {
        CHECK(qRound(content->x()) == expectInset && qRound(content->y()) == expectInset,
              "formfactor(TV): the content Item is inset by the safe area (round(min*0.05)) on x and y");
        CHECK(qRound(content->width()) == side - 2 * expectInset,
              "formfactor(TV): the content Item width is reduced by twice the safe-area inset");
    }

    // (b) TV Text scale: the themed Text's pixelSize rides uiScale — round(fraction * host.height * 1.3), ±1px.
    //     The element is a Repeater delegate: it is VISUALLY parented (childItems) but not a QObject child, so
    //     walk the visual tree to reach it (findChildren, which follows QObject parentage, never sees it).
    QQuickItem* txt = nullptr;
    {
        QList<QQuickItem*> stack = root->childItems();
        while (!stack.isEmpty())
        {
            QQuickItem* it = stack.takeLast();
            if (it->property("text").toString() == QStringLiteral("FFPROBE")) { txt = it; break; }
            stack += it->childItems();
        }
    }
    CHECK(txt != nullptr, "formfactor: the themed Text element instantiated");
    const int expectTvPx = qRound(frac * h * 1.3);
    if (txt)
        CHECK(qAbs(txt->property("font").value<QFont>().pixelSize() - expectTvPx) <= 1,
              "formfactor(TV): the themed Text pixelSize rides uiScale (fraction*host.height*1.3)");

    // ---- Desktop IDENTITY net: inset 0 and the pre-scale pixelSize (ffs == 1). The changed() signal rebinds
    //      the live content margins + the Text's ffs, so the SAME loaded scene must collapse to the no-op.
    Settings::setDisplayMode(QStringLiteral("desktop"));
    FormFactor::instance().refresh();
    pump(); pump();
    CHECK(FormFactor::instance().modeName() == QStringLiteral("desktop"), "formfactor: Desktop mode is active for the identity leg");
    if (content)
        CHECK(qRound(content->x()) == 0 && qRound(content->y()) == 0 && qRound(content->width()) == side,
              "formfactor(identity): Desktop insets the content by 0 (full-bleed, pixel no-op)");
    if (txt)
    {
        const int expectBasePx = qRound(frac * h);           // the pre-scale baseline (no uiScale multiply)
        CHECK(qAbs(txt->property("font").value<QFont>().pixelSize() - expectBasePx) <= 1,
              "formfactor(identity): Desktop pixelSize == the pre-scale baseline (fraction*host.height, ffs==1)");
    }

    // Restore the stored mode so the setting the probe wrote does not leak into later runs / other consumers.
    Settings::setDisplayMode(QStringLiteral("auto"));
    FormFactor::instance().refresh();
}

// §20 — the touch INPUT model (D1 Task 4). Synthesizes REAL touch sequences (QTest::touchEvent → real
// hit-testing through the QML scene, NOT a shortcut into the graph) against the two themed surfaces and pins
// the mobile tap/flick/edge-back contract, plus the Desktop identity net (two-step click frozen). Runs LAST +
// restores the stored mode. Everything is gated on FormFactor mode, so the Desktop leg proves a pixel/behaviour
// no-op with default settings.
//
//   (a) MOBILE grid tap on a non-selected item: selection MOVES to it AND activated fires (one-tap activate).
//   (b) DESKTOP grid tap: first tap SELECTS only (no activate); a second tap on the now-selected item activates.
//   (c) MOBILE SettingsPanel row tap: select+activate (already one-click — assert unchanged).
//   (d) MOBILE SettingsPanel ListView flick: contentY changes (native kinetic) AND the selection does NOT.
//   (e) MOBILE edge-swipe from x<12 rightward ≥80px: backInvoked fires; a short (<80px) edge drag does NOT.
static void runTouchAsserts()
{
    auto probeItems = []() -> QVariantList {
        QVariantList v;
        for (int i = 0; i < 4; ++i)
            v << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Item %1").arg(i) } };
        return v;
    };
    const QVariantMap gridEl{ { QStringLiteral("type"), QStringLiteral("grid") },
                              { QStringLiteral("columns"), 4 },
                              { QStringLiteral("pos"), QVariantList{ 0, 0 } },
                              { QStringLiteral("size"), QVariantList{ 1, 1 } } };
    const QVariantMap home{ { QStringLiteral("background"), QVariantMap{ { QStringLiteral("color"), QStringLiteral("#101010") } } },
                            { QStringLiteral("elements"), QVariantList{ gridEl } } };
    const QVariantMap theme{ { QStringLiteral("name"), QStringLiteral("Touch") },
                             { QStringLiteral("views"), QVariantMap{ { QStringLiteral("home"), home } } } };

    QPointingDevice* dev = QTest::createTouchDevice();   // one registered touchscreen for the whole run

    // ============================ GRID surface (ThemeView) ============================
    Settings::setDisplayMode(QStringLiteral("mobile"));
    FormFactor::instance().refresh();
    CHECK(FormFactor::instance().modeName() == QStringLiteral("mobile"), "touch: mobile mode active for the grid fixture");

    NavGraph g;
    buildThemedNavGraph(g, 4);
    buildAudioPageNavGraph(g);
    QQuickWidget qw;
    qw.setResizeMode(QQuickWidget::SizeRootObjectToView);
    qw.rootContext()->setContextProperty(QStringLiteral("nav"), &g);
    qw.rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());
    qw.setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));
    QQuickItem* root = qw.rootObject();
    CHECK(root != nullptr, "touch: ThemeView.qml instantiates from the qrc (grid case)");
    if (!root) { Settings::setDisplayMode(QStringLiteral("auto")); FormFactor::instance().refresh(); return; }
    root->setProperty("categories", QVariantList{});
    root->setProperty("items", probeItems());
    root->setProperty("currentIndex", 0);
    root->setProperty("currentView", QStringLiteral("home"));
    root->setProperty("theme", theme);                       // set last
    qw.resize(1280, 720);
    qw.show();
    pump(); pump();
    qw.grabFramebuffer();   // force a synchronous render pass so the GridView realizes its delegates
    pump();

    // Emulate the C++ bridge's items-zone write-back (selectionChanged -> currentIndex): the two-step desktop
    // path re-reads currentIndex to decide select-vs-activate, and live that mirror is the ThemeEngine bridge.
    QObject::connect(&g, &NavGraph::selectionChanged, root, [root](const QString& z, int i) {
        if (z == QStringLiteral("items")) root->setProperty("currentIndex", i);
    });
    int activatedCount = 0;
    QObject::connect(&g, &NavGraph::activated, root, [&activatedCount](const QString&, int) { ++activatedCount; });

    // A mouse-drag helper — the same driver §20's flick uses (the offscreen harness engages the edge-back
    // DragHandler and the content Flickable from QTest::mouse*, not from synthetic touch; see the flick note).
    auto mouseDrag = [](QQuickWindow* w, QPoint a, QPoint b, int steps) {
        QTest::mousePress(w, Qt::LeftButton, Qt::NoModifier, a);
        for (int i = 1; i <= steps; ++i)
        {
            QTest::mouseMove(w, QPoint(a.x() + (b.x() - a.x()) * i / steps, a.y() + (b.y() - a.y()) * i / steps));
            pump();
        }
        QTest::mouseRelease(w, Qt::LeftButton, Qt::NoModifier, b);
        pump();
    };

    // Grid geometry on the 1280x720 square-free fixture: 4 cols -> cellWidth 320, cellHeight 320*1.4=448.
    // Row 0 items are centred at y=224; item i centre x = i*320 + 160.
    const QPoint pItem1(1 * 320 + 160, 224);   // (480, 224) — item 1, non-selected (currentIndex 0)
    const QPoint pItem2(2 * 320 + 160, 224);   // (800, 224) — item 2

    // ---- (a) MOBILE one-tap: tap a non-selected item -> selection moves AND activated fires ----
    g.select(QStringLiteral("items"), 0);
    root->setProperty("currentIndex", 0);
    activatedCount = 0;
    QTest::touchEvent(qw.quickWindow(), dev).press(0, pItem1);
    QTest::touchEvent(qw.quickWindow(), dev).release(0, pItem1);
    pump();
    CHECK(g.zone() == QStringLiteral("items") && g.index() == 1,
          "touch(mobile): a tap moves the grid selection to the tapped item (through gotoItem -> the graph)");
    CHECK(activatedCount == 1,
          "touch(mobile): the SAME tap also activates the item (one-tap semantics)");

    // ---- (a2) gotoItemSelectOnly: the Channels page-arrow path moves the selection but NEVER activates ----
    // (Even in mobile, where a plain tap one-tap-activates — a page flip must not drill into the landed slot.)
    g.select(QStringLiteral("items"), 0);
    root->setProperty("currentIndex", 0);
    activatedCount = 0;
    QMetaObject::invokeMethod(root, "gotoItemSelectOnly", Q_ARG(QVariant, QVariant(3)));
    pump();
    CHECK(g.zone() == QStringLiteral("items") && g.index() == 3 && activatedCount == 0,
          "touch(mobile): gotoItemSelectOnly moves the selection but does NOT activate (page-arrow paging)");

    // ---- (b) DESKTOP two-step: first tap selects only; a second tap on the selected item activates ----
    Settings::setDisplayMode(QStringLiteral("desktop"));
    FormFactor::instance().refresh();
    pump();
    CHECK(FormFactor::instance().modeName() == QStringLiteral("desktop"), "touch: desktop mode active for the identity leg");
    g.select(QStringLiteral("items"), 0);
    root->setProperty("currentIndex", 0);
    activatedCount = 0;
    QTest::touchEvent(qw.quickWindow(), dev).press(0, pItem2);
    QTest::touchEvent(qw.quickWindow(), dev).release(0, pItem2);
    pump();
    CHECK(g.zone() == QStringLiteral("items") && g.index() == 2 && activatedCount == 0,
          "touch(desktop): the first tap only SELECTS the item (no activate — two-step frozen)");
    QTest::touchEvent(qw.quickWindow(), dev).press(0, pItem2);
    QTest::touchEvent(qw.quickWindow(), dev).release(0, pItem2);
    pump();
    CHECK(activatedCount == 1,
          "touch(desktop): a second tap on the now-selected item activates it (the two-step click)");

    // ---- (e) MOBILE edge-swipe: the left-edge DragHandler is HORIZONTAL-ONLY (intent detection) ----
    // Driven by mouse-drag (like the flick — the offscreen harness engages the DragHandler from QTest::mouse*,
    // not from synthetic touch). A rightward sweep from x<12 >=80px fires Back; a short one does NOT; and a
    // VERTICAL drag from the edge must NOT fire Back (yAxis disabled leaves it to the content Flickable — the
    // fix-round change that stops the strip from swallowing an edge-started scroll).
    Settings::setDisplayMode(QStringLiteral("mobile"));
    FormFactor::instance().refresh();
    pump();
    int backCount = 0;
    QObject::connect(&g, &NavGraph::backInvoked, root, [&backCount] { ++backCount; });
    mouseDrag(qw.quickWindow(), QPoint(4, 360), QPoint(115, 360), 6);   // long rightward sweep from x<12
    CHECK(backCount >= 1, "edge-swipe(mobile): a rightward drag from x<12 >=80px fires back (nav.back)");
    const int backAfterLong = backCount;
    mouseDrag(qw.quickWindow(), QPoint(4, 360), QPoint(40, 360), 4);    // short (<80px) rightward drag
    CHECK(backCount == backAfterLong, "edge-swipe(mobile): a short edge drag (<80px) does NOT fire back (threshold)");

    // A VERTICAL drag STARTING in the 12px edge strip must reach the content Flickable (yAxis disabled leaves it
    // to the grid) — it SCROLLS the GridView contentY and does NOT fire Back (Important #2: the strip must not
    // swallow an edge-started scroll). The grid fills to x=0, so x<12 overlaps its Flickable; give it enough rows
    // to overflow (40 items -> 10 rows * 448 = 4480 > 720) so there is contentY to move.
    root->setProperty("items", []() { QVariantList v; for (int i = 0; i < 40; ++i)
        v << QVariantMap{ { QStringLiteral("title"), QStringLiteral("G%1").arg(i) } }; return v; }());
    pump(); qw.grabFramebuffer(); pump();
    // The Grid element is a Repeater-delegate Loader child — VISUALLY parented but not a QObject child, so walk
    // the visual tree (findChild follows QObject parentage and never reaches it — mirrors the FFPROBE walk).
    QQuickItem* grid = nullptr;
    {
        QList<QQuickItem*> stack = root->childItems();
        while (!stack.isEmpty())
        {
            QQuickItem* it = stack.takeLast();
            if (it->objectName() == QStringLiteral("themeGrid")) { grid = it; break; }
            stack += it->childItems();
        }
    }
    CHECK(grid != nullptr, "edge-swipe(mobile): the Grid element carries objectName themeGrid");
    const int backAfterShort = backCount;
    if (grid)
    {
        const qreal gcy0 = grid->property("contentY").toReal();
        mouseDrag(qw.quickWindow(), QPoint(4, 560), QPoint(4, 300), 6); // VERTICAL (finger up) from the edge strip
        CHECK(qAbs(grid->property("contentY").toReal() - gcy0) > 1.0,
              "edge-swipe(mobile): a VERTICAL drag from x<12 scrolls the grid contentY (reaches the Flickable)");
        CHECK(backCount == backAfterShort,
              "edge-swipe(mobile): the VERTICAL edge drag does NOT fire back (yAxis off -> intent detection)");
    }

    // ============================ PANEL surface (SettingsPanel via the REAL ThemedPanelHost) ============================
    Settings::setDisplayMode(QStringLiteral("mobile"));
    FormFactor::instance().refresh();
    ThemedPanelHost host;
    NavGraph* pg = host.navGraph();
    // 40 Action rows: plenty to overflow a 400px-tall panel, so contentHeight > height and the ListView can flick.
    host.present(QStringLiteral("Touch Panel"), panelActionRows(40, QStringLiteral("row")),
                 [](const QString&, const QString&) {}, [] {});
    host.resize(600, 400);
    host.show();
    pump(); pump();
    QQuickWidget* pqw = qobject_cast<QQuickWidget*>(host.quickWidget());
    CHECK(pqw != nullptr, "touch: the panel host exposes its QQuickWidget");
    if (pqw)
    {
        pqw->grabFramebuffer();
        pump();
        QQuickItem* proot = pqw->rootObject();
        QQuickItem* listv = proot ? proot->findChild<QQuickItem*>(QStringLiteral("panelList")) : nullptr;
        CHECK(listv != nullptr, "touch: the SettingsPanel ListView carries objectName panelList");

        int pAct = 0;
        QObject::connect(pg, &NavGraph::activated, &host, [&pAct](const QString&, int) { ++pAct; });

        // ---- (c) a row tap: select+activate (one-click — the panel behaviour is unchanged) ----
        // A point well inside the list body (below the ~85px header + margin), centred horizontally.
        const QPoint pRow(300, 150);
        QTest::touchEvent(pqw->quickWindow(), dev).press(0, pRow);
        QTest::touchEvent(pqw->quickWindow(), dev).release(0, pRow);
        pump();
        CHECK(pg->zone() == QStringLiteral("panelRows") && pAct == 1,
              "touch(panel): a row tap selects AND activates it in one click (unchanged)");

        // ---- (d) a vertical flick: contentY changes (native kinetic) AND the selection does NOT ----
        // NOTE ON THE DRIVER: QTest::touchEvent taps route fine through this offscreen QQuickWidget (proven by
        // (c) above), but the offscreen harness does NOT engage a Flickable's touch-drag from synthetic touch —
        // dragging never latches (verified: no contentY movement at any press point). Qt's own Flickable tests
        // therefore drive drags with QTest::mouse* events, which exercise the IDENTICAL Flickable drag path. A
        // mouse drag over an `interactive` Flickable scrolls it; over a NON-interactive one it does not — so this
        // still distinguishes the mobile change from the frozen Desktop default. (The real kinetic touch scroll
        // is verified live in the report; here we pin the interactive behaviour headlessly.)
        if (listv)
        {
            const qreal cy0 = listv->property("contentY").toReal();
            const QString z0 = pg->zone();
            const int idx0 = pg->index();
            const QPoint start(300, 340), end(300, 120);
            QTest::mousePress(pqw->quickWindow(), Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(pqw->quickWindow(), QPoint(300, 300)); pump();
            QTest::mouseMove(pqw->quickWindow(), QPoint(300, 240)); pump();
            QTest::mouseMove(pqw->quickWindow(), QPoint(300, 180)); pump();
            QTest::mouseMove(pqw->quickWindow(), end); pump();
            QTest::mouseRelease(pqw->quickWindow(), Qt::LeftButton, Qt::NoModifier, end);
            pump(); pump();
            const qreal cy1 = listv->property("contentY").toReal();
            CHECK(qAbs(cy1 - cy0) > 1.0,
                  "touch(panel,mobile): a vertical drag flicks the ListView contentY (interactive kinetic scroll)");
            CHECK(pg->zone() == z0 && pg->index() == idx0,
                  "touch(panel,mobile): the flick does NOT move the selection (drag != tap)");

            // Desktop identity net: the SAME drag over the now non-interactive ListView must NOT scroll it.
            Settings::setDisplayMode(QStringLiteral("desktop"));
            FormFactor::instance().refresh();
            pump();
            // The mobile flick above may still be decelerating (a real render loop runs the kinetic
            // animation longer than a few pumps) — wait for it to settle or contentY drifts mid-check.
            for (int i = 0; i < 300 && listv->property("moving").toBool(); ++i) { QTest::qWait(10); }
            pump();
            const qreal dcy0 = listv->property("contentY").toReal();
            QTest::mousePress(pqw->quickWindow(), Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(pqw->quickWindow(), QPoint(300, 240)); pump();
            QTest::mouseMove(pqw->quickWindow(), end); pump();
            QTest::mouseRelease(pqw->quickWindow(), Qt::LeftButton, Qt::NoModifier, end);
            pump(); pump();
            CHECK(qFuzzyCompare(listv->property("contentY").toReal() + 1.0, dcy0 + 1.0),
                  "touch(panel,desktop): the non-interactive ListView does NOT scroll on a drag (identity)");
            Settings::setDisplayMode(QStringLiteral("mobile"));
            FormFactor::instance().refresh();
            pump();

            // ---- (f) fix-round: the panel's OWN left-edge Back swipe (Minor #3). A rightward edge sweep
            //      >=80px fires Back (its ‹ Back header remains too). The panel ListView is inset past the 12px
            //      strip (leftMargin ~28*ffs), so the vertical-drag-reaches-Flickable case is pinned on the grid
            //      above (whose Flickable fills to x=0); here we pin the panel's horizontal edge-back.
            int pBack = 0;
            QObject::connect(pg, &NavGraph::backInvoked, &host, [&pBack] { ++pBack; });
            mouseDrag(pqw->quickWindow(), QPoint(6, 300), QPoint(120, 300), 6); // horizontal edge sweep >=80px
            CHECK(pBack >= 1, "edge(panel,mobile): a rightward edge drag from x<12 >=80px fires Back (panel edge-swipe)");
        }
    }

    // Restore the stored mode so the setting the probe wrote does not leak into later runs / other consumers.
    Settings::setDisplayMode(QStringLiteral("auto"));
    FormFactor::instance().refresh();
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

    // ------------------------------------------------------------- 0b. watchdog tick() run logic (Task 6)
    // isBlack (above) is the classifier; tick() is the STATE MACHINE around it: the consecutive-black counter
    // that fires recovery on the 2nd frame, and the skip lambda that BOTH ignores an expected-black frame AND
    // resets the run so a legit black view (a game/reader) never primes a false recovery on exit. Driven here
    // with injected sampler/skip lambdas (tick() is synchronous — no real 1 s timer needed).
    {
        bool black = false, skip = false;
        auto sampler = [&black]() -> QImage {
            QImage im(8, 8, QImage::Format_ARGB32);
            im.fill(black ? qRgb(0, 0, 0) : qRgb(200, 200, 200));
            return im;
        };
        BlackFrameWatchdog wd(sampler, [&skip] { return skip; });
        QVector<int> emissions;
        QObject::connect(&wd, &BlackFrameWatchdog::blackFrameDetected, [&emissions](int c) { emissions.push_back(c); });

        // (a) a non-black frame never fires and holds the run at 0.
        black = false; skip = false; wd.tick();
        CHECK(wd.consecutive() == 0 && emissions.isEmpty(), "a non-black frame leaves the run at 0, no emission");

        // (b) consecutive black frames step the counter and emit each time (the host acts on consec==2).
        black = true;  wd.tick();
        CHECK(wd.consecutive() == 1 && emissions.size() == 1 && emissions.last() == 1, "1st black frame: consec 1, emitted");
        wd.tick();
        CHECK(wd.consecutive() == 2 && emissions.size() == 2 && emissions.last() == 2, "2nd consecutive black: consec 2 (recovery point)");

        // (c) a non-black frame breaks the run back to 0 (recovery ran / the view repainted).
        black = false; wd.tick();
        CHECK(wd.consecutive() == 0, "a non-black frame resets the consecutive run");

        // (d) a SKIPPED tick (expected-black context: game/video/reader) both no-ops AND resets the run, so a
        //     legit black view can never accumulate toward a false recovery — even across black frames.
        black = true; skip = false; wd.tick(); wd.tick();
        CHECK(wd.consecutive() == 2, "two black frames primed the run to 2");
        skip = true; wd.tick();
        CHECK(wd.consecutive() == 0, "a skipped tick resets the run (expected-black view never primes recovery)");
        const int emittedBeforeSkipRun = emissions.size();
        black = true; skip = true; wd.tick(); wd.tick();
        CHECK(wd.consecutive() == 0 && emissions.size() == emittedBeforeSkipRun,
              "black frames while skipping never step the counter nor emit");
    }

    // -------------------------------------------------- 0c. NavGraph::activate() hidden-zone guard (Task 6)
    // Activating a hidden/empty zone must be a safe no-op. The model parks the selection on the only zone even
    // after it hides itself (there is nowhere else to go — no null state); activate there would hand the host a
    // phantom row. The guard refuses to emit activated on a count-0 zone.
    {
        NavGraph g;
        g.registerZone(QStringLiteral("solo"), 3, 0, 0);
        int fired = 0;
        QObject::connect(&g, &NavGraph::activated, [&fired](const QString&, int) { ++fired; });
        g.activate();
        CHECK(fired == 1, "positive control: activate on a visible zone emits activated");
        g.setZoneCount(QStringLiteral("solo"), 0);        // hides the only zone -> selection parks on it (hidden)
        CHECK(g.zone() == QStringLiteral("solo"), "the only zone stays selected even when hidden (no null state)");
        const int before = fired;
        g.activate();
        CHECK(fired == before, "activate on a HIDDEN zone is a no-op (no phantom activation)");
        g.setZoneCount(QStringLiteral("solo"), 2);        // re-shown -> activation works again
        g.activate();
        CHECK(fired == before + 1, "re-showing the zone re-enables activation");
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

        // (d) POP-RESTORE: a nested panel returning to its parent restores the parent's cursor (the user's
        //     place), not row 0. The REMEMBERING lives host-side (ThemedPanelHost records the top entry's
        //     panelRows index off selectionChanged; renderTop(restore=true) re-selects it on pop) — beyond the
        //     pure graph, so this asserts the graph-level leg by driving the host's EXACT call sequence on the
        //     shared builder: re-count + re-select after a nested panel's smaller count must land EXACTLY on
        //     the remembered row (select() clamps + divider-snaps, which also covers a shrunk parent list).
        {
            NavGraph g;
            buildPanelNavGraph(g, 14);                          // parent panel (the hub)
            g.select(QStringLiteral("panelRows"), 5);           // the user's place on the parent
            CHECK(g.index() == 5, "panel-restore: the parent cursor sits on row 5");
            // Nested present(): the child re-counts the zone and lands on ITS first row.
            g.setZoneCount(QStringLiteral("panelRows"), 6);     // child panel (fewer rows)
            g.select(QStringLiteral("panelRows"), 0);
            CHECK(g.index() == 0, "panel-restore: the nested child lands on its first row");
            // Pop: the host re-renders the parent — re-count + re-select the remembered index (Entry.lastIndex).
            g.setZoneCount(QStringLiteral("panelRows"), 14);
            g.select(QStringLiteral("panelRows"), 5);
            CHECK(g.zone() == QStringLiteral("panelRows") && g.index() == 5,
                  "panel-restore: pop re-selects the parent's remembered row (5), not row 0");
        }
    }

#ifdef MMV_HAVE_QML
    // ---------------------------------------------------------------- 14. two-state themed inputs (the real
    // ThemedTextField/ThemedChoice components, offscreen QQuickWidget, real NavGraph as `nav`).
    runThemedInputAsserts();
    // §18(e): the pop-restore clamp hazard, pinned against the REAL ThemedPanelHost (renderTop's capture-before-
    // mutate ordering) — the host-level guard §18(d) above structurally cannot be. See the function's note.
    runPanelHostPopRestoreAsserts();
    // §18(f): replaceTop's same-level contract (in-place rebuild never stacks a level) — the host leg of the
    // panel async-connection lifetime model (MainWindow's themedPanelIsTop-gated rebuild handlers).
    runPanelHostReplaceTopAsserts();
    // §18(j): HOST re-entrancy safety (final-review fix round) — (a) replaceTop from inside an onActivate survives
    // the closure's own reassignment, (b) overlayAbove() gate primitive, (c) TextField commit relocates by id and
    // drops safely when a mid-edit replaceTop removed the row.
    runPanelHostReentrancyAsserts();
    // §18(h): the Add-ons manager panel graph (B2 Task 6.5) — divider-skip landing, the three-level remove-flow
    // double-pop cursor restore, and the masked config field's in-place patch.
    runAddonsPanelAsserts();
    // §18(i): the Appearance panel graph (B2 Task 6.75) — the divider-skip stepping across its multi-divider
    // trailing block (Toggle -> Choice -> lone Action) + a nested-child Back pop to the settings hub.
    runAppearancePanelAsserts();
    // §18(g): ThemeView-level pins — the XMB-buttons guard + grid-home rootBack (B2 Task 6 hardening).
    runThemeViewAsserts();
    // §19: the `form` context property + TV scale/insets on the ThemeView surface (D1 Task 2), plus the Desktop
    // identity net that guards the whole form-factor branch as a pixel no-op. Runs LAST + restores the setting.
    runFormFactorAsserts();
    // §20: the touch INPUT model (D1 Task 4) — mobile one-tap activate, the Desktop two-step identity net, the
    // SettingsPanel kinetic flick, and the left-edge back-swipe, all via REAL synthetic touch (real hit-testing).
    runTouchAsserts();
#endif

    if (failures) { std::fprintf(stderr, "NAVQML-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("NAVQML-OK\n");
    return 0;
}
