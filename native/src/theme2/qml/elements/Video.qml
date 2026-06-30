// Video element: in the themed layout this is where a preview video plays. Real playback is wired when the
// engine is hosted inside the app (it has QtMultimedia); here it shows the bound poster with a play badge so
// themers can place and size it. Themeable corner radius.
import QtQuick
import "../Theme.js" as T

Item {
    property var el: ({})
    property var ctx: ({})
    property var host

    Rectangle {
        anchors.fill: parent
        radius: Number(T.val(el, "radius", 8))
        color: "#0C0E12"
        clip: true
        Image {
            anchors.fill: parent
            source: host ? host.resolve(T.sourceOf(el, ctx)) : ""
            fillMode: Image.PreserveAspectCrop
            visible: status === Image.Ready
            opacity: 0.85
        }
        // Play badge (drawn, so it needs no font glyph).
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height) * 0.22
            height: width; radius: width / 2
            color: Qt.rgba(0, 0, 0, 0.45)
            border.width: 2; border.color: Qt.rgba(1, 1, 1, 0.85)
            Canvas {
                anchors.centerIn: parent
                width: parent.width * 0.5; height: parent.height * 0.5
                onPaint: {
                    var c = getContext("2d"); c.reset()
                    c.beginPath(); c.moveTo(0, 0); c.lineTo(width, height / 2); c.lineTo(0, height)
                    c.closePath(); c.fillStyle = "white"; c.fill()
                }
            }
        }
    }
}
