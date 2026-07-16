// Video / preview element. When a provider supplied a real, directly-playable clip (selected.videos), this
// streams it in-menu via MpvPreview — a libmpv software-render item (RetroBat/EmulationStation style: mpv
// decodes the clip and hands us frames we paint ourselves, which works on Qt Quick's software backend where
// QML VideoOutput doesn't). Until the first frame arrives — and always, when there's no playable clip — it
// shows a robust Ken Burns pan/zoom over the best available still (a video-ish frame + a ▶ badge when a clip
// exists, else the poster). So it degrades gracefully: no clip, no module, or a backend that can't render ->
// the still preview; a real clip -> real playback fading in over it.
//
// YouTube-id "videos" (IGDB) aren't directly playable, so they're skipped for playback (the badge/still still
// signal that a trailer exists). Set "preview": false on the element to force the still-only behaviour.
import QtQuick
import "../Theme.js" as T

Item {
    id: root
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property var videos: T.mediaList(ctx, "videos")
    readonly property bool hasVideo: videos.length > 0
    function firstPlayable() {
        for (var i = 0; i < videos.length; i++) {
            var u = String(videos[i])
            if (u.indexOf("youtube") < 0 && u.indexOf("youtu.be") < 0) return u // needs a direct file, not YT
        }
        return ""
    }
    readonly property string playUrl: (el.preview === false) ? "" : firstPlayable()

    // Best still to animate: an explicit binding/role, else a video-ish still (hero/screenshot/fanart) when a
    // clip exists, else the poster/box. Always something to show behind a not-yet-playing clip.
    readonly property string still: {
        var s = host ? host.resolve(T.imageSource(el, ctx)) : ""
        if (s) return s
        // Box/poster art is preferred as the still; screenshots are the LAST resort (video is the priority).
        var order = hasVideo ? ["hero", "poster", "box", "fanart", "background", "thumb", "image", "screenshot"]
                             : ["poster", "box", "hero", "fanart", "thumb", "image", "screenshot"]
        for (var i = 0; i < order.length; i++) {
            var u = T.artUrl(ctx, order[i])
            if (u) return host ? host.resolve(u) : u
        }
        return ""
    }

    property bool playing: false   // a real clip is on screen (hides the Ken Burns still)
    property var player: null      // the MpvPreview, created lazily/guarded

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Number(T.val(el, "radius", 8))
        color: "#0C0E12"
        clip: true // keep the zoomed/panned image (or video) inside the rounded frame

        Image {
            id: poster
            anchors.fill: parent
            source: root.still
            fillMode: Image.PreserveAspectCrop
            visible: status === Image.Ready && !root.playing
            opacity: 0.9
            transformOrigin: Item.Center
            property real panX
            transform: Translate { x: poster.panX }
            SequentialAnimation on scale {
                running: poster.status === Image.Ready && !root.playing
                loops: Animation.Infinite
                NumberAnimation { from: 1.0; to: 1.12; duration: 5000; easing.type: Easing.InOutSine }
                NumberAnimation { from: 1.12; to: 1.0; duration: 5000; easing.type: Easing.InOutSine }
            }
            SequentialAnimation on panX {
                running: poster.status === Image.Ready && !root.playing
                loops: Animation.Infinite
                NumberAnimation { from: -0.04 * poster.width; to: 0.04 * poster.width; duration: 7000; easing.type: Easing.InOutSine }
                NumberAnimation { from: 0.04 * poster.width; to: -0.04 * poster.width; duration: 7000; easing.type: Easing.InOutSine }
            }
        }

        // A ▶ badge ONLY when there's a directly-playable clip (it starts on its own a moment later, so this
        // is just a brief "trailer loading" cue). No playable video -> it's plain artwork, no dead play button.
        Rectangle {
            anchors.centerIn: parent
            visible: !root.playing && root.playUrl !== ""
            width: Math.min(parent.width, parent.height) * 0.22
            height: width; radius: width / 2
            color: Qt.rgba(0.85, 0.1, 0.1, 0.55)
            border.width: 2; border.color: Qt.rgba(1, 1, 1, 0.9)
            Canvas {
                anchors.centerIn: parent
                width: parent.width * 0.5; height: parent.height * 0.5
                onPaint: {
                    var c = getContext("2d"); c.reset()
                    c.beginPath(); c.moveTo(0, 0); c.lineTo(width, height / 2); c.lineTo(0, height)
                    c.closePath(); c.fillStyle = "white"; c.fill()
                }
            }
        }
    }

    // --- real playback via the libmpv software-render item -------------------------------------------------
    // Created lazily and guarded: if the MMV type isn't registered (e.g. the headless theme probe) or mpv
    // can't open the url, `player` stays without frames and the Ken Burns still keeps showing.
    function ensurePlayer() {
        if (player || playUrl === "") return
        try {
            player = Qt.createQmlObject(
                'import QtQuick; import MMV 1.0; MpvPreview { anchors.fill: parent }', frame, "mpvPreview")
            player.playingChanged.connect(function() { root.playing = player.playing })
        } catch (e) { player = null } // no module / not registered -> stay on the still
    }
    // A short hover-stable delay before streaming, so scrolling quickly past items doesn't load a clip each.
    Timer {
        id: startDelay
        interval: Number(T.val(el, "delay", 700)); repeat: false
        // Hand mpv the RAW url/path (not host.resolve): mpv opens native paths, http and av:// directly, and a
        // naive file:/// url would mangle the spaces/parens in RetroBat filenames.
        onTriggered: { root.ensurePlayer(); if (root.player) root.player.source = root.playUrl }
    }
    onPlayUrlChanged: {
        root.playing = false
        if (player) player.source = ""
        if (playUrl !== "") startDelay.restart(); else startDelay.stop()
    }
    Component.onCompleted: if (playUrl !== "") startDelay.restart()
    Component.onDestruction: if (player) { try { player.source = ""; player.destroy() } catch (e) {} }
}
