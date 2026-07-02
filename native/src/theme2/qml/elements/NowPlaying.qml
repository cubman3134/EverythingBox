// NowPlaying element: the current background-music track (host.nowPlaying). Right/left/centre aligned when it
// fits; if the name is wider than the box it scrolls sideways (pause, ease across, pause, jump back). Theme
// keys: color, fontSize (fraction of view height), align, bold, prefix (default "♪  ").
import QtQuick
import "../Theme.js" as T

Item {
    id: np
    property var el: ({})
    property var ctx: ({})
    property var host
    property string txt: (host && host.nowPlaying) ? host.nowPlaying : ""
    clip: true
    visible: txt.length > 0

    readonly property string align: T.val(el, "align", "left")

    Text {
        id: label
        text: (np.el && np.el.prefix !== undefined ? np.el.prefix : "♪  ") + np.txt
        color: T.val(np.el, "color", "#FFFFFF")
        font.pixelSize: Math.max(1, Number(T.val(np.el, "fontSize", 0.024)) * (np.host ? np.host.height : 720))
        font.bold: np.el.bold === true
        height: np.height
        verticalAlignment: Text.AlignVCenter

        readonly property bool overflow: paintedWidth > np.width + 1
        readonly property real aligned: np.align === "right"  ? np.width - paintedWidth
                                      : np.align === "center" ? (np.width - paintedWidth) / 2 : 0
        property real sx: 0
        x: overflow ? sx : aligned // scroll when too wide, otherwise honour the alignment

        onTextChanged: restart()
        onOverflowChanged: restart()
        function restart() { marquee.stop(); sx = 0; if (overflow && np.visible) marquee.start() }

        SequentialAnimation {
            id: marquee; loops: Animation.Infinite; running: false
            PauseAnimation { duration: 1600 }
            NumberAnimation { target: label; property: "sx"; to: Math.min(0, np.width - label.paintedWidth)
                              duration: Math.max(1500, (label.paintedWidth - np.width) * 30); easing.type: Easing.Linear }
            PauseAnimation { duration: 1400 }
            NumberAnimation { target: label; property: "sx"; to: 0; duration: 500; easing.type: Easing.InOutQuad }
        }
    }
}
