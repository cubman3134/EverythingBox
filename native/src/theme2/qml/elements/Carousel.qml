// Carousel element: a horizontal strip (ES-DE "system view" style) with the selected item centered and
// scaled up. Themeable item size, spacing, and selection accent.
import QtQuick
import "../Theme.js" as T

ListView {
    id: lv
    property var el: ({})
    property var ctx: ({})
    property var host

    orientation: ListView.Horizontal
    clip: true
    interactive: false
    spacing: Number(T.val(el, "spacing", 0.01)) * (host ? host.width : 1280)
    model: ctx ? ctx.items : []
    currentIndex: ctx ? ctx.index : 0
    highlightRangeMode: ListView.StrictlyEnforceRange
    preferredHighlightBegin: width / 2 - itemW / 2
    preferredHighlightEnd: width / 2 + itemW / 2
    highlightMoveDuration: 220

    property real itemW: width * Number(T.val(el, "itemWidth", 0.16))

    delegate: Item {
        width: lv.itemW
        height: lv.height
        required property var modelData
        required property int index
        property bool sel: index === lv.currentIndex
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * (sel ? 1.0 : 0.82)
            height: parent.height * (sel ? 1.0 : 0.82)
            Behavior on width  { NumberAnimation { duration: 180 } }
            Behavior on height { NumberAnimation { duration: 180 } }
            radius: Number(T.val(T.val(lv.el, "card", ({})), "radius", 10))
            clip: true
            color: (modelData && modelData.accent) ? modelData.accent : "#23272F"
            border.width: sel ? 4 : 0
            border.color: T.val(lv.el, "color", "#E07A2E")
            Image {
                anchors.fill: parent
                source: (modelData && modelData.image && lv.host) ? lv.host.resolve(modelData.image) : ""
                fillMode: Image.PreserveAspectCrop
                visible: status === Image.Ready
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - 16
                visible: !(modelData && modelData.image)
                text: (modelData && modelData.title) ? modelData.title : ""
                color: "white"; horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Math.max(10, 0.026 * (lv.host ? lv.host.height : 720)); font.bold: true
                elide: Text.ElideRight
            }
        }
    }
}
