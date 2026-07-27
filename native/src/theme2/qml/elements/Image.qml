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
    // Double-buffered display: `shown` is the last source that ACTUALLY loaded; `probe` test-loads every
    // new src off-screen and only promotes it on Ready. Without this, a source swap (e.g. the detail page's
    // async meta replacing the catalog thumb with hi-res art) blanked the element while loading — and if the
    // new URL failed, the poster that WAS showing vanished for good ("the poster starts to load, then
    // doesn't"). The two Images share Qt's cache, so promoting a probed source repaints instantly.
    property string shown: ""
    // A selection change to an item with NO art must clear the frame — holding the previous item's art
    // (which the promote-on-Ready rule alone would do) shows the wrong poster instead of the placeholder.
    onSrcChanged: if (src === "") shown = ""
    // Show the text fallback only when the theme asked for it AND nothing displayable ever resolved.
    readonly property bool showText: (el.textFallback === true)
                                     && shown === "" && (src === "" || probe.status === Image.Error)

    Rectangle { // placeholder while loading / when empty (hidden if we're drawing the text fallback)
        anchors.fill: parent
        visible: img.status !== Image.Ready && !showText
        color: T.val(el, "color", "#1A1E25")
        radius: Number(T.val(el, "radius", 0))
    }
    Image { // off-screen loader: promotes src -> shown only once it is known-good
        id: probe
        source: parent.src
        asynchronous: true
        visible: false
        width: 1; height: 1   // rendering is img's job; this only drives the cache/status
        onStatusChanged: if (status === Image.Ready && source != "") parent.shown = source
    }
    Image {
        id: img
        anchors.fill: parent
        source: parent.shown
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
        // An empty fontFamily must fall back to the working application default, NOT to Qt's "" -> "MS Sans
        // Serif" (matches Text.qml — the logo text fallback should read in the theme's font, not a legacy face).
        font.family: T.val(el, "fontFamily", Qt.application.font.family)
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
