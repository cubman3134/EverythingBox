// ThemedChoice — the two-state sibling of ThemedTextField for a pick-one-from-a-list field on nav-driven
// (TV / controller) surfaces. Same shared-NavGraph contract:
//
//   * SELECTED — nav.zone === navZone: the theme accent outline + slight scale (a highlighted row).
//   * EDITING  — the option list is "open" inline. Entered when nav ACTIVATES our zone; we also emit
//                editRequested(navZone) so a host could raise a richer picker. While open, Up/Left and
//                Down/Right move a PENDING highlight over `options` (they DON'T move the nav selection — the
//                FocusScope holds focus and swallows them), Enter commits (currentOption := pending, chosen),
//                Escape reverts to the previous option without committing.
//
// Self-registers as a single-count nav zone on completion via nav.registerZoneQml — the same QML registration
// path subsystem B reuses. Ships unused by production screens; B is the first consumer.
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

    readonly property bool selected: (typeof nav !== "undefined") && nav && nav.zone === navZone
    property bool editing: false
    property int  pending: 0           // the highlighted option while the list is open

    signal chosen(int index)
    signal editRequested(string zone)

    implicitWidth: 240
    implicitHeight: 40
    width: implicitWidth
    height: implicitHeight

    Component.onCompleted: if (typeof nav !== "undefined" && nav) nav.registerZoneQml(navZone, 1, navRow, navCol)

    Connections {
        target: (typeof nav !== "undefined") ? nav : null
        function onActivated(zone, index) { if (zone === tc.navZone) tc.beginEdit() }
    }

    function beginEdit() {
        editing = true
        pending = currentOption
        editRequested(navZone)
        forceActiveFocus()
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
        border.width: (tc.selected || tc.editing) ? 2 : 1
        border.color: tc.editing ? Qt.lighter(tc.accent, 1.3)
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
