// DateTime element: the current date/time in a themeable format (e.g. "hh:mm", "ddd d MMM"). `fontFile` loads
// a bundled font from the theme folder (any format Qt supports, e.g. .ttf); else `fontFamily` names a system
// font.
import QtQuick
import "../Theme.js" as T

Text {
    id: dt
    property var el: ({})
    property var ctx: ({})
    property var host
    property var now: new Date()
    Timer { interval: 1000; running: true; repeat: true; onTriggered: now = new Date() }
    FontLoader { id: fl; source: (el && el.fontFile && host) ? host.themeAsset(el.fontFile) : "" }
    text: Qt.formatDateTime(now, T.val(el, "format", "hh:mm"))
    color: T.val(el, "color", "#FFFFFF")
    // Empty fontFamily -> the app default (NOT Qt's "" -> "MS Sans Serif", which fails DirectWrite and paints nothing).
    font.family: (fl.status === FontLoader.Ready) ? fl.name : T.val(el, "fontFamily", Qt.application.font.family)
    // The themed size is a HEIGHT fraction — on a narrow (portrait) screen the string can outgrow the
    // element's width box. Measure at the themed size and scale the pixel size down to fit the box.
    readonly property real basePx: Math.max(1, Number(T.val(el, "fontSize", 0.03)) * (host ? host.height : 720))
    TextMetrics {
        id: tm
        font.family: dt.font.family
        font.bold: dt.font.bold
        font.pixelSize: dt.basePx
        text: dt.text
    }
    // 0.93 safety: TextMetrics advance widths and the renderer disagree by a hair — without the margin
    // the final glyph lands exactly on the box edge and can vanish under a neighbouring element.
    font.pixelSize: Math.max(10, Math.min(basePx, dt.width > 0 ? 0.93 * basePx * dt.width / Math.max(1, tm.width) : basePx))
    font.bold: el.bold === true
    horizontalAlignment: T.val(el, "align", "left") === "center" ? Text.AlignHCenter
                       : T.val(el, "align", "left") === "right"  ? Text.AlignRight : Text.AlignLeft
    verticalAlignment: Text.AlignVCenter
}
