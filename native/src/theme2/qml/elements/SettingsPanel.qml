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

    // LogView scroll-mode (Debug log): activating a LogView row enters a read-only scroll state where Up/Down
    // scroll the text and Esc/Enter return to row selection — NavTextField's read-only two-state semantics lifted
    // to the panel level. The single `panelRows` zone can't own per-row modes, so the state lives here and the
    // active LogView delegate scrolls in response to `scrollLog`.
    property bool logScroll: false
    signal scrollLog(int delta)
    // The currently-selected row's kind — the selected delegate (always realized: it's the ListView currentItem)
    // publishes it, so the root can tell whether Enter should enter LogView scroll mode without reaching into the
    // model by index.
    property int selRowKind: -1
    readonly property bool onLogRow: g && g.zone === "panelRows" && selRowKind === kLogView
    // Leaving scroll mode whenever the selection moves off the log row keeps the state honest.
    onOnLogRowChanged: if (!onLogRow) logScroll = false

    // Route every nav key through the shared graph (host-driven; no focus-grabbing per-row editors exist).
    Keys.onPressed: function(e) {
        if (!g) return
        // Scroll mode: keys drive the log, not the cursor.
        if (root.logScroll) {
            if (e.key === Qt.Key_Up)         { root.scrollLog(-48); e.accepted = true }
            else if (e.key === Qt.Key_Down)  { root.scrollLog(48);  e.accepted = true }
            else if (e.key === Qt.Key_PageUp)   { root.scrollLog(-260); e.accepted = true }
            else if (e.key === Qt.Key_PageDown) { root.scrollLog(260);  e.accepted = true }
            else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_Backspace
                     || e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { root.logScroll = false; e.accepted = true }
            return
        }
        // Enter on a LogView row enters scroll mode (host-side activation is a no-op for LogView).
        if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && root.onLogRow) {
            root.logScroll = true; e.accepted = true; return
        }
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
            readonly property bool isLog: kind === root.kLogView
            readonly property bool sel: root.g && root.g.zone === "panelRows" && root.g.index === index
            readonly property bool destructive: rowData && rowData.destructive === true
            readonly property bool dim: rowData && rowData.enabled === false
            readonly property bool scrolling: isLog && sel && root.logScroll
            height: isSep ? 40 : (isLog ? 320 : 56)

            // Publish the selected row's kind to the root (only the sel==true delegate writes, so it's unambiguous).
            onSelChanged: if (sel) root.selRowKind = kind
            Component.onCompleted: if (sel) root.selRowKind = kind

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
                border.width: (del.sel || del.scrolling) ? 2 : 1
                border.color: del.scrolling ? Qt.lighter(root.cAccent, 1.4)
                                            : (del.sel ? root.cAccent : Qt.darker(root.cRow, 1.25))
                opacity: del.dim ? 0.5 : 1.0

                Text {   // left label (log rows anchor it to the top instead of centring)
                    id: lbl
                    anchors.left: parent.left; anchors.leftMargin: 18
                    anchors.right: rightSide.left; anchors.rightMargin: 12
                    anchors.verticalCenter: del.isLog ? undefined : parent.verticalCenter
                    anchors.top: del.isLog ? parent.top : undefined
                    anchors.topMargin: del.isLog ? 12 : 0
                    text: del.isLog
                          ? (del.rowData ? del.rowData.label : "") + (del.scrolling ? "   — scrolling (Esc to exit)"
                                                                                     : (del.sel ? "   — Enter to scroll" : ""))
                          : (del.rowData ? del.rowData.label : "")
                    color: del.destructive ? root.cWarn : (del.isLog ? root.cDim : root.cText)
                    font.pixelSize: del.isLog ? 13 : 17
                    font.bold: del.isLog; elide: Text.ElideRight
                }

                // LogView: a scrollable, read-only monospace tail of the log. Up/Down scroll it in scroll mode
                // (root.scrollLog), and the wheel/drag scroll it directly.
                Flickable {
                    id: logFlick
                    visible: del.isLog
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.top: lbl.bottom; anchors.bottom: parent.bottom
                    anchors.leftMargin: 16; anchors.rightMargin: 12
                    anchors.topMargin: 6; anchors.bottomMargin: 12
                    clip: true
                    contentWidth: logText.paintedWidth; contentHeight: logText.paintedHeight
                    boundsBehavior: Flickable.StopAtBounds
                    Connections {
                        target: root
                        enabled: del.scrolling
                        function onScrollLog(delta) {
                            var maxY = Math.max(0, logFlick.contentHeight - logFlick.height)
                            logFlick.contentY = Math.max(0, Math.min(maxY, logFlick.contentY + delta))
                        }
                    }
                    Text {
                        id: logText
                        text: del.rowData ? del.rowData.value : ""
                        color: root.cText
                        font.family: "Consolas, 'Courier New', monospace"; font.pixelSize: 12
                        textFormat: Text.PlainText
                    }
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

                        // TextField: current text (masked to dots for credentials), or a dim "—" when empty.
                        Text {
                            visible: del.kind === root.kTextField
                            anchors.verticalCenter: parent.verticalCenter
                            readonly property bool has: del.rowData && del.rowData.value !== ""
                            readonly property bool masked: del.rowData && del.rowData.masked === true
                            text: !has ? "—"
                                       : (masked ? "•".repeat(Math.min(24, String(del.rowData.value).length))
                                                 : del.rowData.value)
                            color: has ? root.cText : root.cDim
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

                // Row activation (not for LogView — its Flickable owns pointer input natively: wheel + drag scroll
                // it regardless of keyboard scroll-mode, so it needs no activate MouseArea).
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    enabled: !del.dim && !del.isLog
                    onClicked: { if (root.g) { root.g.select("panelRows", del.index); root.g.activate() } }
                }
            }
        }
    }
}
