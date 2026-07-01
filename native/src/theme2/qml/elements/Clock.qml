// Clock element: the current time as a 7-segment digital display (h:mm) with a small AM/PM. Redrawn once a
// second on a static Canvas (a once-a-second repaint is software-renderer safe). Theme keys: onColor (lit
// segments), offColor (unlit "ghost" segments; default transparent), ampmColor, ampmSize (fraction of the
// element height), thickness (segment thickness, as a fraction of a digit's width).
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
        function seg(c, x1, y1, x2, y2, on, onC, offC, t) {
            c.strokeStyle = on ? onC : offC; c.lineWidth = t; c.lineCap = "round"
            c.beginPath(); c.moveTo(x1, y1); c.lineTo(x2, y2); c.stroke()
        }
        function drawDigit(c, x, y, dw, dh, t, n, onC, offC) {
            var p = pats[n]; if (!p) return
            var pad = t * 0.7
            var xl = x + pad, xr = x + dw - pad, yt = y + pad, ym = y + dh / 2, yb = y + dh - pad
            seg(c, xl, yt, xr, yt, p[0], onC, offC, t)   // a  top
            seg(c, xr, yt, xr, ym, p[1], onC, offC, t)   // b  top-right
            seg(c, xr, ym, xr, yb, p[2], onC, offC, t)   // c  bottom-right
            seg(c, xl, yb, xr, yb, p[3], onC, offC, t)   // d  bottom
            seg(c, xl, ym, xl, yb, p[4], onC, offC, t)   // e  bottom-left
            seg(c, xl, yt, xl, ym, p[5], onC, offC, t)   // f  top-left
            seg(c, xl, ym, xr, ym, p[6], onC, offC, t)   // g  middle
        }
        function drawColon(c, x, y, w, dh, onC) {
            c.fillStyle = onC; var r = w * 0.16, cx = x + w / 2
            c.beginPath(); c.arc(cx, y + dh * 0.36, r, 0, 6.2832); c.fill()
            c.beginPath(); c.arc(cx, y + dh * 0.64, r, 0, 6.2832); c.fill()
        }

        onPaint: {
            var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
            var W = width, H = height
            var onC = T.val(clk.el, "onColor", "#3A4657")
            var offC = T.val(clk.el, "offColor", "#00000000")
            var ampmC = T.val(clk.el, "ampmColor", onC)

            var d = clk.now
            var hr = d.getHours(), mn = d.getMinutes(), pm = hr >= 12
            var hs = String(((hr + 11) % 12) + 1)          // 1..12, no leading zero
            var ms = (mn < 10 ? "0" : "") + mn

            var dh = H * 0.80, dw = dh * 0.52
            var t = dw * Number(T.val(clk.el, "thickness", 0.16))
            var gap = dw * 0.18, colonW = dw * 0.55
            var ampmSize = H * Number(T.val(clk.el, "ampmSize", 0.30))
            c.font = "bold " + ampmSize + "px sans-serif"; c.textBaseline = "top"
            var ampmW = c.measureText(pm ? "PM" : "AM").width
            var ampmGap = dw * 0.4

            var totalW = hs.length * dw + (hs.length - 1) * gap + gap + colonW + gap
                       + ms.length * dw + (ms.length - 1) * gap + ampmGap + ampmW
            var x = (W - totalW) / 2, y = (H - dh) / 2

            for (var i = 0; i < hs.length; i++) { drawDigit(c, x, y, dw, dh, t, hs.charCodeAt(i) - 48, onC, offC); x += dw + gap }
            drawColon(c, x, y, colonW, dh, onC); x += colonW + gap
            for (var j = 0; j < ms.length; j++) { drawDigit(c, x, y, dw, dh, t, ms.charCodeAt(j) - 48, onC, offC); x += dw + gap }
            x += ampmGap - gap
            c.fillStyle = ampmC; c.fillText(pm ? "PM" : "AM", x, y + dh * 0.08)
        }
    }
}
