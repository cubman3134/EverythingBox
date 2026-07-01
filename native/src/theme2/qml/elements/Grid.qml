// Grid element: a grid of item cards (poster + label) with themeable columns, aspect, spacing, card radius
// and a selection border. Driven by the data context's `items` / `index`.
//
// Card knobs (all optional; defaults preserve the original look): fill (tile colour behind the poster),
// border + borderWidth (an always-on outline on every card), selectedBorder + selectedWidth, selectedScale
// (the selected card grows and lifts above its neighbours - a Wii-menu "pop"), label ("overlay" scrim over
// the poster [default], "below" a name-plate strip inside the card, or "none"), labelColor, labelBg.
import QtQuick
import "../Theme.js" as T

GridView {
    id: gv
    property var el: ({})
    property var ctx: ({})
    property var host
    property var card: T.val(el, "card", ({}))
    property int cols: Number(T.val(el, "columns", 4))
    property string labelMode: T.val(card, "label", "overlay")
    property real labelFrac: labelMode === "below" ? 0.22 : 0.0

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
        z: sel ? 2 : 0 // a scaled-up selection draws above its neighbours
        MouseArea { // click a card to select it; click the selected card to open it
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor; z: 10
            onClicked: if (gv.host && gv.host.gotoItem) gv.host.gotoItem(index)
        }
        Rectangle {
            id: cardRect
            anchors.fill: parent
            anchors.margins: Math.max(2, Number(T.val(gv.el, "spacing", 0.008)) * (gv.host ? gv.host.width : 1280))
            radius: Number(T.val(gv.card, "radius", 10))
            clip: true
            color: (modelData && modelData.accent) ? modelData.accent : T.val(gv.card, "fill", "#23272F")
            border.width: sel ? Number(T.val(gv.card, "selectedWidth", 4)) : Number(T.val(gv.card, "borderWidth", 0))
            border.color: sel ? T.val(gv.card, "selectedBorder", T.val(gv.el, "color", "#E07A2E"))
                              : T.val(gv.card, "border", "#00000000")
            scale: sel ? Number(T.val(gv.card, "selectedScale", 1.0)) : 1.0
            Behavior on scale { NumberAnimation { duration: 130; easing.type: Easing.OutBack } }

            // Poster area: the whole card in "overlay"/"none" label modes, or the top part in "below" mode.
            Item {
                id: posterArea
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                height: parent.height * (1 - gv.labelFrac)
                clip: true
                Image {
                    anchors.fill: parent
                    source: (modelData && modelData.image && gv.host) ? gv.host.resolve(modelData.image) : ""
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready
                }
                // Overlay label: a dark scrim + title at the bottom of the poster (the original look).
                Rectangle {
                    visible: gv.labelMode === "overlay"
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    height: parent.height * 0.32
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "transparent" }
                        GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.65) }
                    }
                }
                Text {
                    visible: gv.labelMode === "overlay"
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    anchors.margins: 10
                    text: (modelData && modelData.title) ? modelData.title : ""
                    color: T.val(gv.card, "labelColor", "#FFFFFF")
                    font.pixelSize: Math.max(10, 0.024 * (gv.host ? gv.host.height : 720))
                    font.bold: true
                    elide: Text.ElideRight
                }
            }
            // "Below" label: a name-plate strip inside the bottom of the card (Wii-channel style).
            Rectangle {
                visible: gv.labelMode === "below"
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                height: parent.height * gv.labelFrac
                color: T.val(gv.card, "labelBg", "#F4F7FB")
                Text {
                    anchors.fill: parent; anchors.margins: parent.height * 0.16
                    text: (modelData && modelData.title) ? modelData.title : ""
                    color: T.val(gv.card, "labelColor", "#3A4657")
                    font.pixelSize: Math.max(9, 0.022 * (gv.host ? gv.host.height : 720))
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight; maximumLineCount: 2; wrapMode: Text.WordWrap
                }
            }
        }
    }
}
