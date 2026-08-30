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
//   * lyrics panel     — the `lyrics` ELEMENT, loaded (not re-implemented) below the queue when the track has
//                        lyrics. A theme may place that element itself, anywhere; this is where the page puts
//                        it when the theme does not. Selecting one of its lines seeks there (issue #142).
//
// Keyboard / controller selection is arbitrated by the NavGraph (transport ↔ queue ↔ lyrics zones); this
// element only DRAWS the cursor the host writes back into audioZone / audioTransportIndex / audioQueueIndex /
// audioLyricIndex.
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
    // Issue #218: inside a multi-file audiobook `pos` and `dur` are the whole BOOK's, and these two are the
    // span of the part playing within it. Both 0 for every other kind of audio — see ThemeView.qml.
    readonly property real partStart: host ? host.audioPartStart : 0
    readonly property real partEnd: host ? host.audioPartEnd : 0
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
    // verb -> the name of a DRAWN glyph (PlayerIconProvider), or "" for the verbs that are still text.
    //
    // The six transport marks used to be Unicode media characters, which is the one thing a themed surface
    // cannot afford: those characters carry emoji presentation, so the platform painted them out of a colour
    // emoji font in ITS colours and the theme's `color` was ignored — the page asked for near-white and got
    // blue lozenges. Drawn from geometry they are whatever colour this page hands them, on every platform,
    // and they are the same marks the classic player's bar draws.
    function iconNameFor(v) {
        if (v === "prevTrack") return "prevTrack"
        if (v === "seekBack")  return "seekBack"
        if (v === "playPause") return paused ? "play" : "pause"   // the one glyph that reflects live state
        if (v === "seekFwd")   return "seekFwd"
        if (v === "nextTrack") return "nextTrack"
        if (v === "stop")      return "stop"   // #193 inc 3: Back leaves the page now, so STOP is its own verb
        return ""
    }

    // A colour as the six hex digits the provider's URL takes ("#ff9900" -> "ff9900"). Read off the END of
    // the string because Qt spells a non-opaque colour "#aarrggbb", and the provider wants rrggbb.
    function hexOf(c) {
        var s = Qt.color(c).toString()
        return s.substring(s.length - 6)
    }

    // verb -> a compact text glyph, for the verbs that are NOT drawn (speed reflects live state). The
    // chevrons and the lyric pair stay text on purpose: they are ordinary typographic characters, they take
    // the page's colour like any other text, and there is nothing for a drawn shape to add.
    function glyphFor(v) {
        if (v === "prevChapter") return "«"
        if (v === "nextChapter") return "»"
        // The lyric nudge pair (#142). Present only for a track with timed lyrics; the note says WHAT is being
        // shifted, the arrow says which way (◀ = the words arrive earlier).
        if (v === "lyricEarlier") return "♪◀"
        if (v === "lyricLater")   return "♪▶"
        // Strip ALL trailing zeros then a bare dot ("1.00"→"1", "1.50"→"1.5", "1.25" stays) — /0$/ only ate one.
        if (v === "speed")       return spd.toFixed(spd < 10 ? 2 : 1).replace(/0+$/, "").replace(/\.$/, "") + "×"
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
            source: host ? T.imageUrl({ role: "poster", fallback: "thumb" }, page.ctx, host) : ""
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
    // The bar is a SEEK control, not a readout: pressing it jumps there and starts a drag, exactly as every
    // other player has taught people it should. While the drag is live the fill follows the POINTER rather
    // than host.audioPosition — that feed is throttled to ~1 Hz, so binding the fill to it would make the bar
    // snap back under the finger and then catch up a second later. The commit goes down the transport verb
    // channel as "seek:<fraction>" rather than a fourth signal of its own, so the host keeps ONE place that
    // decides what a transport gesture means.
    Item {
        id: progress
        x: info.x
        width: info.width
        y: page.height * 0.63
        height: page.height * 0.06
        property real scrubFrac: -1                       // -1 = not scrubbing; else the pointer's fraction
        readonly property bool scrubbing: scrubFrac >= 0
        readonly property real playFrac: page.dur > 0 ? Math.max(0, Math.min(1, page.pos / page.dur)) : 0
        readonly property real shownFrac: scrubbing ? scrubFrac : playFrac
        // --- the part of the book a drag may land in (issue #218) ------------------------------------
        // A book-scale bar makes a drag mean "somewhere in fifteen hours", and most of that is in files
        // this app is not holding: reaching one means minting its link, which is a fresh resolve of the
        // whole release, and #216 is the issue of one of those taking sixty-five seconds and returning
        // nothing. A scrub that can stop the book a third of the time is worse than one that cannot leave
        // the part, so the knob STOPS at the part's edge — a limit you can see, with the destination
        // readout stopping with it, rather than a gesture that is accepted and then quietly does something
        // else. Crossing a part is what the queue list and the track buttons are for; both already mint.
        readonly property bool partBound: page.dur > 0 && page.partEnd > page.partStart
        readonly property real lowFrac: partBound ? Math.max(0, page.partStart / page.dur) : 0
        readonly property real highFrac: partBound ? Math.min(1, page.partEnd / page.dur) : 1
        Text {
            id: elapsed
            anchors.left: parent.left; anchors.verticalCenter: bar.verticalCenter
            // While scrubbing this reads the DESTINATION, so the jump can be aimed instead of guessed.
            text: page.fmtTime(progress.scrubbing ? progress.scrubFrac * page.dur : page.pos)
            color: progress.scrubbing ? page.accent : page.fgDim; font.pixelSize: page.h3
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
            // THE PART OF THE BOOK PLAYING NOW (#218), drawn under the fill: a lighter stretch of track,
            // which is the stretch the knob can be dragged within. Without it the clamp is a knob that
            // mysteriously stops; with it the bar has said, before the drag, which piece of the book is in
            // hand. Zero-width and invisible for every single-file play, where the whole bar is the file.
            Rectangle {
                id: partBand
                visible: progress.partBound
                x: parent.width * progress.lowFrac
                width: parent.width * Math.max(0, progress.highFrac - progress.lowFrac)
                height: parent.height; radius: parent.radius
                color: Qt.rgba(1, 1, 1, 0.34)
            }
            Rectangle {
                id: fill
                height: parent.height; radius: parent.radius
                width: parent.width * progress.shownFrac
                color: page.accent
                // The ease is what makes the once-a-second position step look like motion; during a drag it
                // would instead make the bar trail the finger, so it is off for exactly that time.
                Behavior on width {
                    enabled: !progress.scrubbing
                    NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                }
            }
            // The play head. Shown on hover or during a drag — the affordance that says the bar is grabbable,
            // and the thing under the finger while it is being dragged.
            Rectangle {
                id: knob
                visible: seekArea.enabled && (seekArea.containsMouse || progress.scrubbing)
                width: parent.height * 3; height: width; radius: width / 2
                x: fill.width - width / 2
                anchors.verticalCenter: parent.verticalCenter
                color: page.accent
                border.width: 2; border.color: Qt.rgba(1, 1, 1, 0.85)
            }
        }
        // A grabbable BAND, not the hairline the bar draws: a 4px target is unhittable with a finger and
        // fiddly with a mouse, and this page also runs on a TV-sized surface.
        MouseArea {
            id: seekArea
            anchors.left: bar.left; anchors.right: bar.right
            anchors.verticalCenter: bar.verticalCenter
            height: Math.max(bar.height * 5, page.height * 0.035)
            enabled: page.dur > 0                          // nothing to seek within until a length is known
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            preventStealing: true                          // a horizontal drag is a scrub, never a page flick
            // Clamped into the current part's span (#218). The clamp lives HERE, in the one function every
            // press, move and release reads its fraction from, so the fill, the knob, the destination
            // readout and the committed seek cannot disagree about where the gesture points.
            function fracAt(px) {
                var f = width > 0 ? Math.max(0, Math.min(1, px / width)) : 0
                return Math.max(progress.lowFrac, Math.min(progress.highFrac, f))
            }
            onPressed: function(mouse) { progress.scrubFrac = fracAt(mouse.x) }
            onPositionChanged: function(mouse) { if (pressed) progress.scrubFrac = fracAt(mouse.x) }
            onCanceled: progress.scrubFrac = -1            // grab lost (overlay, window deactivate): no seek
            onReleased: function(mouse) {
                var f = fracAt(mouse.x)
                progress.scrubFrac = -1
                if (!page.host) return
                if (page.host.forceActiveFocus) page.host.forceActiveFocus()
                page.host.audioTransportRequested("seek:" + f.toFixed(6))
            }
        }
    }

    // --- back out of the player (the top-left chrome affordance) ---------------------------------------
    // Deliberately NOT part of the transport strip: it is not a playback control, and putting it in the strip
    // would sit it between the cursor and the buttons someone came here to press. Fires the same verb channel
    // the strip uses, so the host owns what "back" means — the level pop, which stops playback and restores
    // the surface underneath exactly as Esc always has.
    Rectangle {
        id: backChip
        readonly property bool focused: !!(page.host && page.host.audioZone === "chrome")
        x: page.width * 0.03
        y: page.height * 0.045
        height: page.height * 0.058
        width: backLabel.implicitWidth + height * 0.9
        radius: height / 2
        color: Qt.rgba(1, 1, 1, 0.08)
        border.width: 2
        border.color: Qt.rgba(1, 1, 1, 0.14)
        scale: focused ? 1.1 : (backMa.pressed ? 0.94 : (backMa.containsMouse ? 1.05 : 1.0))
        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
        Rectangle {                       // the same focus ring the transport buttons draw
            visible: backChip.focused
            anchors.fill: parent; anchors.margins: -Math.max(3, parent.height * 0.12)
            radius: parent.height / 2 + 3
            color: "transparent"; border.width: Math.max(2, parent.height * 0.10); border.color: "#2FA1E6"
        }
        Text {
            id: backLabel
            anchors.centerIn: parent
            text: "‹  Back"
            color: page.fg
            font.bold: true
            font.pixelSize: Math.max(12, parent.height * 0.42)
        }
        MouseArea {
            id: backMa
            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            onClicked: {
                if (page.host) {
                    if (page.host.forceActiveFocus) page.host.forceActiveFocus()
                    page.host.audioTransportRequested("back")
                }
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
                // Two-glyph buttons need the speed pill's width or they clip (#142's nudge pair).
                readonly property bool isWide: isSpeed || modelData === "lyricEarlier"
                                                       || modelData === "lyricLater"
                readonly property bool focused: page.transportFocused && page.transportIdx === index
                height: page.height * 0.085
                width: isWide ? height * 1.6 : (isPlay ? height * 1.2 : height)
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
                // Drawn or typed, never both: iconName decides, and the one that is not in use is not built.
                readonly property string iconName: page.iconNameFor(modelData)
                readonly property color inkColor: isPlay ? "#FFFFFF" : page.fg
                Text {
                    visible: btn.iconName === ""
                    anchors.centerIn: parent
                    text: page.glyphFor(btn.modelData)
                    color: btn.inkColor
                    font.bold: true
                    font.pixelSize: btn.isWide ? page.h3 : parent.height * 0.44
                }
                Image {
                    visible: btn.iconName !== ""
                    anchors.centerIn: parent
                    width: Math.round(btn.height * 0.46); height: width
                    // Asked for at twice the size it is shown at, then scaled down: the page sizes its
                    // buttons off the window, so this lands on any pixel ratio and any TV, and a mark drawn
                    // large and shrunk stays sharp where one drawn small and stretched would not.
                    sourceSize.width: width * 2; sourceSize.height: width * 2
                    smooth: true
                    source: btn.iconName === "" ? ""
                          : "image://ebicon/" + btn.iconName + "/" + page.hexOf(btn.inkColor)
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
        // Panel header (issue #193). It exists to SAY that the queue can be edited: the verbs live behind one
        // gesture, and a gesture nobody knows about is a feature nobody has. The chip is the mouse route; the
        // caption names the two the rest of the app already uses (Start on a controller, M on a keyboard), and
        // all three land on the same host handler, so there is one menu and not three.
        Item {
            id: queueHeader
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: queuePanel.height * 0.06
            height: page.h3 * 1.5
            Text {
                id: queueCaption
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Queue")
                color: page.fgDim
                font.pixelSize: page.h3 * 0.9
                font.bold: true
            }
            Rectangle {
                id: queueEditChip
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                height: parent.height
                width: editLabel.implicitWidth + height * 0.7
                radius: height / 2
                color: Qt.rgba(1, 1, 1, 0.08)
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.14)
                scale: editMa.pressed ? 0.94 : (editMa.containsMouse ? 1.06 : 1.0)
                Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                Text {
                    id: editLabel
                    anchors.centerIn: parent
                    text: "☰  " + qsTr("Edit")
                    color: page.fg
                    font.pixelSize: page.h3 * 0.85
                }
                MouseArea {
                    id: editMa
                    anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (page.host) {
                            if (page.host.forceActiveFocus) page.host.forceActiveFocus()
                            page.host.actionRequested("queuemenu")
                        }
                    }
                }
            }
        }
        ListView {
            id: queueList
            anchors.top: queueHeader.bottom; anchors.topMargin: queuePanel.height * 0.03
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: queuePanel.height * 0.06; anchors.rightMargin: queuePanel.height * 0.06
            anchors.bottomMargin: queuePanel.height * 0.06
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

    // --- lyrics (karaoke scroll, #142) ----------------------------------------------------------------
    // Additive: only drawn when the current track HAS lyrics, in the right column below the transport strip so
    // it disturbs none of the existing zones. The panel itself is the `lyrics` ELEMENT, loaded here rather than
    // written out again — a theme can place that element anywhere it likes, and when it does not, this is where
    // the audio page puts it. One implementation, so the scroll, the current-line emphasis, the offset chip and
    // the seek-to-a-line gesture cannot differ between a theme that places it and a theme that does not.
    //
    // `el` is handed straight through: the style keys the panel reads (accent / color / dimColor / panelColor)
    // are the ones this page already declares, so the embedded panel inherits the page's palette for free.
    Loader {
        id: lyricsPanel
        source: Qt.resolvedUrl("Lyrics.qml")
        x: info.x
        width: info.width
        y: page.height * 0.835
        height: page.height * 0.15
        // Qt.binding, not a plain assignment, and for an ORDERING reason that has bitten this file's
        // neighbours: this Loader finishes as soon as the page is instantiated, which is BEFORE ThemeView's own
        // loader has run `item.host = root` on the page. A one-shot assignment would therefore hand the panel
        // `undefined` for ever, which is precisely the shape of the bug #142's lyrics already shipped once.
        onLoaded: {
            if (!item) return
            item.el   = Qt.binding(function() { return page.el })
            item.ctx  = Qt.binding(function() { return page.ctx })
            item.host = Qt.binding(function() { return page.host })
        }
    }
}
