// Image element: a literal `path` or a bound image (e.g. the selected item's poster), with a themeable
// fill mode, corner radius and a placeholder when there's no/failed source.
import QtQuick
import "../Theme.js" as T

Item {
    property var el: ({})
    property var ctx: ({})
    property var host

    Rectangle { // placeholder while loading / when empty
        anchors.fill: parent
        visible: img.status !== Image.Ready
        color: T.val(el, "color", "#1A1E25")
        radius: Number(T.val(el, "radius", 0))
    }
    Image {
        id: img
        anchors.fill: parent
        source: host ? host.resolve(T.sourceOf(el, ctx)) : ""
        asynchronous: true
        fillMode: T.val(el, "fillMode", "contain") === "cover"   ? Image.PreserveAspectCrop
                : T.val(el, "fillMode", "contain") === "stretch" ? Image.Stretch
                                                                 : Image.PreserveAspectFit
        layer.enabled: Number(T.val(el, "radius", 0)) > 0
        layer.effect: null
        visible: status === Image.Ready
    }
}
