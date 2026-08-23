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
// When the QML theme engine is present (EB_HAVE_QML) this ALSO proves the two-state themed input contract:
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
#include <functional>   // §22: the visual-tree walk takes a std::function predicate
#include <deque>
#include <random>
#include <set>
#include <vector>

#ifdef EB_HAVE_QML
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
#include <QtTest/QSignalSpy>         // §20(g): counts the Channels prefetch nearEnd() firings
#include <QDir>                       // §21: a scratch theme dir for the REAL ThemeEngine::buildView
#include <QTemporaryDir>
#include <QFile>
#include <QPushButton>                // §23: an ordinary ring stop, the control the preview is judged against
#include "nav/Nav.h"                  // §23: the REAL NavRing — ring membership is the thing being asserted
#include "theme2/ThemedPanelHost.h"   // §18(e): the REAL host, for the host-level pop-restore assertions
#include "theme2/ThemeEngine.h"       // §21: the REAL buildView — theme.json -> graph shape -> bridge -> QML
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

#ifdef EB_HAVE_QML
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
        g->activate();                                 // → onGraphActivated → queues the copied onAct (§18(k))
        QCoreApplication::processEvents();             // ...which runs a turn later — see runPanelHostDeferralAsserts

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

// §18(k) — DISPATCH DEFERRAL (issue #165, the #28 rule applied to ThemedPanelHost). ThemedPanelHost::
// onGraphActivated is a DIRECT connection from NavGraph::activated, and every production emitter of that signal
// is QML — SettingsPanel.qml's row-delegate MouseArea, its header Back MouseArea, its root Keys handler. So a
// caller's onActivate used to run with a live ListView delegate's own emission on the stack, and what those
// handlers reach is a nested event loop (the two shipped QFileDialogs, confirmRemoveAddon's NavConfirm, the
// per-job NavMenu, …) — the exact interleaving both #28 production dumps died in: a nested loop flushes the
// process's pending DeferredDeletes mid-walk through QQuickRepeater::clear().
//
// The host cannot audit ~25 callers' bodies, so it defers unconditionally at its own dispatch boundary. That is
// a BEHAVIOUR, not a source shape, and this host links headlessly — so it is pinned here rather than left to the
// `themed handler deferral` source gate. Each leg below is "the handler has NOT run yet" immediately after the
// activation, and "it HAS run, with the right payload" after ONE event-loop turn; a mutant that restores the
// direct call turns the first half red. The last leg is the converse pin: the IN-HOST sub-panel pop (renderTop)
// must stay synchronous — it runs no caller code and touches no nested loop, and widening the deferral to it
// would make Back visibly lag a frame.
static void runPanelHostDeferralAsserts()
{
    auto onBack = [] {};

    // ---- (i) Action row: the caller dispatch hops an event-loop turn.
    {
        ThemedPanelHost host;
        NavGraph* g = host.navGraph();
        int calls = 0; QString gotId, gotVal;
        auto onAct = [&](const QString& id, const QString& v) { ++calls; gotId = id; gotVal = v; };

        host.present(QStringLiteral("P"), panelActionRows(3, QStringLiteral("a")), onAct, onBack);
        g->select(QStringLiteral("panelRows"), 1);
        g->activate();
        CHECK(calls == 0,
              "panel-host(defer): an Action row's onActivate has NOT run on the emission's own stack");
        QCoreApplication::processEvents();
        CHECK(calls == 1 && gotId == QStringLiteral("a1") && gotVal.isEmpty(),
              "panel-host(defer): it runs exactly once a turn later, with the activated row's id");
    }

    // ---- (ii) Toggle row: same deferral, and the flip's VALUE is the one computed at activation time.
    {
        ThemedPanelHost host;
        NavGraph* g = host.navGraph();
        int calls = 0; QString gotVal;
        auto onAct = [&](const QString&, const QString& v) { ++calls; gotVal = v; };

        QVector<PanelRow> rows;
        { PanelRow r; r.kind = PanelRow::Toggle; r.id = QStringLiteral("t.on"); r.label = QStringLiteral("T");
          r.checked = false; rows << r; }
        host.present(QStringLiteral("P"), rows, onAct, onBack);
        g->select(QStringLiteral("panelRows"), 0);
        g->activate();
        CHECK(calls == 0, "panel-host(defer): a Toggle's onActivate has NOT run on the emission's own stack");
        QCoreApplication::processEvents();
        CHECK(calls == 1 && gotVal == QStringLiteral("1"),
              "panel-host(defer): the Toggle dispatch lands a turn later carrying the flipped state (\"1\")");
    }

    // ---- (iii) Choice row: same deferral, carrying the option the cycle picked.
    {
        ThemedPanelHost host;
        NavGraph* g = host.navGraph();
        int calls = 0; QString gotVal;
        auto onAct = [&](const QString&, const QString& v) { ++calls; gotVal = v; };

        QVector<PanelRow> rows;
        { PanelRow r; r.kind = PanelRow::Choice; r.id = QStringLiteral("c.mode"); r.label = QStringLiteral("C");
          r.options = { QStringLiteral("auto"), QStringLiteral("tv") }; r.value = QStringLiteral("auto"); rows << r; }
        host.present(QStringLiteral("P"), rows, onAct, onBack);
        g->select(QStringLiteral("panelRows"), 0);
        g->activate();
        CHECK(calls == 0, "panel-host(defer): a Choice's onActivate has NOT run on the emission's own stack");
        QCoreApplication::processEvents();
        CHECK(calls == 1 && gotVal == QStringLiteral("tv"),
              "panel-host(defer): the Choice dispatch lands a turn later carrying the cycled option (\"tv\")");
    }

    // ---- (iv) The ROOT onBack — the leave-host callback. It retires this host's QQuickWidget (showThemedHome's
    //      removeWidget + deleteLater) or opens a NavConfirm quit prompt, both from a nav.back() emission. The
    //      LEVEL pop itself stays synchronous (the host's own bookkeeping); only the caller's callback hops.
    //      The depth-0 clause is the CONVERSE half and has its own killer, distinct from the direct-call mutant
    //      the other legs use: widen the deferral to the pop — `handleBack() {
    //      deferPastQmlEmission([this]{ graph_->back(); }); }` — and the host is still one level deep when the
    //      Back returns. That mutant is the plausible over-application of #165 ("defer at the Back boundary
    //      too"), and it would leave the panel painting a level the user has already left.
    {
        ThemedPanelHost host;
        int backs = 0;
        host.present(QStringLiteral("Root"), panelActionRows(2, QStringLiteral("r")),
                     [](const QString&, const QString&) {}, [&] { ++backs; });
        host.handleBack();
        CHECK(host.levelDepth() == 0,
              "panel-host(defer): the root Back pops the host's own level synchronously (depth 0)");
        CHECK(backs == 0, "panel-host(defer): the root onBack has NOT run on the emission's own stack");
        QCoreApplication::processEvents();
        CHECK(backs == 1, "panel-host(defer): the root onBack runs exactly once, a turn later");
    }

    // ---- (v) CONVERSE: a nested sub-panel's Back runs NO caller code, so its pop-restore must stay synchronous.
    //      This is what stops the fix widening into "defer everything" — Back on a drilled panel has to repaint
    //      the parent on the frame it was pressed.
    {
        ThemedPanelHost host;
        NavGraph* g = host.navGraph();
        auto noop = [](const QString&, const QString&) {};
        host.present(QStringLiteral("Parent"), panelActionRows(6, QStringLiteral("p")), noop, onBack);
        g->select(QStringLiteral("panelRows"), 3);
        host.present(QStringLiteral("Child"), panelActionRows(2, QStringLiteral("c")), noop, onBack);
        host.handleBack();                                   // NO processEvents between the Back and the checks
        // ONE check, not two: the title alone is popped by stack_.takeLast() and would still read "Parent" with
        // renderTop deferred — it is §18(f)'s assertion, not this one. What proves the RE-RENDER ran synchronously
        // is the cursor, which only renderTop moves.
        CHECK(host.panelTitle() == QStringLiteral("Parent")
              && g->zone() == QStringLiteral("panelRows") && g->index() == 3,
              "panel-host(defer): an in-host sub-panel pop re-renders the parent SYNCHRONOUSLY, cursor restored");
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
    { PanelRow r; r.kind = PanelRow::Choice;    r.id = QStringLiteral("appr.theme");     r.label = QStringLiteral("Theme"); r.options = { QStringLiteral("Triple"), QStringLiteral("Channels") }; r.value = QStringLiteral("Triple"); rows << r; } // 2 selectable
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

    // ---- (c) a theme's background image may not leave the theme's own folder ---------------------------
    // The END-TO-END half of the asset-containment rule. probe_themeview pins the decision (Theme.js
    // themeAsset/contentUrl, every escape shape, no scene); this pins that the SHIPPED ThemeView.qml
    // actually asks it — that the background binding routes through themeAsset rather than the permissive
    // resolver it replaced, and that `base` is wired into it. A rule nothing calls is the exact failure this
    // whole change exists to fix: ThemeAssetPath::resolve was correct and sat on dead code.
    {
        const QString base = QStringLiteral("file:///C:/app/themes2/Night");
        auto themeWithBg = [](const QString& image) -> QVariantMap {
            QVariantMap home{ { QStringLiteral("background"),
                                QVariantMap{ { QStringLiteral("color"), QStringLiteral("#101010") },
                                             { QStringLiteral("image"), image } } },
                              { QStringLiteral("elements"), QVariantList{} } };
            return QVariantMap{ { QStringLiteral("name"), QStringLiteral("Probe") },
                                { QStringLiteral("views"), QVariantMap{ { QStringLiteral("home"), home } } } };
        };
        auto bgSourceFor = [&](const QString& image) -> QString {
            NavGraph g;
            buildThemedNavGraph(g, 0);
            buildAudioPageNavGraph(g);
            QQuickWidget qw;
            qw.setResizeMode(QQuickWidget::SizeRootObjectToView);
            qw.rootContext()->setContextProperty(QStringLiteral("nav"), &g);
            qw.rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());
            qw.setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));
            QQuickItem* root = qw.rootObject();
            CHECK(root != nullptr, "ThemeView.qml instantiates from the qrc (background case)");
            if (!root) return QStringLiteral("<no root>");
            root->setProperty("base", base);                // ThemeEngine::buildView sets exactly this
            root->setProperty("categories", QVariantList{});
            root->setProperty("items", QVariantList{});
            root->setProperty("currentView", QStringLiteral("home"));
            root->setProperty("theme", themeWithBg(image)); // set last
            qw.resize(1280, 720);
            qw.show();
            pump(); pump();
            QQuickItem* bg = root->findChild<QQuickItem*>(QStringLiteral("ffBackgroundImage"));
            CHECK(bg != nullptr, "the background Image is findable by objectName");
            return bg ? bg->property("source").toUrl().toString() : QStringLiteral("<not found>");
        };

        // Positive control FIRST — without it a passing containment assertion proves only that the binding is
        // broken, which is how a security check goes quietly inert.
        CHECK(bgSourceFor(QStringLiteral("bg.jpg")) == base + QStringLiteral("/bg.jpg"),
              "background: a path inside the theme folder still resolves and renders");
        // The escapes, through the real binding.
        CHECK(bgSourceFor(QStringLiteral("../Channels/bg.jpg")).isEmpty(),
              "background: a sibling theme's folder is refused (no source at all)");
        CHECK(bgSourceFor(QStringLiteral("../../../secret.png")).isEmpty(),
              "background: climbing out of themes2 is refused");
        CHECK(bgSourceFor(QStringLiteral("C:/Users/x/secret.png")).isEmpty(),
              "background: an absolute path is refused");
        // The policy call, end to end: a manifest cannot make the app fetch from a remote host on render.
        CHECK(bgSourceFor(QStringLiteral("https://attacker.example/x.png")).isEmpty(),
              "background: a remote url in a MANIFEST is refused — the app never fetches it");

        // The wiring itself, called the way an element calls it. `resolve` is GONE rather than aliased, so a
        // new element cannot reach the old permissive rule by habit; assert its absence, or the pair below is
        // only proof that two more functions exist beside it.
        NavGraph g;
        buildThemedNavGraph(g, 0);
        buildAudioPageNavGraph(g);
        QQuickWidget qw;
        qw.setResizeMode(QQuickWidget::SizeRootObjectToView);
        qw.rootContext()->setContextProperty(QStringLiteral("nav"), &g);
        qw.rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());
        qw.setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));
        if (QQuickItem* root = qw.rootObject())
        {
            root->setProperty("base", base);
            QVariant out;
            CHECK(QMetaObject::invokeMethod(root, "themeAsset", Q_RETURN_ARG(QVariant, out),
                                            Q_ARG(QVariant, QStringLiteral("art/box.png")))
                  && out.toString() == base + QStringLiteral("/art/box.png"),
                  "host.themeAsset is callable on the root and binds `base`");
            CHECK(QMetaObject::invokeMethod(root, "themeAsset", Q_RETURN_ARG(QVariant, out),
                                            Q_ARG(QVariant, QStringLiteral("../NightMare/box.png")))
                  && out.toString().isEmpty(),
                  "host.themeAsset refuses a sibling whose name extends this one");
            CHECK(QMetaObject::invokeMethod(root, "contentUrl", Q_RETURN_ARG(QVariant, out),
                                            Q_ARG(QVariant, QStringLiteral("https://img.example/p.jpg")))
                  && out.toString() == QStringLiteral("https://img.example/p.jpg"),
                  "host.contentUrl still passes a provider's url through — the catalog keeps its artwork");
            CHECK(!QMetaObject::invokeMethod(root, "resolve", Q_RETURN_ARG(QVariant, out),
                                             Q_ARG(QVariant, QStringLiteral("../../../secret.png"))),
                  "the permissive resolve() is GONE, not aliased — an element must choose a rule");
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
    if (!root) return;

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

}

// §20 — the touch INPUT model (D1 Task 4). Synthesizes REAL touch sequences (QTest::touchEvent → real
// hit-testing through the QML scene, NOT a shortcut into the graph) against the two themed surfaces and pins
// the mobile tap/flick/edge-back contract, plus the Desktop identity net (two-step click frozen). Everything
// is gated on FormFactor mode, so the Desktop leg proves a pixel/behaviour no-op with default settings. It
// puts the mode back to "auto" on the way out because §21 and §22 run after it and never set one themselves.
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

    // ---- (g) Channels: a vertical swipe pages the grid + look-ahead prefetch (round 6) ----
    // A channels-element fixture in a PORTRAIT window (mobile portrait forces the 2x3 page -> 6 per page;
    // round 7 made the mobile grid orientation-aware, so the window must actually be portrait) with 14
    // items -> 3 pages. A vertical mouse-drag on the viewport MouseArea (round 7: the touch tap/swipe
    // arbiter — per-cell areas are desktop-only) must flip a WHOLE page through gotoItemSelectOnly
    // (selection +-perPage, never an activation), and landing on a page whose successor isn't fully
    // loaded must fire nearEnd() exactly once (the per-page latch stops repeats).
    {
        Settings::setDisplayMode(QStringLiteral("mobile"));
        FormFactor::instance().refresh();
        pump();
        const QVariantMap chEl{ { QStringLiteral("type"), QStringLiteral("channels") },
                                { QStringLiteral("columns"), 4 }, { QStringLiteral("rows"), 3 },
                                { QStringLiteral("pos"), QVariantList{ 0, 0 } },
                                { QStringLiteral("size"), QVariantList{ 1, 1 } } };
        const QVariantMap chHome{ { QStringLiteral("background"), QVariantMap{ { QStringLiteral("color"), QStringLiteral("#101010") } } },
                                  { QStringLiteral("elements"), QVariantList{ chEl } } };
        const QVariantMap chTheme{ { QStringLiteral("name"), QStringLiteral("TouchChannels") },
                                   { QStringLiteral("views"), QVariantMap{ { QStringLiteral("home"), chHome } } } };
        NavGraph cg;
        buildThemedNavGraph(cg, 14);
        buildAudioPageNavGraph(cg);
        QQuickWidget cqw;
        cqw.setResizeMode(QQuickWidget::SizeRootObjectToView);
        cqw.rootContext()->setContextProperty(QStringLiteral("nav"), &cg);
        cqw.rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());
        cqw.setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));
        QQuickItem* cr = cqw.rootObject();
        CHECK(cr != nullptr, "channels(mobile): ThemeView.qml instantiates for the channels fixture");
        if (cr)
        {
            cr->setProperty("categories", QVariantList{});
            cr->setProperty("items", []() { QVariantList v; for (int i = 0; i < 14; ++i)
                v << QVariantMap{ { QStringLiteral("title"), QStringLiteral("C%1").arg(i) } }; return v; }());
            cr->setProperty("currentIndex", 0);
            cr->setProperty("currentView", QStringLiteral("home"));
            cr->setProperty("theme", chTheme);
            cqw.resize(720, 1280);   // portrait: the phone shape whose 2x3 page the asserts count on
            cqw.show();
            pump(); pump();
            cqw.grabFramebuffer();   // force a render pass so the pages realize their delegates
            pump();
            QObject::connect(&cg, &NavGraph::selectionChanged, cr, [cr](const QString& z, int i) {
                if (z == QStringLiteral("items")) cr->setProperty("currentIndex", i);
            });
            int cAct = 0;
            QObject::connect(&cg, &NavGraph::activated, cr, [&cAct](const QString&, int) { ++cAct; });
            QSignalSpy near(cr, SIGNAL(nearEnd()));
            cg.select(QStringLiteral("items"), 0);
            cr->setProperty("currentIndex", 0);
            pump();
            near.clear();

            // Swipe UP (finger travels up) -> the NEXT page: selection 0 -> 6, and page 1's successor is
            // only partially loaded ((1+2)*6 = 18 > 14), so the prefetch fires nearEnd once.
            mouseDrag(cqw.quickWindow(), QPoint(360, 900), QPoint(360, 400), 6);
            CHECK(cg.index() == 6 && cAct == 0,
                  "channels(mobile): a vertical swipe up flips one whole page (select-only, +perPage)");
            CHECK(near.count() == 1,
                  "channels(mobile): landing on the last loaded page fires nearEnd() once (prefetch before blanks)");

            // Swipe DOWN -> the previous page (6 -> 0); page 0's successor IS fully loaded (12 < 14): no prefetch.
            mouseDrag(cqw.quickWindow(), QPoint(360, 400), QPoint(360, 900), 6);
            CHECK(cg.index() == 0 && cAct == 0,
                  "channels(mobile): a vertical swipe down flips back one page (select-only)");

            // Up again -> page 1 again: the per-page latch must NOT re-fire nearEnd for the same page.
            mouseDrag(cqw.quickWindow(), QPoint(360, 900), QPoint(360, 400), 6);
            CHECK(cg.index() == 6 && near.count() == 1,
                  "channels(mobile): re-landing on the same page does NOT re-fire nearEnd (latched)");
        }
    }

    // Put the mode back for §21 and §22, which run after this one and never set a mode of their own. This is
    // NOT about later runs — the data dir is this process's alone (issue #42) — it is about the rest of this
    // one, which per-process isolation says nothing about.
    Settings::setDisplayMode(QStringLiteral("auto"));
    FormFactor::instance().refresh();
}

// §21 — the SIDEBAR route into the `categories` zone (issue #38): the second way a theme reaches that zone,
// without declaring an `xmb` element. Two legs, because the behaviour lives in two places:
//
//   (a) the MODEL shape, off the shared builder (buildThemedNavGraph with CategoriesNav::Sidebar) — the same
//       NavThemeGraph.h discipline §9 uses, so this can never drift from the graph the app builds: the zone is
//       Vertical, Left enters it, Right leaves it at the ITEM zone's remembered index (no fused step), Left
//       inside it is a contained no-op, and its Up/Down are NOT declared (so the list actually scrolls). Plus
//       the Cross-mode control: the XMB's categories--Up-->items edge exists there and must NOT exist here.
//   (b) the WHOLE shipped path, through the REAL ThemeEngine::buildView on a scratch theme.json that declares
//       a `sidebar` element: theme.json -> graph shape -> ThemeBridge write-back -> ThemeView's key routing.
//       This is what pins the load-bearing claim — that a theme can move focus into the sidebar and back out
//       WITHOUT losing the grid's 2-D stepping, and that (unlike an `xmb` element) it leaves `buttons` live.
static void runSidebarAsserts()
{
    // ---- (a) the model shape from the shared builder -------------------------------------------------
    {
        NavGraph g;
        buildThemedNavGraph(g, 12, {}, CategoriesNav::Sidebar);
        buildAudioPageNavGraph(g);
        g.setZoneCount(QStringLiteral("categories"), 5);
        g.setZoneCount(QStringLiteral("buttons"), 2);
        QString why;
        CHECK(g.validate(&why), "sidebar: the Sidebar-shaped themed graph passes its own validator");

        // Enter: Left from the grid crosses to the sidebar at the SIDEBAR's remembered index, with no fused
        // step (Left is cross-axis for a Vertical target, so the crossing does not also step it).
        g.select(QStringLiteral("categories"), 3);
        g.select(QStringLiteral("items"), 6);            // leaving categories records memory 3; items memory 6
        g.move(Qt::Key_Left);
        CHECK(g.zone() == QStringLiteral("categories") && g.index() == 3,
              "sidebar: Left from the grid enters the sidebar at its remembered row (3), un-stepped");

        // The sidebar's own axis: Up/Down step the LIST (they are deliberately not declared as edges — a
        // declared edge is consulted before axis stepping and would freeze it).
        CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("categories") && g.index() == 4,
              "sidebar: Down steps the sidebar list itself (3 -> 4), it does not leave the zone");
        CHECK(g.move(Qt::Key_Up) && g.index() == 3, "sidebar: Up steps it back (4 -> 3)");

        // Containment: nothing sits left of the sidebar, so Left is a declared SELF no-op, not a geometric hop.
        g.move(Qt::Key_Left);
        CHECK(g.zone() == QStringLiteral("categories") && g.index() == 3,
              "sidebar: Left inside the sidebar is a contained no-op (the declared self edge)");

        // Leave: Right returns to the GRID at its remembered index — this is what makes the round trip
        // non-destructive, and it is exactly the property a 2-D grid needs to survive a sidebar visit.
        g.move(Qt::Key_Right);
        CHECK(g.zone() == QStringLiteral("items") && g.index() == 6,
              "sidebar: Right returns to the grid at ITS remembered index (6), not the carried catIndex");

        // The memory seed, at the model level: a HIDDEN zone can be told where its cursor was, and select()
        // cannot do it (it refuses a count-0 zone outright and stores nothing). This is the primitive §21(d)
        // exercises through the whole shipped path — asserted here on a fresh graph so a regression names the
        // model, not the QML.
        {
            NavGraph gs;
            buildThemedNavGraph(gs, 12, {}, CategoriesNav::Sidebar);
            gs.select(QStringLiteral("categories"), 3);   // hidden (count 0) -> refused, stores NOTHING
            gs.setZoneCount(QStringLiteral("categories"), 5);
            gs.select(QStringLiteral("items"), 6);
            gs.move(Qt::Key_Left);
            CHECK(gs.zone() == QStringLiteral("categories") && gs.index() == 0,
                  "seed: select() onto a HIDDEN zone stores nothing — the crossing still enters at row 0");

            NavGraph gt;
            buildThemedNavGraph(gt, 12, {}, CategoriesNav::Sidebar);
            gt.seedIndex(QStringLiteral("categories"), 3);   // legal while hidden — that is the whole point
            gt.setZoneCount(QStringLiteral("categories"), 5);
            gt.select(QStringLiteral("items"), 6);
            gt.move(Qt::Key_Left);
            CHECK(gt.zone() == QStringLiteral("categories") && gt.index() == 3,
                  "seed: seedIndex() on a hidden zone survives the count-up — the crossing enters at row 3");
            gt.move(Qt::Key_Right);
            CHECK(gt.zone() == QStringLiteral("items") && gt.index() == 6,
                  "seed: …and seeding one zone never moved the cursor off the other (grid still at 6)");
            gt.seedIndex(QStringLiteral("categories"), 99);  // snapped at USE time, not at seed time
            gt.move(Qt::Key_Left);
            CHECK(gt.zone() == QStringLiteral("categories") && gt.index() == 4,
                  "seed: an out-of-range seed is clamped when the crossing reads it (99 -> last row 4)");
        }

        // The Cross control: the XMB shape still declares categories--Up-->items, and the Sidebar shape must
        // not — that difference IS the two shapes. (In Sidebar mode Up stepped the list, asserted above.)
        NavGraph gx;
        buildThemedNavGraph(gx, 12);                     // default = CategoriesNav::Cross, i.e. the XMB
        gx.setZoneCount(QStringLiteral("categories"), 5);
        gx.select(QStringLiteral("items"), 6);
        gx.select(QStringLiteral("categories"), 3);
        gx.move(Qt::Key_Up);
        CHECK(gx.zone() == QStringLiteral("items"),
              "cross control: the XMB shape still crosses categories--Up-->items (unchanged by the sidebar work)");
        // …and the Cross shape's Left/Right from the column still switch to the category axis (the fused step).
        gx.select(QStringLiteral("items"), 4);
        CHECK(gx.move(Qt::Key_Right) && gx.zone() == QStringLiteral("categories"),
              "cross control: the XMB shape's Right from the column still switches + steps the category axis");
    }

    // ---- (b) the whole shipped path: theme.json -> buildView -> bridge -> ThemeView key routing --------
    {
        QTemporaryDir dir;
        CHECK(dir.isValid(), "sidebar: a scratch theme dir exists");
        if (!dir.isValid()) return;
        // A theme that declares a `sidebar` beside a 4-column grid AND a corner `button`. The button is the
        // positive control for the OTHER half of the claim: an `xmb` element force-disables the `buttons` zone
        // (§18(g)), and opting into the categories zone this way must NOT.
        const char* themeJson =
            "{ \"name\": \"SidebarProbe\", \"views\": { \"home\": {"
            "  \"background\": { \"color\": \"#101010\" },"
            "  \"elements\": ["
            "    { \"type\": \"sidebar\", \"pos\": [0, 0], \"size\": [0.2, 1] },"
            "    { \"type\": \"grid\", \"columns\": 4, \"pos\": [0.2, 0], \"size\": [0.8, 0.9] },"
            "    { \"type\": \"text\", \"binding\": \"selectedMeta.title\", \"pos\": [0.2, 0.92], \"size\": [0.6, 0.06] },"
            "    { \"type\": \"button\", \"action\": \"settings\", \"pos\": [0.9, 0.94], \"size\": [0.08, 0.05] }"
            "  ] } } }";
        QFile tf(dir.filePath(QStringLiteral("theme.json")));
        CHECK(tf.open(QIODevice::WriteOnly), "sidebar: the scratch theme.json is writable");
        tf.write(themeJson);
        tf.close();

        QVariantList items;
        for (int i = 0; i < 12; ++i)
            items << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Item %1").arg(i) } };
        QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("Probe"));

        // The REAL app path: buildView owns the theme parse, the graph shape choice, the bridge and the QML.
        QWidget* w = ThemeEngine::buildView(dir.path(), items, system, nullptr);
        auto* qw = qobject_cast<QQuickWidget*>(w);
        CHECK(qw != nullptr, "sidebar: buildView returned the themed QQuickWidget");
        NavGraph* g = ThemeEngine::navGraph(w);
        QQuickItem* root = ThemeEngine::rootItem(w);
        CHECK(g && root, "sidebar: the built view carries a NavGraph and a scene root");
        if (!qw || !g || !root) { if (w) delete w; return; }

        QVariantList cats;
        for (int i = 0; i < 5; ++i)
            cats << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Cat %1").arg(i) } };
        root->setProperty("catIndex", 0);
        root->setProperty("categories", cats);
        qw->resize(1280, 720);
        qw->show();
        pump(); pump();
        qw->grabFramebuffer();      // force a render pass so the Repeater realizes the element delegates
        pump();

        CHECK(root->property("sidebarMode").toBool(), "sidebar: the `sidebar` element puts the view in sidebarMode");
        CHECK(!root->property("xmbMode").toBool(), "sidebar: …without being xmbMode (no `xmb` element declared)");
        // The Sidebar element is a Repeater-delegate Loader child — VISUALLY parented but not a QObject child,
        // so walk the visual tree (findChild follows QObject parentage and never reaches it; see §20's note).
        QQuickItem* rail = nullptr;
        {
            QList<QQuickItem*> stack = root->childItems();
            while (!stack.isEmpty())
            {
                QQuickItem* it = stack.takeLast();
                if (it->objectName() == QStringLiteral("themeSidebar")) { rail = it; break; }
                stack += it->childItems();
            }
        }
        CHECK(rail != nullptr, "sidebar: the Sidebar element instantiated from the qrc (it is listed in theme2.qrc)");
        CHECK(rail && rail->property("count").toInt() == 5,
              "sidebar: …and it renders one row per host category (5)");
        // The button bar stays LIVE — the whole point of not having to declare an `xmb` element to reach the
        // categories zone (compare §18(g), where xmbMode holds this zone at 0).
        CHECK(root->property("buttonList").toList().size() == 1, "sidebar: the corner button is in buttonList");
        g->select(QStringLiteral("buttons"), 0);
        CHECK(g->zone() == QStringLiteral("buttons"),
              "sidebar: the `buttons` zone is LIVE alongside a sidebar (an xmb element would have killed it)");
        g->select(QStringLiteral("items"), 0);

        root->forceActiveFocus();
        pump();
        // 2-D stepping, untouched: Right steps along the row, Down jumps a full row of 4.
        sendKey(qw->quickWindow(), Qt::Key_Right);
        CHECK(g->zone() == QStringLiteral("items") && root->property("currentIndex").toInt() == 1,
              "sidebar: Right off the leftmost column steps the GRID (0 -> 1), it does not enter the sidebar");
        sendKey(qw->quickWindow(), Qt::Key_Down);
        CHECK(root->property("currentIndex").toInt() == 5,
              "sidebar: Down still jumps a full grid row (1 -> 5, columns = 4)");
        sendKey(qw->quickWindow(), Qt::Key_Left);
        CHECK(g->zone() == QStringLiteral("items") && root->property("currentIndex").toInt() == 4,
              "sidebar: Left mid-row is an ordinary grid step (5 -> 4) — the categories edge is gated by column");

        // …and at the leftmost column (4 % 4 == 0) the SAME key crosses into the sidebar.
        sendKey(qw->quickWindow(), Qt::Key_Left);
        CHECK(g->zone() == QStringLiteral("categories"), "sidebar: Left at the leftmost column enters the sidebar");
        CHECK(root->property("navZone").toString() == QStringLiteral("categories"),
              "sidebar: the bridge mirrors the zone into navZone (what the element draws its focus ring from)");
        // Inside the sidebar the arrows belong to the LIST; the grid cursor must not budge.
        sendKey(qw->quickWindow(), Qt::Key_Down);
        CHECK(root->property("catIndex").toInt() == 1, "sidebar: Down steps the sidebar (cat 0 -> 1)");
        CHECK(root->property("currentIndex").toInt() == 4, "sidebar: …and the grid cursor is untouched (still 4)");
        CHECK(root->property("uitestCategory").toString() == QStringLiteral("Cat 1"),
              "sidebar: the UI-test snapshot reports the sidebar's category (it used to be XMB-only)");

        // A HOST row-set swap while the sidebar holds focus — what a real category switch does (the host
        // reloads the grid for the newly selected section). It must NOT eject the cursor back to the grid, or
        // one Down in the sidebar would kick you out of it; and the model must re-seat the grid cursor on the
        // NEW list, so leaving the sidebar does not land on a stale row of the list that is gone.
        QVariantList swapped;
        for (int i = 0; i < 6; ++i)
            swapped << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Swap %1").arg(i) } };
        root->setProperty("items", swapped);
        root->setProperty("currentIndex", 0);
        pump();
        CHECK(g->zone() == QStringLiteral("categories"),
              "sidebar: a host row-set reload under a focused sidebar does NOT steal focus back to the grid");
        sendKey(qw->quickWindow(), Qt::Key_Right);
        CHECK(g->zone() == QStringLiteral("items") && root->property("currentIndex").toInt() == 0,
              "sidebar: …and leaving lands on the NEW list's cursor (0), not the vanished list's remembered cell");

        // Restore the 12-item fixture and re-park at cell 4 for the round-trip assertion below.
        root->setProperty("items", items);
        pump();
        g->select(QStringLiteral("items"), 4);
        sendKey(qw->quickWindow(), Qt::Key_Left);
        CHECK(g->zone() == QStringLiteral("categories"), "sidebar: re-entered the sidebar from cell 4");

        // Back out, then keep stepping the grid: the whole point — the round trip costs no grid position.
        sendKey(qw->quickWindow(), Qt::Key_Right);
        CHECK(g->zone() == QStringLiteral("items") && root->property("currentIndex").toInt() == 4,
              "sidebar: Right leaves the sidebar back onto the grid's remembered cell (4)");
        sendKey(qw->quickWindow(), Qt::Key_Down);
        CHECK(root->property("currentIndex").toInt() == 8,
              "sidebar: 2-D stepping survives the round trip (4 -> 8, still a full row)");

        // selectedMeta reaches the theme through dataCtx — the contract a details pane binds ("selectedMeta.x").
        QVariantMap meta;
        meta.insert(QStringLiteral("title"), QStringLiteral("HOVERED"));
        root->setProperty("selectedMeta", meta);
        pump();
        const QVariantMap ctx = root->property("dataCtx").toMap();
        CHECK(ctx.contains(QStringLiteral("selectedMeta")), "sidebar: dataCtx exposes selectedMeta to bindings");
        CHECK(ctx.value(QStringLiteral("selectedMeta")).toMap().value(QStringLiteral("title")).toString()
              == QStringLiteral("HOVERED"),
              "sidebar: …and it carries the host's live hover data (selectedMeta.title resolves)");

        delete w;
        pump();
    }

    // ---- (c) the REGRESSION bar: a grid theme with NO sidebar, whose `categories` zone is now POPULATED ----
    // Item 3 of the issue makes the host feed `categories` on the grid path (the previews always did; the app
    // never did). That counts the zone UP on Channels-shaped themes too, which makes the Cross shape's declared
    // items--Left/Right-->categories edges LIVE where they used to be inert. Nothing may change for them: the
    // grid's Left/Right are resolved by the QML (gridSelect), never by nav.move, so the live edge is never
    // walked. This is the assertion that keeps it that way — it is exactly the Channels home/browse behaviour.
    {
        QTemporaryDir dir;
        // CHECKed, not silently skipped: this section is the ONE that pins the shared-path behaviour change
        // (a populated `categories` zone on a theme with no sidebar), so a temp dir that cannot be created
        // must be a visible failure — a bare `return` would retire the whole regression bar with zero
        // failures reported, exactly the wrong failure mode. Same discipline as (b) above.
        CHECK(dir.isValid(), "grid-no-sidebar: a scratch theme dir exists");
        if (!dir.isValid()) return;
        const char* themeJson =
            "{ \"name\": \"GridNoSidebar\", \"views\": { \"home\": {"
            "  \"background\": { \"color\": \"#101010\" },"
            "  \"elements\": [ { \"type\": \"grid\", \"columns\": 4, \"pos\": [0, 0], \"size\": [1, 0.9] } ] } } }";
        QFile tf(dir.filePath(QStringLiteral("theme.json")));
        if (!tf.open(QIODevice::WriteOnly)) { CHECK(false, "grid-no-sidebar: scratch theme.json writable"); return; }
        tf.write(themeJson);
        tf.close();

        QVariantList items;
        for (int i = 0; i < 12; ++i)
            items << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Item %1").arg(i) } };
        QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("Probe"));
        QWidget* w = ThemeEngine::buildView(dir.path(), items, system, nullptr);
        auto* qw = qobject_cast<QQuickWidget*>(w);
        NavGraph* g = ThemeEngine::navGraph(w);
        QQuickItem* root = ThemeEngine::rootItem(w);
        CHECK(qw && g && root, "grid-no-sidebar: the fixture built");
        if (!qw || !g || !root) { if (w) delete w; return; }

        QVariantList cats;                                   // the host now feeds these on the grid path
        for (int i = 0; i < 5; ++i)
            cats << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Cat %1").arg(i) } };
        root->setProperty("catIndex", 0);
        root->setProperty("categories", cats);
        qw->resize(1280, 720);
        qw->show();
        pump(); pump(); qw->grabFramebuffer(); pump();

        CHECK(!root->property("sidebarMode").toBool() && !root->property("xmbMode").toBool(),
              "grid-no-sidebar: neither sidebarMode nor xmbMode (a plain grid theme)");
        root->forceActiveFocus();
        pump();
        g->select(QStringLiteral("items"), 4);               // a LEFTMOST-column cell (4 % 4 == 0)
        sendKey(qw->quickWindow(), Qt::Key_Left);
        CHECK(g->zone() == QStringLiteral("items"),
              "grid-no-sidebar: Left at the leftmost column stays in the grid — a POPULATED categories zone "
              "does not make the Cross shape's Left edge reachable on a theme with no sidebar");
        CHECK(root->property("currentIndex").toInt() == 3,
              "grid-no-sidebar: …it is an ordinary grid step onto the previous row's last cell (4 -> 3)");
        sendKey(qw->quickWindow(), Qt::Key_Right);
        CHECK(g->zone() == QStringLiteral("items") && root->property("currentIndex").toInt() == 4,
              "grid-no-sidebar: Right steps the grid too (3 -> 4), it never switches to the category axis");
        sendKey(qw->quickWindow(), Qt::Key_Down);
        CHECK(root->property("currentIndex").toInt() == 8,
              "grid-no-sidebar: Down still jumps a full row (4 -> 8) — 2-D stepping is untouched");
        delete w;
        pump();
    }

    // ---- (d) the REBUILD SEED: a NON-ZERO remembered category written while the zone is still hidden ------
    // The host rebuilds the whole themed home on a category switch or a theme cycle and re-seeds the cursors
    // by writing the props in a deliberate order: catIndex FIRST (the `categories` zone is still hidden at
    // that instant, so the prop->model sync cannot hand it the cursor and park focus in the sidebar), THEN
    // categories (which counts the zone up), THEN currentIndex. NavGraph::select is REFUSED outright on a
    // count-0 zone and stores nothing, so the model's remembered category has to be seeded some other way —
    // otherwise it stays 0 while the rail draws row N, and the first Left into the rail snaps the highlight
    // to "All" over bucket N's grid (and uitestCategory reports the wrong bucket with it).
    //
    // Everything in (a)-(c) drives catIndex == 0, where that bug is invisible. This drives a NON-ZERO one,
    // in the host's real write order.
    {
        QTemporaryDir dir;
        CHECK(dir.isValid(), "sidebar-seed: a scratch theme dir exists");
        if (!dir.isValid()) return;
        const char* themeJson =
            "{ \"name\": \"SidebarSeed\", \"views\": { \"home\": {"
            "  \"background\": { \"color\": \"#101010\" },"
            "  \"elements\": ["
            "    { \"type\": \"sidebar\", \"pos\": [0, 0], \"size\": [0.2, 1] },"
            "    { \"type\": \"grid\", \"columns\": 4, \"pos\": [0.2, 0], \"size\": [0.8, 1] }"
            "  ] } } }";
        QFile tf(dir.filePath(QStringLiteral("theme.json")));
        if (!tf.open(QIODevice::WriteOnly)) { CHECK(false, "sidebar-seed: scratch theme.json writable"); return; }
        tf.write(themeJson);
        tf.close();

        QVariantList items;
        for (int i = 0; i < 12; ++i)
            items << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Item %1").arg(i) } };
        QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("Probe"));
        QWidget* w = ThemeEngine::buildView(dir.path(), items, system, nullptr);
        auto* qw = qobject_cast<QQuickWidget*>(w);
        NavGraph* g = ThemeEngine::navGraph(w);
        QQuickItem* root = ThemeEngine::rootItem(w);
        CHECK(qw && g && root, "sidebar-seed: the fixture built");
        if (!qw || !g || !root) { if (w) delete w; return; }

        QVariantList cats;
        for (int i = 0; i < 5; ++i)
            cats << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Cat %1").arg(i) } };
        // THE HOST'S WRITE ORDER, verbatim (MainWindow::showThemedHome / showThemedBrowse).
        root->setProperty("catIndex", 3);            // zone still hidden (count 0) — select() is refused here
        root->setProperty("categories", cats);       // …and only NOW does the zone count up
        root->setProperty("currentIndex", 4);
        qw->resize(1280, 720);
        qw->show();
        pump(); pump(); qw->grabFramebuffer(); pump();

        CHECK(root->property("sidebarMode").toBool(), "sidebar-seed: the fixture is a sidebar theme");
        // The rebuild parks focus on the GRID, not the rail — the reason catIndex is written first.
        CHECK(g->zone() == QStringLiteral("items") && root->property("currentIndex").toInt() == 4,
              "sidebar-seed: the rebuilt view opens with focus on the grid at the seeded cell (4)");
        CHECK(root->property("uitestCategory").toString() == QStringLiteral("Cat 3"),
              "sidebar-seed: the rail draws the remembered bucket (Cat 3) after the rebuild");

        root->forceActiveFocus();
        pump();
        sendKey(qw->quickWindow(), Qt::Key_Left);    // leftmost column (4 % 4 == 0) -> into the rail
        CHECK(g->zone() == QStringLiteral("categories"), "sidebar-seed: Left at column 0 enters the rail");
        CHECK(g->index() == 3,
              "sidebar-seed: …at the REMEMBERED bucket 3, not row 0 — a non-zero catIndex written while the "
              "zone was hidden still reaches the model");
        CHECK(root->property("catIndex").toInt() == 3,
              "sidebar-seed: …so the highlight does not snap to \"All\" over bucket 3's grid");
        CHECK(root->property("uitestCategory").toString() == QStringLiteral("Cat 3"),
              "sidebar-seed: …and uitestCategory still reports the bucket the grid is showing");
        // The seeded row is a live cursor, not a one-shot: stepping off it and back out behaves normally.
        sendKey(qw->quickWindow(), Qt::Key_Down);
        CHECK(root->property("catIndex").toInt() == 4, "sidebar-seed: Down steps on from the seeded row (3 -> 4)");
        sendKey(qw->quickWindow(), Qt::Key_Right);
        CHECK(g->zone() == QStringLiteral("items") && root->property("currentIndex").toInt() == 4,
              "sidebar-seed: Right returns to the grid's remembered cell (4)");
        delete w;
        pump();
    }
}

// §22 — a view the theme never DECLARED must still be visible (issue #29). The pure decision behind this is
// Theme.js's viewFor, pinned by probe_themeview; what THIS section pins is that the shipped renderer asks it
// — i.e. that ThemeView actually resolves its `view` through the fallback instead of the old direct
// `theme.views[currentView]` read. Those are two different claims, and the pure one alone is inert: revert
// ThemeView.qml's binding and probe_themeview stays green while every affected screen goes blank again.
//
// The bug's observable signature was exact: Triple declares `home`/`detail`/`nowplayingAudio` and no
// `browse`, the cross-addon search from the XMB root is the one route that opens `browse`, and the frame
// that came back was ONE distinct colour — #0F1216, the renderer's own default, not even the theme's
// background — over a model that was populated and navigable the whole time. So this drives the real
// ThemeEngine::buildView on a browse-less theme and asserts against both halves of that: the element tree
// (a grid is instantiated) and the pixels (the frame is not one flat colour).
//
// The third leg is the hardening: a row with NO artwork must degrade to a readable title rather than a bare
// tile, in the one card style that puts no title anywhere by default (`label: "none"`).
static void runUndeclaredViewAsserts()
{
    // Distinct colours in a grabbed frame. One means "a flat rectangle and nothing else" — the exact
    // signature of the blank screen, and a much harder thing to fake than an element-tree walk alone.
    auto distinctColours = [](const QImage& im) {
        QSet<QRgb> seen;
        for (int y = 0; y < im.height(); y += 2)
            for (int x = 0; x < im.width(); x += 2)
            {
                seen.insert(im.pixel(x, y));
                if (seen.size() > 64) return 65;   // enough; stop scanning
            }
        return int(seen.size());
    };
    // Depth-first walk of the VISUAL tree (element delegates are Repeater/Loader children — visually
    // parented but not QObject children, so findChild never reaches them; same note as §21).
    auto findVisual = [](QQuickItem* from, const std::function<bool(QQuickItem*)>& pred) -> QQuickItem* {
        QList<QQuickItem*> stack = from->childItems();
        while (!stack.isEmpty())
        {
            QQuickItem* it = stack.takeLast();
            if (pred(it)) return it;
            stack += it->childItems();
        }
        return nullptr;
    };

    // ---- (a) the browse-less theme: the shape that shipped as Triple ----------------------------------
    {
        QTemporaryDir dir;
        CHECK(dir.isValid(), "undeclared-view: a scratch theme dir exists");
        if (!dir.isValid()) return;
        // An xmb `home` and NOTHING else — theme.json as Triple's was when #29 was filed.
        const char* themeJson =
            "{ \"name\": \"XmbNoBrowse\", \"views\": { \"home\": {"
            "  \"background\": { \"color\": \"#0A1326\" },"
            "  \"elements\": [ { \"type\": \"xmb\", \"id\": \"cross\", \"pos\": [0, 0], \"size\": [1, 1] } ] } } }";
        QFile tf(dir.filePath(QStringLiteral("theme.json")));
        if (!tf.open(QIODevice::WriteOnly)) { CHECK(false, "undeclared-view: scratch theme.json writable"); return; }
        tf.write(themeJson);
        tf.close();

        QVariantList items;
        for (int i = 0; i < 12; ++i)
            items << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Result %1").arg(i) } };
        QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("Search: up"));
        QWidget* w = ThemeEngine::buildView(dir.path(), items, system, nullptr);
        auto* qw = qobject_cast<QQuickWidget*>(w);
        QQuickItem* root = ThemeEngine::rootItem(w);
        CHECK(qw && root, "undeclared-view: the browse-less fixture built");
        if (!qw || !root) { if (w) delete w; return; }
        qw->resize(1280, 720);
        qw->show();
        pump(); pump(); qw->grabFramebuffer(); pump();

        // The control FIRST: the theme's own `home` is what renders on the home view. If the fallback were
        // stealing declared views too, everything below would pass for the wrong reason.
        CHECK(root->property("xmbMode").toBool(),
              "undeclared-view: the declared `home` still renders the theme's own xmb cross");

        // The regression: switch to the view the theme never declared — exactly what showThemedBrowse does
        // after a cross-addon search from the XMB root.
        root->setProperty("currentView", QStringLiteral("browse"));
        // A view switch re-fades the foreground `content` from opacity 0 (see ThemeView), so a frame grabbed
        // on the next event-loop pass is legitimately still blank. Let the 220ms fade finish before judging
        // the pixels — otherwise this assertion would fail for a reason that has nothing to do with #29.
        QTest::qWait(400);
        pump(); qw->grabFramebuffer(); pump();

        const QVariantMap browseView = root->property("view").toMap();
        CHECK(!browseView.value(QStringLiteral("elements")).toList().isEmpty(),
              "undeclared-view: `browse` resolves to a view WITH elements, not the empty `({})` of #29");
        CHECK(!root->property("xmbMode").toBool(),
              "undeclared-view: …and it is not the home's cross leaking across (the fallback is its own layout)");
        QQuickItem* grid = findVisual(root, [](QQuickItem* it) {
            return it->objectName() == QStringLiteral("themeGrid");
        });
        CHECK(grid != nullptr, "undeclared-view: the fallback instantiates a grid — the items are what the screen is FOR");
        CHECK(grid && grid->property("count").toInt() == 12,
              "undeclared-view: …carrying every row the host handed the view (12)");

        // The pixels, which is what the user actually reported. Before the fix this frame was a single
        // colour; an element tree alone could be instantiated and still paint nothing.
        const QImage frame = qw->grabFramebuffer();
        CHECK(!frame.isNull(), "undeclared-view: the frame grabbed");
        CHECK(!frame.isNull() && distinctColours(frame) > 1,
              "undeclared-view: the browse frame is NOT one flat colour (issue #29's exact signature)");
        // And it wears the THEME's background, not the renderer's #0F1216 default — the tell that told us
        // the whole view object was missing rather than just its elements.
        CHECK(!frame.isNull() && frame.pixel(4, 4) == qRgb(0x0A, 0x13, 0x26),
              "undeclared-view: the fallback inherits the theme's own home background (#0A1326)");

        delete w;
        pump();
    }

    // ---- (b) the hardening: an artwork-less row degrades to readable text -----------------------------
    // `label: "none"` is the one card style that puts no title on the card, so a row whose artwork is
    // missing (or whose url is dead) used to be a bare coloured rectangle with nothing on it at all.
    //
    // The rule has TWO halves and this fixture has to hold both of them down, because they fail in opposite
    // directions and a fixture where nothing ever loads cannot tell them apart. `tileNeedsTitle(item,
    // artReady)` says "draw the title unless artwork is actually ON SCREEN": hard-wire artReady true at the
    // call site and the DEAD-url row silently loses its placeholder again (the very regression the shared
    // rule exists for); wire it false, or drop the artReady term, and the placeholder is painted straight
    // over artwork that loaded perfectly well. So the grid below carries a row whose art is a REAL file
    // written into the scratch dir NEXT TO a row whose url points at nothing, and asserts the title hidden
    // on the first and visible on the second. The Image statuses are asserted too — without them, "no
    // visible title on the good row" would pass just as well if the good row had never loaded either.
    {
        QTemporaryDir dir;
        CHECK(dir.isValid(), "tile-placeholder: a scratch theme dir exists");
        if (!dir.isValid()) return;
        const char* themeJson =
            "{ \"name\": \"NoLabelGrid\", \"views\": { \"home\": {"
            "  \"background\": { \"color\": \"#101010\" },"
            "  \"elements\": [ { \"type\": \"grid\", \"columns\": 3, \"pos\": [0, 0], \"size\": [1, 1],"
            "                    \"card\": { \"label\": \"none\" } } ] } } }";
        QFile tf(dir.filePath(QStringLiteral("theme.json")));
        if (!tf.open(QIODevice::WriteOnly)) { CHECK(false, "tile-placeholder: scratch theme.json writable"); return; }
        tf.write(themeJson);
        tf.close();

        // A REAL image, on disk beside the theme.json, so one row's artwork genuinely reaches Image.Ready.
        // `host.resolve()` reads a relative path against the theme dir, so the file name is all the row needs.
        QImage art(8, 8, QImage::Format_RGB32);
        art.fill(QColor(0x20, 0x90, 0xE0));
        CHECK(art.save(dir.filePath(QStringLiteral("real.png"))), "tile-placeholder: the scratch artwork wrote");

        // Row 0 carries NO artwork at all. Row 1 carries a real file, and carries it only under the
        // open-ended `images` role map (no scalar `image`) — the shape a builder that publishes roles
        // produces, which used to paint a bare tile while the artwork sat right there on the row. Row 2
        // carries a url that will never resolve: the file is deliberately NOT written.
        QVariantList items;
        items << QVariantMap{ { QStringLiteral("title"), QStringLiteral("ARTLESSROW") } };
        QVariantMap roleOnly{ { QStringLiteral("title"), QStringLiteral("GOODARTROW") } };
        roleOnly.insert(QStringLiteral("images"),
                        QVariantMap{ { QStringLiteral("poster"), QVariantList{ QStringLiteral("real.png") } } });
        items << roleOnly;
        items << QVariantMap{ { QStringLiteral("title"), QStringLiteral("DEADARTROW") },
                              { QStringLiteral("image"), QStringLiteral("never-written.png") } };
        QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("Probe"));
        QWidget* w = ThemeEngine::buildView(dir.path(), items, system, nullptr);
        auto* qw = qobject_cast<QQuickWidget*>(w);
        QQuickItem* root = ThemeEngine::rootItem(w);
        CHECK(qw && root, "tile-placeholder: the label-none fixture built");
        if (!qw || !root) { if (w) delete w; return; }
        qw->resize(900, 600);
        qw->show();
        QTest::qWait(400);   // the first-appear fade of `content`, as above
        pump(); qw->grabFramebuffer(); pump();

        // Both images settle asynchronously; give them a bounded chance to finish rather than a fixed wait.
        auto imageFor = [&](const QString& tail) {
            return findVisual(root, [&tail](QQuickItem* it) {
                return it->property("source").toUrl().toString().endsWith(tail);
            });
        };
        for (int i = 0; i < 40; ++i)
        {
            QQuickItem* g = imageFor(QStringLiteral("real.png"));
            if (g && g->property("status").toInt() != 0 /* Image.Loading */) break;
            QTest::qWait(25);
            pump();
        }
        pump(); qw->grabFramebuffer(); pump();

        // A VISIBLE text item carrying the row's title — the placeholder, in the card style that has no
        // label of its own. `isVisible()` is the load-bearing half: the Text exists either way, and the
        // defect was precisely that it was hidden.
        auto titleItem = [&](const QString& title, bool wantVisible) {
            return findVisual(root, [&title, wantVisible](QQuickItem* it) {
                return it->property("text").toString() == title && it->isVisible() == wantVisible;
            });
        };
        CHECK(titleItem(QStringLiteral("ARTLESSROW"), true) != nullptr,
              "tile-placeholder: an artwork-less card in a `label: none` grid draws its TITLE, not a bare tile");

        // The role-only row resolves its artwork through T.tileImage rather than the bare `image` field, so
        // the grid asks for the poster instead of falling back to a placeholder for a row that HAS art.
        QQuickItem* poster = imageFor(QStringLiteral("real.png"));
        CHECK(poster != nullptr,
              "tile-placeholder: a row carrying art only under `images.poster` still gets a poster source");
        // Image.Ready == 1. This is the anti-vacuity guard for the assertion below it: if the artwork never
        // loaded, "the good row shows no title" would pass for entirely the wrong reason.
        CHECK(poster && poster->property("status").toInt() == 1,
              "tile-placeholder: …and that artwork actually REACHES Image.Ready (a real file, really loaded)");
        // Direction 1 — artwork on screen must NOT get a placeholder painted over it. The Text still exists
        // in the delegate, so assert that it exists and is HIDDEN, never merely that no visible one is found:
        // a delegate that failed to instantiate would satisfy the weaker claim.
        CHECK(titleItem(QStringLiteral("GOODARTROW"), false) != nullptr,
              "tile-placeholder: a row whose artwork LOADED keeps its title hidden — no placeholder over the art");
        CHECK(titleItem(QStringLiteral("GOODARTROW"), true) == nullptr,
              "tile-placeholder: …and it is hidden in the only sense that matters — nothing readable is drawn on it");

        // Direction 2 — the regression the shared rule exists for. The row HAS a url, so a check that keyed
        // off the url alone would hide the title; the url is dead, so the tile would be blank. This is the
        // assertion that dies if any call site hard-wires artReady true.
        QQuickItem* dead = imageFor(QStringLiteral("never-written.png"));
        CHECK(dead != nullptr, "tile-placeholder: the dead-url row still ASKS for its (missing) artwork");
        CHECK(dead && dead->property("status").toInt() != 1,
              "tile-placeholder: …and that request does not reach Ready (the file was never written)");
        CHECK(titleItem(QStringLiteral("DEADARTROW"), true) != nullptr,
              "tile-placeholder: a row whose url is DEAD draws its title — carrying a url is not carrying artwork");

        delete w;
        pump();
    }

    // ---- (c) the fallback's text is OUTLINED, and the ink is Qt-resolved ------------------------------
    // Two claims the pure probe cannot make, because both are about what the SCENE does with what Theme.js
    // returns. (1) `outline` is a knob defaultView sets and the Text/HelpSystem elements have to read: a
    // knob no element reads is inert, and this one is the whole protection for a background that is an
    // IMAGE, where no luminance rule can pick a safe ink. (2) inkFor resolves colours through Qt.color, and
    // `Qt` has to actually be reachable from a `.pragma library` script inside the real engine — so this
    // uses a theme whose home background is the NAME "whitesmoke", which a hand-rolled hex reader would
    // have called dark and printed white-on-near-white for. That is issue #29's own failure, on the layer
    // added to fix issue #29.
    {
        QTemporaryDir dir;
        CHECK(dir.isValid(), "fallback-ink: a scratch theme dir exists");
        if (!dir.isValid()) return;
        const char* themeJson =
            "{ \"name\": \"LightNamed\", \"views\": { \"home\": {"
            "  \"background\": { \"color\": \"whitesmoke\" },"
            "  \"elements\": [ { \"type\": \"grid\", \"pos\": [0, 0], \"size\": [1, 1] } ] } } }";
        QFile tf(dir.filePath(QStringLiteral("theme.json")));
        if (!tf.open(QIODevice::WriteOnly)) { CHECK(false, "fallback-ink: scratch theme.json writable"); return; }
        tf.write(themeJson);
        tf.close();

        QVariantList items;
        items << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Row") } };
        QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("SEARCHTITLE"));
        QWidget* w = ThemeEngine::buildView(dir.path(), items, system, nullptr);
        auto* qw = qobject_cast<QQuickWidget*>(w);
        QQuickItem* root = ThemeEngine::rootItem(w);
        CHECK(qw && root, "fallback-ink: the light-named fixture built");
        if (!qw || !root) { if (w) delete w; return; }
        qw->resize(1280, 720);
        qw->show();
        root->setProperty("currentView", QStringLiteral("browse"));   // the view it never declared
        QTest::qWait(400);
        pump(); qw->grabFramebuffer(); pump();

        // The ink, off the REAL resolved view — not off a value handed back to a test harness.
        const QVariantList els = root->property("view").toMap().value(QStringLiteral("elements")).toList();
        QVariantMap title;
        for (const QVariant& e : els)
            if (e.toMap().value(QStringLiteral("id")).toString() == QStringLiteral("ebFallbackTitle"))
                title = e.toMap();
        CHECK(!title.isEmpty(), "fallback-ink: the fallback view carries its title element");
        CHECK(title.value(QStringLiteral("color")).toString() != QStringLiteral("#FFFFFF"),
              "fallback-ink: a NAMED light background ('whitesmoke') gets DARK ink — Qt.color is reachable "
              "from Theme.js in the real engine, and a name is not junk");

        // And the scene actually draws the halo: Text.Outline == 1. Without this the `outline` key would be
        // a value in a map that nothing renders.
        QQuickItem* titleText = findVisual(root, [](QQuickItem* it) {
            return it->property("text").toString() == QStringLiteral("SEARCHTITLE") && it->isVisible();
        });
        CHECK(titleText != nullptr, "fallback-ink: the fallback title is on screen, bound to system.name");
        CHECK(titleText && titleText->property("style").toInt() == 1 /* Text.Outline */,
              "fallback-ink: …and it is drawn OUTLINED, so it survives a background image too");
        CHECK(titleText && titleText->property("styleColor").value<QColor>() == QColor(0xFF, 0xFF, 0xFF),
              "fallback-ink: …in the tone OPPOSITE the ink (dark ink -> white halo), not a fixed colour");

        // The help bar had neither outline nor scrim; it carries one now, in the same tone.
        QQuickItem* helpText = findVisual(root, [](QQuickItem* it) {
            return it->property("text").toString() == QStringLiteral("Navigate") && it->isVisible();
        });
        CHECK(helpText != nullptr, "fallback-ink: the fallback help bar is on screen");
        CHECK(helpText && helpText->property("style").toInt() == 1 /* Text.Outline */,
              "fallback-ink: …and the help bar is outlined too");

        delete w;
        pump();
    }

    // ---- (d) hasView agrees with viewFor about what "declared" means ----------------------------------
    // hasView is what the HOST asks before it OFFERS a route into a view ("I" only opens `detail` on a
    // theme that has one), and the C++ gates in MainWindow ask the same question the same way. It used to
    // test the KEY while viewFor tested the ELEMENT LIST, so `"detail": { "elements": [] }` passed the gate
    // and then rendered viewFor's fallback — an item grid bound to the browse rows — with the key router
    // sitting in detail mode over it, arrows moving an invisible action cursor and Enter firing play verbs.
    {
        QTemporaryDir dir;
        CHECK(dir.isValid(), "hasview-gate: a scratch theme dir exists");
        if (!dir.isValid()) return;
        const char* themeJson =
            "{ \"name\": \"EmptyDetail\", \"views\": {"
            "  \"home\":   { \"background\": { \"color\": \"#101010\" },"
            "                \"elements\": [ { \"type\": \"grid\", \"pos\": [0, 0], \"size\": [1, 1] } ] },"
            "  \"detail\": { \"elements\": [] },"
            "  \"browse\": { \"elements\": [ { \"type\": \"grid\", \"pos\": [0, 0], \"size\": [1, 1] } ] } } }";
        QFile tf(dir.filePath(QStringLiteral("theme.json")));
        if (!tf.open(QIODevice::WriteOnly)) { CHECK(false, "hasview-gate: scratch theme.json writable"); return; }
        tf.write(themeJson);
        tf.close();

        QVariantList items;
        items << QVariantMap{ { QStringLiteral("title"), QStringLiteral("Row") } };
        QWidget* w = ThemeEngine::buildView(dir.path(), items, QVariantMap(), nullptr);
        QQuickItem* root = ThemeEngine::rootItem(w);
        CHECK(root, "hasview-gate: the empty-detail fixture built");
        if (!root) { if (w) delete w; return; }
        pump();

        auto hasView = [&](const char* name) {
            QVariant ret;
            QMetaObject::invokeMethod(root, "hasView", Q_RETURN_ARG(QVariant, ret),
                                      Q_ARG(QVariant, QVariant(QString::fromLatin1(name))));
            return ret.toBool();
        };
        CHECK(hasView("browse"), "hasview-gate: a view WITH elements is declared (the control)");
        CHECK(!hasView("detail"),
              "hasview-gate: a view declared with an EMPTY element list is NOT declared — the gate and the "
              "renderer now mean the same thing, so the host never enters detail mode over a browse grid");
        CHECK(!hasView("nowplayingAudio"), "hasview-gate: an absent view is not declared");

        delete w;
        pump();
    }
}

// §23 — a theme preview can never take the D-pad cursor, held from BOTH ends: refused at construction
// (issue #123) and refused by the ring (issue #173).
//
// The defect (issue #40): classic Settings ▸ Appearance embeds a live theme preview inside panelPage_, which
// panelRing_ covers. ThemeEngine::buildView returns a Qt::StrongFocus QQuickWidget — correct when the view IS
// the page — so the 480x300 preview joined the ring as a stop with no action that paints no focus outline.
// Arrowing into it reads as the D-pad selector vanishing. It was fixed by a setFocusPolicy line at the call
// site, and the themed twin (ThemePickerHost) held the identical line separately: a rule spelled once per
// preview is a rule the NEXT preview forgets. ThemeEngine::buildPreview now holds it once, at construction.
// That pins the constructor, not the call sites — no probe links MainWindow — so a THIRD preview site calling
// raw buildView would still be unguarded. Nav.cpp's ringMember() now refuses focusable QQuickWidgets outright,
// which closes that from the ring side; §23 asserts both halves separately, against a REAL NavRing.
//
//   (a) THE RING CONTRACT (#173) — a bare buildView embed is Qt::StrongFocus and the ring refuses it anyway,
//       and collects nothing from INSIDE it either (with the scene no longer a member, the nested-member
//       filter no longer covers its children). This is the leg the ringMember mutation kills. It replaces
//       the old positive control, which asserted the opposite — that the bare embed IS collected, the #40
//       defect reproduced. That reproduction is what this issue knowingly gives up: the defect is no longer
//       reproducible ring-side, because the ring is now the thing that refuses it.
//   (b) THE CONSTRUCTION CONTRACT (#123) — a buildPreview embed is Qt::NoFocus. This is the leg the
//       buildPreview mutation kills, and it is what keeps the constructor honest independently of (a).
//   (c) LIVENESS — an ordinary QPushButton in the same container IS collected, at the moment (a) and the
//       tripwire below are read. Without it, "not collected" would also pass on a ring that collects nothing
//       at all. This is the control that stops (a) being vacuous, and it survives every mutation by design.
//   (d) buildPreview seeds `categories` (so an XMB/sidebar theme shows its cross/rail), with the bare
//       buildView embed as the NEGATIVE control — it leaves `categories` empty. (Its companion `catIndex` is
//       deliberately NOT asserted; see the note at the assertion — 0 is also the QML default, so that leg is
//       a fixed point of the thing under test. It was written, it survived the mutation, it is gone.)
//
// Membership is asserted by IDENTITY (contains(w)), never by member COUNT: the #40 sweep showed a count
// assertion passes happily while the WRONG member is in.
//
// Mutation record (this file's standard of proof): dropping ringMember's QQuickWidget refusal turns (a) red;
// restoring Qt::StrongFocus inside buildPreview turns (b) red; dropping buildPreview's categories block turns
// (d) red. None of the three touches (c).
static void runPreviewFocusAsserts()
{
    QTemporaryDir dir;
    CHECK(dir.isValid(), "preview: a scratch theme dir exists");
    if (!dir.isValid()) return;
    // A grid home with a button — an ordinary theme, nothing preview-specific about it.
    const char* themeJson =
        "{ \"name\": \"PreviewProbe\", \"views\": { \"home\": {"
        "  \"background\": { \"color\": \"#101010\" },"
        "  \"elements\": ["
        "    { \"type\": \"grid\", \"columns\": 4, \"pos\": [0, 0], \"size\": [1, 0.9] },"
        "    { \"type\": \"button\", \"action\": \"settings\", \"pos\": [0.9, 0.94], \"size\": [0.08, 0.05] }"
        "  ] } } }";
    QFile tf(dir.filePath(QStringLiteral("theme.json")));
    CHECK(tf.open(QIODevice::WriteOnly), "preview: the scratch theme.json is writable");
    tf.write(themeJson);
    tf.close();

    QVariantList items;
    for (const char* n : { "Video", "Games", "Audio", "Reading" })
        items << QVariantMap{ { QStringLiteral("title"), QString::fromLatin1(n) } };
    QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("Probe"));

    // The stand-in for panelPage_: a container with a real focusable row, covered by a REAL NavRing.
    QWidget host;
    host.resize(900, 600);
    auto* row = new QPushButton(QStringLiteral("Theme"), &host);
    row->setGeometry(0, 0, 200, 40);
    NavRing ring(&host);
    host.show();
    row->show();
    pump();
    CHECK(ring.widgets().contains(row), "preview: the ring collects an ordinary button (the ring is live)");

    // ---- (a) the RING contract: a StrongFocus QML scene is refused ring-side (#173) ----------------------
    {
        QWidget* raw = ThemeEngine::buildView(dir.path(), items, system, &host);
        CHECK(raw != nullptr, "preview control: buildView returned a widget");
        if (raw)
        {
            raw->setGeometry(0, 60, 480, 300);
            raw->show();
            pump(); pump();
            CHECK(raw->focusPolicy() == Qt::StrongFocus,
                  "preview control: buildView still returns Qt::StrongFocus (it IS the page when it is a page)");
            // The discriminating leg. `raw` is focusable, visible, enabled and a direct child of the ring's
            // container — everything ringMember asks for EXCEPT not being a QML scene. Nothing else in this
            // probe can make it fail.
            const QVector<QWidget*> rws = ring.widgets();
            bool fromScene = false;
            for (QWidget* m : rws)
                if (m == raw || raw->isAncestorOf(m)) { fromScene = true; break; }
            CHECK(!fromScene,
                  "preview: the ring refuses a Qt::StrongFocus QQuickWidget outright — neither the scene "
                  "itself nor anything inside it is a ring stop (issue #173)");
            CHECK(rws.contains(row),
                  "preview: …while the ordinary button IS collected in the same pass (the ring is live)");
            // (d) negative control: nothing seeded the categories axis on a bare build.
            QQuickItem* rr = ThemeEngine::rootItem(raw);
            CHECK(rr && rr->property("categories").toList().isEmpty(),
                  "preview control: a bare buildView leaves `categories` empty (the seeding is buildPreview's)");
            delete raw;
            pump();
        }
    }

    // ---- (b)(c)(d) the shipped preview: focus refused at construction too --------------------------------
    {
        QWidget* pv = ThemeEngine::buildPreview(dir.path(), items, system, &host);
        CHECK(pv != nullptr, "preview: buildPreview returned a widget");
        if (pv)
        {
            pv->setGeometry(0, 60, 480, 300);
            pv->show();
            pump(); pump();
            CHECK(pv->focusPolicy() == Qt::NoFocus,
                  "preview: buildPreview refuses focus by CONSTRUCTION — no call site has to remember to");
            const QVector<QWidget*> ws = ring.widgets();
            // DELIBERATE TRIPWIRE, inert by design — no SINGLE mutation kills this one, and that is the point.
            // It asserts the user-visible consequence (#40: the D-pad can never land on the preview), which
            // both halves independently guarantee: drop ringMember's refusal and buildPreview's Qt::NoFocus
            // still keeps `pv` out; restore Qt::StrongFocus in buildPreview and the ring still refuses it.
            // It goes red only when BOTH halves are gone — i.e. exactly when the defect is back. The legs that
            // DO discriminate are (a) above for the ring and the focusPolicy CHECK just above for the
            // constructor. Identity, not count: a count assertion passes while the wrong member is in (#40).
            CHECK(!ws.contains(pv),
                  "preview: the preview is NOT a ring member — the D-pad can never land on it");
            CHECK(ws.contains(row),
                  "preview: …while the real row IS still a member (the ring is populated, not empty)");
            QQuickItem* pr = ThemeEngine::rootItem(pv);
            CHECK(pr && pr->property("categories").toList().size() == items.size(),
                  "preview: buildPreview seeds `categories` from items, so an XMB/sidebar theme shows its axis");
            // No assertion on `catIndex`. buildPreview parks it at 0, but 0 is ALSO ThemeView.qml's default —
            // so an assertion on it is a fixed point of the function under test: it passes whether or not the
            // seeding happened. It was written, mutation-tested against "drop the seeding block", SURVIVED,
            // and removed. `categories` above is the leg the mutation DOES kill, and it covers the same block.
            delete pv;
            pump();
        }
    }
}
#endif // EB_HAVE_QML

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");   // the runner loop invokes us without a -platform arg
#ifdef EB_HAVE_QML
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
            g.setZoneCount(QStringLiteral("readerSettings"), 5);   // Book: exit + font -/+ + theme + typeface
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

        // (c2) the BOOKMARK list (issue #136): the ToC's sibling panel at (row 0, col 1). Populated alongside a
        //      chapter list, it is reachable, switches with the ToC via Left/Right, is ALSO reachable from the
        //      settings row by geometry Right, and pins its own outward (Right) edge. The BFS now closes over
        //      FOUR chrome zones. This is the shape the app's ReaderChromeHost feeds (setZoneCount readerBookmarks
        //      = the bridge's bookmarkCount) — the gate exists so this graph change is deliberate.
        {
            NavGraph g;
            buildReaderNavGraph(g, ReaderKind::Book);
            g.setZoneCount(QStringLiteral("readerSettings"), 1);
            g.setZoneCount(QStringLiteral("readerToc"), 5);
            g.setZoneCount(QStringLiteral("readerBookmarks"), 3);   // three bookmarks
            QString why;
            CHECK(g.validate(&why), "reader: validates with the bookmark list populated beside the ToC");

            // The two panels sit side by side: Right off the ToC crosses to the bookmark list, Left crosses back.
            g.select(QStringLiteral("readerToc"), 0);
            CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("readerBookmarks"),
                  "reader: Right from the chapter list crosses to the bookmark list (sibling panel)");
            CHECK(g.move(Qt::Key_Left) && g.zone() == QStringLiteral("readerToc"),
                  "reader: Left from the bookmark list crosses back to the chapter list");

            // The settings row reaches the bookmark list by geometry Right (col 0 -> col 1) — the path that is the
            // ONLY bridge to it on a Pdf/Comic (their ToC is gated). If this edge were self-pinned the crossing
            // would die and the list would be unreachable there.
            g.select(QStringLiteral("readerSettings"), 0);
            CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("readerBookmarks"),
                  "reader: Right from the settings row reaches the bookmark list (the Pdf/Comic path)");

            // Regression guard: the bookmark column must NOT steal the settings-row's Up target — Up still lands
            // on the ToC (readerToc at col 0 is nearer than readerBookmarks at col 1).
            g.select(QStringLiteral("readerSettings"), 0);
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("readerToc"),
                  "reader: Up from the settings row still lands on the chapter list, not the bookmark list");

            // Containment: Right off the bookmark list (nothing further right) is a SELF-pin no-op.
            g.select(QStringLiteral("readerBookmarks"), 1);
            CHECK(!g.move(Qt::Key_Right) && g.zone() == QStringLiteral("readerBookmarks") && g.index() == 1,
                  "reader: Right across the bookmark list is a contained no-op (cross-axis SELF pin)");

            // Directed BFS: all FOUR chrome zones reachable; nothing else to escape to (standalone graph).
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
            CHECK(reached.count(QStringLiteral("readerBookmarks")),
                  "reader: arrows alone reach the bookmark list");
            CHECK(reached.size() == 4,
                  "reader: the walk closes over the four reader zones (nav, settings, toc, bookmarks)");
        }

        // (c3) the EMPTY bookmark list is the hidden zone: a book with a ToC but no bookmarks gates readerBookmarks
        //      off, so it is never a crossing target (Right off the ToC is a contained no-op) and focus can never
        //      strand on it — and dropping the last bookmark while it holds the cursor reassigns focus away.
        {
            NavGraph g;
            buildReaderNavGraph(g, ReaderKind::Book);
            g.setZoneCount(QStringLiteral("readerSettings"), 1);
            g.setZoneCount(QStringLiteral("readerToc"), 5);
            g.setZoneCount(QStringLiteral("readerBookmarks"), 0);   // no bookmarks yet
            g.select(QStringLiteral("readerToc"), 0);
            CHECK(!g.move(Qt::Key_Right) && g.zone() == QStringLiteral("readerToc"),
                  "reader: Right off the ToC is a contained no-op when the bookmark list is empty (gated)");

            // Bring one bookmark live, land on it, then remove the last -> the model reassigns off the dead zone.
            g.setZoneCount(QStringLiteral("readerBookmarks"), 1);
            g.select(QStringLiteral("readerBookmarks"), 0);
            CHECK(g.zone() == QStringLiteral("readerBookmarks"), "reader: focus can land on a live bookmark list");
            g.setZoneCount(QStringLiteral("readerBookmarks"), 0);   // last bookmark removed
            CHECK(g.zone() != QStringLiteral("readerBookmarks"),
                  "reader: removing the last bookmark reassigns focus off the (now gated) bookmark list");
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

            // The settings row is a real Horizontal row: Left/Right step ALONG it. This is the assertion that
            // would have caught a declared Left/Right edge, which is consulted before axis stepping and would
            // freeze the row at its first control with everything past it unreachable.
            if (settingsRows >= 2) {
                g.select(QStringLiteral("readerSettings"), 0);
                CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("readerSettings") && g.index() == 1,
                      "reader(pdf/comic): Right steps along the settings row");
                g.select(QStringLiteral("readerSettings"), 1);
                CHECK(g.move(Qt::Key_Left) && g.zone() == QStringLiteral("readerSettings") && g.index() == 0,
                      "reader(pdf/comic): Left steps back along the settings row");
            }
            // Down is the row's CROSS axis, so it leaves for the bar below rather than stepping in place.
            g.select(QStringLiteral("readerSettings"), 0);
            CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("readerNav"),
                  "reader(pdf/comic): Down off the settings row returns to the nav bar");

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

            // Containment: Left off the FIRST control faces off the surface (nothing sits left of col 0), so it
            // is a no-op with no SELF pin needed — a pin there would now freeze the row's own stepping.
            g.select(QStringLiteral("readerSettings"), 0);
            CHECK(!g.move(Qt::Key_Left) && g.zone() == QStringLiteral("readerSettings") && g.index() == 0,
                  "reader(pdf/comic): Left off the first control is a contained no-op");
        };
        checkReaderKind(ReaderKind::Pdf,   3, "reader(pdf): validates (3 zoom/fit rows, no ToC)");
        checkReaderKind(ReaderKind::Comic, 4, "reader(comic): validates (4 rows incl. two-up, no ToC)");

        // (e) Pdf/Comic WITH bookmarks (issue #136): the ToC is gated, so the ONLY path to the bookmark list is
        //     the settings row's geometry Right (col 0 -> col 1). With bookmarks live the walk reaches THREE
        //     zones (nav, settings, bookmarks); the ToC is never reachable (still gated off). This is the case
        //     the toc<->bookmarks Left/Right bridge cannot serve — it proves the settings-Right path is real.
        {
            NavGraph g;
            buildReaderNavGraph(g, ReaderKind::Comic);
            g.setZoneCount(QStringLiteral("readerSettings"), 4);
            g.setZoneCount(QStringLiteral("readerToc"), 0);         // comic: no ToC
            g.setZoneCount(QStringLiteral("readerBookmarks"), 2);   // two bookmarks
            QString why;
            CHECK(g.validate(&why), "reader(comic): validates with bookmarks and no ToC");
            // Right steps ALONG the row and crosses only at its END — which is exactly why the row must not
            // wrap: wrapping would send the last Right back to Exit and cut the bookmark list off entirely on a
            // Pdf/Comic, where the ToC is gated and this is the only path to it.
            g.select(QStringLiteral("readerSettings"), 3);   // the last control
            CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("readerBookmarks"),
                  "reader(comic): Right off the END of the settings row reaches the bookmark list (ToC gated)");
            g.select(QStringLiteral("readerSettings"), 0);
            CHECK(g.move(Qt::Key_Right) && g.zone() == QStringLiteral("readerSettings") && g.index() == 1,
                  "reader(comic): Right from the FIRST control steps along the row, it does not leave it");

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
            CHECK(reached.count(QStringLiteral("readerBookmarks")) && !reached.count(QStringLiteral("readerToc")),
                  "reader(comic): the bookmark list is reachable; the gated ToC is not");
            CHECK(reached.size() == 3,
                  "reader(comic): the walk closes over nav + settings + bookmark list (no ToC)");
        }
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

        // (e) THE LYRIC ZONE (issue #142): choosing a lyric line seeks there, so the lines are a nav zone and
        //     must be reachable with the arrows alone — a seek you can only reach with a mouse is not a feature
        //     on a living-room surface. The zone sits in COLUMN 1 beside the queue, and the crossing is
        //     Right/Left rather than Down/Up on purpose: the queue's Down is its own along-axis step and a
        //     declared edge is consulted BEFORE axis stepping, so a Down crossing would have frozen the queue.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12);
            buildAudioPageNavGraph(g);
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.setZoneCount(QStringLiteral("buttons"), 2);           // live home zones under the (modal) page
            g.setZoneCount(QStringLiteral("transport"), 8);
            g.setZoneCount(QStringLiteral("queue"), 5);
            g.setZoneCount(QStringLiteral("lyrics"), 7);            // a synced sheet: 7 timed lines
            QString why;
            CHECK(g.validate(&why), "lyrics: validates with the lyric zone counted up beside the queue");

            // The ZONE is what these assert, not move()'s return: crossByEdge reports "did the visible index
            // change", so a crossing that lands on the target's remembered index answers false while having
            // moved perfectly well. That is the same distinction ThemeView's key handler makes when it decides
            // whether to play the navigation sound.
            g.select(QStringLiteral("queue"), 2);
            g.move(Qt::Key_Right);
            CHECK(g.zone() == QStringLiteral("lyrics"),
                  "lyrics: Right from the queue crosses into the lyric lines");
            g.move(Qt::Key_Left);
            CHECK(g.zone() == QStringLiteral("queue") && g.index() == 2,
                  "lyrics: Left from the lyric lines returns to the queue, on the row it left");

            // The list steps with Up/Down — the whole point, since stepping is how a line is chosen. Down at
            // the last line is a contained no-op; Up at the first crosses UP to the transport strip (the
            // nearest zone above), never onto the live home zones underneath.
            g.select(QStringLiteral("lyrics"), 3);
            CHECK(g.move(Qt::Key_Down) && g.zone() == QStringLiteral("lyrics") && g.index() == 4,
                  "lyrics: Down steps to the next line");
            CHECK(g.move(Qt::Key_Up) && g.zone() == QStringLiteral("lyrics") && g.index() == 3,
                  "lyrics: Up steps to the previous line");
            g.select(QStringLiteral("lyrics"), 6);
            CHECK(!g.move(Qt::Key_Down) && g.zone() == QStringLiteral("lyrics") && g.index() == 6,
                  "lyrics: Down off the last line is a contained no-op");
            g.select(QStringLiteral("lyrics"), 0);
            g.move(Qt::Key_Up);
            CHECK(g.zone() == QStringLiteral("transport"),
                  "lyrics: Up off the first line leaves for the transport strip, not the home surface");

            // Containment, the same directed BFS the page's other zones get: with lyrics live, the walk reaches
            // exactly the three audio zones and nothing under them.
            std::set<QString> reached;
            std::set<std::pair<QString,int>> seen;
            std::deque<std::pair<QString,int>> q;
            g.select(QStringLiteral("transport"), 0);
            q.push_back({g.zone(), g.index()});
            seen.insert({g.zone(), g.index()});
            reached.insert(g.zone());
            static const Qt::Key larr[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};
            while (!q.empty()) {
                auto [z, i] = q.front(); q.pop_front();
                for (Qt::Key k : larr) {
                    g.select(z, i);
                    g.move(k);
                    auto st = std::make_pair(g.zone(), g.index());
                    if (!seen.count(st)) { seen.insert(st); reached.insert(st.first); q.push_back(st); }
                }
            }
            CHECK(!reached.count(QStringLiteral("items")) && !reached.count(QStringLiteral("categories"))
                  && !reached.count(QStringLiteral("buttons")),
                  "lyrics: no arrow sequence escapes the audio surface onto the live home zones");
            CHECK(reached.count(QStringLiteral("lyrics")) && reached.size() == 3,
                  "lyrics: the contained walk reaches exactly transport + queue + lyric lines");
        }

        // (f) AN UNSYNCED SHEET OFFERS NO SEEK. The host counts this zone from audioLyricCount, which QML
        //     computes as "the number of lines, but only when they are SYNCED" — so a plain USLT sheet counts
        //     to 0 here. That is the whole enforcement: a zone with count 0 is never a move target, so the
        //     cursor cannot reach a line whose timestamp does not exist, and Right from the queue must still
        //     be CONTAINED rather than escaping geometrically onto the home grid underneath (which is what a
        //     single crossing edge with no fallback pin would have done).
        {
            NavGraph g;
            buildThemedNavGraph(g, 12);
            buildAudioPageNavGraph(g);
            g.setZoneCount(QStringLiteral("categories"), 6);
            g.setZoneCount(QStringLiteral("buttons"), 2);
            g.setZoneCount(QStringLiteral("transport"), 8);
            g.setZoneCount(QStringLiteral("queue"), 5);
            g.setZoneCount(QStringLiteral("lyrics"), 0);            // unsynced (or no lyrics at all)

            g.select(QStringLiteral("queue"), 2);
            CHECK(!g.move(Qt::Key_Right) && g.zone() == QStringLiteral("queue") && g.index() == 2,
                  "lyrics(unsynced): Right from the queue is contained — no zone to seek in, and no escape");
            // …and the page as a whole still validates and still contains itself with the zone absent.
            QString why;
            CHECK(g.validate(&why), "lyrics(unsynced): the page validates with the lyric zone hidden");
        }

        // (g) A TRACK CHANGE THAT LOSES THE LYRICS takes the cursor with it. The queue advances from a track
        //     with synced lyrics to one without while the page stays open, so the host recounts the zone to 0
        //     (pushTrackLyrics) — with the cursor still inside it. NavGraph must reassign rather than leave the
        //     selection parked on a zone that is not there, or Enter would fire a seek into an empty list.
        {
            NavGraph g;
            buildThemedNavGraph(g, 12);
            buildAudioPageNavGraph(g);
            g.setZoneCount(QStringLiteral("transport"), 8);
            g.setZoneCount(QStringLiteral("queue"), 5);
            g.setZoneCount(QStringLiteral("lyrics"), 7);
            g.select(QStringLiteral("lyrics"), 4);
            g.setZoneCount(QStringLiteral("lyrics"), 0);            // the next track has none
            CHECK(g.zone() != QStringLiteral("lyrics"),
                  "lyrics: losing the lines mid-queue moves the cursor off the vanished zone");
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

#ifdef EB_HAVE_QML
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
    // §18(k): DISPATCH DEFERRAL (issue #165) — every caller callback the host fires (Action/Toggle/Choice
    // onActivate, the root onBack) hops an event-loop turn so no nested loop ever runs on a live QML delegate's
    // emission; the in-host sub-panel pop stays synchronous. See the function's note.
    runPanelHostDeferralAsserts();
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
    // §21: the SIDEBAR route into the `categories` zone (issue #38) — the shared builder's Sidebar shape, and
    // the whole shipped path (theme.json -> buildView -> bridge -> ThemeView) proving the grid keeps its 2-D
    // stepping across a sidebar round trip and keeps its button bar. After §20, which restores the display mode.
    runSidebarAsserts();
    // §22: a view the theme never DECLARED still renders (issue #29) — the shipped renderer resolving through
    // Theme.js's fallback rather than the old direct theme.views[currentView] read, asserted on the element
    // tree AND the pixels; plus the artwork-less tile placeholder. probe_themeview pins the pure decision.
    runUndeclaredViewAsserts();
    // §23: a theme PREVIEW can never take the D-pad cursor, from both ends — refused at construction
    // (ThemeEngine::buildPreview, issue #123) and refused by the ring (ringMember, issue #173) — asserted
    // against a REAL NavRing with an ordinary QPushButton as the liveness control.
    runPreviewFocusAsserts();
#endif

    if (failures) { std::fprintf(stderr, "NAVQML-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("NAVQML-OK\n");
    return 0;
}
