// Channels element: a Wii-menu-style PAGED grid. Items are laid out in fixed pages (columns x rows); the last
// page is padded with greyed-out empty slots, and left/right page arrows sit in the side gutters (greyed when
// there's no page that way). The host owns the selection (currentIndex): moving past a page edge flips the
// page, and clicking an arrow jumps a whole page. Card knobs mirror the `grid` element's (fill / border /
// selected* / label centred). Extra knobs: rows, emptyFill, emptyBorder.
import QtQuick
import "../Theme.js" as T

Item {
    id: ch
    property var el: ({})
    property var ctx: ({})
    property var host
    property var card: T.val(el, "card", ({}))

    readonly property int cols: Math.max(1, Number(T.val(el, "columns", 4)))
    readonly property int rows: Math.max(1, Number(T.val(el, "rows", 3)))
    readonly property int perPage: cols * rows
    readonly property var items: (ctx && ctx.items) ? ctx.items : []
    readonly property int count: items.length
    readonly property int cur: (ctx && ctx.index !== undefined) ? ctx.index : 0
    // Only pages that actually hold channels are reachable - you never arrow onto an all-empty page.
    readonly property int pageCount: Math.max(1, Math.ceil(count / perPage))
    // One extra (empty) page is rendered so a sliver of greyed-out slots always peeks past the current page's
    // right edge - the Wii "there's room for more" hint - but it can't be navigated to.
    readonly property int renderedPages: pageCount + 1
    readonly property int page: Math.min(pageCount - 1, Math.floor(cur / perPage))

    readonly property real arrowW: height * 0.16         // page-arrow diameter (the arrows float over the grid)
    readonly property real gap: Number(T.val(el, "spacing", 0.01)) * (host ? host.width : 1280)
    // The grid spans the full width so the next column peeks right to the edge; the arrows float on top of it
    // (no reserved gutter, so nothing masks the peek).
    readonly property real vpW: width
    readonly property real peek: Number(T.val(el, "peek", 0.35))  // width of the peeking column, in cells
    readonly property real cellW: vpW / (cols + peek)    // a page is `cols` wide; the peek shows the next column
    readonly property real pageW: cols * cellW
    readonly property real cellH: height / rows

    // ---- the pages (a Row of full-width pages, translated to the current page) ----
    Item {
        id: vp
        x: 0; width: ch.vpW; height: ch.height; clip: true
        Row {
            x: -ch.page * ch.pageW
            Behavior on x { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
            Repeater {
                model: ch.renderedPages
                delegate: Item {
                    id: pg
                    required property int index
                    readonly property int pageBase: index * ch.perPage
                    width: ch.pageW; height: ch.height
                    Repeater {
                        model: ch.perPage
                        delegate: Item {
                            id: cell
                            required property int index
                            readonly property int slot: pg.pageBase + index
                            readonly property var item: (slot < ch.count) ? ch.items[slot] : null
                            readonly property bool empty: item === null
                            readonly property bool sel: !empty && slot === ch.cur
                            x: (index % ch.cols) * ch.cellW
                            y: Math.floor(index / ch.cols) * ch.cellH
                            width: ch.cellW; height: ch.cellH
                            z: sel ? 2 : 0

                            MouseArea {
                                anchors.fill: parent; enabled: !cell.empty
                                cursorShape: Qt.PointingHandCursor
                                onClicked: if (ch.host && ch.host.gotoItem) ch.host.gotoItem(cell.slot)
                            }
                            Rectangle {
                                anchors.fill: parent; anchors.margins: ch.gap
                                radius: Number(T.val(ch.card, "radius", 14))
                                clip: true
                                color: cell.empty ? T.val(ch.card, "emptyFill", "#D9E0EA")
                                                  : ((cell.item && cell.item.accent) ? cell.item.accent
                                                                                     : T.val(ch.card, "fill", "#23272F"))
                                opacity: cell.empty ? 0.55 : 1.0
                                border.width: cell.sel ? Number(T.val(ch.card, "selectedWidth", 5))
                                                       : Number(T.val(ch.card, "borderWidth", 2))
                                border.color: cell.sel ? T.val(ch.card, "selectedBorder", "#2FA1E6")
                                                       : (cell.empty ? T.val(ch.card, "emptyBorder", "#C4CCD6")
                                                                     : T.val(ch.card, "border", "#B7C3D4"))
                                scale: cell.sel ? Number(T.val(ch.card, "selectedScale", 1.0)) : 1.0
                                Behavior on scale { NumberAnimation { duration: 130; easing.type: Easing.OutBack } }
                                Image {
                                    anchors.fill: parent
                                    source: (cell.item && cell.item.image && ch.host) ? ch.host.resolve(cell.item.image) : ""
                                    fillMode: Image.PreserveAspectCrop; visible: status === Image.Ready
                                }
                                Text {
                                    visible: !cell.empty
                                    anchors.centerIn: parent; width: parent.width * 0.88
                                    text: (cell.item && cell.item.title) ? cell.item.title : ""
                                    color: T.val(ch.card, "labelColor", "#FFFFFF")
                                    style: Text.Outline; styleColor: Qt.rgba(0, 0, 0, 0.35)
                                    font.pixelSize: Math.max(12, 0.03 * (ch.host ? ch.host.height : 720)); font.bold: true
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                    wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- page arrows in the side gutters (greyed when there's no page that way) ----
    component PageArrow: Rectangle {
        property bool forward: true
        property bool on: false
        width: ch.arrowW * 0.72; height: width; radius: width / 2
        color: T.val(ch.card, "labelBg", "#F7FAFD"); border.width: 2; border.color: "#AEBBCB"
        opacity: on ? 1.0 : 0.3
        Canvas {
            anchors.centerIn: parent; width: parent.width * 0.5; height: parent.height * 0.5
            onWidthChanged: requestPaint(); onHeightChanged: requestPaint()
            onPaint: {
                var c = getContext("2d"); c.reset(); c.clearRect(0, 0, width, height)
                c.fillStyle = "#4A5A72"; var w = width, h = height
                c.beginPath()
                if (forward) { c.moveTo(w * 0.35, h * 0.15); c.lineTo(w * 0.75, h * 0.5); c.lineTo(w * 0.35, h * 0.85) }
                else         { c.moveTo(w * 0.65, h * 0.15); c.lineTo(w * 0.25, h * 0.5); c.lineTo(w * 0.65, h * 0.85) }
                c.closePath(); c.fill()
            }
        }
        MouseArea { anchors.fill: parent; enabled: parent.on; cursorShape: Qt.PointingHandCursor
            onClicked: {
                if (!ch.host || !ch.host.gotoItem) return
                ch.host.gotoItem(parent.forward ? Math.min(ch.count - 1, (ch.page + 1) * ch.perPage)
                                                 : Math.max(0, (ch.page - 1) * ch.perPage))
            }
        }
    }
    PageArrow { anchors.left: parent.left;  anchors.verticalCenter: parent.verticalCenter; forward: false; on: ch.page > 0 }
    PageArrow { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; forward: true;  on: ch.page < ch.pageCount - 1 }
}
