// Panel element: a filled bar with an optionally shaped top edge - the Wii-menu bottom shelf. Drawn with a
// static Canvas (painted once, not animated, so it's safe under the software render backend).
//
// Top-edge shape: flat at the two ends, an eased (S-curve) dip, then flat again in the middle - the Wii shelf
// profile. `sideFlat` is the flat length at each end and `midFlat` the flat length in the middle (both a
// fraction of the width); `curve` is how far the middle drops (a fraction of the height). With sideFlat =
// midFlat = 0 it degrades to a single smooth dip. Theme keys: color (solid) or gradient ["#top","#bottom"];
// topColor + topWidth (accent line along the edge); curve, sideFlat, midFlat.
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
            var edge = Number(T.val(panel.el, "topWidth", 0))
            var top = Math.max(1, edge)          // a hair below the top so a wide accent stroke isn't clipped
            var dip = Number(T.val(panel.el, "curve", 0.0)) * H
            var sideW = Number(T.val(panel.el, "sideFlat", 0.0)) * W
            var midW = Number(T.val(panel.el, "midFlat", 0.0)) * W
            var m0 = (W - midW) / 2, m1 = (W + midW) / 2   // the middle flat runs [m0, m1]
            var cxL = (sideW + m0) / 2, cxR = (m1 + (W - sideW)) / 2

            // Trace the top edge: flat side, S-curve down, flat middle, S-curve up, flat side. The bezier
            // control points sit at the segment midpoints (horizontal tangents), so it flows out of and into
            // the flat runs smoothly.
            function topEdge() {
                c.moveTo(0, top)
                c.lineTo(sideW, top)
                c.bezierCurveTo(cxL, top, cxL, top + dip, m0, top + dip)
                c.lineTo(m1, top + dip)
                c.bezierCurveTo(cxR, top + dip, cxR, top, W - sideW, top)
                c.lineTo(W, top)
            }

            // Filled body.
            c.beginPath(); topEdge(); c.lineTo(W, H); c.lineTo(0, H); c.closePath()
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
                c.beginPath(); topEdge()
                c.strokeStyle = T.val(panel.el, "topColor", "#A9C7E4")
                c.lineWidth = edge; c.lineCap = "round"; c.lineJoin = "round"; c.stroke()
            }
        }
    }
}
