// Panel element: a filled bar, optionally with a curved (dipped) top edge - the Wii-menu bottom shelf. Drawn
// with a static Canvas (painted once, not animated, so it's safe under the software render backend). Theme
// keys: color (solid fill) or gradient ["#top","#bottom"] (vertical fill); topColor + topWidth (an accent
// line traced along the top edge); curve (0..1 - how far the middle of the top edge dips down, as a fraction
// of the element height; 0 = a flat top).
import QtQuick
import "../Theme.js" as T

Item {
    id: panel
    property var el: ({})
    property var ctx: ({})
    property var host

    Canvas {
        id: cv
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
            var W = width, H = height
            var curve = Number(T.val(panel.el, "curve", 0.0)) * H
            var edge = Number(T.val(panel.el, "topWidth", 0))
            var top = Math.max(1, edge)   // start a hair below the top so a wide accent stroke isn't clipped

            // Filled body: straight sides, a top edge that dips to (top + curve) in the middle.
            c.beginPath()
            c.moveTo(0, H); c.lineTo(0, top)
            c.quadraticCurveTo(W / 2, top + curve * 2, W, top)
            c.lineTo(W, H); c.closePath()
            var grad = T.val(panel.el, "gradient", null)
            if (grad && grad.length >= 2) {
                var g = c.createLinearGradient(0, 0, 0, H)
                g.addColorStop(0, grad[0]); g.addColorStop(1, grad[1])
                c.fillStyle = g
            } else {
                c.fillStyle = T.val(panel.el, "color", "#DCE3EC")
            }
            c.fill()

            // Accent line along the top edge.
            if (edge > 0) {
                c.beginPath(); c.moveTo(0, top)
                c.quadraticCurveTo(W / 2, top + curve * 2, W, top)
                c.strokeStyle = T.val(panel.el, "topColor", "#A9C7E4")
                c.lineWidth = edge; c.lineCap = "round"; c.stroke()
            }
        }
    }
}
