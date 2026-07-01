// Button element: a clickable rounded/pill button that fires a named host action (the host decides what it
// does). Used for the Wii-menu corner buttons (Settings, Profile). A `round` button gets a Wii-style bevel:
// a metallic outer ring (bright top -> dark bottom) around a glossy inner face; a `pill` button is a plain
// flat capsule. Theme keys: action (the name passed to the host, e.g. "settings" / "profile"), glyph
// ("settings"|"profile" drawn icon), label (optional text), color (pill fill), textColor, borderColor,
// shape ("pill" [default] | "round").
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
    readonly property bool round: T.val(el, "shape", "pill") === "round"

    // Soft drop shadow beneath a round button (a radial fade; static Canvas, software-renderer safe).
    Canvas {
        visible: btn.round
        anchors.horizontalCenter: cap.horizontalCenter
        anchors.verticalCenter: cap.verticalCenter
        anchors.verticalCenterOffset: cap.height * 0.11
        width: cap.width * 1.16; height: cap.height * 1.16
        onWidthChanged: requestPaint(); onHeightChanged: requestPaint()
        onPaint: {
            var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
            var cx = width / 2, cy = height / 2, R = Math.min(width, height) / 2
            var g = c.createRadialGradient(cx, cy, R * 0.4, cx, cy, R)
            g.addColorStop(0, "#34000000"); g.addColorStop(1, "#00000000")
            c.fillStyle = g; c.beginPath(); c.arc(cx, cy, R, 0, 6.2832); c.fill()
        }
    }

    Rectangle {
        id: cap
        anchors.fill: parent
        radius: btn.round ? Math.min(width, height) / 2 : height / 2
        scale: ma.pressed ? 0.93 : (ma.containsMouse ? 1.05 : 1.0)
        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
        color: btn.round ? "#9FB0C4" : btn.bg           // round: base under the bevel ring; pill: flat fill
        border.width: btn.round ? 0 : 2
        border.color: btn.bc

        // ---- round Wii-style bevel: a metallic ring + a glossy inner face ----
        Rectangle {                                     // outer ring (bright top -> dark bottom)
            visible: btn.round; anchors.fill: parent; radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#FFFFFF" }
                GradientStop { position: 1.0; color: "#8698AE" }
            }
        }
        Rectangle {                                     // inner face
            id: face
            visible: btn.round; anchors.centerIn: parent
            width: parent.width - parent.height * 0.08; height: width; radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#FFFFFF" }
                GradientStop { position: 0.55; color: "#EAEFF6" }
                GradientStop { position: 1.0; color: "#C9D5E4" }
            }
            Rectangle {                                 // top gloss highlight
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top; anchors.topMargin: parent.height * 0.08
                width: parent.width * 0.74; height: parent.height * 0.40; radius: height / 2
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#D8FFFFFF" }
                    GradientStop { position: 1.0; color: "#00FFFFFF" }
                }
            }
        }

        // ---- glyph (+ label for pill) ----
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
