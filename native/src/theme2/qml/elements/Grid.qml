// Grid element: a grid of item cards (poster + label) with themeable columns, aspect, spacing, card radius
// and a selection border. Driven by the data context's `items` / `index`.
import QtQuick
import "../Theme.js" as T

GridView {
    id: gv
    property var el: ({})
    property var ctx: ({})
    property var host
    property var card: T.val(el, "card", ({}))
    property int cols: Number(T.val(el, "columns", 4))

    clip: true
    interactive: false
    cellWidth: width / cols
    cellHeight: cellWidth * Number(T.val(el, "aspect", 1.4))
    model: ctx ? ctx.items : []
    currentIndex: ctx ? ctx.index : 0

    delegate: Item {
        width: gv.cellWidth
        height: gv.cellHeight
        required property var modelData
        required property int index
        property bool sel: index === gv.currentIndex
        MouseArea { // click a card to select it; click the selected card to open it
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor; z: 10
            onClicked: if (gv.host && gv.host.gotoItem) gv.host.gotoItem(index)
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: Math.max(2, Number(T.val(gv.el, "spacing", 0.008)) * (gv.host ? gv.host.width : 1280))
            radius: Number(T.val(gv.card, "radius", 10))
            clip: true
            color: (modelData && modelData.accent) ? modelData.accent : "#23272F"
            border.width: sel ? Number(T.val(gv.card, "selectedWidth", 4)) : 0
            border.color: T.val(gv.card, "selectedBorder", T.val(gv.el, "color", "#E07A2E"))
            Image {
                anchors.fill: parent
                source: (modelData && modelData.image && gv.host) ? gv.host.resolve(modelData.image) : ""
                fillMode: Image.PreserveAspectCrop
                visible: status === Image.Ready
            }
            Rectangle { // label scrim
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                height: parent.height * 0.32
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.65) }
                }
            }
            Text {
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                anchors.margins: 10
                text: (modelData && modelData.title) ? modelData.title : ""
                color: "white"
                font.pixelSize: Math.max(10, 0.024 * (gv.host ? gv.host.height : 720))
                font.bold: true
                elide: Text.ElideRight
            }
        }
    }
}
