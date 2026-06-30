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

    property real phase
    NumberAnimation on phase { from: 0; to: 6.2832; duration: 9000 / wave.speed; loops: Animation.Infinite; running: true }

    Repeater {
        model: wave.bands * wave.cols
        delegate: Rectangle {
            required property int index
            readonly property int band: Math.floor(index / wave.cols)
            readonly property int col: index % wave.cols
            readonly property real fx: col / (wave.cols - 1)
            readonly property real yTop: wave.height * (0.28 + band * 0.16)
                                         + Math.sin(fx * 6.2832 * 1.4 + wave.phase + band * 0.9) * wave.amp * (1 - band * 0.18)
                                         + Math.sin(fx * 6.2832 * 0.5 - wave.phase * 0.6 + band) * wave.amp * 0.35
            x: fx * wave.width
            width: wave.width / wave.cols + 1.5      // slight overlap so bars read as one filled band
            y: yTop
            height: wave.height - yTop
            color: Qt.rgba(wave.color.r, wave.color.g, wave.color.b, Math.max(0.03, 0.12 - band * 0.025))
        }
    }
}
