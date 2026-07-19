// SettingsPanel — the themed-mode settings surface rendered by ThemedPanelHost: a title bar with a Back
// affordance over a ListView of row delegates, driven entirely by the Nav Contract. It reads two context
// properties the host installs: `nav` (the panel's NavGraph — a Vertical `panelRows` zone + a `panelBack`
// header zone, the shape probe_navqml §18 asserts) and `panel` (title + resolved `settingsPanel` style block +
// the ListView model). Every color falls back HARD to a dark default, so a theme without a `settingsPanel`
// block still renders (panels are styled, not per-theme views).
//
// Navigation is host-driven: the root's Keys handler routes arrows/Enter/Back through `nav`; activation lands
// in ThemedPanelHost::onGraphActivated (Action dispatches, Toggle flips, Choice cycles, TextField runs the OSK),
// so the delegates are pure render — no per-row nav zones (the single `panelRows` zone cannot host sub-zones).
import QtQuick

Rectangle {
    id: root
    focus: true

    // Row-kind ints — must match PanelRow::Kind order (PanelModel.h).
    readonly property int kAction: 0
    readonly property int kToggle: 1
    readonly property int kChoice: 2
    readonly property int kTextField: 3
    readonly property int kInfo: 4
    readonly property int kProgress: 5
    readonly property int kSeparator: 6
    readonly property int kLogView: 7

    readonly property var g:  (typeof nav !== "undefined") ? nav : null
    readonly property var st: (typeof panel !== "undefined" && panel && panel.style) ? panel.style : ({})
    function col(key, def) { return (st && st[key] !== undefined && st[key] !== "") ? st[key] : def }

    readonly property color cBg:      col("background",   "#0F1216")
    readonly property color cPanel:   col("panel",        "#161A20")
    readonly property color cRow:     col("row",          "#1A1F27")
    readonly property color cRowSel:  col("rowSelected",  "#243244")
    readonly property color cAccent:  col("accent",       "#3A6FB0")
    readonly property color cText:    col("text",         "#E6ECF3")
    readonly property color cDim:     col("dim",          "#9AA6B2")
    readonly property color cSep:     col("separator",    "#7C8794")
    readonly property color cWarn:    col("warning",      "#E0524E")

    color: cBg

    // Route every nav key through the shared graph (host-driven; no focus-grabbing per-row editors exist).
    Keys.onPressed: function(e) {
        if (!g) return
        if (e.key === Qt.Key_Up)          { g.move(Qt.Key_Up);    e.accepted = true }
        else if (e.key === Qt.Key_Down)   { g.move(Qt.Key_Down);  e.accepted = true }
        else if (e.key === Qt.Key_Left)   { g.move(Qt.Key_Left);  e.accepted = true }
        else if (e.key === Qt.Key_Right)  { g.move(Qt.Key_Right); e.accepted = true }
        else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { g.activate(); e.accepted = true }
        else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_Backspace) { g.back(); e.accepted = true }
    }

    // ---- Title bar: the Back affordance (panelBack zone) + the panel title ---------------------------------
    Rectangle {
        id: header
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: 74
        color: cBg
        readonly property bool sel: root.g && root.g.zone === "panelBack"

        Rectangle {
            id: backBtn
            anchors.left: parent.left; anchors.leftMargin: 28
            anchors.verticalCenter: parent.verticalCenter
            width: 104; height: 42; radius: 9
            color: header.sel ? root.cAccent : root.cRow
            border.width: header.sel ? 2 : 1
            border.color: header.sel ? Qt.lighter(root.cAccent, 1.3) : Qt.darker(root.cRow, 1.2)
            Text {
                anchors.centerIn: parent
                text: "‹ Back"; color: root.cText; font.pixelSize: 16
            }
            MouseArea {
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: { if (root.g) { root.g.select("panelBack", 0); root.g.activate() } }
            }
        }
        Text {
            anchors.left: backBtn.right; anchors.leftMargin: 22
            anchors.verticalCenter: parent.verticalCenter
            text: (typeof panel !== "undefined" && panel) ? panel.title : ""
            color: root.cText; font.pixelSize: 26; font.bold: true
        }
        Rectangle {   // hairline under the header
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            height: 1; color: Qt.darker(root.cPanel, 1.15)
        }
    }

    // ---- The row list --------------------------------------------------------------------------------------
    ListView {
        id: list
        anchors.top: header.bottom; anchors.topMargin: 12
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.leftMargin: 28; anchors.rightMargin: 28; anchors.bottomMargin: 20
        clip: true
        spacing: 8
        interactive: false
        model: (typeof panel !== "undefined" && panel) ? panel.model : null
        currentIndex: (root.g && root.g.zone === "panelRows") ? root.g.index : -1
        onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)

        delegate: Item {
            id: del
            required property int index
            required property var rowData
            width: list.width
            readonly property int kind: rowData ? rowData.kind : root.kAction
            readonly property bool isSep: kind === root.kSeparator
            readonly property bool sel: root.g && root.g.zone === "panelRows" && root.g.index === index
            readonly property bool destructive: rowData && rowData.destructive === true
            readonly property bool dim: rowData && rowData.enabled === false
            height: isSep ? 40 : 56

            // Separator = a section header (no card; dim, spaced label).
            Text {
                visible: del.isSep
                anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.bottomMargin: 6
                text: (del.rowData ? del.rowData.label : "").toUpperCase()
                color: root.cSep; font.pixelSize: 13; font.bold: true; font.letterSpacing: 1.5
            }

            // Every other kind = a card row.
            Rectangle {
                visible: !del.isSep
                anchors.fill: parent
                radius: 10
                color: del.sel ? root.cRowSel : root.cRow
                border.width: del.sel ? 2 : 1
                border.color: del.sel ? root.cAccent : Qt.darker(root.cRow, 1.25)
                opacity: del.dim ? 0.5 : 1.0

                Text {   // left label
                    id: lbl
                    anchors.left: parent.left; anchors.leftMargin: 18
                    anchors.right: rightSide.left; anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: del.rowData ? del.rowData.label : ""
                    color: del.destructive ? root.cWarn : root.cText
                    font.pixelSize: 17; elide: Text.ElideRight
                }

                // Right-side affordance, by kind.
                Item {
                    id: rightSide
                    anchors.right: parent.right; anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: rightRow.width; height: parent.height
                    Row {
                        id: rightRow
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8

                        // Toggle: a pill showing ON/OFF.
                        Rectangle {
                            visible: del.kind === root.kToggle
                            anchors.verticalCenter: parent.verticalCenter
                            width: 58; height: 28; radius: 14
                            readonly property bool on: del.rowData && del.rowData.checked === true
                            color: on ? root.cAccent : Qt.darker(root.cRow, 1.4)
                            border.width: 1; border.color: on ? Qt.lighter(root.cAccent, 1.2) : root.cDim
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                x: parent.on ? 10 : parent.width - width - 10
                                text: parent.on ? "ON" : "OFF"
                                color: parent.on ? root.cText : root.cDim; font.pixelSize: 12; font.bold: true
                            }
                        }

                        // Choice: current option + a ▾ affordance.
                        Text {
                            visible: del.kind === root.kChoice
                            anchors.verticalCenter: parent.verticalCenter
                            text: (del.rowData ? (del.rowData.value + "  ▾") : "")
                            color: root.cAccent; font.pixelSize: 16
                        }

                        // TextField: current text, or a dim "—" when empty.
                        Text {
                            visible: del.kind === root.kTextField
                            anchors.verticalCenter: parent.verticalCenter
                            text: (del.rowData && del.rowData.value !== "") ? del.rowData.value : "—"
                            color: (del.rowData && del.rowData.value !== "") ? root.cText : root.cDim
                            font.pixelSize: 16; elide: Text.ElideRight
                        }

                        // Info: static right value (dim).
                        Text {
                            visible: del.kind === root.kInfo
                            anchors.verticalCenter: parent.verticalCenter
                            text: del.rowData ? del.rowData.value : ""
                            color: root.cDim; font.pixelSize: 16
                        }

                        // Action: a chevron.
                        Text {
                            visible: del.kind === root.kAction || del.kind === root.kLogView
                            anchors.verticalCenter: parent.verticalCenter
                            text: "›"
                            color: root.cDim; font.pixelSize: 20
                        }
                    }
                }

                // Progress: a bar spanning under the label (0..100).
                Rectangle {
                    visible: del.kind === root.kProgress
                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 18
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 8
                    height: 6; radius: 3
                    color: Qt.darker(root.cRow, 1.5)
                    Rectangle {
                        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                        radius: 3
                        readonly property int pct: (del.rowData && del.rowData.progress >= 0) ? del.rowData.progress : 0
                        width: Math.max(0, parent.width * Math.min(100, pct) / 100.0)
                        color: root.cAccent
                    }
                }

                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    enabled: !del.dim
                    onClicked: { if (root.g) { root.g.select("panelRows", del.index); root.g.activate() } }
                }
            }
        }
    }
}
