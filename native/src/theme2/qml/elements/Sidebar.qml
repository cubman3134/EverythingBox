// Sidebar element — a VERTICAL list of the host's `categories`, the non-XMB way to render (and navigate) that
// zone. Until this existed, `Xmb.qml` was the ONLY file in the element set that read `categories`: every other
// content element renders ctx.items, so a theme wanting a persistent category rail beside a grid had to declare
// an `xmb` element for it, which brought the whole cross with it. Declaring a `sidebar` instead gives a theme
// the categories zone on its own terms — the model registers that zone Vertical with Left-in/Right-out edges
// (CategoriesNav::Sidebar, NavThemeGraph.h) and ThemeView routes arrows to it, so the grid keeps its own 2-D
// Left/Right stepping and the bottom `buttons` bar stays live.
//
// Reads off the host (the ThemeView root), like Xmb does: categories, catIndex, and navZone (for the focus
// ring). Mouse parity is free — clicking a row calls host.gotoCat(i), which lives in ThemeView, not here.
//
// Knobs (all optional; the defaults render a plain dark rail):
//   fill / radius / border / borderWidth  — the panel behind the rows
//   rowHeight (fraction of the view height, default 0.072), rowSpacing (fraction, default 0.004)
//   fontSize (fraction of the view height, default 0.026), fontFamily, bold
//   color            — a resting row's label
//   selectedColor    — the selected row's label
//   selectedBg       — the selected row's plate while the sidebar does NOT have focus
//   focusBg          — the selected row's plate while the sidebar HAS focus (this is the focus ring)
//   accentBar (default true) — a bar down the selected row's leading edge, tinted by the row's own `accent`
//   title            — an optional heading above the list
//   titleColor / titleSize (fraction of the view height, default 0.022)
//   showIcons (default true) — draw a row's `icon` image / `accent` swatch before its title
import QtQuick
import "../Theme.js" as T

Item {
    id: sb
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property var cats: (host && host.categories) ? host.categories : []
    readonly property int catIndex: host ? host.catIndex : 0
    // The sidebar draws a FOCUS ring only when the cursor is actually in the categories zone; the selected row
    // stays marked (dimmer) when focus is out in the grid, so the current section is always readable.
    readonly property bool focused: !!(host && host.navZone === "categories")
    readonly property real vh: host ? host.height : 720
    readonly property real ffs: (typeof form !== "undefined" && form) ? form.uiScale : 1

    readonly property real rowH: Math.max(18, Number(T.val(el, "rowHeight", 0.072)) * vh * ffs)
    readonly property real rowGap: Number(T.val(el, "rowSpacing", 0.004)) * vh
    readonly property string headingText: T.val(el, "title", "")
    readonly property real headingSize: Number(T.val(el, "titleSize", 0.022)) * vh * ffs

    Rectangle {   // the rail panel
        anchors.fill: parent
        color: T.val(sb.el, "fill", "#00000000")
        radius: Number(T.val(sb.el, "radius", 0))
        border.width: Number(T.val(sb.el, "borderWidth", 0))
        border.color: T.val(sb.el, "border", "#00000000")
    }

    Text {
        id: heading
        visible: sb.headingText !== ""
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.leftMargin: sb.rowH * 0.32; anchors.rightMargin: sb.rowH * 0.32
        anchors.topMargin: sb.rowH * 0.28
        height: visible ? sb.headingSize * 1.6 : 0
        text: sb.headingText
        color: T.val(sb.el, "titleColor", T.val(sb.el, "color", "#8A93A5"))
        font.family: T.val(sb.el, "fontFamily", Qt.application.font.family)
        font.pixelSize: Math.max(1, sb.headingSize)
        font.bold: true
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    // A ListView (not a Column) so a long category list scrolls instead of overflowing the rail. Selection is
    // keyboard/controller-driven through the nav model — this only follows it — so the view is not interactive
    // on desktop/TV; mobile gets the native kinetic flick, matching Grid.qml's rule.
    ListView {
        id: lv
        objectName: "themeSidebar"          // probe_navqml introspects the rendered rail through this
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.top: heading.visible ? heading.bottom : parent.top
        anchors.topMargin: heading.visible ? sb.rowH * 0.2 : sb.rowH * 0.28
        clip: true
        interactive: (typeof form !== "undefined" && form) ? form.mode === "mobile" : false
        model: sb.cats
        currentIndex: sb.catIndex
        highlightMoveDuration: 0
        spacing: sb.rowGap
        // Keep the selected row on screen as the cursor walks a list longer than the rail.
        preferredHighlightBegin: sb.rowH; preferredHighlightEnd: height - sb.rowH
        highlightRangeMode: ListView.ApplyRange

        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool sel: index === sb.catIndex
            readonly property string rowTitle: (modelData && modelData.title) ? modelData.title
                                             : (modelData ? String(modelData) : "")
            readonly property string rowIcon: (modelData && modelData.icon) ? modelData.icon : ""
            readonly property string rowAccent: (modelData && modelData.accent) ? modelData.accent : "#5B6470"
            width: lv.width
            height: sb.rowH

            Rectangle {
                id: plate
                anchors.fill: parent
                anchors.leftMargin: sb.rowH * 0.12; anchors.rightMargin: sb.rowH * 0.12
                radius: Number(T.val(sb.el, "rowRadius", 6))
                color: !row.sel ? "#00000000"
                     : sb.focused ? T.val(sb.el, "focusBg", "#2F6FD0")
                                  : T.val(sb.el, "selectedBg", "#23272F")
                Behavior on color { ColorAnimation { duration: 120 } }
            }
            Rectangle {   // leading accent bar on the selected row
                visible: row.sel && T.val(sb.el, "accentBar", true) !== false
                anchors.left: plate.left; anchors.top: plate.top; anchors.bottom: plate.bottom
                width: Math.max(2, sb.rowH * 0.07)
                radius: width / 2
                color: row.rowAccent
            }
            Rectangle {   // the row's colour swatch (or its icon, below)
                id: swatch
                visible: T.val(sb.el, "showIcons", true) !== false && row.rowIcon === ""
                anchors.left: plate.left; anchors.leftMargin: sb.rowH * 0.34
                anchors.verticalCenter: parent.verticalCenter
                width: sb.rowH * 0.30; height: width; radius: width * 0.25
                color: row.rowAccent
                opacity: row.sel ? 1.0 : 0.75
            }
            Image {
                id: rowIconImg
                visible: T.val(sb.el, "showIcons", true) !== false && row.rowIcon !== ""
                anchors.left: plate.left; anchors.leftMargin: sb.rowH * 0.34
                anchors.verticalCenter: parent.verticalCenter
                width: sb.rowH * 0.42; height: width
                source: (row.rowIcon !== "" && sb.host) ? sb.host.resolve(row.rowIcon) : ""
                fillMode: Image.PreserveAspectFit; smooth: true
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: (swatch.visible || rowIconImg.visible)
                              ? (swatch.visible ? swatch.right : rowIconImg.right) : plate.left
                anchors.leftMargin: (swatch.visible || rowIconImg.visible) ? sb.rowH * 0.28 : sb.rowH * 0.34
                anchors.right: plate.right; anchors.rightMargin: sb.rowH * 0.24
                text: row.rowTitle
                color: row.sel ? T.val(sb.el, "selectedColor", "#FFFFFF") : T.val(sb.el, "color", "#B7C0D0")
                font.family: T.val(sb.el, "fontFamily", Qt.application.font.family)
                font.pixelSize: Math.max(1, Number(T.val(sb.el, "fontSize", 0.026)) * sb.vh * sb.ffs)
                font.bold: row.sel || sb.el.bold === true
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            MouseArea {   // click a row to switch to that category (host.gotoCat lives in ThemeView)
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: if (sb.host && sb.host.gotoCat) sb.host.gotoCat(row.index)
            }
        }
    }
}
