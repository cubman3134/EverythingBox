// XMB element - a PlayStation-style XrossMediaBar. The horizontal axis is the categories (host.categories,
// each = an inherent bucket: Video/Game/Audio/Reading/Settings), the vertical axis is the column the host
// feeds (that bucket's catalogs, or - once you open a catalog - its live items). Both axes slide so the active
// element stays pinned at the "cross". Purely presentational: ThemeView owns the keys (xmb mode) and the host
// (C++) swaps the column. Reads off the host (the ThemeView root): categories, catIndex, items, currentIndex.
//
// Category tiles show, in order of preference: a theme `icon` image, else a drawn glyph for the bucket
// (modelData.glyph: "video"|"audio"|"game"|"reading"|"settings"), else the title's first letter.
// Theme keys: color, subColor, descColor, crossX/crossY, catSpacing, itemSpacing, iconSize (fractions).
import QtQuick
import "../Theme.js" as T

Item {
    id: xmb
    property var el: ({})
    property var ctx: ({})
    property var host

    readonly property var cats: host && host.categories ? host.categories : []
    readonly property int catIndex: host ? host.catIndex : 0
    readonly property var items: host && host.items ? host.items : []
    readonly property int itemIndex: host ? host.currentIndex : 0

    readonly property color textColor: T.val(el, "color", "#FFFFFF")
    readonly property color subColor: T.val(el, "subColor", "#C2C7D4")
    readonly property color descColor: T.val(el, "descColor", "#9AA0AE")
    readonly property real crossX: Number(T.val(el, "crossX", 0.20)) * width
    readonly property real crossY: Number(T.val(el, "crossY", 0.30)) * height
    readonly property real catGap: Number(T.val(el, "catSpacing", 0.135)) * width
    readonly property real itemGap: Number(T.val(el, "itemSpacing", 0.082)) * height
    readonly property real iconSize: Number(T.val(el, "iconSize", 0.11)) * height
    // Column tile size is derived from the row spacing (and stays under it even when the selected row scales
    // up 1.12x), so column items never overlap regardless of the theme's iconSize/itemSpacing.
    readonly property real rowSize: itemGap * 0.8
    // The column starts well below the category row + its label so the first item never collides with them.
    readonly property real colTop: crossY + iconSize * 1.7

    // Smoothly slide both axes toward the selection (the PS3 "everything glides to the cross" feel).
    property real catScroll: catIndex
    property real itemScroll: itemIndex
    onCatIndexChanged: catScroll = catIndex
    onItemIndexChanged: itemScroll = itemIndex
    Behavior on catScroll  { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
    Behavior on itemScroll { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

    // ---- horizontal category axis (pinned at crossX, slides left/right) ----------------------------------
    Repeater {
        model: xmb.cats
        delegate: Item {
            id: cat
            required property var modelData
            required property int index
            readonly property bool sel: index === xmb.catIndex
            readonly property real dx: (index - xmb.catScroll) * xmb.catGap
            readonly property string glyph: (modelData && modelData.glyph) ? modelData.glyph : ""
            readonly property string icon: (modelData && modelData.icon) ? modelData.icon : ""
            width: xmb.iconSize; height: xmb.iconSize
            x: xmb.crossX + dx - width / 2
            y: xmb.crossY - height / 2
            opacity: sel ? 1.0 : Math.max(0.18, 0.55 - Math.abs(dx) / xmb.width * 0.6)
            scale: sel ? 1.18 : 0.88
            Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

            Rectangle {
                anchors.fill: parent; radius: width * 0.18
                color: (cat.modelData && cat.modelData.accent) ? cat.modelData.accent : "#2A2E36"
                visible: cat.icon === ""

                // Drawn glyph (static Canvas - software-renderer safe, painted once). White on the accent tile.
                Canvas {
                    anchors.centerIn: parent
                    width: parent.width * 0.58; height: parent.height * 0.58
                    visible: cat.glyph !== ""
                    onPaint: {
                        var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
                        var w = width, h = height
                        c.fillStyle = "#FFFFFF"; c.strokeStyle = "#FFFFFF"
                        c.lineWidth = Math.max(2, w * 0.08); c.lineCap = "round"; c.lineJoin = "round"
                        if (cat.glyph === "video") {                       // play triangle
                            c.beginPath(); c.moveTo(w * 0.28, h * 0.2); c.lineTo(w * 0.8, h * 0.5); c.lineTo(w * 0.28, h * 0.8); c.closePath(); c.fill()
                        } else if (cat.glyph === "audio") {                // musical note
                            c.beginPath(); c.ellipse(w * 0.24, h * 0.62, w * 0.26, h * 0.22); c.fill()
                            c.beginPath(); c.rect(w * 0.46, h * 0.16, w * 0.08, h * 0.52); c.fill()
                            c.beginPath(); c.moveTo(w * 0.5, h * 0.16); c.quadraticCurveTo(w * 0.86, h * 0.22, w * 0.74, h * 0.42); c.lineTo(w * 0.54, h * 0.34); c.closePath(); c.fill()
                        } else if (cat.glyph === "game") {                 // gamepad: d-pad cross + two buttons
                            var cxp = w * 0.34, cyp = h * 0.5, arm = w * 0.17, th = w * 0.11
                            c.fillRect(cxp - arm, cyp - th / 2, arm * 2, th)   // d-pad horizontal
                            c.fillRect(cxp - th / 2, cyp - arm, th, arm * 2)   // d-pad vertical
                            c.beginPath(); c.arc(w * 0.68, h * 0.4, w * 0.075, 0, 6.2832); c.fill()  // buttons
                            c.beginPath(); c.arc(w * 0.8, h * 0.6, w * 0.075, 0, 6.2832); c.fill()
                        } else if (cat.glyph === "reading") {              // open book
                            c.beginPath(); c.moveTo(w * 0.5, h * 0.26); c.lineTo(w * 0.16, h * 0.34); c.lineTo(w * 0.16, h * 0.78); c.lineTo(w * 0.5, h * 0.72); c.closePath(); c.fill()
                            c.beginPath(); c.moveTo(w * 0.5, h * 0.26); c.lineTo(w * 0.84, h * 0.34); c.lineTo(w * 0.84, h * 0.78); c.lineTo(w * 0.5, h * 0.72); c.closePath(); c.fill()
                            c.strokeStyle = (cat.modelData && cat.modelData.accent) ? cat.modelData.accent : "#444"
                            c.beginPath(); c.moveTo(w * 0.5, h * 0.3); c.lineTo(w * 0.5, h * 0.72); c.stroke()
                        } else if (cat.glyph === "profiles") {             // a person (user)
                            c.beginPath(); c.arc(w * 0.5, h * 0.34, w * 0.16, 0, 6.2832); c.fill()   // head
                            c.beginPath(); c.moveTo(w * 0.18, h * 0.86)
                            c.bezierCurveTo(w * 0.18, h * 0.56, w * 0.82, h * 0.56, w * 0.82, h * 0.86)
                            c.closePath(); c.fill()                                                  // shoulders
                        } else {                                           // settings: a gear
                            var gx = w * 0.5, gy = h * 0.5, R = w * 0.3, teeth = 8, tw = w * 0.13, tl = w * 0.16
                            for (var k = 0; k < teeth; k++) {
                                c.save(); c.translate(gx, gy); c.rotate(k / teeth * 6.2832)
                                c.fillRect(-tw / 2, -(R + tl), tw, tl + R * 0.3); c.restore()
                            }
                            c.beginPath(); c.arc(gx, gy, R, 0, 6.2832); c.fill()
                            c.save(); c.globalCompositeOperation = "destination-out"
                            c.beginPath(); c.arc(gx, gy, R * 0.42, 0, 6.2832); c.fill(); c.restore()
                        }
                    }
                }
                Text { // ultimate fallback when there's no icon and no glyph
                    anchors.centerIn: parent
                    visible: cat.glyph === ""
                    text: (cat.modelData && cat.modelData.title) ? cat.modelData.title.charAt(0) : ""
                    color: "#FFFFFF"; font.bold: true; font.pixelSize: parent.height * 0.5
                }
            }
            Image {
                anchors.fill: parent; visible: cat.icon !== ""
                source: (cat.icon !== "" && xmb.host) ? xmb.host.resolve(cat.icon) : ""
                fillMode: Image.PreserveAspectFit; smooth: true
            }
            Text { // the selected bucket's name, just under its tile
                id: catLabel
                visible: cat.sel
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.bottom; anchors.topMargin: xmb.height * 0.02
                text: (cat.modelData && cat.modelData.title) ? cat.modelData.title : ""
                color: xmb.textColor; font.bold: true; font.pixelSize: Math.max(12, xmb.height * 0.03)
            }
            // Loading spinner under the selected category while its column is being fetched: the cross scrolls
            // to the category right away and this shows the content is on the way (instead of an empty column).
            Item {
                id: catSpinner
                visible: cat.sel && !!(xmb.host && xmb.host.catLoading)
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: catLabel.bottom; anchors.topMargin: xmb.height * 0.018
                width: xmb.height * 0.045; height: width
                Canvas {
                    anchors.fill: parent
                    onPaint: {
                        var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
                        c.lineWidth = Math.max(2, width * 0.13); c.lineCap = "round"
                        c.strokeStyle = xmb.textColor
                        c.beginPath(); c.arc(width / 2, height / 2, width * 0.38, -Math.PI / 2, Math.PI * 0.85); c.stroke()
                    }
                }
                RotationAnimation on rotation {
                    from: 0; to: 360; duration: 850; loops: Animation.Infinite; running: catSpinner.visible
                }
            }
            MouseArea { // click a category tile to switch to it
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                onClicked: if (xmb.host) xmb.host.gotoCat(cat.index)
            }
        }
    }

    // ---- vertical column (the bucket's catalogs, or a catalog's items), descending from below the cross ----
    Repeater {
        model: xmb.items
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool sel: index === xmb.itemIndex
            readonly property real dy: (index - xmb.itemScroll) * xmb.itemGap
            width: xmb.rowSize; height: xmb.rowSize
            x: xmb.crossX - width / 2
            y: xmb.colTop + dy - height / 2
            opacity: dy < -xmb.itemGap * 0.5 ? 0.0 : (sel ? 1.0 : 0.6) // fade rows that rise above the column top
            scale: sel ? 1.12 : 0.94
            Behavior on scale { NumberAnimation { duration: 140 } }

            Rectangle {
                anchors.fill: parent; radius: 8
                color: (row.modelData && row.modelData.accent) ? row.modelData.accent : "#23272F"
                // Show the accent tile until the icon is ready (and for off-screen rows whose icon isn't loaded).
                visible: !(row.modelData && row.modelData.image) || rowIcon.status !== Image.Ready
            }
            Image {
                id: rowIcon
                anchors.fill: parent; visible: !!(row.modelData && row.modelData.image)
                asynchronous: true; cache: true
                sourceSize.width: width; sourceSize.height: height
                // Only rasterize icons for rows near the cross: a full console list is ~56 SVGs, and decoding
                // them all at once on the software renderer is the hitch you feel landing on a big category.
                source: (row.modelData && row.modelData.image && xmb.host && Math.abs(row.index - xmb.itemScroll) < 9)
                        ? xmb.host.resolve(row.modelData.image) : ""
                fillMode: Image.PreserveAspectCrop; smooth: true
            }
            // Title (and, for the selected row, a subtitle line beneath it) to the right of the icon.
            Column {
                anchors.left: parent.right; anchors.leftMargin: xmb.width * 0.015
                anchors.verticalCenter: parent.verticalCenter
                // Kept narrow so it never runs under the metadata panel on the right (the full title shows there).
                width: xmb.width * 0.30; spacing: xmb.height * 0.004
                Text {
                    width: parent.width
                    text: (row.modelData && row.modelData.title) ? row.modelData.title : ""
                    color: row.sel ? xmb.textColor : xmb.subColor
                    font.bold: row.sel; font.pixelSize: Math.max(11, xmb.height * (row.sel ? 0.03 : 0.024))
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: row.sel && !!(row.modelData && row.modelData.subtitle)
                    text: (row.modelData && row.modelData.subtitle) ? row.modelData.subtitle : ""
                    color: xmb.descColor; font.pixelSize: Math.max(10, xmb.height * 0.022)
                    elide: Text.ElideRight
                }
            }
            // Click the row (icon + title region) to select it; click the selected one to open/drill. Sized to
            // the row's vertical slot (itemGap) so adjacent rows don't overlap; disabled for faded-out rows.
            MouseArea {
                anchors.verticalCenter: parent.verticalCenter
                x: 0; width: parent.width + xmb.width * 0.30; height: xmb.itemGap
                enabled: row.opacity > 0.1; cursorShape: Qt.PointingHandCursor
                onClicked: if (xmb.host) xmb.host.gotoItem(row.index)
            }
        }
    }

    // ---- metadata panel for the selected leaf (Triple theme: "all the info beside the item") --------------
    // Fed live by the host (host.selectedMeta) as the column selection moves: a skeleton from the catalog row
    // first, then the addon's synopsis + facts when they arrive. Hidden whenever there's nothing selected (the
    // catalog list, Profiles, Settings - the host leaves selectedMeta empty there).
    Item {
        id: meta
        readonly property var m: (xmb.host && xmb.host.selectedMeta) ? xmb.host.selectedMeta : ({})
        readonly property bool shown: !!(m && m.title)
        visible: opacity > 0.01
        opacity: shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        x: xmb.width * 0.58; width: xmb.width * 0.38
        // Start up near the category row and stop above the help bar, so long metadata (a big poster + a full
        // synopsis) can't run off the bottom of the screen. clip guarantees nothing draws past the panel.
        y: xmb.crossY - xmb.iconSize * 1.1
        height: xmb.height * 0.90 - y
        clip: true

        // Top block: poster, title, subtitle, facts (fixed, anchored to the top of the panel).
        Column {
            id: metaTop
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            spacing: xmb.height * 0.014
            Image {
                width: parent.width * 0.36; height: Math.min(width * 1.4, meta.height * 0.38)
                source: (meta.m && meta.m.image && xmb.host) ? xmb.host.resolve(meta.m.image) : ""
                fillMode: Image.PreserveAspectCrop; smooth: true; visible: source != ""
            }
            Text {
                width: parent.width; text: meta.m.title ? meta.m.title : ""
                color: xmb.textColor; font.bold: true; font.pixelSize: Math.max(15, xmb.height * 0.036)
                wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
            }
            Text { // year/subtitle, with play history (last played · time played) folded onto the same line
                width: parent.width; visible: text !== ""; textFormat: Text.RichText
                text: {
                    var parts = []
                    if (meta.m.subtitle)   parts.push(meta.m.subtitle)
                    if (meta.m.lastPlayed) parts.push("<b>Last played:</b> " + meta.m.lastPlayed)
                    if (meta.m.timePlayed) parts.push("<b>Time played:</b> " + meta.m.timePlayed)
                    return parts.join("&nbsp;&nbsp;·&nbsp;&nbsp;")
                }
                color: xmb.subColor; font.pixelSize: Math.max(11, xmb.height * 0.024)
                wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
            }
            Text { // facts (rating / genres / developer / …) the addon supplied, a few at most
                width: parent.width; visible: text !== ""; textFormat: Text.RichText
                text: {
                    if (meta.m.type === "game") return "" // games: year + play line covers it; drop the facts/platforms row
                    var f = meta.m.facts; if (!f || !f.length) return ""
                    var out = []
                    for (var i = 0; i < f.length && i < 4; i++) out.push("<b>" + f[i].label + ":</b> " + f[i].value)
                    return out.join("&nbsp;&nbsp;&nbsp;")
                }
                color: xmb.descColor; font.pixelSize: Math.max(10, xmb.height * 0.021)
                wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
            }
            // RetroAchievements: earned badges shown bright at the front, locked ones dimmed (Steam-style).
            Text {
                width: parent.width
                visible: !!(meta.m.achievements && meta.m.achievements.length)
                text: "🏆 Achievements — " + (meta.m.achEarned || 0) + " / " + (meta.m.achTotal || 0)
                color: xmb.textColor; font.bold: true; font.pixelSize: Math.max(10, xmb.height * 0.022)
            }
            Flow { // a single row of badges (earned first, bright); overflow past one line is clipped away
                width: parent.width; spacing: xmb.height * 0.006
                height: xmb.height * 0.05; clip: true
                visible: !!(meta.m.achievements && meta.m.achievements.length)
                Repeater {
                    model: meta.m.achievements ? meta.m.achievements.slice(0, 16) : []
                    delegate: Image {
                        required property var modelData
                        width: xmb.height * 0.05; height: width
                        source: modelData.icon ? modelData.icon : "" // full badge/icon URL (RetroAchievements or Steam)
                        opacity: modelData.earned ? 1.0 : 0.28
                        smooth: true; asynchronous: true; fillMode: Image.PreserveAspectFit
                    }
                }
            }
        }
        // "Favorited" pinned to the bottom of the panel.
        Text {
            id: favLine
            anchors.left: parent.left; anchors.bottom: parent.bottom
            visible: !!meta.m.favorite; text: "★  Favorited"
            color: "#FFD66B"; font.bold: true; font.pixelSize: Math.max(10, xmb.height * 0.021)
        }
        // Synopsis fills the space between the top block and the bottom. If the description is taller than that
        // space, it auto-scrolls in place - pause at the top, ease down to the end, pause, ease back up, repeat -
        // so the whole thing is readable without going off-screen.
        Item {
            id: synBox
            anchors.top: metaTop.bottom; anchors.topMargin: xmb.height * 0.016
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: favLine.visible ? favLine.top : parent.bottom
            anchors.bottomMargin: favLine.visible ? xmb.height * 0.008 : 0
            clip: true
            property real over: Math.max(0, synText.paintedHeight - height)
            function restartScroll() { scroller.stop(); synText.y = 0; if (over > 2 && meta.shown) scroller.start() }
            onOverChanged: restartScroll()
            Component.onCompleted: restartScroll()
            Text {
                id: synText
                width: parent.width; y: 0
                visible: !!meta.m.overview; text: meta.m.overview ? meta.m.overview : ""
                color: xmb.subColor; font.pixelSize: Math.max(11, xmb.height * 0.0225)
                wrapMode: Text.WordWrap; lineHeight: 1.15
                onPaintedHeightChanged: synBox.restartScroll() // new selection -> re-measure + restart at the top
            }
            SequentialAnimation {
                id: scroller; loops: Animation.Infinite; running: false
                PauseAnimation { duration: 1800 }
                NumberAnimation { target: synText; property: "y"; to: -synBox.over
                                  duration: Math.max(1500, synBox.over * 40); easing.type: Easing.InOutQuad }
                PauseAnimation { duration: 1600 }
                NumberAnimation { target: synText; property: "y"; to: 0; duration: 800; easing.type: Easing.InOutQuad }
            }
        }
    }

    // ---- inline Play / Favorite chooser over the selected leaf (host.actionsOpen) -------------------------
    // Opening a leaf shows this instead of jumping anywhere: Up/Down (or click) pick, Enter fires. The host
    // (C++) plays / favourites the row at host.actionItem and closes it.
    Rectangle {
        id: actions
        visible: opacity > 0.01
        opacity: (xmb.host && xmb.host.actionsOpen) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 130 } }
        z: 50
        width: xmb.width * 0.26; height: xmb.height * 0.36
        x: xmb.crossX + xmb.width * 0.11 // a clear gap from the item column instead of hugging it
        y: xmb.colTop - xmb.itemGap * 0.5
        radius: 12; color: "#EE0E141E"; border.color: "#3A6FB0"; border.width: 2

        Column {
            anchors.centerIn: parent; spacing: parent.height * 0.05; width: parent.width * 0.88
            Repeater {
                model: [ { k: 0, label: "▶  Play" },
                         { k: 1, label: (xmb.host && xmb.host.actionFav) ? "★  Favorited" : "☆  Favorite" },
                         { k: 2, label: "＋  Add to playlist" },
                         { k: 3, label: "⭳  Download" } ]
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool sel: !!(xmb.host && xmb.host.actionIndex === modelData.k)
                    width: parent.width; height: actions.height * 0.20; radius: 8
                    color: sel ? "#3A6FB0" : "#1A222E"
                    Text {
                        anchors.centerIn: parent; text: modelData.label
                        color: "#FFFFFF"; font.bold: sel; font.pixelSize: Math.max(13, xmb.height * 0.03)
                    }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: if (xmb.host) { xmb.host.actionIndex = modelData.k; xmb.host.actionChosen(modelData.k) }
                    }
                }
            }
        }
    }
}
