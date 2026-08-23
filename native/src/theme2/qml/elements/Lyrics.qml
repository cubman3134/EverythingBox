// Lyrics element: the karaoke panel, as a THEME-PLACEABLE element (issue #142). Everything it shows is
// host-fed, exactly like the rest of the audio page:
//
//   * host.lyrics       — [{ time, text }], the winning source's lines (empty = this track has none, and the
//                         panel draws nothing at all rather than an empty box);
//   * host.lyricsSynced — true when those lines carry real timestamps. A synced set emphasises the current
//                         line and auto-scrolls to keep it centred; an UNSYNCED sheet is a plain, hand-
//                         scrollable block with no highlight and no seek (degrade, don't hide);
//   * host.lyricLine    — the current line index, recomputed by the host on each ~1 Hz position tick with the
//                         per-item offset already applied;
//   * host.lyricOffset  — that offset, in seconds, shown as a chip while it is non-zero so a nudge is visible
//                         as you make it;
//   * host.audioZone / host.audioLyricIndex — the nav cursor, when it is in the `lyrics` zone.
//
// SELECTING A LINE SEEKS THERE, which is the whole point of the element: Enter on the focused line (or a
// click) emits host.lyricSeekRequested(index) and the host seeks to that line's timestamp. The gesture is
// offered ONLY for a synced set — host.audioLyricCount is 0 otherwise, so the nav zone is not enterable and
// the rows are not clickable. Without that gate every line of a USLT sheet, whose times are all 0.0, would
// jump playback to 0:00.
//
// PLACEABLE, and that is why this is its own file rather than a block inside NowPlayingAudio.qml: a theme can
// give lyrics a whole column, a corner, or a full-screen karaoke view, sized and coloured its own way. It is
// documented in THEME_FORMAT.md as the `lyrics` element. NowPlayingAudio loads THIS file for its built-in
// panel (a Loader, one implementation) so a theme that places `lyrics` itself and a theme that does not are
// looking at the same code.
import QtQuick
import "../Theme.js" as T

Item {
    id: panel
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property var lines: (host && host.lyrics) ? host.lyrics : []
    readonly property bool synced: !!(host && host.lyricsSynced)
    readonly property int curLine: host ? host.lyricLine : -1
    readonly property real offset: host ? host.lyricOffset : 0
    // Seekable == the nav zone exists. Read from the host's own count rather than re-deriving "synced and
    // non-empty" here, so the thing that decides whether a row is clickable is the same thing that decides
    // whether the cursor can reach it.
    readonly property int seekCount: host ? host.audioLyricCount : 0
    readonly property bool zoneFocused: !!(host && host.audioZone === "lyrics")
    readonly property int navIdx: host ? host.audioLyricIndex : 0

    // Theme-tunable, each with a default that matches the audio page's own palette so a bare placement reads.
    readonly property color fg:       T.val(el, "color", "#FFFFFF")
    readonly property color fgDim:    T.val(el, "dimColor", "#AEB4C2")
    readonly property color accent:   T.val(el, "accent", "#E07A2E")
    readonly property color panelCol: T.val(el, "panelColor", "#161A20")
    readonly property real  radius:   Number(T.val(el, "radius", 12))
    // Font sizes are fractions of the VIEW height, the convention every other element uses, so a theme's
    // numbers mean the same thing here as they do next door.
    readonly property real  fsCur:  Math.max(1, Number(T.val(el, "fontSize", 0.028)) * (host ? host.height : 720))
    readonly property real  fsRest: Math.max(1, Number(T.val(el, "restSize", 0.024)) * (host ? host.height : 720))

    // The whole panel is absent when the track has no lyrics — no empty box, on any theme.
    visible: panel.lines.length > 0

    Rectangle {
        anchors.fill: parent
        radius: panel.radius
        color: panel.panelCol
        clip: true

        // The offset chip. Present only while a nudge is set, in the corner, so the panel is unchanged for the
        // 95% of files that need none — and so the 5% can see what they have dialled in.
        Rectangle {
            id: offsetChip
            visible: panel.offset !== 0
            anchors.top: parent.top; anchors.right: parent.right
            anchors.margins: Math.max(4, parent.height * 0.05)
            width: offsetText.implicitWidth + panel.fsRest * 0.8
            height: offsetText.implicitHeight + panel.fsRest * 0.35
            radius: height / 2
            color: Qt.rgba(1, 1, 1, 0.10)
            border.width: 1
            border.color: panel.accent
            z: 2
            Text {
                id: offsetText
                anchors.centerIn: parent
                // The sign is the user-facing convention: + means the words appear LATER.
                text: (panel.offset > 0 ? "+" : "−") + Math.abs(panel.offset).toFixed(1) + " s"
                color: panel.accent
                font.pixelSize: panel.fsRest * 0.8
                font.bold: true
            }
        }

        ListView {
            id: lyricList
            anchors.fill: parent
            anchors.margins: Math.max(2, parent.height * 0.10)
            model: panel.lines
            spacing: Math.max(1, parent.height * 0.04)
            // Synced: the auto-scroll owns the view (the highlight range keeps the current line centred) unless
            // the nav cursor is IN here, in which case the user is picking a line and their cursor leads.
            // Unsynced: no current line, so the sheet is theirs to scroll.
            interactive: !panel.synced || panel.zoneFocused
            currentIndex: panel.zoneFocused ? panel.navIdx
                                            : (panel.synced ? panel.curLine : -1)
            highlightFollowsCurrentItem: true
            highlightMoveDuration: 250
            highlight: Item {}   // emphasis lives on the delegate; no separate highlight bar
            preferredHighlightBegin: height / 2 - panel.fsCur
            preferredHighlightEnd: height / 2 + panel.fsCur
            highlightRangeMode: (panel.synced || panel.zoneFocused) ? ListView.StrictlyEnforceRange
                                                                    : ListView.NoHighlightRange
            delegate: Item {
                id: row
                required property var modelData
                required property int index
                width: lyricList.width
                height: lineText.implicitHeight + (focused ? panel.fsRest * 0.4 : 0)
                readonly property bool isCur: panel.synced && index === panel.curLine
                readonly property bool focused: panel.zoneFocused && index === panel.navIdx
                Rectangle {                       // keyboard / controller focus ring on the pickable line
                    visible: row.focused
                    anchors.fill: parent
                    anchors.leftMargin: -2; anchors.rightMargin: -2
                    radius: 6
                    color: Qt.rgba(0.18, 0.63, 0.9, 0.22)
                    border.width: 2; border.color: "#2FA1E6"
                }
                Text {
                    id: lineText
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: (row.modelData && row.modelData.text !== undefined) ? row.modelData.text : ""
                    color: row.isCur ? panel.fg : panel.fgDim
                    opacity: row.isCur ? 1.0 : (row.focused ? 0.9 : 0.5)
                    font.pixelSize: row.isCur ? panel.fsCur : panel.fsRest
                    font.bold: row.isCur
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    Behavior on opacity { NumberAnimation { duration: 150 } }
                }
                MouseArea {
                    // Clickable only where a click has an answer: seekCount is 0 on an unsynced sheet, so its
                    // rows are inert and the cursor stays an arrow over them.
                    anchors.fill: parent
                    enabled: row.index < panel.seekCount
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: {
                        if (panel.host) {
                            if (panel.host.forceActiveFocus) panel.host.forceActiveFocus()
                            panel.host.lyricSeekRequested(row.index)
                        }
                    }
                }
            }
        }
    }
}
