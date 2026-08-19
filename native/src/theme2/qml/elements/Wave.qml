// Wave element - the flowing translucent bands behind a PlayStation-style XMB. Built from plain animated
// Rectangles (NOT a Canvas: a continuously-repainted Canvas blanks the scene under the software render
// backend the themed view uses). Each band is a row of thin vertical bars whose tops follow a sine of
// (x + phase); animating phase makes the crest travel. Theme keys: color, bands (1-4), amplitude (fraction
// of the element height), speed.
import QtQuick
import "../Theme.js" as T

Item {
    id: wave
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property color color: T.val(el, "color", "#3A6FB0")
    readonly property int bands: Math.max(1, Math.min(4, Number(T.val(el, "bands", 3))))
    readonly property real amp: Number(T.val(el, "amplitude", 0.06)) * Math.max(1, height)
    readonly property real speed: Math.max(0.1, Number(T.val(el, "speed", 1.0)))
    // Bars per band. Kept modest by default: the themed view is software-rendered, and a very high bar count
    // stacked with other heavy elements (e.g. an xmb cross) can exceed the renderer's budget and blank.
    readonly property int cols: Math.max(6, Math.min(80, Number(T.val(el, "segments", 24))))
    clip: true

    // A normalised 0..1 phase. Each travelling sine multiplies (t * tau) by a WHOLE number, so every component
    // completes a whole number of cycles per loop and the wrap from 1 back to 0 is perfectly seamless.
    readonly property real tau: 6.283185307179586
    // LOW-RATE drift. The original 60fps NumberAnimation re-laid-out and re-blended all bands*cols translucent
    // bars every frame on the CPU software backend — four stacked waves burned ~1.4 cores at idle and starved
    // the shelves. The wave's crests travel slowly (a 9s loop), so a ~12fps phase tick looks just as fluid at a
    // fraction of the cost, and it runs CONTINUOUSLY — an earlier pause-while-navigating variant read as "the
    // wave froze" the moment anyone browsed. Wall-clock phase (not per-tick increments) keeps the speed exact.
    property real t: 0.18
    Timer {
        interval: 80; repeat: true
        running: wave.visible
        onTriggered: wave.t = (Date.now() % (9000 / wave.speed)) / (9000 / wave.speed)
    }

    Repeater {
        model: wave.bands * wave.cols
        delegate: Rectangle {
            required property int index
            readonly property int band: Math.floor(index / wave.cols)
            readonly property int col: index % wave.cols
            readonly property real fx: col / (wave.cols - 1)
            readonly property real yTop: wave.height * (0.28 + band * 0.16)
                                         + Math.sin(fx * wave.tau * 1.4 + wave.t * wave.tau + band * 0.9) * wave.amp * (1 - band * 0.18)
                                         + Math.sin(fx * wave.tau * 0.5 - wave.t * wave.tau * 2 + band) * wave.amp * 0.35
            x: fx * wave.width
            width: wave.width / wave.cols + 1.5      // slight overlap so bars read as one filled band
            y: yTop
            height: wave.height - yTop
            color: Qt.rgba(wave.color.r, wave.color.g, wave.color.b, Math.max(0.03, 0.12 - band * 0.025))
        }
    }
}
