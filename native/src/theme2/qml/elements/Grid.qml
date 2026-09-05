// Grid element: a grid of item cards (poster + label) with themeable columns, aspect, spacing, card radius
// and a selection border. Driven by the data context's `items` / `index`.
//
// Card knobs (all optional; defaults preserve the original look): fill (tile colour behind the poster),
// border + borderWidth (an always-on outline on every card), selectedBorder + selectedWidth, selectedScale
// (the selected card grows and lifts above its neighbours - a Wii-menu "pop"), label ("overlay" scrim over
// the poster [default], "center" the title centred over the whole card with no bar, "top" a title bar across
// the top of the card [Wii-channel style], "below" a plate under the poster, or "none"), labelColor, labelBg.
import QtQuick
import "../Theme.js" as T

GridView {
    id: gv
    objectName: "themeGrid"   // probe_navqml §20 introspects contentY / interactive through this
    property var el: ({})
    property var ctx: ({})
    property var host
    property var card: T.val(el, "card", ({}))
    property int cols: Number(T.val(el, "columns", 4))
    property string labelMode: T.val(card, "label", "overlay")
    property real labelFrac: (labelMode === "below" || labelMode === "top") ? Number(T.val(card, "labelSize", 0.24)) : 0.0

    clip: true
    // Native kinetic scrolling on touch (D1 Task 4): a mobile drag flicks the grid; key/controller nav still
    // snaps via currentIndex. Desktop/TV stay non-interactive (wheel + tap-to-move), a pixel/behaviour no-op.
    interactive: (typeof form !== "undefined" && form) ? form.mode === "mobile" : false
    cellWidth: width / cols
    cellHeight: cellWidth * Number(T.val(el, "aspect", 1.4))
    model: ctx ? ctx.items : []
    currentIndex: ctx ? ctx.index : 0

    delegate: Item {
        width: gv.cellWidth
        height: gv.cellHeight
        required property var modelData
        required property int index
        property bool sel: index === gv.currentIndex
        z: sel ? 2 : 0 // a scaled-up selection draws above its neighbours
        MouseArea { // click a card to select it; click the selected card to open it
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor; z: 10
            onClicked: if (gv.host && gv.host.gotoItem) gv.host.gotoItem(index)
        }
        Rectangle {
            id: cardRect
            anchors.fill: parent
            anchors.margins: Math.max(2, Number(T.val(gv.el, "spacing", 0.008)) * (gv.host ? gv.host.width : 1280))
            radius: Number(T.val(gv.card, "radius", 10))
            clip: true
            color: (modelData && modelData.accent) ? modelData.accent : T.val(gv.card, "fill", "#23272F")
            border.width: sel ? Number(T.val(gv.card, "selectedWidth", 4)) : Number(T.val(gv.card, "borderWidth", 0))
            border.color: sel ? T.val(gv.card, "selectedBorder", T.val(gv.el, "color", "#E07A2E"))
                              : T.val(gv.card, "border", "#00000000")
            scale: sel ? Number(T.val(gv.card, "selectedScale", 1.0)) : 1.0
            Behavior on scale { NumberAnimation { duration: 130; easing.type: Easing.OutBack } }

            // Title bar across the top of the card (label === "top", Wii-channel style).
            Rectangle {
                id: topBar
                visible: gv.labelMode === "top"
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                height: parent.height * gv.labelFrac
                color: T.val(gv.card, "labelBg", "#F4F7FB")
                Text {
                    anchors.fill: parent; anchors.margins: parent.height * 0.16
                    text: (modelData && modelData.title) ? modelData.title : ""
                    color: T.val(gv.card, "labelColor", "#38455A")
                    font.pixelSize: Math.max(9, 0.023 * (gv.host ? gv.host.height : 720)); font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight; maximumLineCount: 2; wrapMode: Text.WordWrap
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#00000018" } // hairline
            }

            // Poster / colour area: the rest of the card (below a "top" bar, above a "below" plate, else full).
            Item {
                id: posterArea
                anchors.left: parent.left; anchors.right: parent.right
                anchors.top: gv.labelMode === "top" ? topBar.bottom : parent.top
                anchors.bottom: gv.labelMode === "below" ? belowBar.top : parent.bottom
                clip: true
                Image {
                    id: poster
                    anchors.fill: parent
                    // T.tileImage, not a bare modelData.image read: a row that carries its art only under
                    // the open-ended `images` role map still gets a poster instead of a blank tile.
                    source: (gv.host && T.tileImage(modelData) !== "") ? gv.host.contentUrl(T.tileImage(modelData)) : ""
                    fillMode: Image.PreserveAspectCrop
                    visible: status === Image.Ready
                }
                // The artwork-less fallback for `label: "none"` — the one card style that puts no title on
                // the card at all, so a row whose art is missing OR dead draws as a bare coloured rectangle
                // with nothing readable on it. Every other label mode already carries a title (see below),
                // and T.tileNeedsTitle is the shared rule for "no artwork actually on screen" (issue #29).
                Text {
                    visible: gv.labelMode === "none" && T.tileNeedsTitle(modelData, poster.status === Image.Ready)
                    anchors.centerIn: parent; width: parent.width * 0.88
                    text: (modelData && modelData.title) ? modelData.title : ""
                    color: T.val(gv.card, "labelColor", "#FFFFFF")
                    style: Text.Outline; styleColor: Qt.rgba(0, 0, 0, 0.35)
                    font.pixelSize: Math.max(11, 0.028 * (gv.host ? gv.host.height : 720)); font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
                }
                // Overlay label: a dark scrim + title at the bottom of the poster (the original look), and
                // — on the SELECTED card only — the row's subtitle under it.
                //
                // THE SUBTITLE IS WHERE THE FACTS ARE. Every synthetic library level in this app puts its
                // numbers there ("3 part(s) · 1h", "12 track(s)", and #139 increment 2's "29m left"), and
                // this element rendered none of them: a grid theme showed titles and nothing else, so a
                // line the classic grid has always printed simply did not exist on the layout most people
                // run. Xmb.qml has shown the selected row's subtitle since it was written; this is that
                // same rule for the card grid, restricted the same way — one card, the one being looked
                // at, so a wall of tiles does not turn into a wall of small print.
                Rectangle {
                    visible: gv.labelMode === "overlay"
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    height: parent.height * (cardSub.visible ? 0.44 : 0.32)
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "transparent" }
                        GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.65) }
                    }
                }
                Text {
                    id: cardSub
                    visible: gv.labelMode === "overlay" && sel && !!(modelData && modelData.subtitle)
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    anchors.margins: 10
                    text: (modelData && modelData.subtitle) ? modelData.subtitle : ""
                    color: T.val(gv.card, "labelColor", "#FFFFFF")
                    opacity: 0.82
                    font.pixelSize: Math.max(9, 0.019 * (gv.host ? gv.host.height : 720))
                    wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
                }
                Text {
                    visible: gv.labelMode === "overlay"
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.bottom: cardSub.visible ? cardSub.top : parent.bottom
                    anchors.margins: 10
                    anchors.bottomMargin: cardSub.visible ? 2 : 10
                    text: (modelData && modelData.title) ? modelData.title : ""
                    color: T.val(gv.card, "labelColor", "#FFFFFF")
                    font.pixelSize: Math.max(10, 0.024 * (gv.host ? gv.host.height : 720))
                    font.bold: true
                    elide: Text.ElideRight
                }
                // Centred label: the title over the whole card, no bar (label === "center"). An outline keeps
                // it readable over any tile colour.
                Text {
                    visible: gv.labelMode === "center"
                    anchors.centerIn: parent; width: parent.width * 0.88
                    text: (modelData && modelData.title) ? modelData.title : ""
                    color: T.val(gv.card, "labelColor", "#FFFFFF")
                    style: Text.Outline; styleColor: Qt.rgba(0, 0, 0, 0.35)
                    font.pixelSize: Math.max(12, 0.032 * (gv.host ? gv.host.height : 720)); font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
                }
            }

            // "Below" label: a name-plate strip under the poster.
            Rectangle {
                id: belowBar
                visible: gv.labelMode === "below"
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                height: parent.height * gv.labelFrac
                color: T.val(gv.card, "labelBg", "#F4F7FB")
                Text {
                    anchors.fill: parent; anchors.margins: parent.height * 0.16
                    text: (modelData && modelData.title) ? modelData.title : ""
                    color: T.val(gv.card, "labelColor", "#38455A")
                    font.pixelSize: Math.max(9, 0.022 * (gv.host ? gv.host.height : 720)); font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight; maximumLineCount: 2; wrapMode: Text.WordWrap
                }
            }

            // "N NEW" badge for a followed series carrying unseen children (issue #155). browseItems() sets
            // modelData.newCount only on a followed tile with unread children, so every other tile renders
            // exactly as it did. Top-LEFT, deliberately: the "on disk" badge below owns the top-right corner
            // and a local library series can be both.
            Rectangle {
                // Yields to the #239 marker below: a followed series that also refused to open has one piece
                // of news worth reading first, and two badges in the same corner would overlap.
                visible: !!(modelData && modelData.newCount) && !(modelData && modelData.openFailed)
                anchors { top: parent.top; left: parent.left; margins: 4 }
                radius: 3
                color: "#1E5B33"
                implicitWidth: newText.implicitWidth + 10
                implicitHeight: newText.implicitHeight + 4
                width: implicitWidth; height: implicitHeight
                Text {
                    id: newText
                    anchors.centerIn: parent
                    text: (modelData && modelData.newCount) ? (modelData.newCount + " NEW") : ""
                    color: "white"
                    font.pixelSize: 10
                    font.bold: true
                }
            }

            // "Continue watching/listening" bar along the bottom of the card (issue #139 increment 2), the
            // themed counterpart of the classic grid's poster overlay. browseItems() sets modelData.progress
            // (0..1) only for a row that has somewhere to be — a part-way film, episode, track or audiobook
            // — and leaves the key ABSENT otherwise, which reads as undefined and hides this.
            Rectangle {
                visible: !!(modelData && modelData.progress > 0)
                // Above the name-plate when there is one, so the bar never lies across the title: the
                // "below" label mode owns the bottom strip of the card.
                anchors { left: parent.left; right: parent.right
                          bottom: belowBar.visible ? belowBar.top : parent.bottom }
                height: Math.max(3, parent.height * 0.022)
                color: Qt.rgba(0, 0, 0, 0.55)
                Rectangle {
                    anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
                    width: parent.width * Math.max(0, Math.min(1, modelData ? modelData.progress : 0))
                    color: T.val(gv.card, "progressColor", "#E53E3E")
                }
            }

            // "This didn't open" marker (issue #239). browseItems() sets modelData.openFailed when the item's
            // last open failed and the record has not expired, cleared, or been dismissed. Deliberately the
            // SAME affordance as the "on disk" badge below -- same rectangle, same radius, same 10pt bold
            // caption -- because it is the same kind of statement about the item; a new vocabulary for it
            // would be one more thing to learn. Top-LEFT, where the "N NEW" badge lives (which yields to it
            // above); the top-right corner stays the "on disk" badge's, and an item can be both.
            Rectangle {
                visible: !!(modelData && modelData.openFailed)
                anchors { top: parent.top; left: parent.left; margins: 4 }
                radius: 3
                color: "#B3261E"
                implicitWidth: failText.implicitWidth + 10
                implicitHeight: failText.implicitHeight + 4
                width: implicitWidth; height: implicitHeight
                Text {
                    id: failText
                    anchors.centerIn: parent
                    text: "⚠ DIDN'T OPEN"
                    color: "white"
                    font.pixelSize: 10
                    font.bold: true
                }
            }

            // "On disk" badge for locally-owned items (LocalLibrary Seam A). browseItems() sets modelData.onDisk
            // (and onDiskCount for a series) when OwnedIndex owns the tile's id; un-owned tiles never carry it.
            Rectangle {
                visible: !!(modelData && modelData.onDisk)
                anchors { top: parent.top; right: parent.right; margins: 4 }
                radius: 3
                color: Qt.rgba(0, 0, 0, 0.65)
                implicitWidth: badgeText.implicitWidth + 10
                implicitHeight: badgeText.implicitHeight + 4
                width: implicitWidth; height: implicitHeight
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: (modelData && modelData.onDiskCount) ? ("● " + modelData.onDiskCount) : "● ON DISK"
                    color: "white"
                    font.pixelSize: 10
                    font.bold: true
                }
            }
        }
    }
}
