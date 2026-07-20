// ThemedChoice — the two-state sibling of ThemedTextField for a pick-one-from-a-list field on nav-driven
// (TV / controller) surfaces. Same shared-NavGraph contract:
//
//   * SELECTED — nav.zone === navZone: the theme accent outline + slight scale (a highlighted row).
//   * EDITING  — entered when nav ACTIVATES our zone. TWO modes, host-chosen:
//       - externalEdit: false (default, inline): the option list is "open" in place. Up/Left and Down/Right
//         move a PENDING highlight over `options` (they DON'T move the nav selection — the FocusScope holds
//         focus and swallows them), Enter commits (currentOption := pending, chosen(index) once), Escape
//         reverts without committing. editRequested is NOT emitted — the inline flow is self-contained.
//       - externalEdit: true (TV / richer picker): activation emits editRequested(navZone) and goes PENDING
//         only — no inline list, no focus grab, arrows still move the nav selection. The host runs its picker,
//         writes `currentOption` back, then calls finishEdit(true) to emit chosen(currentOption) once (or
//         finishEdit(false) to abandon). Either way the component returns to plain selected.
//
// Self-registers as a single-count nav zone on Component.onCompleted via nav.registerZoneQml and DEregisters
// on Component.onDestruction via nav.removeZone (no phantom zones after a Loader unload) — the same QML
// zone-lifecycle path subsystem B reuses. Ships unused by production screens; B is the first consumer.
import QtQuick

FocusScope {
    id: tc

    // ---- contract API (subsystem B binds these) ----------------------------------------------------------
    property string navZone: ""
    property int    navRow: 0
    property int    navCol: 0
    property var    options: []
    property int    currentOption: 0
    property color  accent: "#3A6FB0"
    // true = the HOST owns the picker: activation only emits editRequested + goes pending; the host writes
    // `currentOption` and calls finishEdit(committed). false = self-contained inline list (no editRequested).
    property bool   externalEdit: false

    readonly property bool selected: (typeof nav !== "undefined") && nav && nav.zone === navZone
    property bool editing: false          // the inline list is open (externalEdit: false path)
    property bool externalPending: false  // an external pick is in flight (host owes a finishEdit call)
    property int  pending: 0              // the highlighted option while the inline list is open

    signal chosen(int index)
    signal editRequested(string zone)

    implicitWidth: 240
    implicitHeight: 40
    width: implicitWidth
    height: implicitHeight

    Component.onCompleted: if (typeof nav !== "undefined" && nav) nav.registerZoneQml(navZone, 1, navRow, navCol)
    // Deregister so a dynamically unloaded choice never leaves a phantom zone (see the header note).
    Component.onDestruction: if (typeof nav !== "undefined" && nav) nav.removeZone(navZone)

    Connections {
        target: (typeof nav !== "undefined") ? nav : null
        function onActivated(zone, index) { if (zone === tc.navZone) tc.beginEdit() }
    }

    function beginEdit() {
        // Empty-options guard (B2 Task 6 hardening): a Choice row with no options has nothing to pick, so
        // activation is a no-op — never open an empty inline list (pending would clamp to -1, arrows would
        // wedge) nor emit an editRequested the host can't service. A late-populated `options` re-enables it.
        if (!options || options.length === 0) return
        if (externalEdit) {                 // the HOST picks — we only signal + go pending.
            if (externalPending) return     // one outstanding request at a time
            externalPending = true
            editRequested(navZone)
            return                          // no inline list, no focus grab: arrows keep moving the nav selection
        }
        editing = true
        pending = currentOption
        forceActiveFocus()
    }
    // The host's return leg (external mode): it wrote `currentOption` (on commit) and tells us whether to
    // fire chosen(). Also usable as a programmatic Enter/Escape for the inline list.
    function finishEdit(commitOk) {
        if (editing) { if (commitOk) commit(); else cancel(); return }
        if (!externalPending) return
        externalPending = false
        if (commitOk) chosen(currentOption)
    }
    function commit() {
        if (!editing) return
        editing = false
        currentOption = pending
        chosen(currentOption)
        forceActiveFocus()
    }
    function cancel() {
        if (!editing) return
        editing = false
        pending = currentOption
        forceActiveFocus()
    }

    // While editing the FocusScope holds focus, so the arrow keys land HERE (moving the pending highlight) and
    // are accepted — they never reach the host's nav router. When NOT editing we ignore them so they bubble up
    // to the host, which drives nav.move() as usual.
    Keys.onPressed: (event) => {
        if (!tc.editing) return
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Left) {
            tc.pending = Math.max(0, tc.pending - 1); event.accepted = true
        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Right) {
            tc.pending = Math.min((tc.options ? tc.options.length : 1) - 1, tc.pending + 1); event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            tc.commit(); event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            tc.cancel(); event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: "#141A22"
        border.width: (tc.selected || tc.editing || tc.externalPending) ? 2 : 1
        border.color: (tc.editing || tc.externalPending) ? Qt.lighter(tc.accent, 1.3)
                    : tc.selected ? tc.accent
                    : "#2A2E36"
        scale: (tc.selected && !tc.editing) ? 1.03 : 1.0
        Behavior on scale { NumberAnimation { duration: 120 } }

        Text {
            anchors.left: parent.left; anchors.right: caret.left; anchors.margins: 8
            anchors.verticalCenter: parent.verticalCenter
            text: {
                var i = tc.editing ? tc.pending : tc.currentOption
                return (tc.options && i >= 0 && i < tc.options.length) ? String(tc.options[i]) : ""
            }
            color: tc.editing ? "#FFFFFF" : "#E6ECF3"; elide: Text.ElideRight
        }
        Text {                          // ▾ closed / ▲▼ open — a cheap "the list is open" affordance
            id: caret
            anchors.right: parent.right; anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: tc.editing ? "▲▼" : "▾"
            color: "#6B7480"; font.pixelSize: 12
        }
    }

    MouseArea {
        anchors.fill: parent; enabled: !tc.editing
        onClicked: { if (typeof nav !== "undefined" && nav) nav.select(tc.navZone, 0); tc.beginEdit() }
    }
}
