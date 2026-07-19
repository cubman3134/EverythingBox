// ThemedTextField — a two-state themed text input for nav-driven (TV / controller) surfaces. It reflects the
// shared NavGraph selection exposed as the `nav` context property (the same model every themed screen uses):
//
//   * SELECTED  — nav.zone === navZone: draws the theme's accent outline (the Xmb selection accent) + a small
//                 scale, exactly like a highlighted XMB row.
//   * EDITING   — an inline TextInput is active. Entered when nav ACTIVATES our zone (Enter on the selection);
//                 we ALSO emit editRequested(navZone) so a host may answer on TV by running the on-screen
//                 keyboard (Osk) instead of the inline field — the HOST decides. For headless / inline use the
//                 TextInput edits directly (this is what probe_navqml drives).
//
// Enter commits once (committed(text), back to selected); Escape reverts to selected WITHOUT committing. While
// editing the TextInput holds focus and swallows the arrow keys, so the host's nav router never sees them and
// the selection can't move off the field (the "keys stay in the field" contract). Each instance self-registers
// as a single-count nav zone on completion via nav.registerZoneQml — the QML registration path subsystem B's
// themed surfaces reuse wholesale. Ships unused by production screens; B is the first consumer.
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

    readonly property bool selected: (typeof nav !== "undefined") && nav && nav.zone === navZone
    property bool editing: false

    signal committed(string text)
    signal editRequested(string zone)

    implicitWidth: 240
    implicitHeight: 40
    width: implicitWidth
    height: implicitHeight

    // Self-register as a 1-count zone (row/col place it on the nav grid). registerZoneQml is the QML-facing
    // overload (defaults the axis to Horizontal) — the C++ Qt::Orientation arg doesn't marshal from QML.
    Component.onCompleted: if (typeof nav !== "undefined" && nav) nav.registerZoneQml(navZone, 1, navRow, navCol)

    // Activation on OUR zone enters editing; the host may divert to the OSK by answering editRequested.
    Connections {
        target: (typeof nav !== "undefined") ? nav : null
        function onActivated(zone, index) { if (zone === tf.navZone) tf.beginEdit() }
    }

    function beginEdit() {
        editing = true
        input.text = tf.text
        editRequested(navZone)
        input.forceActiveFocus()
        input.selectAll()
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
        border.width: (tf.selected || tf.editing) ? 2 : 1
        border.color: tf.editing ? Qt.lighter(tf.accent, 1.3)
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
