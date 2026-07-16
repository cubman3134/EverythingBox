// Video / preview element. Providers now supply real preview/trailer clips (selected.videos), but the themed
// view runs on Qt Quick's SOFTWARE backend so it can coexist with the app's libmpv video widget, and QML
// VideoOutput does not render reliably there. So the DEFAULT is a robust, always-works "live preview": a slow
// Ken Burns (zoom + drift) over the best available still — a video thumbnail / hero / screenshot when a clip
// exists (with a ▶ trailer badge), else the poster — which reads as motion without the backend risk.
//
// A theme (or a build where the QtMultimedia QML module is present and the backend cooperates) can opt into a
// real-playback ATTEMPT with `"tryPlayback": true`: we create a MediaPlayer/VideoOutput at runtime inside a
// try/catch, so a missing module or a backend that can't render simply falls back to the Ken Burns still
// instead of breaking the element. The video URLs are fully plumbed either way for a future GPU path.
import QtQuick
import "../Theme.js" as T

Item {
    id: root
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property var videos: T.mediaList(ctx, "videos")
    readonly property bool hasVideo: videos.length > 0
    // Best still to animate: an explicit binding/role, else a video-ish still (hero/screenshot/fanart) when a
    // clip exists, else the poster/box. Always something to show.
    readonly property string still: {
        var s = host ? host.resolve(T.imageSource(el, ctx)) : ""
        if (s) return s
        var order = hasVideo ? ["hero", "screenshot", "fanart", "background", "poster", "box"]
                             : ["poster", "box", "hero", "screenshot"]
        for (var i = 0; i < order.length; i++) {
            var u = T.artUrl(ctx, order[i])
            if (u) return host ? host.resolve(u) : u
        }
        return ""
    }

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

            // Gentle, continuous motion so a still reads as a "preview". Runs once the image is ready; the
            // alternating pan + zoom loop indefinitely and restart with each new selection source.
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

        // Play badge: filled when a real clip exists, hollow otherwise. Drawn, so it needs no font glyph.
        Rectangle {
            anchors.centerIn: parent
            visible: !root.playing
            width: Math.min(parent.width, parent.height) * 0.22
            height: width; radius: width / 2
            color: root.hasVideo ? Qt.rgba(0.85, 0.1, 0.1, 0.55) : Qt.rgba(0, 0, 0, 0.45)
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

    // --- optional real-playback attempt (opt-in; guarded so it can never break the element) ---------------
    property bool playing: false
    property var player: null
    function tryRealPlayback() {
        if (!(el.tryPlayback === true) || !hasVideo) return
        try {
            // Runtime-create the QtMultimedia objects so a missing module throws here (caught) instead of
            // failing the whole element at import time.
            var out = Qt.createQmlObject(
                'import QtQuick; import QtMultimedia; VideoOutput { anchors.fill: parent; fillMode: VideoOutput.PreserveAspectCrop }',
                frame, "themedVideoOut")
            var mp = Qt.createQmlObject('import QtMultimedia; MediaPlayer { loops: MediaPlayer.Infinite; audioOutput: null }',
                                        root, "themedVideoPlayer")
            mp.videoOutput = out
            mp.source = host ? host.resolve(videos[0]) : videos[0]
            // Only commit to "playing" (which hides the Ken Burns still) once frames actually arrive; if the
            // software backend can't render, we never flip and the still keeps showing.
            mp.onPlayingChanged = function() { if (mp.playbackState === MediaPlayer.PlayingState) root.playing = true }
            mp.play()
            player = mp
        } catch (e) {
            root.playing = false // no module / backend can't do it -> stay on the robust Ken Burns still
        }
    }
    onStillChanged: { if (player) { try { player.stop(); player.destroy() } catch (e) {} player = null; playing = false } tryRealPlayback() }
    Component.onCompleted: tryRealPlayback()
}
