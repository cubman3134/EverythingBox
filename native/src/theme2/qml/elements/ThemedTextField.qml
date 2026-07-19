// ThemedTextField — a two-state themed text input for nav-driven (TV / controller) surfaces. It reflects the
// shared NavGraph selection exposed as the `nav` context property (the same model every themed screen uses):
//
//   * SELECTED  — nav.zone === navZone: draws the theme's accent outline (the Xmb selection accent) + a small
//                 scale, exactly like a highlighted XMB row.
//   * EDITING   — entered when nav ACTIVATES our zone (Enter on the selection). TWO modes, host-chosen:
//       - externalEdit: false (default, inline): the inline TextInput takes focus and edits in place. Enter
//         commits once (committed(text), back to selected); Escape reverts WITHOUT committing. While editing
//         the TextInput holds focus and swallows the arrows, so the host's nav router never sees them and the
//         selection can't move off the field ("keys stay in the field"). editRequested is NOT emitted — the
//         inline flow is self-contained.
//       - externalEdit: true (TV / OSK): activation emits editRequested(navZone) and enters a PENDING visual
//         state only — no inline editor, no focus grab, arrows still move the nav selection. The host runs its
//         editor (Osk::getText), writes `text` back, then calls finishEdit(true) to emit committed(text) once
//         (or finishEdit(false) to abandon). Either way the component returns to plain selected.
//
// Each instance self-registers as a single-count nav zone on Component.onCompleted via nav.registerZoneQml and
// DEregisters on Component.onDestruction via nav.removeZone — a Loader unload / Repeater shrink never leaves a
// phantom zone the graph could land on. This is the QML zone-lifecycle path subsystem B's themed surfaces
// reuse wholesale. Ships unused by production screens; B is the first consumer.
import QtQuick

FocusScope {
    id: tf

    // ---- contract API (subsystem B binds these) ----------------------------------------------------------
    property string navZone: ""
    property int    navRow: 0
    property int    navCol: 0
    property string text: ""
    property string placeholder: ""
    // The theme accent for the selection outline — the exact value the Xmb inline chooser uses for its border.
    property color  accent: "#3A6FB0"
    // true = the HOST owns the editor (TV / OSK): activation only emits editRequested + goes pending; the host
    // writes `text` and calls finishEdit(committed). false = self-contained inline editing (no editRequested).
    property bool   externalEdit: false

    readonly property bool selected: (typeof nav !== "undefined") && nav && nav.zone === navZone
    property bool editing: false          // the inline editor is live (externalEdit: false path)
    property bool externalPending: false  // an external edit is in flight (host owes a finishEdit call)

    signal committed(string text)
    signal editRequested(string zone)

    implicitWidth: 240
    implicitHeight: 40
    width: implicitWidth
    height: implicitHeight

    // Self-register as a 1-count zone (row/col place it on the nav grid). registerZoneQml is the QML-facing
    // overload (defaults the axis to Horizontal) — the C++ Qt::Orientation arg doesn't marshal from QML.
    Component.onCompleted: if (typeof nav !== "undefined" && nav) nav.registerZoneQml(navZone, 1, navRow, navCol)
    // Deregister so a dynamically unloaded field never leaves a phantom zone (see the header note).
    Component.onDestruction: if (typeof nav !== "undefined" && nav) nav.removeZone(navZone)

    // Activation on OUR zone enters editing (inline) or requests the host's editor (externalEdit).
    Connections {
        target: (typeof nav !== "undefined") ? nav : null
        function onActivated(zone, index) { if (zone === tf.navZone) tf.beginEdit() }
    }

    function beginEdit() {
        if (externalEdit) {                 // TV / OSK: the HOST edits — we only signal + go pending.
            if (externalPending) return     // one outstanding request at a time
            externalPending = true
            editRequested(navZone)
            return                          // no inline editor, no focus grab: arrows keep moving the nav selection
        }
        editing = true
        input.text = tf.text
        input.forceActiveFocus()
        input.selectAll()
    }
    // The host's return leg (external mode): it wrote `text` (on commit) and tells us whether to fire
    // committed(). Also usable as a programmatic Enter/Escape for the inline editor.
    function finishEdit(commitOk) {
        if (editing) { if (commitOk) commit(); else cancel(); return }
        if (!externalPending) return
        externalPending = false
        if (commitOk) committed(tf.text)
    }
    function commit() {
        if (!editing) return
        editing = false
        tf.text = input.text
        committed(tf.text)
        tf.forceActiveFocus()          // hand focus back so the host's arrow routing resumes
    }
    function cancel() {                 // Escape: leave editing, keep the old value, no commit
        if (!editing) return
        editing = false
        input.text = tf.text
        tf.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: "#141A22"
        border.width: (tf.selected || tf.editing || tf.externalPending) ? 2 : 1
        border.color: (tf.editing || tf.externalPending) ? Qt.lighter(tf.accent, 1.3)
                    : tf.selected ? tf.accent
                    : "#2A2E36"
        scale: (tf.selected && !tf.editing) ? 1.03 : 1.0
        Behavior on scale { NumberAnimation { duration: 120 } }

        Text {                          // placeholder (empty + not editing)
            anchors.fill: parent; anchors.margins: 8
            verticalAlignment: Text.AlignVCenter
            visible: !tf.editing && tf.text === ""
            text: tf.placeholder; color: "#6B7480"; elide: Text.ElideRight
        }
        Text {                          // static value (not editing)
            anchors.fill: parent; anchors.margins: 8
            verticalAlignment: Text.AlignVCenter
            visible: !tf.editing && tf.text !== ""
            text: tf.text; color: "#E6ECF3"; elide: Text.ElideRight
        }
        TextInput {
            id: input
            objectName: "tfInput"
            anchors.fill: parent; anchors.margins: 8
            verticalAlignment: Text.AlignVCenter
            visible: tf.editing; enabled: tf.editing
            color: "#FFFFFF"; clip: true; selectByMouse: true
            // Handle the transition keys HERE and accept them, so they never bubble to the host's nav router
            // while editing: Enter commits once (TextInput's own accepted() does NOT stop propagation, which
            // would let the host re-activate us), Escape cancels, and the vertical arrows are swallowed
            // (TextInput already consumes Left/Right/Home/End for the caret). "Keys stay in the field."
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { tf.commit(); event.accepted = true }
                else if (event.key === Qt.Key_Escape) { tf.cancel(); event.accepted = true }
                else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) event.accepted = true
            }
        }
    }

    // Mouse parity: click selects + edits (the host owns controller nav; this keeps the desktop usable).
    MouseArea {
        anchors.fill: parent; enabled: !tf.editing
        onClicked: { if (typeof nav !== "undefined" && nav) nav.select(tf.navZone, 0); tf.beginEdit() }
    }
}
