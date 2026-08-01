// ThemePicker — the themed theme-chooser surface rendered by ThemePickerHost, used BOTH as the forced
// first-run step and from Appearance. A list of installed themes on the left; the right pane is filled by the
// host with a real ThemeEngine::buildView of the highlighted theme, so the preview IS the renderer and a
// community theme previews for free.
//
// It reads two context properties the host installs: `nav` (a Vertical `themeRows` zone — the ONLY zone this
// surface registers) and `picker` (title + resolved settingsPanel style + the theme display names + the
// per-row form-factor verdict + whether Back is allowed). Style resolution and the form-factor tokens follow
// elements/SettingsPanel.qml verbatim.
//
// EVERY installed theme is listed, always. A theme that does not declare support for this device gets a note
// under its name (issue #32) and is otherwise a perfectly ordinary, selectable row — the list is never
// filtered or reordered by the verdict. See ThemePickerHost::present() for why.
//
// THE PREVIEW IS NOT A NAV ZONE. It is a live QQuickWidget parented in by the host with Qt::NoFocus, and it
// is registered in no zone: a focusable live view inside a nav surface takes the cursor and strands the user
// in a preview they cannot leave.
import QtQuick

Rectangle {
    id: root
    focus: true

    // Form-factor tokens (subsystem D) — identical resolution to elements/SettingsPanel.qml, typeof-guarded so
    // a fixture loaded without `form`/`safeArea` still renders. Desktop is IDENTITY (ffs 1, density 1, frac 0).
    readonly property real ffs:     (typeof form !== "undefined" && form) ? form.uiScale : 1
    readonly property real density: (typeof form !== "undefined" && form) ? form.density : 1
    readonly property int  safeInset: Math.round(Math.min(width, height) * ((typeof form !== "undefined" && form) ? form.safeAreaFrac : 0))
    readonly property real safeTop:    (typeof safeArea !== "undefined" && safeArea) ? safeArea.top : 0
    readonly property real safeBottom: (typeof safeArea !== "undefined" && safeArea) ? safeArea.bottom : 0

    readonly property var g:  (typeof nav !== "undefined") ? nav : null
    readonly property var st: (typeof picker !== "undefined" && picker && picker.style) ? picker.style : ({})
    function col(key, def) { return (st && st[key] !== undefined && st[key] !== "") ? st[key] : def }

    readonly property color cBg:     col("background",  "#0F1216")
    readonly property color cPanel:  col("panel",       "#161A20")
    readonly property color cRow:    col("row",         "#1A1F27")
    readonly property color cRowSel: col("rowSelected", "#243244")
    readonly property color cAccent: col("accent",      "#3A6FB0")
    readonly property color cText:   col("text",        "#E6ECF3")
    readonly property color cDim:    col("dim",         "#9AA6B2")
    readonly property color cWarn:   col("warning",     "#E0574E")

    color: cBg

    readonly property var names: (typeof picker !== "undefined" && picker) ? picker.names : []
    // Index-aligned with `names`; see ThemePickerHost.h. Defaulted to [] so a fixture loaded without `picker`
    // still renders — the delegate falls back to "no note", which is exactly the undecorated row.
    readonly property var fitKinds: (typeof picker !== "undefined" && picker) ? picker.fitKinds : []
    readonly property var fitNotes: (typeof picker !== "undefined" && picker) ? picker.fitNotes : []
    // The live cursor. NavGraph exposes the selection as (zone, index) — there is no per-zone index getter, and
    // this surface registers exactly ONE zone, so "the themeRows index" IS nav.index whenever nav.zone is ours.
    readonly property int sel: (g && g.zone === "themeRows") ? g.index : 0

    // The host reads these to know where to place the preview QQuickWidget (scene coords == widget coords: the
    // host sizes the QQuickWidget to its own rect, so the QML scene and the host share an origin).
    property alias previewX: previewSlot.x
    property alias previewY: previewSlot.y
    property alias previewW: previewSlot.width
    property alias previewH: previewSlot.height

    // Route every nav key through the shared graph (host-driven; there are no focus-grabbing child editors).
    Keys.onPressed: function(e) {
        if (!g) return
        if (e.key === Qt.Key_Up)          { g.move(Qt.Key_Up);    e.accepted = true }
        else if (e.key === Qt.Key_Down)   { g.move(Qt.Key_Down);  e.accepted = true }
        else if (e.key === Qt.Key_Left)   { g.move(Qt.Key_Left);  e.accepted = true }
        else if (e.key === Qt.Key_Right)  { g.move(Qt.Key_Right); e.accepted = true }
        else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { g.activate(); e.accepted = true }
        else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_Backspace) { g.back(); e.accepted = true }
    }

    Text {
        id: heading
        x: Math.round(28 * ffs) + safeInset
        y: Math.round(22 * ffs) + safeInset + safeTop
        text: (typeof picker !== "undefined" && picker) ? picker.title : ""
        color: cText
        font.pixelSize: Math.round(26 * ffs)
        font.bold: true
    }

    Text {
        id: hint
        anchors.left: heading.left
        anchors.top: heading.bottom
        anchors.topMargin: Math.round(6 * ffs)
        text: (typeof picker !== "undefined" && picker && picker.mustChoose)
              ? qsTr("Pick a look for this profile. You can change it later in Settings ▸ Appearance.")
              : qsTr("Previews live. Press Enter to keep the highlighted theme.")
        color: cDim
        font.pixelSize: Math.round(14 * ffs)
    }

    Rectangle {
        id: listPane
        x: heading.x
        anchors.top: hint.bottom
        anchors.topMargin: Math.round(18 * ffs)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.round(28 * ffs) + safeInset + safeBottom
        width: Math.round(300 * ffs)
        color: cPanel
        radius: Math.round(10 * ffs)

        ListView {
            id: list
            objectName: "themeRowsList"
            anchors.fill: parent
            anchors.margins: Math.round(8 * ffs)
            clip: true
            spacing: Math.round(6 * ffs)
            model: root.names
            currentIndex: root.sel
            highlightFollowsCurrentItem: true
            // Keep the cursor visible when the list is longer than the pane (a user with many community
            // themes) — the same reason NavMenu had to learn to scroll.
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

            delegate: Rectangle {
                id: row
                required property int index
                required property string modelData
                // The form-factor verdict for this row (issue #32). Guarded by length: `names` and the two fit
                // lists are filled in one pass by the host, but a QML property update is not atomic across
                // three properties, so a mid-update binding can legitimately see the new names and the old fits.
                readonly property string fitKind: (index < root.fitKinds.length) ? root.fitKinds[index] : ""
                readonly property string fitNote: (index < root.fitNotes.length) ? root.fitNotes[index] : ""
                width: list.width
                // A fitting theme's row is EXACTLY the height it always was — the note is additive, so a list of
                // themes that all fit looks untouched.
                height: Math.round((fitNote === "" ? 46 : 60) * root.ffs * root.density)
                radius: Math.round(7 * root.ffs)
                color: index === root.sel ? root.cRowSel : root.cRow
                border.width: index === root.sel ? Math.max(1, Math.round(2 * root.ffs)) : 0
                border.color: root.cAccent

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: Math.round(14 * root.ffs)
                    anchors.right: parent.right
                    anchors.rightMargin: Math.round(10 * root.ffs)
                    spacing: Math.round(3 * root.ffs)

                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: row.modelData
                        color: root.cText
                        font.pixelSize: Math.round(15 * root.ffs)
                    }

                    // The note. A Column skips invisible children entirely, so a fitting row lays out as the
                    // single line it was before. "unsupported" (the author listed devices and not this one)
                    // takes the theme's warning colour; "undeclared" (nobody said) is merely dim — flagging an
                    // absent declaration as loudly as a refused one is what would train people to ignore both.
                    Text {
                        width: parent.width
                        visible: row.fitNote !== ""
                        elide: Text.ElideRight
                        text: row.fitNote
                        color: row.fitKind === "unsupported" ? root.cWarn : root.cDim
                        font.pixelSize: Math.round(12 * root.ffs)
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    // NavGraph::activate() takes no argument (it fires on the LIVE selection), so a
                    // double-click commits by selecting first and then activating — the same two-step the
                    // SettingsPanel Back affordance uses.
                    onClicked: if (root.g) root.g.select("themeRows", index)
                    onDoubleClicked: if (root.g) { root.g.select("themeRows", index); root.g.activate() }
                }
            }
        }
    }

    // The slot the host fills with the live preview widget. Deliberately an empty Item: nothing in QML draws
    // the preview, because the preview is a real QQuickWidget the host parents over this rectangle.
    //
    // The slot PUSHES its geometry at the host (picker.slotMoved) instead of letting the host poll: an anchor
    // chain resolves on the scene's polish pass, so a C++ read taken right after setGeometry can legitimately
    // see a stale/zero rect. Pushing from the change handler means the host reads previewX/Y/W/H exactly when
    // they are settled.
    Item {
        id: previewSlot
        anchors.left: listPane.right
        anchors.leftMargin: Math.round(20 * ffs)
        anchors.top: listPane.top
        anchors.bottom: listPane.bottom
        anchors.right: parent.right
        anchors.rightMargin: Math.round(28 * ffs) + safeInset

        function report() { if (typeof picker !== "undefined" && picker) picker.slotMoved() }
        onXChanged:      report()
        onYChanged:      report()
        onWidthChanged:  report()
        onHeightChanged: report()
        Component.onCompleted: report()
    }
}
