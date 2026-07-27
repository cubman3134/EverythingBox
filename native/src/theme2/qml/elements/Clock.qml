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

            // Digit height fits BOTH axes: H:MM + AM/PM needs ~3.9x the digit height in width, and a
            // portrait phone's clock box is width-bound — without the clamp the digits run off the box.
            var dh = Math.min(H * 0.82, W / 4.3), dw = dh * 0.54
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

            // AM/PM: stroke-drawn letters in the SAME slanted segment style as the digits (a plain
            // canvas font read as a different typeface and, being sized off the element height, was
            // the one run that could still overflow the box). Everything below scales with dh, so the
            // whole unit has an EXACT width and centers as one piece.
            var lh = dh * 0.38, lw = lh * 0.72, lgap = lh * 0.24, ampmGap = dw * 0.42
            var ampmW = 2 * lw + lgap
            function letter(ch, x0, y0) {
                c.strokeStyle = ampmC; c.lineWidth = Math.max(2, t * 0.7); c.lineCap = "round"; c.lineJoin = "round"
                function L(pts) {
                    c.beginPath(); c.moveTo(SX(x0 + pts[0][0] * lw, y0 + pts[0][1] * lh), y0 + pts[0][1] * lh)
                    for (var q = 1; q < pts.length; q++) c.lineTo(SX(x0 + pts[q][0] * lw, y0 + pts[q][1] * lh), y0 + pts[q][1] * lh)
                    c.stroke()
                }
                if (ch === "A")      { L([[0, 1], [0.5, 0], [1, 1]]); L([[0.2, 0.62], [0.8, 0.62]]) }
                else if (ch === "P") { L([[0, 1], [0, 0], [0.85, 0], [0.85, 0.5], [0, 0.5]]) }
                else if (ch === "M") { L([[0, 1], [0, 0], [0.5, 0.5], [1, 0], [1, 1]]) }
            }

            // Exact unit width (all terms scale with dh) -> re-derive dh so the unit always fits, then center.
            function unitW(dhv) {
                var dwv = dhv * 0.54, gv = dwv * 0.22, cwv = dwv * 0.52
                var lhv = dhv * 0.38, lwv = lhv * 0.72
                return hs.length * dwv + (hs.length - 1) * gv + gv + cwv + gv
                     + ms.length * dwv + (ms.length - 1) * gv + dwv * 0.42 + (2 * lwv + lhv * 0.24) + slant * dhv
            }
            if (unitW(dh) > W) {
                dh = dh * W / unitW(dh)
                dw = dh * 0.54; t = dw * Number(T.val(clk.el, "thickness", 0.16)); t2 = t / 2
                gap = dw * 0.22; colonW = dw * 0.52; y = (H - dh) / 2; yb = y + dh
                lh = dh * 0.38; lw = lh * 0.72; lgap = lh * 0.24; ampmGap = dw * 0.42; ampmW = 2 * lw + lgap
            }
            var totalW = unitW(dh)
            var x = (T.val(clk.el, "align", "center") === "left") ? 0 : (W - totalW) / 2

            for (var i = 0; i < hs.length; i++) { digit(x, hs.charCodeAt(i) - 48); x += dw + gap }
            colon(x); x += colonW + gap
            for (var j = 0; j < ms.length; j++) { digit(x, ms.charCodeAt(j) - 48); x += dw + gap }
            x += ampmGap - gap
            var ly = y + dh * 0.08
            letter(pm ? "P" : "A", x, ly); letter("M", x + lw + lgap, ly)
        }
    }
}
