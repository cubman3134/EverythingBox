// Clock element: the current time as a 7-segment digital display (h:mm) with a small AM/PM, drawn in a
// Digital-7 style - angled (hexagonal) segment ends and a slight italic slant. Repainted once a second on a
// static Canvas (a once-a-second repaint is software-renderer safe). Theme keys: onColor (lit segments),
// offColor (unlit "ghost" segments; default transparent), ampmColor, ampmSize (fraction of the element
// height), thickness (segment thickness, fraction of a digit's width), slant (italic lean, 0 = upright).
import QtQuick
import "../Theme.js" as T

Item {
    id: clk
    property var el: ({})
    property var ctx: ({})
    property var host
    property var now: new Date()
    Timer { interval: 1000; running: true; repeat: true; onTriggered: { clk.now = new Date(); cv.requestPaint() } }

    Canvas {
        id: cv
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        // 7-segment patterns for 0-9: [a, b, c, d, e, f, g]
        readonly property var pats: [
            [1,1,1,1,1,1,0], [0,1,1,0,0,0,0], [1,1,0,1,1,0,1], [1,1,1,1,0,0,1], [0,1,1,0,0,1,1],
            [1,0,1,1,0,1,1], [1,0,1,1,1,1,1], [1,1,1,0,0,0,0], [1,1,1,1,1,1,1], [1,1,1,1,0,1,1]
        ]

        onPaint: {
            var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
            var W = width, H = height
            var onC = T.val(clk.el, "onColor", "#3A4657")
            var offC = T.val(clk.el, "offColor", "#00000000")
            var ampmC = T.val(clk.el, "ampmColor", onC)
            var slant = Number(T.val(clk.el, "slant", 0.11))
            var pats = cv.pats

            var d = clk.now, hr = d.getHours(), mn = d.getMinutes(), pm = hr >= 12
            var hs = String(((hr + 11) % 12) + 1)          // 1..12, no leading zero
            var ms = (mn < 10 ? "0" : "") + mn

            var dh = H * 0.82, dw = dh * 0.54
            var t = dw * Number(T.val(clk.el, "thickness", 0.16))
            var gap = dw * 0.22, colonW = dw * 0.52
            var y = (H - dh) / 2, yb = y + dh

            // Filled polygon; x of every point is skewed for the italic lean (bottom fixed, top leans right).
            function SX(px, py) { return px + slant * (yb - py) }
            function poly(pts, on) {
                c.fillStyle = on ? onC : offC
                c.beginPath(); c.moveTo(SX(pts[0][0], pts[0][1]), pts[0][1])
                for (var i = 1; i < pts.length; i++) c.lineTo(SX(pts[i][0], pts[i][1]), pts[i][1])
                c.closePath(); c.fill()
            }
            var t2 = t / 2
            function hseg(x1, x2, yc, on) {   // horizontal segment (angled ends)
                poly([[x1, yc], [x1 + t2, yc - t2], [x2 - t2, yc - t2], [x2, yc], [x2 - t2, yc + t2], [x1 + t2, yc + t2]], on)
            }
            function vseg(xc, y1, y2, on) {    // vertical segment (angled ends)
                poly([[xc, y1], [xc + t2, y1 + t2], [xc + t2, y2 - t2], [xc, y2], [xc - t2, y2 - t2], [xc - t2, y1 + t2]], on)
            }
            function digit(x, n) {
                var p = pats[n]; if (!p) return
                var pad = t * 0.6
                var xl = x + pad, xr = x + dw - pad, yt = y + pad, ym = y + dh / 2, ybb = y + dh - pad
                hseg(xl, xr, yt, p[0]); vseg(xr, yt, ym, p[1]); vseg(xr, ym, ybb, p[2])
                hseg(xl, xr, ybb, p[3]); vseg(xl, ym, ybb, p[4]); vseg(xl, yt, ym, p[5]); hseg(xl, xr, ym, p[6])
            }
            function colon(x) {
                c.fillStyle = onC; var r = colonW * 0.15, cx = x + colonW / 2
                var y1 = y + dh * 0.36, y2 = y + dh * 0.64
                c.beginPath(); c.arc(SX(cx, y1), y1, r, 0, 6.2832); c.fill()
                c.beginPath(); c.arc(SX(cx, y2), y2, r, 0, 6.2832); c.fill()
            }

            // Layout: hour digits, colon, minute digits, then a small AM/PM.
            var ampmSize = H * Number(T.val(clk.el, "ampmSize", 0.30))
            c.font = "bold " + ampmSize + "px sans-serif"; c.textBaseline = "top"
            var ampmTxt = pm ? "PM" : "AM", ampmW = c.measureText(ampmTxt).width, ampmGap = dw * 0.42
            var totalW = hs.length * dw + (hs.length - 1) * gap + gap + colonW + gap
                       + ms.length * dw + (ms.length - 1) * gap + ampmGap + ampmW + slant * dh
            var x = (W - totalW) / 2

            for (var i = 0; i < hs.length; i++) { digit(x, hs.charCodeAt(i) - 48); x += dw + gap }
            colon(x); x += colonW + gap
            for (var j = 0; j < ms.length; j++) { digit(x, ms.charCodeAt(j) - 48); x += dw + gap }
            x += ampmGap - gap
            c.fillStyle = ampmC; c.fillText(ampmTxt, x, y + dh * 0.06)
        }
    }
}
