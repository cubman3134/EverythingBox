// DateTime element: the current date/time in a themeable format (e.g. "hh:mm", "ddd d MMM"). `fontFile` loads
// a bundled font from the theme folder (any format Qt supports, e.g. .ttf); else `fontFamily` names a system
// font.
import QtQuick
import "../Theme.js" as T

Text {
    property var el: ({})
    property var ctx: ({})
    property var host
    property var now: new Date()
    Timer { interval: 1000; running: true; repeat: true; onTriggered: now = new Date() }
    FontLoader { id: fl; source: (el && el.fontFile && host) ? host.resolve(el.fontFile) : "" }
    text: Qt.formatDateTime(now, T.val(el, "format", "hh:mm"))
    color: T.val(el, "color", "#FFFFFF")
    // Empty fontFamily -> the app default (NOT Qt's "" -> "MS Sans Serif", which fails DirectWrite and paints nothing).
    font.family: (fl.status === FontLoader.Ready) ? fl.name : T.val(el, "fontFamily", Qt.application.font.family)
    font.pixelSize: Math.max(1, Number(T.val(el, "fontSize", 0.03)) * (host ? host.height : 720))
    font.bold: el.bold === true
    horizontalAlignment: T.val(el, "align", "left") === "center" ? Text.AlignHCenter
                       : T.val(el, "align", "left") === "right"  ? Text.AlignRight : Text.AlignLeft
    verticalAlignment: Text.AlignVCenter
}
