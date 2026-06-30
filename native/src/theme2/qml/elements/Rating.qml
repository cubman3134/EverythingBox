// Rating element: a 0..1 value drawn as five stars, filled proportionally, in the themed accent colour.
import QtQuick
import "../Theme.js" as T

Canvas {
    property var el: ({})
    property var ctx: ({})
    property var host
    property real value: {
        var v = Number(T.val(el, "binding", null) ? T.dig(ctx, el.binding) : T.val(el, "value", 0))
        return isNaN(v) ? 0 : Math.max(0, Math.min(1, v))
    }
    property color fill: T.val(el, "color", "#E07A2E")
    property color empty: T.val(el, "emptyColor", Qt.rgba(1, 1, 1, 0.18))
    onValueChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    function star(c, cx, cy, r, col) {
        c.beginPath()
        for (var i = 0; i < 5; i++) {
            var a = (Math.PI / 180) * (-90 + i * 72)
            var ai = a + (Math.PI / 180) * 36
            c.lineTo(cx + r * Math.cos(a), cy + r * Math.sin(a))
            c.lineTo(cx + (r * 0.45) * Math.cos(ai), cy + (r * 0.45) * Math.sin(ai))
        }
        c.closePath(); c.fillStyle = col; c.fill()
    }

    onPaint: {
        var c = getContext("2d")
        c.reset()
        var n = 5, r = Math.min(height, width / n) * 0.45, gap = width / n
        for (var i = 0; i < n; i++) {
            var cx = gap * (i + 0.5), cy = height / 2
            var filledThru = value * n
            star(c, cx, cy, r, (i + 1) <= Math.round(filledThru) ? fill : empty)
        }
    }
}
