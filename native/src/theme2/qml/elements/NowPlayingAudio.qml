// NowPlayingAudio element: the WHOLE themed audiobook now-playing page (Plan B1, Task 5). Audio has no video,
// so there is nothing to composite — this element IS the surface, drawn over the theme's background while mpv
// plays invisibly behind it (the classic player page is never shown). It is the single element the theme.json
// `nowplayingAudio` view places full-screen; everything it shows is host-fed:
//
//   * cover art        — ctx.selected art roles (poster / thumb, then a graceful placeholder), like the detail
//                        poster; the host pushes the now-playing item into host.audioData → ctx.selected.
//   * title / author   — ctx.selected.title / .subtitle.
//   * status line      — "Track i of n" (from host.audioQueueCurrent / host.audioQueue.length) or a chapter hint.
//   * progress bar     — host.audioPosition / host.audioDuration (seconds; the host throttles the feed to ~1 Hz).
//   * transport strip  — host.audioTransportList (the `transport` nav zone). Focus ring from host.audioZone ===
//                        "transport" && host.audioTransportIndex. A click / the view's Enter emits
//                        host.audioTransportRequested(verb), which the host maps to the player/session verb.
//   * queue list       — host.audioQueue titles (the `queue` nav zone). Focus from host.audioZone === "queue"
//                        && host.audioQueueIndex; the currently-playing row (host.audioQueueCurrent) is marked.
//                        Activating a row emits host.audioQueueActivateRequested(row) → session_->playIndex(row).
//
// Keyboard / controller selection is arbitrated by the NavGraph (transport ↔ queue zones); this element only
// DRAWS the cursor the host writes back into audioZone / audioTransportIndex / audioQueueIndex.
import QtQuick
import "../Theme.js" as T

Item {
    id: page
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property var sel: (ctx && ctx.selected) ? ctx.selected : ({})
    readonly property var verbs: (host && host.audioTransportList) ? host.audioTransportList : []
    readonly property bool transportFocused: !!(host && host.audioZone === "transport")
    readonly property bool queueFocused: !!(host && host.audioZone === "queue")
    readonly property int transportIdx: host ? host.audioTransportIndex : 0
    readonly property int queueIdx: host ? host.audioQueueIndex : 0
    readonly property int queueCurrent: host ? host.audioQueueCurrent : 0
    readonly property var queue: (host && host.audioQueue) ? host.audioQueue : []
    readonly property real pos: host ? host.audioPosition : 0
    readonly property real dur: host ? host.audioDuration : 0
    readonly property bool paused: !!(host && host.audioPaused)
    readonly property real spd: host ? host.audioSpeed : 1.0

    // Theme-tunable accents (each with a sensible default so a bare view still reads well).
    readonly property color accent:   T.val(el, "accent", "#E07A2E")
    readonly property color fg:       T.val(el, "color", "#FFFFFF")
    readonly property color fgDim:    T.val(el, "dimColor", "#AEB4C2")
    readonly property color panelCol: T.val(el, "panelColor", "#161A20")
    readonly property real  h1: Math.max(1, Number(T.val(el, "titleSize", 0.05)) * (host ? host.height : 720))
    readonly property real  h2: Math.max(1, Number(T.val(el, "subSize", 0.028)) * (host ? host.height : 720))
    readonly property real  h3: Math.max(1, Number(T.val(el, "metaSize", 0.024)) * (host ? host.height : 720))

    function fmtTime(s) {
        if (!s || s < 0 || isNaN(s)) s = 0
        var t = Math.floor(s)
        var h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), sec = t % 60
        var mm = (h > 0 && m < 10 ? "0" : "") + m
        var ss = (sec < 10 ? "0" : "") + sec
        return (h > 0 ? (h + ":") : "") + mm + ":" + ss
    }
    // verb -> a compact transport glyph (play/pause + speed reflect live state).
    function glyphFor(v) {
        if (v === "prevTrack")   return "⏮"
        if (v === "prevChapter") return "«"
        if (v === "seekBack")    return "⏪"
        if (v === "playPause")   return paused ? "▶" : "⏸"
        if (v === "seekFwd")     return "⏩"
        if (v === "nextChapter") return "»"
        if (v === "nextTrack")   return "⏭"
        if (v === "speed")       return spd.toFixed(spd < 10 ? 2 : 1).replace(/0$/, "").replace(/\.$/, "") + "×"
        return v
    }

    // --- cover art (left) -----------------------------------------------------------------------------
    Item {
        id: coverBox
        x: page.width * 0.06
        y: page.height * 0.5 - height / 2
        width: Math.min(page.width * 0.30, page.height * 0.52)
        height: width
        Rectangle {                              // placeholder / frame behind the art
            anchors.fill: parent
            radius: 14
            color: page.panelCol
            visible: cover.status !== Image.Ready
            Text {
                anchors.centerIn: parent
                text: "♪"
                color: page.accent
                font.pixelSize: parent.height * 0.4
            }
        }
        Image {
            id: cover
            anchors.fill: parent
            asynchronous: true
            fillMode: Image.PreserveAspectCrop
            source: host ? host.resolve(T.imageSource({ role: "poster", fallback: "thumb" }, page.ctx)) : ""
            layer.enabled: true
        }
    }

    // --- title / author / status (right of the cover) -------------------------------------------------
    Column {
        id: info
        x: coverBox.x + coverBox.width + page.width * 0.05
        width: page.width * 0.52
        y: coverBox.y + page.height * 0.02
        spacing: page.height * 0.012
        Text {
            width: parent.width
            text: (page.sel && page.sel.title) ? page.sel.title : ""
            color: page.fg; font.pixelSize: page.h1; font.bold: true
            elide: Text.ElideRight; wrapMode: Text.WordWrap; maximumLineCount: 2
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: (page.sel && page.sel.subtitle) ? page.sel.subtitle : ""
            color: page.fgDim; font.pixelSize: page.h2
            elide: Text.ElideRight; maximumLineCount: 1
        }
        Text {
            width: parent.width
            text: {
                var n = page.queue.length
                if (n > 1) return "Track " + (page.queueCurrent + 1) + " of " + n
                return page.paused ? "Paused" : "Now playing"
            }
            color: page.accent; font.pixelSize: page.h3; font.bold: true
        }
    }

    // --- progress bar ---------------------------------------------------------------------------------
    Item {
        id: progress
        x: info.x
        width: info.width
        y: page.height * 0.63
        height: page.height * 0.06
        Text {
            id: elapsed
            anchors.left: parent.left; anchors.verticalCenter: bar.verticalCenter
            text: page.fmtTime(page.pos); color: page.fgDim; font.pixelSize: page.h3
        }
        Text {
            id: total
            anchors.right: parent.right; anchors.verticalCenter: bar.verticalCenter
            text: page.fmtTime(page.dur); color: page.fgDim; font.pixelSize: page.h3
        }
        Rectangle {
            id: bar
            anchors.left: elapsed.right; anchors.right: total.left
            anchors.leftMargin: page.width * 0.012; anchors.rightMargin: page.width * 0.012
            anchors.top: parent.top; anchors.topMargin: parent.height * 0.35
            height: Math.max(4, page.height * 0.010)
            radius: height / 2
            color: Qt.rgba(1, 1, 1, 0.18)
            Rectangle {
                height: parent.height; radius: parent.radius
                width: parent.width * (page.dur > 0 ? Math.max(0, Math.min(1, page.pos / page.dur)) : 0)
                color: page.accent
                Behavior on width { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
            }
        }
    }

    // --- transport strip (the `transport` nav zone) ---------------------------------------------------
    Row {
        id: strip
        x: info.x
        y: page.height * 0.74
        spacing: page.width * 0.012
        Repeater {
            model: page.verbs
            delegate: Rectangle {
                id: btn
                required property var modelData
                required property int index
                readonly property bool isSpeed: modelData === "speed"
                readonly property bool isPlay:  modelData === "playPause"
                readonly property bool focused: page.transportFocused && page.transportIdx === index
                height: page.height * 0.085
                width: isSpeed ? height * 1.6 : (isPlay ? height * 1.2 : height)
                radius: height / 2
                color: isPlay ? page.accent : Qt.rgba(1, 1, 1, 0.08)
                border.width: 2
                border.color: isPlay ? Qt.darker(page.accent, 1.25) : Qt.rgba(1, 1, 1, 0.14)
                scale: focused ? 1.1 : (ma.pressed ? 0.94 : (ma.containsMouse ? 1.05 : 1.0))
                Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                Rectangle {                       // keyboard / controller focus ring
                    visible: btn.focused
                    anchors.fill: parent; anchors.margins: -Math.max(3, parent.height * 0.12)
                    radius: parent.height / 2 + 3
                    color: "transparent"; border.width: Math.max(2, parent.height * 0.10); border.color: "#2FA1E6"
                }
                Text {
                    anchors.centerIn: parent
                    text: page.glyphFor(btn.modelData)
                    color: btn.isPlay ? "#FFFFFF" : page.fg
                    font.bold: true
                    font.pixelSize: btn.isSpeed ? page.h3 : parent.height * 0.44
                }
                MouseArea {
                    id: ma
                    anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (page.host) {
                            if (page.host.forceActiveFocus) page.host.forceActiveFocus()
                            page.host.audioTransportRequested(btn.modelData)
                        }
                    }
                }
            }
        }
    }

    // --- queue list (the `queue` nav zone) ------------------------------------------------------------
    Rectangle {
        id: queuePanel
        visible: page.queue.length > 0
        x: coverBox.x
        y: page.height * 0.74
        width: coverBox.width + page.width * 0.02
        height: page.height * 0.22
        radius: 12
        color: page.panelCol
        clip: true
        ListView {
            id: queueList
            anchors.fill: parent; anchors.margins: parent.height * 0.06
            interactive: false
            model: page.queue
            currentIndex: page.queueIdx
            spacing: 2
            // Keep the highlighted row in view as the cursor steps (the model owns the clamp; we just scroll).
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
            delegate: Rectangle {
                required property var modelData
                required property int index
                width: queueList.width
                height: page.h3 * 2.0
                radius: 6
                readonly property bool focused: page.queueFocused && page.queueIdx === index
                readonly property bool current: index === page.queueCurrent
                color: focused ? Qt.rgba(0.18, 0.63, 0.9, 0.30) : "transparent"
                border.width: focused ? 2 : 0
                border.color: "#2FA1E6"
                Text {
                    anchors.left: parent.left; anchors.leftMargin: page.width * 0.008
                    anchors.right: parent.right; anchors.rightMargin: page.width * 0.006
                    anchors.verticalCenter: parent.verticalCenter
                    text: (parent.current ? "▶  " : "") + parent.modelData
                    color: parent.current ? page.accent : page.fg
                    font.pixelSize: page.h3
                    font.bold: parent.current
                    elide: Text.ElideRight; maximumLineCount: 1
                }
                MouseArea {
                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (page.host) {
                            if (page.host.forceActiveFocus) page.host.forceActiveFocus()
                            page.host.audioQueueActivateRequested(parent.index)
                        }
                    }
                }
            }
        }
    }
}
