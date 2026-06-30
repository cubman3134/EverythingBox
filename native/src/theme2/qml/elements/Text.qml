// Text element: a literal string or a data binding, with themeable colour/font/size/alignment.
import QtQuick
import "../Theme.js" as T

Text {
    property var el: ({})
    property var ctx: ({})
    property var host
    text: T.textOf(el, ctx)
    color: T.val(el, "color", "#FFFFFF")
    font.family: T.val(el, "fontFamily", "")
    font.pixelSize: Math.max(1, Number(T.val(el, "fontSize", 0.03)) * (host ? host.height : 720))
    font.bold: el.bold === true
    horizontalAlignment: T.val(el, "align", "left") === "center" ? Text.AlignHCenter
                       : T.val(el, "align", "left") === "right"  ? Text.AlignRight : Text.AlignLeft
    verticalAlignment: Text.AlignVCenter
    elide: el.wrap === true ? Text.ElideNone : Text.ElideRight
    wrapMode: el.wrap === true ? Text.WordWrap : Text.NoWrap
    maximumLineCount: Number(T.val(el, "lines", el.wrap === true ? 6 : 1))
}
