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
    // Direct-file candidates only (YouTube ids aren't directly playable). Tried in order: a url that mpv
    // reports dead (404, unsupported, dropped stream) advances clipIdx to the next; when they're all dead
    // the element falls back to plain artwork with no play badge.
    readonly property var playables: {
        var out = []
        for (var i = 0; i < videos.length; i++) {
            var u = String(videos[i])
            if (u.indexOf("youtube") < 0 && u.indexOf("youtu.be") < 0) out.push(u)
        }
        return out
    }
    property int clipIdx: 0
    onVideosChanged: clipIdx = 0 // new item -> start over with its first candidate

    // The "Video previews" setting + snap volume, read off the `videoPreview` context bridge (ThemeEngine
    // registers it; issue #55). typeof-guarded so an element instantiated without the bridge (a bare element
    // probe) defaults to enabled+muted rather than throwing a ReferenceError. Bound, so toggling the setting
    // re-evaluates live: disabling collapses playUrl to "" (stops the clip, shows the Ken Burns still).
    readonly property bool previewsEnabled: (typeof videoPreview !== 'undefined' && videoPreview)
                                            ? videoPreview.enabled : true
    readonly property int snapVolume: (typeof videoPreview !== 'undefined' && videoPreview)
                                      ? videoPreview.volume : 0
    onSnapVolumeChanged: if (player) player.volume = snapVolume

    readonly property string playUrl: (!previewsEnabled || el.preview === false || clipIdx >= playables.length)
                                      ? "" : String(playables[clipIdx])

    // Stills to animate, best-first: an explicit binding/role, then every art role in preference order. A
    // candidate LIST (not just the best) so a url that fails to load (404, dead host, undecodable) falls
    // through to the next role we do have, instead of leaving the frame black.
    readonly property var stillCandidates: {
        var out = []
        var s = T.imageUrl(el, ctx, host)   // per-branch rule; see Image.qml
        if (s) out.push(s)
        // Box/poster art is preferred as the still; screenshots are the LAST resort (video is the priority).
        var order = hasVideo ? ["hero", "poster", "box", "fanart", "background", "thumb", "image", "screenshot"]
                             : ["poster", "box", "hero", "fanart", "thumb", "image", "screenshot"]
        for (var i = 0; i < order.length; i++) {
            var u = T.artUrl(ctx, order[i])
            if (u) {
                var r = host ? host.contentUrl(u) : u
                if (out.indexOf(r) < 0) out.push(r)
            }
        }
        return out
    }
    property int stillIdx: 0
    onStillCandidatesChanged: stillIdx = 0 // new item -> back to its best still
    readonly property string still: stillIdx < stillCandidates.length ? String(stillCandidates[stillIdx]) : ""

    property bool playing: false   // a real clip is on screen (hides the Ken Burns still)
    property var player: null      // the MpvPreview, created lazily/guarded

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Number(T.val(el, "radius", 8))
        color: "#0C0E12"
        clip: true // keep the zoomed/panned image (or video) inside the rounded frame

        // No loadable still and no clip on screen: a soft accent panel with the item's title, instead of the
        // bare near-black frame (the "black screen" a console with a dead thumbnail used to get).
        Rectangle {
            anchors.fill: parent
            visible: !root.playing && (root.still === "" || poster.status === Image.Error)
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#1A2030" }
                GradientStop {
                    position: 1.0
                    color: (root.ctx && root.ctx.selected && root.ctx.selected.accent)
                           ? Qt.darker(String(root.ctx.selected.accent), 1.6) : "#232A3C"
                }
            }
            Text {
                anchors.centerIn: parent; width: parent.width * 0.8
                text: (root.ctx && root.ctx.selected && root.ctx.selected.title) ? root.ctx.selected.title : ""
                color: Qt.rgba(1, 1, 1, 0.85); font.bold: true
                font.pixelSize: Math.max(13, parent.height * 0.14)
                horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                maximumLineCount: 2; elide: Text.ElideRight
            }
        }

        Image {
            id: poster
            anchors.fill: parent
            source: root.still
            fillMode: Image.PreserveAspectCrop
            // Cap the DECODE resolution to the panel height (not the source's native pixels). Provider hero/
            // background art can be enormous — some blow Qt's 256MB decode limit, which rejects the image
            // (Image.Error), stalls the UI for seconds while it tries, and then cascades stillIdx++ through
            // every oversized candidate (the "binding loop" churn). Decoding straight to display size fixes
            // all three. asynchronous keeps the decode off the render-critical path so scrolling stays smooth.
            sourceSize.height: 1080
            asynchronous: true
            // A failed candidate advances to the next role we have; when they're all dead `still` collapses
            // to "" and the accent panel above takes over. Ready frames render exactly as before.
            onStatusChanged: if (status === Image.Error) root.stillIdx++
            visible: status === Image.Ready && !root.playing
            opacity: 0.9
            // The still is STATIC. A Ken Burns pan/zoom (infinite scale + translate) lived here, but the themed
            // UI renders on Qt Quick's CPU software backend (main.cpp forces it — the GPU path conflicts with the
            // libmpv video widget), and continuously re-rasterizing a full-panel image every frame in software
            // burned ~1.5 cores at idle and made every shelf lag. A trailer, when one exists, still fades in over
            // this still and animates via mpv's own decode — only the always-on artwork drift is dropped.
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
    // Created lazily and guarded: if the EB type isn't registered (e.g. the headless theme probe) or mpv
    // can't open the url, `player` stays without frames and the Ken Burns still keeps showing.
    function ensurePlayer() {
        if (player || playUrl === "") return
        try {
            player = Qt.createQmlObject(
                'import QtQuick; import EB 1.0; MpvPreview { anchors.fill: parent }', frame, "mpvPreview")
            player.volume = root.snapVolume // muted by default (0); the player reports "audible" only when >0
            player.playingChanged.connect(function() { root.playing = player.playing })
            // A dead clip (mpv error before any frame): move on to the next candidate — or, none left,
            // playUrl collapses to "" and the ▶ badge disappears (plain artwork, no dead play button).
            player.failedChanged.connect(function() { if (player.failed) root.clipIdx++ })
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
