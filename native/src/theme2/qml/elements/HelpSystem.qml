// HelpSystem element: a centered row of button hints (button glyph + action label), ES-DE style.
import QtQuick
import "../Theme.js" as T

Item {
    property var el: ({})
    property var ctx: ({})
    property var host
    property color fg: T.val(el, "color", "#FFFFFF")
    property color chip: Qt.rgba(fg.r, fg.g, fg.b, 0.16) // a tint of the text colour, so it reads on any bg
    property real fs: Number(T.val(el, "fontSize", 0.024)) * (host ? host.height : 720)

    Row {
        anchors.centerIn: parent
        spacing: Math.max(10, fs * 0.9)
        Repeater {
            model: el.entries ? el.entries : []
            delegate: Row {
                required property var modelData
                spacing: 6
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 5
                    color: chip
                    height: Math.max(1, fs * 1.25)
                    width: Math.max(height, btn.implicitWidth + 12)
                    Text {
                        id: btn; anchors.centerIn: parent
                        text: modelData.button ? modelData.button : ""
                        color: fg; font.pixelSize: fs * 0.85; font.bold: true
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label ? modelData.label : ""
                    color: fg; opacity: 0.85; font.pixelSize: fs
                }
            }
        }
    }
}
