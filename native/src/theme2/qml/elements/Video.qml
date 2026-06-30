// Video / preview element. True per-item trailer playback would need (a) a preview-video source per item -
// our catalog/addon data has only posters, no clips - and (b) a GPU render path; the themed view runs on Qt
// Quick's software backend so it can coexist with the app's libmpv video widget, and QML VideoOutput doesn't
// render reliably there. So this gives the "live preview" feel that DOES work in software: a slow Ken Burns
// (zoom + drift) loop over the bound poster, with a play badge. Drops to a static poster if there's no image.
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
        clip: true // keep the zoomed/panned image inside the rounded frame

        Image {
            id: poster
            anchors.fill: parent
            source: host ? host.resolve(T.sourceOf(el, ctx)) : ""
            fillMode: Image.PreserveAspectCrop
            visible: status === Image.Ready
            opacity: 0.9
            transformOrigin: Item.Center

            // Gentle, continuous motion so a still poster reads as a "preview". Runs only once the image is
            // ready; the alternating pan + zoom loop indefinitely and restart with each new selection source.
            // panX has no initializer - the SequentialAnimation below is its value source (can't have both).
            property real panX
            transform: Translate { x: poster.panX }
            SequentialAnimation on scale {
                running: poster.status === Image.Ready
                loops: Animation.Infinite
                NumberAnimation { from: 1.0; to: 1.12; duration: 5000; easing.type: Easing.InOutSine }
                NumberAnimation { from: 1.12; to: 1.0; duration: 5000; easing.type: Easing.InOutSine }
            }
            SequentialAnimation on panX {
                running: poster.status === Image.Ready
                loops: Animation.Infinite
                NumberAnimation { from: -0.04 * poster.width; to: 0.04 * poster.width; duration: 7000; easing.type: Easing.InOutSine }
                NumberAnimation { from: 0.04 * poster.width; to: -0.04 * poster.width; duration: 7000; easing.type: Easing.InOutSine }
            }
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
