// Image element. Sources, in order: a literal `path` / `binding`, then an artwork `role` (the selected
// item's images[role], e.g. "logo", "box", "hero", "poster"), then a `fallback` (another role, or a literal
// default path). This is how a theme shows the title logo / box art / poster a provider supplied, and a
// sensible default when it didn't. When nothing resolves and `textFallback` is set, the element renders the
// bound text instead (the classic "clear logo, or the game's name if there's no logo").
import QtQuick
import "../Theme.js" as T

Item {
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property string src: host ? host.resolve(T.imageSource(el, ctx)) : ""
    // Show the text fallback only when the theme asked for it AND no image resolved (or it failed to load).
    readonly property bool showText: (el.textFallback === true)
                                     && (src === "" || img.status === Image.Error)

    Rectangle { // placeholder while loading / when empty (hidden if we're drawing the text fallback)
        anchors.fill: parent
        visible: img.status !== Image.Ready && !showText
        color: T.val(el, "color", "#1A1E25")
        radius: Number(T.val(el, "radius", 0))
    }
    Image {
        id: img
        anchors.fill: parent
        source: parent.src
        asynchronous: true
        fillMode: T.val(el, "fillMode", "contain") === "cover"   ? Image.PreserveAspectCrop
                : T.val(el, "fillMode", "contain") === "stretch" ? Image.Stretch
                                                                 : Image.PreserveAspectFit
        layer.enabled: Number(T.val(el, "radius", 0)) > 0
        layer.effect: null
        visible: status === Image.Ready && !parent.showText
    }
    Text { // logo-or-title fallback
        anchors.fill: parent
        visible: parent.showText
        // The text comes from `text` / `textBinding` (a data path, kept separate from `binding` so `binding`
        // can still be an image URL for plain images), else the selected item's title.
        text: el.text ? el.text
            : (el.textBinding ? (T.dig(ctx, el.textBinding) || "")
            : (ctx && ctx.selected ? (ctx.selected.title || "") : ""))
        color: T.val(el, "textColor", "#FFFFFF")
        font.family: T.val(el, "fontFamily", "")
        font.pixelSize: Math.max(1, Number(T.val(el, "fontSize", 0.045)) * (host ? host.height : 720))
        font.bold: el.bold !== false
        horizontalAlignment: T.val(el, "align", "center") === "left" ? Text.AlignLeft
                           : T.val(el, "align", "center") === "right" ? Text.AlignRight : Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        wrapMode: Text.WordWrap
        maximumLineCount: Number(T.val(el, "lines", 2))
    }
}
