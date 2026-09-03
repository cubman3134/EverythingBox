// ActionRow element: the themed detail view's row of media actions — Play / Download / Favorite / Add-to-
// playlist, plus the external-player one-offs (Open in external player / Play with built-in player) on video
// leaves — as the `detailActions` contract zone. The available verbs are supplied by the host on the selected
// detail item (ctx.selected.actions, a list of "play"/"download"/"favorite"/"playlist"/"external"/"builtin"
// filtered per-item, mirroring the classic detail page's playBtn_/downloadBtn_/favBtn_ visibility rules plus
// ExternalPlayer::available()+restricted gating for the external pair). Keyboard /
// controller focus is driven by the NavGraph: the host writes host.detailZone ("actions" when this row holds
// the cursor) and host.detailActionIndex (which button). Activating a button — by click or by the detail
// key handler's Enter — emits host.detailActionRequested(verb), which the host routes to the same
// playThemedLeaf / favoriteThemedLeaf / downloadThemedLeaf / addBrowseItemToPlaylist methods the XMB inline
// chooser uses. The look reuses the pill-button idiom (focus ring + scale) from Button.qml.
import QtQuick
import "../Theme.js" as T

Item {
    id: rowEl
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property var sel: (ctx && ctx.selected) ? ctx.selected : ({})
    readonly property var verbs: (sel && sel.actions && sel.actions.length) ? sel.actions : []
    readonly property bool favorited: !!(sel && sel.favorite)
    readonly property bool readable: !!(sel && sel.readable)
    // Library-management state for the hide/status pills (supplied by themedDetailData).
    readonly property bool hidden: !!(sel && sel.hidden)
    readonly property string completion: (sel && sel.completion) ? sel.completion : "none"
    // Does this item carry a metadata correction (issue #24)? The pill says so, because otherwise there is
    // nothing on any screen to distinguish corrected data from scraped data — and "reset to scraped" would
    // be an option with no visible reason to reach for it.
    readonly property bool edited: !!(sel && sel.edited)
    // Is this series linked to a tracker entry (issue #156)? The pill says so, because otherwise a tracked
    // series and an untracked one look identical and "unlink" is an option with no visible reason to reach
    // for it. Supplied by themedDetailData only when a tracker is configured at all.
    readonly property bool tracked: !!(sel && sel.tracked)
    function statusLabel(c) {
        if (c === "inProgress") return "In progress"
        if (c === "finished")   return "Finished"
        if (c === "abandoned")  return "Abandoned"
        if (c === "planned")    return "Planned"
        return "Set status"
    }
    // This row holds the nav cursor when the host parks the detail selection in the "actions" zone.
    // Following (issue #155): whether this item is followed, and how many unseen children it holds.
    readonly property bool followed: !!(sel && sel.followed)
    readonly property int  newCount: (sel && sel.newCount) ? sel.newCount : 0
    readonly property bool zoneFocused: !!(host && host.detailZone === "actions")
    readonly property int focusIdx: (host ? host.detailActionIndex : 0)

    property real fs: Number(T.val(el, "fontSize", 0.026)) * (host ? host.height : 720)
    // The verb list easily outgrows one row (Play / Choose source / Romhacks… / Favorite / Download /
    // Playlist / Hide / Status / Tags / Fix info… / Select…) and a single Row marches straight off the
    // screen edge, so the pills WRAP — on every form factor, not just the phone where it was first noticed.
    // Phone: the detail relayout hands this element a box tall enough for the wrapped rows and the block sits
    // at the top of it. Desktop/TV: every theme reserves exactly ONE pill row here, so the block grows out of
    // that box about its centre and ThemeView slides the detail elements below it down by `bottomOverflow`
    // (see its detail-row block) — otherwise a second row would be drawn straight over the facts line.
    readonly property bool topAligned: (typeof form !== "undefined") && form && form.mode === "mobile"
    // How far the wrapped block reaches past the BOTTOM of the box the theme gave this element — 0 while the
    // pills still fit on the one row every theme sizes for. ThemeView reads this off the loaded element.
    readonly property real bottomOverflow: Math.max(0, topAligned ? btnRow.implicitHeight - height
                                                                  : (btnRow.implicitHeight - height) / 2)

    // verb -> { label, color, textColor } (favourite flips its label/colour with the item's current state).
    function metaFor(verb) {
        if (verb === "play")     return { label: (readable ? "📖  Read" : "▶  Play"), color: "#3FA95E", textColor: "#FFFFFF" }
        if (verb === "source")   return { label: "🔀  Choose source…",                 color: "#EDE4FF", textColor: "#3A2A7A" }
        if (verb === "download") return { label: "⬇  Download",                       color: "#5A8CFF", textColor: "#FFFFFF" }
        // The PC-game merge override (issue #44) — offered only on a merged PC game, whose identity is a
        // title heuristic the user may need to overrule ("these are two games" / "these are one").
        if (verb === "pcfix")    return { label: "⚙  Fix this entry…",                color: "#E7EBF2", textColor: "#33405A" }
        // Romhacks for THIS game — translations and hacks, installed as their own playable copy.
        if (verb === "romhack")  return { label: "🧩  Romhacks…",                     color: "#E4F5E8", textColor: "#1E5B33" }
        if (verb === "favorite") return { label: (favorited ? "★  Favorited" : "☆  Favorite"),
                                          color: (favorited ? "#E0A92E" : "#FFF1CC"), textColor: (favorited ? "#3A2A00" : "#7A4E00") }
        // Following a series (issue #155). The pill states what it IS, not what pressing it does — the
        // favourite pill's convention right above — and carries the unread count once there is one, so the
        // detail page says how far behind you are without leaving it.
        if (verb === "follow")   return { label: (followed ? (newCount > 0 ? ("\u2713  Following \u00b7 " + newCount + " new")
                                                                          : "\u2713  Following")
                                                           : "\uff0b  Follow"),
                                          color: (followed ? "#CFE3D2" : "#E7EBF2"), textColor: "#33405A" }
        if (verb === "markseen") return { label: "\u2713\u2713  Mark all seen", color: "#E7EBF2", textColor: "#33405A" }
        if (verb === "playlist") return { label: "➕  Playlist",                        color: "#E7EBF2", textColor: "#33405A" }
        if (verb === "external") return { label: "🔗  Open in external player",         color: "#7C5CFF", textColor: "#FFFFFF" }
        if (verb === "builtin")  return { label: "🖥  Play with built-in player",       color: "#E7EBF2", textColor: "#33405A" }
        if (verb === "hide")     return { label: (hidden ? "🙈  Unhide" : "🙈  Hide"),
                                          color: (hidden ? "#D8C7E8" : "#E7EBF2"), textColor: "#33405A" }
        if (verb === "status")   return { label: "◐  " + statusLabel(completion),
                                          color: (completion === "none" ? "#E7EBF2" : "#CFE3D2"), textColor: "#33405A" }
        if (verb === "tags")     return { label: "🏷  Tags",                              color: "#E7EBF2", textColor: "#33405A" }
        // The AniList link for this series: link, re-link, refresh or unlink (issue #156).
        if (verb === "tracker")  return { label: (tracked ? "📈  Tracking" : "📈  Track…"),
                                          color: (tracked ? "#CFE0F5" : "#E4F0FF"), textColor: "#1B3A63" }
        if (verb === "editmeta") return { label: (edited ? "✎  Info edited" : "✎  Fix info…"),
                                          color: (edited ? "#CFE3D2" : "#E7EBF2"), textColor: "#33405A" }
        // Per-game launch overrides (issue #51): which core / standalone emulator / extra args THIS game runs on.
        if (verb === "launchopts") return { label: "🎮  Launch options…", color: "#E7EBF2", textColor: "#33405A" }
        // Bulk edit (issue #65): enter multi-select and apply one action to many of this level's items.
        if (verb === "select")   return { label: "☑  Select…", color: "#E7EBF2", textColor: "#33405A" }
        // The region/revision siblings region-collapsing hid (issue #50): a collapsed game's other
        // dumps, reachable from the one tile that survived the collapse.
        if (verb === "otherversions") return { label: "🗂  Other versions…", color: "#E7EBF2", textColor: "#33405A" }
        return { label: verb, color: "#E7EBF2", textColor: "#33405A" }
    }

    Flow {
        id: btnRow
        anchors.left: parent.left
        anchors.right: parent.right                      // a wrapping Flow needs the width to wrap AT
        anchors.verticalCenter: topAligned ? undefined : parent.verticalCenter
        anchors.top: topAligned ? parent.top : undefined
        spacing: Math.max(8, fs * 0.5)
        Repeater {
            model: rowEl.verbs
            delegate: Rectangle {
                id: pill
                required property var modelData
                required property int index
                property var m: rowEl.metaFor(modelData)
                readonly property bool focused: rowEl.zoneFocused && rowEl.focusIdx === index
                height: Math.max(28, fs * 1.9)
                width: lbl.implicitWidth + fs * 1.6
                radius: height / 2
                color: m.color
                border.width: 2
                border.color: Qt.darker(m.color, 1.25)
                scale: focused ? 1.08 : (ma.pressed ? 0.94 : (ma.containsMouse ? 1.04 : 1.0))
                Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }

                Rectangle { // keyboard/controller focus ring
                    visible: pill.focused
                    anchors.fill: parent; anchors.margins: -Math.max(3, parent.height * 0.10)
                    radius: parent.height / 2 + 3
                    color: "transparent"; border.width: Math.max(2, parent.height * 0.09); border.color: "#2FA1E6"
                }
                Text {
                    id: lbl
                    anchors.centerIn: parent
                    text: pill.m.label
                    color: pill.m.textColor
                    font.bold: true
                    font.pixelSize: Math.max(10, rowEl.fs)
                }
                MouseArea {
                    id: ma
                    anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (rowEl.host) {
                            if (rowEl.host.forceActiveFocus) rowEl.host.forceActiveFocus()
                            rowEl.host.detailActionRequested(pill.modelData)
                        }
                    }
                }
            }
        }
    }
}
