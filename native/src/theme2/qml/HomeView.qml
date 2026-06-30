// First themed view of the new QML theme engine (Phase 1). Everything visible here is driven by the injected
// `theme` object (parsed from a theme file) and the `items` model - nothing is hard-coded to a look. Later
// phases generalize this into named views + a full element library (image/text/grid/carousel/video/...).
import QtQuick

Item {
    id: root
    width: 1280
    height: 720

    // Injected from C++: `theme` (the parsed theme file) and `items` (the catalog rows).
    property var t: theme
    property var rows: items

    // --- helpers: read a theme value with a fallback ---------------------------------------------------
    function v(obj, key, fallback) { return (obj && obj[key] !== undefined && obj[key] !== "") ? obj[key] : fallback }
    property var bg: v(t, "background", ({}))
    property var grid: v(t, "grid", ({}))
    property var card: v(t, "card", ({}))
    property string accent: v(t, "accent", "#E07A2E")
    property string fg: v(t, "foreground", "#FFFFFF")
    property string fontFamily: v(t, "fontFamily", "")

    // --- background: colour, optional image, dim overlay -----------------------------------------------
    Rectangle { anchors.fill: parent; color: v(bg, "color", "#0F1216") }
    Image {
        anchors.fill: parent
        source: v(bg, "image", "")
        visible: source != ""
        fillMode: Image.PreserveAspectCrop
    }
    Rectangle { anchors.fill: parent; color: "black"; opacity: Number(v(bg, "dim", 0)) }

    // --- header --------------------------------------------------------------------------------------
    Text {
        x: 48; y: 30
        text: v(t, "name", "My Media Vault")
        color: fg
        font.family: fontFamily
        font.pixelSize: 34
        font.bold: true
    }
    Text {
        anchors.right: parent.right; anchors.rightMargin: 48; y: 38
        text: "QML theme engine — preview"
        color: Qt.rgba(1, 1, 1, 0.5)
        font.family: fontFamily
        font.pixelSize: 18
    }

    // --- the catalog grid ----------------------------------------------------------------------------
    GridView {
        id: gv
        anchors.fill: parent
        anchors.leftMargin: 48; anchors.rightMargin: 48
        anchors.topMargin: 104; anchors.bottomMargin: 48
        clip: true
        property int cols: Number(v(grid, "columns", 6))
        cellWidth: width / cols
        cellHeight: cellWidth * Number(v(grid, "aspect", 1.4))
        model: rows
        currentIndex: 0

        delegate: Item {
            width: gv.cellWidth
            height: gv.cellHeight
            readonly property bool selected: index === gv.currentIndex
            Rectangle {
                anchors.fill: parent
                anchors.margins: 8
                radius: Number(root.v(root.card, "radius", 12))
                color: modelData && modelData.accent ? modelData.accent : "#2A2D34"
                border.width: selected ? 4 : 0
                border.color: root.accent
                Text {
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    anchors.margins: 12
                    text: modelData && modelData.title ? modelData.title : ""
                    color: "white"
                    font.family: root.fontFamily
                    font.pixelSize: 20
                    font.bold: true
                    elide: Text.ElideRight
                }
            }
        }
    }
}
