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
    function statusLabel(c) {
        if (c === "inProgress") return "In progress"
        if (c === "finished")   return "Finished"
        if (c === "abandoned")  return "Abandoned"
        if (c === "planned")    return "Planned"
        return "Set status"
    }
    // This row holds the nav cursor when the host parks the detail selection in the "actions" zone.
    readonly property bool zoneFocused: !!(host && host.detailZone === "actions")
    readonly property int focusIdx: (host ? host.detailActionIndex : 0)

    property real fs: Number(T.val(el, "fontSize", 0.026)) * (host ? host.height : 720)

    // verb -> { label, color, textColor } (favourite flips its label/colour with the item's current state).
    function metaFor(verb) {
        if (verb === "play")     return { label: (readable ? "📖  Read" : "▶  Play"), color: "#3FA95E", textColor: "#FFFFFF" }
        if (verb === "download") return { label: "⬇  Download",                       color: "#5A8CFF", textColor: "#FFFFFF" }
        if (verb === "favorite") return { label: (favorited ? "★  Favorited" : "☆  Favorite"),
                                          color: (favorited ? "#E0A92E" : "#FFF1CC"), textColor: (favorited ? "#3A2A00" : "#7A4E00") }
        if (verb === "playlist") return { label: "➕  Playlist",                        color: "#E7EBF2", textColor: "#33405A" }
        if (verb === "external") return { label: "🔗  Open in external player",         color: "#7C5CFF", textColor: "#FFFFFF" }
        if (verb === "builtin")  return { label: "🖥  Play with built-in player",       color: "#E7EBF2", textColor: "#33405A" }
        if (verb === "hide")     return { label: (hidden ? "🙈  Unhide" : "🙈  Hide"),
                                          color: (hidden ? "#D8C7E8" : "#E7EBF2"), textColor: "#33405A" }
        if (verb === "status")   return { label: "◐  " + statusLabel(completion),
                                          color: (completion === "none" ? "#E7EBF2" : "#CFE3D2"), textColor: "#33405A" }
        if (verb === "tags")     return { label: "🏷  Tags",                              color: "#E7EBF2", textColor: "#33405A" }
        return { label: verb, color: "#E7EBF2", textColor: "#33405A" }
    }

    Row {
        id: btnRow
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
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
