// XMB element - a PlayStation-style XrossMediaBar. The horizontal axis is the categories (host.categories,
// each = a media type), the vertical axis is the selected category's live items (host.items). Both axes slide
// so the active element stays pinned at the "cross". This element is purely presentational: ThemeView owns
// the keys (xmb mode) and the host (C++) loads each category's column on categoryChanged. Data it reads off
// the host (the ThemeView root): categories, catIndex, items, currentIndex.
//
// Theme keys: color, subColor, crossX/crossY (fraction - where the cross sits), catSpacing, itemSpacing,
// iconSize (all fractions), descColor (the selected item's description text).
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
    readonly property real crossY: Number(T.val(el, "crossY", 0.34)) * height
    readonly property real catGap: Number(T.val(el, "catSpacing", 0.135)) * width
    readonly property real itemGap: Number(T.val(el, "itemSpacing", 0.085)) * height
    readonly property real iconSize: Number(T.val(el, "iconSize", 0.11)) * height

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
            width: xmb.iconSize; height: xmb.iconSize
            x: xmb.crossX + dx - width / 2
            y: xmb.crossY - height / 2
            opacity: sel ? 1.0 : Math.max(0.18, 0.55 - Math.abs(dx) / xmb.width * 0.6)
            scale: sel ? 1.18 : 0.88
            Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

            Rectangle {
                anchors.fill: parent; radius: width * 0.18
                color: (cat.modelData && cat.modelData.accent) ? cat.modelData.accent : "#2A2E36"
                visible: !(cat.modelData && cat.modelData.icon)
                Text { // first letter as a stand-in glyph when the theme ships no icon
                    anchors.centerIn: parent
                    text: (cat.modelData && cat.modelData.title) ? cat.modelData.title.charAt(0) : ""
                    color: "#FFFFFF"; font.bold: true; font.pixelSize: parent.height * 0.5
                }
            }
            Image {
                anchors.fill: parent; visible: !!(cat.modelData && cat.modelData.icon)
                source: (cat.modelData && cat.modelData.icon && xmb.host) ? xmb.host.resolve(cat.modelData.icon) : ""
                fillMode: Image.PreserveAspectFit; smooth: true
            }
            Text {
                visible: cat.sel
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.bottom; anchors.topMargin: xmb.height * 0.018
                text: (cat.modelData && cat.modelData.title) ? cat.modelData.title : ""
                color: xmb.textColor; font.bold: true; font.pixelSize: Math.max(12, xmb.height * 0.03)
            }
        }
    }

    // ---- vertical item axis (the selected category's column, descending from the cross) ------------------
    Repeater {
        model: xmb.items
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool sel: index === xmb.itemIndex
            readonly property real dy: (index - xmb.itemScroll) * xmb.itemGap
            width: xmb.iconSize * 0.9; height: xmb.iconSize * 0.9
            x: xmb.crossX - width / 2
            y: xmb.crossY + xmb.iconSize * 1.05 + dy - height / 2 // start clear below the category icon + label
            // Fade out as a row rises above the cross (XMB shows the selection + what's below it).
            opacity: dy < -xmb.itemGap * 0.5 ? 0.0 : (sel ? 1.0 : 0.6)
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
            Text {
                anchors.left: parent.right; anchors.leftMargin: xmb.width * 0.015
                anchors.verticalCenter: parent.verticalCenter
                width: xmb.width * 0.5
                text: (row.modelData && row.modelData.title) ? row.modelData.title : ""
                color: row.sel ? xmb.textColor : xmb.subColor
                font.bold: row.sel; font.pixelSize: Math.max(11, xmb.height * (row.sel ? 0.03 : 0.024))
                elide: Text.ElideRight
            }
        }
    }

    // ---- selected item's subtitle/description (to the right of the cross) ---------------------------------
    Text {
        readonly property var selItem: (xmb.items && xmb.items.length > xmb.itemIndex && xmb.itemIndex >= 0) ? xmb.items[xmb.itemIndex] : null
        x: xmb.crossX + xmb.width * 0.32
        y: xmb.crossY - xmb.height * 0.04
        width: xmb.width * 0.42
        visible: !!selItem
        text: selItem && selItem.subtitle ? selItem.subtitle : ""
        color: xmb.descColor; font.pixelSize: Math.max(11, xmb.height * 0.024)
        wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
    }
}
