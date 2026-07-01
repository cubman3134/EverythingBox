// Button element: a clickable rounded/pill button that fires a named host action (the host decides what it
// does). Used for the Wii-menu corner buttons (Settings, Profile). Theme keys: action (the name passed to the
// host, e.g. "settings" / "profile"), glyph ("settings"|"profile" drawn icon), label (optional text),
// color (fill), textColor, borderColor, shape ("pill" [default] | "round").
import QtQuick
import "../Theme.js" as T

Item {
    id: btn
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property string action: T.val(el, "action", "")
    readonly property string glyph: T.val(el, "glyph", "")
    readonly property string label: T.val(el, "label", "")
    readonly property color bg: T.val(el, "color", "#F4F7FB")
    readonly property color fg: T.val(el, "textColor", "#38455A")
    readonly property color bc: T.val(el, "borderColor", "#AEBBCB")

    Rectangle {
        id: cap
        anchors.fill: parent
        radius: T.val(btn.el, "shape", "pill") === "round" ? Math.min(width, height) / 2 : height / 2
        color: btn.bg
        border.width: 2; border.color: btn.bc
        scale: ma.pressed ? 0.93 : (ma.containsMouse ? 1.05 : 1.0)
        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }

        Row {
            anchors.centerIn: parent; spacing: cap.height * 0.16
            Canvas { // the drawn glyph (software-renderer safe: painted once)
                width: cap.height * 0.5; height: cap.height * 0.5
                anchors.verticalCenter: parent.verticalCenter
                visible: btn.glyph !== ""
                onPaint: {
                    var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
                    var w = width, h = height
                    c.fillStyle = btn.fg; c.strokeStyle = btn.fg
                    c.lineWidth = Math.max(2, w * 0.08); c.lineCap = "round"; c.lineJoin = "round"
                    if (btn.glyph === "profile") {                       // a person
                        c.beginPath(); c.arc(w * 0.5, h * 0.32, w * 0.18, 0, 6.2832); c.fill()
                        c.beginPath(); c.moveTo(w * 0.16, h * 0.9)
                        c.bezierCurveTo(w * 0.16, h * 0.56, w * 0.84, h * 0.56, w * 0.84, h * 0.9)
                        c.closePath(); c.fill()
                    } else {                                             // settings: a gear
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
            Text {
                visible: btn.label !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: btn.label; color: btn.fg
                font.bold: true; font.pixelSize: Math.max(10, cap.height * 0.30)
            }
        }
        MouseArea {
            id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            onClicked: if (btn.host && btn.host.buttonAction) btn.host.buttonAction(btn.action)
        }
    }
}
