// XMB element - a PlayStation-style XrossMediaBar. The horizontal axis is the categories (host.categories,
// each = an inherent bucket: Video/Game/Audio/Reading/Settings), the vertical axis is the column the host
// feeds (that bucket's catalogs, or - once you open a catalog - its live items). Both axes slide so the active
// element stays pinned at the "cross". Purely presentational: ThemeView owns the keys (xmb mode) and the host
// (C++) swaps the column. Reads off the host (the ThemeView root): categories, catIndex, items, currentIndex.
//
// Category tiles show, in order of preference: a theme `icon` image, else a drawn glyph for the bucket
// (modelData.glyph: "video"|"audio"|"game"|"reading"|"settings"), else the title's first letter.
// Theme keys: color, subColor, descColor, crossX/crossY, catSpacing, itemSpacing, iconSize (fractions).
import QtQuick
import "../Theme.js" as T

Item {
    id: xmb
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property var cats: host && host.categories ? host.categories : []
    readonly property int catIndex: host ? host.catIndex : 0
    readonly property var items: host && host.items ? host.items : []
    readonly property int itemIndex: host ? host.currentIndex : 0

    readonly property color textColor: T.val(el, "color", "#FFFFFF")
    readonly property color subColor: T.val(el, "subColor", "#C2C7D4")
    readonly property color descColor: T.val(el, "descColor", "#9AA0AE")
    readonly property real crossX: Number(T.val(el, "crossX", 0.20)) * width
    readonly property real crossY: Number(T.val(el, "crossY", 0.30)) * height
    readonly property real catGap: Number(T.val(el, "catSpacing", 0.135)) * width
    readonly property real itemGap: Number(T.val(el, "itemSpacing", 0.082)) * height
    readonly property real iconSize: Number(T.val(el, "iconSize", 0.11)) * height
    // Column tile size is derived from the row spacing (and stays under it even when the selected row scales
    // up 1.12x), so column items never overlap regardless of the theme's iconSize/itemSpacing.
    readonly property real rowSize: itemGap * 0.8
    // The column starts well below the category row + its label so the first item never collides with them.
    readonly property real colTop: crossY + iconSize * 1.7

    // Smoothly slide both axes toward the selection (the PS3 "everything glides to the cross" feel).
    property real catScroll: catIndex
    property real itemScroll: itemIndex
    onCatIndexChanged: catScroll = catIndex
    onItemIndexChanged: itemScroll = itemIndex
    Behavior on catScroll  { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
    Behavior on itemScroll { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

    // ---- horizontal category axis (pinned at crossX, slides left/right) ----------------------------------
    Repeater {
        model: xmb.cats
        delegate: Item {
            id: cat
            required property var modelData
            required property int index
            readonly property bool sel: index === xmb.catIndex
            readonly property real dx: (index - xmb.catScroll) * xmb.catGap
            readonly property string glyph: (modelData && modelData.glyph) ? modelData.glyph : ""
            readonly property string icon: (modelData && modelData.icon) ? modelData.icon : ""
            width: xmb.iconSize; height: xmb.iconSize
            x: xmb.crossX + dx - width / 2
            y: xmb.crossY - height / 2
            opacity: sel ? 1.0 : Math.max(0.18, 0.55 - Math.abs(dx) / xmb.width * 0.6)
            scale: sel ? 1.18 : 0.88
            Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

            Rectangle {
                anchors.fill: parent; radius: width * 0.18
                color: (cat.modelData && cat.modelData.accent) ? cat.modelData.accent : "#2A2E36"
                visible: cat.icon === ""

                // Drawn glyph (static Canvas - software-renderer safe, painted once). White on the accent tile.
                Canvas {
                    anchors.centerIn: parent
                    width: parent.width * 0.58; height: parent.height * 0.58
                    visible: cat.glyph !== ""
                    onPaint: {
                        var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
                        var w = width, h = height
                        c.fillStyle = "#FFFFFF"; c.strokeStyle = "#FFFFFF"
                        c.lineWidth = Math.max(2, w * 0.08); c.lineCap = "round"; c.lineJoin = "round"
                        if (cat.glyph === "video") {                       // play triangle
                            c.beginPath(); c.moveTo(w * 0.28, h * 0.2); c.lineTo(w * 0.8, h * 0.5); c.lineTo(w * 0.28, h * 0.8); c.closePath(); c.fill()
                        } else if (cat.glyph === "audio") {                // musical note
                            c.beginPath(); c.ellipse(w * 0.24, h * 0.62, w * 0.26, h * 0.22); c.fill()
                            c.beginPath(); c.rect(w * 0.46, h * 0.16, w * 0.08, h * 0.52); c.fill()
                            c.beginPath(); c.moveTo(w * 0.5, h * 0.16); c.quadraticCurveTo(w * 0.86, h * 0.22, w * 0.74, h * 0.42); c.lineTo(w * 0.54, h * 0.34); c.closePath(); c.fill()
                        } else if (cat.glyph === "game") {                 // gamepad: d-pad cross + two buttons
                            var cxp = w * 0.34, cyp = h * 0.5, arm = w * 0.17, th = w * 0.11
                            c.fillRect(cxp - arm, cyp - th / 2, arm * 2, th)   // d-pad horizontal
                            c.fillRect(cxp - th / 2, cyp - arm, th, arm * 2)   // d-pad vertical
                            c.beginPath(); c.arc(w * 0.68, h * 0.4, w * 0.075, 0, 6.2832); c.fill()  // buttons
                            c.beginPath(); c.arc(w * 0.8, h * 0.6, w * 0.075, 0, 6.2832); c.fill()
                        } else if (cat.glyph === "reading") {              // open book
                            c.beginPath(); c.moveTo(w * 0.5, h * 0.26); c.lineTo(w * 0.16, h * 0.34); c.lineTo(w * 0.16, h * 0.78); c.lineTo(w * 0.5, h * 0.72); c.closePath(); c.fill()
                            c.beginPath(); c.moveTo(w * 0.5, h * 0.26); c.lineTo(w * 0.84, h * 0.34); c.lineTo(w * 0.84, h * 0.78); c.lineTo(w * 0.5, h * 0.72); c.closePath(); c.fill()
                            c.strokeStyle = (cat.modelData && cat.modelData.accent) ? cat.modelData.accent : "#444"
                            c.beginPath(); c.moveTo(w * 0.5, h * 0.3); c.lineTo(w * 0.5, h * 0.72); c.stroke()
                        } else {                                           // settings: a gear
                            var gx = w * 0.5, gy = h * 0.5, R = w * 0.3, teeth = 8, tw = w * 0.13, tl = w * 0.16
                            for (var k = 0; k < teeth; k++) {
                                c.save(); c.translate(gx, gy); c.rotate(k / teeth * 6.2832)
                                c.fillRect(-tw / 2, -(R + tl), tw, tl + R * 0.3); c.restore()
                            }
                            c.beginPath(); c.arc(gx, gy, R, 0, 6.2832); c.fill()
                            c.save(); c.globalCompositeOperation = "destination-out"
                            c.beginPath(); c.arc(gx, gy, R * 0.42, 0, 6.2832); c.fill(); c.restore()
                        }
                    }
                }
                Text { // ultimate fallback when there's no icon and no glyph
                    anchors.centerIn: parent
                    visible: cat.glyph === ""
                    text: (cat.modelData && cat.modelData.title) ? cat.modelData.title.charAt(0) : ""
                    color: "#FFFFFF"; font.bold: true; font.pixelSize: parent.height * 0.5
                }
            }
            Image {
                anchors.fill: parent; visible: cat.icon !== ""
                source: (cat.icon !== "" && xmb.host) ? xmb.host.resolve(cat.icon) : ""
                fillMode: Image.PreserveAspectFit; smooth: true
            }
            Text { // the selected bucket's name, just under its tile
                visible: cat.sel
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.bottom; anchors.topMargin: xmb.height * 0.02
                text: (cat.modelData && cat.modelData.title) ? cat.modelData.title : ""
                color: xmb.textColor; font.bold: true; font.pixelSize: Math.max(12, xmb.height * 0.03)
            }
            MouseArea { // click a category tile to switch to it
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: if (xmb.host) xmb.host.gotoCat(cat.index)
            }
        }
    }

    // ---- vertical column (the bucket's catalogs, or a catalog's items), descending from below the cross ----
    Repeater {
        model: xmb.items
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool sel: index === xmb.itemIndex
            readonly property real dy: (index - xmb.itemScroll) * xmb.itemGap
            width: xmb.rowSize; height: xmb.rowSize
            x: xmb.crossX - width / 2
            y: xmb.colTop + dy - height / 2
            opacity: dy < -xmb.itemGap * 0.5 ? 0.0 : (sel ? 1.0 : 0.6) // fade rows that rise above the column top
            scale: sel ? 1.12 : 0.94
            Behavior on scale { NumberAnimation { duration: 140 } }

            Rectangle {
                anchors.fill: parent; radius: 8
                color: (row.modelData && row.modelData.accent) ? row.modelData.accent : "#23272F"
                visible: !(row.modelData && row.modelData.image)
            }
            Image {
                anchors.fill: parent; visible: !!(row.modelData && row.modelData.image)
                source: (row.modelData && row.modelData.image && xmb.host) ? xmb.host.resolve(row.modelData.image) : ""
                fillMode: Image.PreserveAspectCrop; smooth: true
            }
            // Title (and, for the selected row, a subtitle line beneath it) to the right of the icon.
            Column {
                anchors.left: parent.right; anchors.leftMargin: xmb.width * 0.015
                anchors.verticalCenter: parent.verticalCenter
                width: xmb.width * 0.5; spacing: xmb.height * 0.004
                Text {
                    width: parent.width
                    text: (row.modelData && row.modelData.title) ? row.modelData.title : ""
                    color: row.sel ? xmb.textColor : xmb.subColor
                    font.bold: row.sel; font.pixelSize: Math.max(11, xmb.height * (row.sel ? 0.03 : 0.024))
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: row.sel && !!(row.modelData && row.modelData.subtitle)
                    text: (row.modelData && row.modelData.subtitle) ? row.modelData.subtitle : ""
                    color: xmb.descColor; font.pixelSize: Math.max(10, xmb.height * 0.022)
                    elide: Text.ElideRight
                }
            }
            // Click the row (icon + title region) to select it; click the selected one to open/drill. Sized to
            // the row's vertical slot (itemGap) so adjacent rows don't overlap; disabled for faded-out rows.
            MouseArea {
                anchors.verticalCenter: parent.verticalCenter
                x: 0; width: parent.width + xmb.width * 0.5; height: xmb.itemGap
                enabled: row.opacity > 0.1; cursorShape: Qt.PointingHandCursor
                onClicked: if (xmb.host) xmb.host.gotoItem(row.index)
            }
        }
    }
}
