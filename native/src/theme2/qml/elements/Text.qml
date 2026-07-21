// Text element: a literal string or a data binding, with themeable colour/font/size/alignment.
import QtQuick
import "../Theme.js" as T

Text {
    property var el: ({})
    property var ctx: ({})
    property var host
    // Form-factor UI scale (subsystem D). Desktop uiScale is 1.0 (identity), so the pixelSize below is unchanged;
    // TV (1.3) / mobile (1.15) grow the type. typeof-guarded so a fixture loaded without `form` still renders.
    readonly property real ffs: (typeof form !== "undefined" && form) ? form.uiScale : 1
    text: T.textOf(el, ctx)
    color: T.val(el, "color", "#FFFFFF")
    // An empty fontFamily must fall back to the working application default, NOT to Qt's "" -> "MS Sans Serif"
    // resolution: that legacy bitmap face fails DirectWrite (CreateFontFaceFromHDC) on many Windows systems, so
    // the whole Text paints nothing. Item-rooted elements (HelpSystem/Carousel) never set family and render fine.
    font.family: T.val(el, "fontFamily", Qt.application.font.family)
    font.pixelSize: Math.max(1, Number(T.val(el, "fontSize", 0.03)) * (host ? host.height : 720) * ffs)
    font.bold: el.bold === true
    horizontalAlignment: T.val(el, "align", "left") === "center" ? Text.AlignHCenter
                       : T.val(el, "align", "left") === "right"  ? Text.AlignRight : Text.AlignLeft
    verticalAlignment: Text.AlignVCenter
    elide: el.wrap === true ? Text.ElideNone : Text.ElideRight
    wrapMode: el.wrap === true ? Text.WordWrap : Text.NoWrap
    maximumLineCount: Number(T.val(el, "lines", el.wrap === true ? 6 : 1))
}
