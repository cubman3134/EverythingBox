// Gallery element: cycles through the selected item's multiple images for a `role` (default "screenshot";
// e.g. "fanart"), cross-fading on a timer. This is how a theme shows a provider's screenshot set / fanart
// reel. Falls back to a single `fallback` image (a role or a literal/default path) when the item has none,
// so it always renders something out of the box.
import QtQuick
import "../Theme.js" as T

Item {
    id: root
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property string role: T.val(el, "role", "screenshot")
    readonly property var urls: {
        var list = T.artList(ctx, role)
        if (list.length > 0) return list
        // No gallery art: fall back to a single image (fallback role, then literal/default path).
        var fb = el.fallback ? (T.artUrl(ctx, el.fallback) || el.fallback) : ""
        return fb ? [fb] : []
    }
    property int idx: 0
    readonly property int interval: Math.max(800, Number(T.val(el, "interval", 4000)))

    onUrlsChanged: { idx = 0; a.source = first(); b.source = ""; a.opacity = 1; b.opacity = 0 } // new item -> restart
    function first() { return urls.length ? (host ? host.resolve(urls[0]) : urls[0]) : "" }
    function at(i) { return (urls.length && host) ? host.resolve(urls[i % urls.length]) : "" }

    Rectangle { // backdrop / placeholder
        anchors.fill: parent
        color: T.val(el, "color", "#0C0E12")
        radius: Number(T.val(el, "radius", 0))
    }

    // Two stacked images crossfade: the incoming one fades up over the outgoing.
    Image {
        id: a
        anchors.fill: parent
        source: root.first()
        asynchronous: true
        fillMode: T.val(el, "fillMode", "cover") === "contain" ? Image.PreserveAspectFit : Image.PreserveAspectCrop
        Behavior on opacity { NumberAnimation { duration: 600; easing.type: Easing.InOutQuad } }
    }
    Image {
        id: b
        anchors.fill: parent
        opacity: 0
        asynchronous: true
        fillMode: a.fillMode
        Behavior on opacity { NumberAnimation { duration: 600; easing.type: Easing.InOutQuad } }
    }

    // Advance only when there's more than one image. Ping-pongs which layer is on top.
    property bool aOnTop: true
    Timer {
        interval: root.interval
        running: root.urls.length > 1
        repeat: true
        onTriggered: {
            root.idx = (root.idx + 1) % root.urls.length
            if (aOnTop) { b.source = root.at(root.idx); b.opacity = 1; a.opacity = 0 }
            else        { a.source = root.at(root.idx); a.opacity = 1; b.opacity = 0 }
            aOnTop = !aOnTop
        }
    }

    // Dot indicators (drawn) when there are several images.
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        spacing: 6
        visible: root.urls.length > 1 && el.dots !== false
        Repeater {
            model: root.urls.length
            delegate: Rectangle {
                required property int index
                width: 6; height: 6; radius: 3
                color: index === root.idx ? Qt.rgba(1, 1, 1, 0.95) : Qt.rgba(1, 1, 1, 0.35)
            }
        }
    }
}
