// DateTime element: the current date/time in a themeable format (e.g. "hh:mm", "ddd d MMM").
import QtQuick
import "../Theme.js" as T

Text {
    property var el: ({})
    property var ctx: ({})
    property var host
    property var now: new Date()
    Timer { interval: 1000; running: true; repeat: true; onTriggered: now = new Date() }
    text: Qt.formatDateTime(now, T.val(el, "format", "hh:mm"))
    color: T.val(el, "color", "#FFFFFF")
    font.family: T.val(el, "fontFamily", "")
    font.pixelSize: Math.max(1, Number(T.val(el, "fontSize", 0.03)) * (host ? host.height : 720))
    font.bold: el.bold === true
    horizontalAlignment: T.val(el, "align", "left") === "center" ? Text.AlignHCenter
                       : T.val(el, "align", "left") === "right"  ? Text.AlignRight : Text.AlignLeft
    verticalAlignment: Text.AlignVCenter
}
