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
    // Native kinetic scrolling on touch (D1 Task 4): a mobile drag flicks the strip; key/controller nav still
    // snaps via currentIndex + StrictlyEnforceRange. Desktop/TV stay non-interactive (a pixel/behaviour no-op).
    interactive: (typeof form !== "undefined" && form) ? form.mode === "mobile" : false
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
        MouseArea { // click a card to select it; click the selected card to open it
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor; z: 10
            onClicked: if (lv.host && lv.host.gotoItem) lv.host.gotoItem(index)
        }
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
                id: poster
                anchors.fill: parent
                // T.tileImage, not a bare modelData.image read: a row carrying art only under the
                // open-ended `images` role map still gets a poster instead of a blank tile.
                source: (lv.host && T.tileImage(modelData) !== "") ? lv.host.contentUrl(T.tileImage(modelData)) : ""
                fillMode: Image.PreserveAspectCrop
                visible: status === Image.Ready
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - 16
                // The shared "no artwork actually on screen" rule. This used to be `!modelData.image`,
                // which hid the title for precisely the rows whose url was DEAD — the card then had
                // nothing readable on it at all (issue #29's hardening).
                visible: T.tileNeedsTitle(modelData, poster.status === Image.Ready)
                text: (modelData && modelData.title) ? modelData.title : ""
                color: "white"; horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Math.max(10, 0.026 * (lv.host ? lv.host.height : 720)); font.bold: true
                // Phone-width cards are narrower than the TV-scale font: shrink to fit before eliding,
                // so "Movies" reads as Movies instead of "M…".
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 10
                elide: Text.ElideRight
            }
        }
    }
}
