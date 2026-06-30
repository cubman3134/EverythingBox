// Particle field element. Native QtQuick.Particles renders through GPU scene-graph materials that Qt Quick's
// software backend (which the themed view uses, so it can coexist with the app's libmpv video widget) does
// NOT implement - so it would draw nothing here. This is a software-friendly field built from plain animated
// items (only transforms + opacity, which the software renderer handles): a set of small dots driven by a
// looping per-particle phase, with presets for the common looks. Deterministic per index (no reseed flicker).
//
// Theme keys: preset ("snow"|"rain"|"embers"|"stars"|"bokeh"|"dust"), count, color, dotSize (fraction of the
// element height), speed (multiplier), image (optional - drawn instead of a dot). The element's own `opacity`
// (a layout key) dims the whole field. NOTE: dotSize/speed are dedicated keys - the layout-reserved `size`
// ([w,h]) and `opacity` are consumed by the host cell, so particle-specific knobs must not reuse those names.
import QtQuick
import "../Theme.js" as T

Item {
    id: field
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property string preset: T.val(el, "preset", "snow")
    readonly property int count: Math.max(0, Math.min(400, Number(T.val(el, "count", 60))))
    readonly property color color: T.val(el, "color", preset === "embers" ? "#FF8A3D" : "#FFFFFF")
    readonly property real baseSize: Number(T.val(el, "dotSize", preset === "bokeh" ? 0.03 : 0.007)) * Math.max(1, height)
    readonly property real speed: Math.max(0.05, Number(T.val(el, "speed", 1.0)))
    readonly property real maxOpacity: preset === "stars" ? 0.95 : 0.7
    readonly property string image: T.val(el, "image", "")
    readonly property bool rises: preset === "embers"
    readonly property bool twinkles: preset === "stars"

    clip: true

    // frac(sin(i)*k) - a cheap stable hash giving a pseudo-random value in [0,1) per particle index.
    function rnd(i, a, b) { var v = Math.sin(i * a + b) * 43758.5453; return v - Math.floor(v) }

    Repeater {
        model: field.count
        delegate: Item {
            id: p
            required property int index
            readonly property real rx: field.rnd(index, 12.9898, 1.7)   // column
            readonly property real ry: field.rnd(index, 78.233, 2.3)    // phase offset / row
            readonly property real rs: field.rnd(index, 3.1719, 0.9)    // size / speed jitter

            readonly property real sz: field.baseSize * (0.5 + rs)
            // Looping phase 0..1; combined with ry it gives staggered, seamless wrap (phase 1 == phase 0).
            property real phase
            NumberAnimation on phase {
                from: 0; to: 1; loops: Animation.Infinite; running: true
                duration: ((field.preset === "rain" ? 1400 : field.preset === "bokeh" ? 14000 : 7000)
                           * (0.6 + p.rs * 0.9)) / field.speed
            }
            // For stars, a separate slower phase drives the twinkle.
            property real tw
            NumberAnimation on tw { from: 0; to: 1; loops: Animation.Infinite; running: field.twinkles; duration: (1600 + p.rs * 2600) }

            readonly property real yFrac: field.rises ? ((p.ry - phase + 1) % 1) : ((p.ry + phase) % 1)
            readonly property real sway: (field.preset === "snow" || field.preset === "dust" || field.preset === "bokeh")
                                         ? Math.sin((phase + p.rx) * 6.2832) * field.width * 0.03 : 0

            width: sz; height: field.preset === "rain" ? sz * 5 : sz
            x: p.rx * field.width + sway
            y: field.twinkles ? p.ry * field.height : (yFrac * (field.height + height) - height)
            opacity: field.twinkles ? field.maxOpacity * (0.25 + 0.75 * (0.5 + 0.5 * Math.sin(tw * 6.2832)))
                   : field.rises   ? field.maxOpacity * (0.2 + 0.8 * yFrac)              // embers fade as they rise
                                   : field.maxOpacity

            // The dot (or a custom image). A circle for most presets; a thin streak for rain.
            Rectangle {
                anchors.fill: parent
                visible: field.image === ""
                radius: field.preset === "rain" ? parent.width * 0.5 : parent.width * 0.5
                color: field.color
            }
            Image {
                anchors.fill: parent
                visible: field.image !== ""
                source: (field.image !== "" && field.host) ? field.host.resolve(field.image) : ""
                fillMode: Image.PreserveAspectFit
                smooth: true
            }
        }
    }
}
