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

    // Phone: the theme's page density (4×3) crams 12 tiny cells onto a phone — drop to a readable,
    // tappable grid per ORIENTATION: portrait stacks 2×3; landscape spreads 4×2 (portrait's 2 columns
    // rotated made hugely wide, stubby cards). TV/desktop keep the theme's grid.
    readonly property bool mobile: (typeof form !== "undefined") && form && form.mode === "mobile"
    readonly property bool portrait: height > width
    readonly property int cols: mobile ? (portrait ? 2 : 4) : Math.max(1, Number(T.val(el, "columns", 4)))
    readonly property int rows: mobile ? (portrait ? 3 : 2) : Math.max(1, Number(T.val(el, "rows", 3)))
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

    // Phone: the theme places the grid near the top of the view — pad it below the device cutout
    // (Dynamic Island) so the first row's labels aren't shadowed. The element sits ~3% down already,
    // so only the remainder of the real inset is added. Zero on TV/desktop.
    readonly property real topPad: mobile && (typeof safeArea !== "undefined") && safeArea
                                   ? Math.max(0, safeArea.top - height * 0.04) : 0
    readonly property real arrowW: height * 0.16         // page-arrow diameter (the arrows float over the grid)
    readonly property real gap: Number(T.val(el, "spacing", 0.01)) * (host ? host.width : 1280)
    // The grid spans the full width so the next column peeks right to the edge; the arrows float on top of it
    // (no reserved gutter, so nothing masks the peek).
    readonly property real vpW: width
    readonly property real peek: Number(T.val(el, "peek", 0.35))  // width of the peeking column, in cells
    readonly property real cellW: vpW / (cols + peek)    // a page is `cols` wide; the peek shows the next column
    readonly property real pageW: cols * cellW
    readonly property real cellH: (height - topPad) / rows

    // ---- the pages (a Row of full-width pages, translated to the current page) ----
    Item {
        id: vp
        x: 0; y: ch.topPad; width: ch.vpW; height: ch.height - ch.topPad; clip: true
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
                            // No grid selection ring while focus has dropped to the bottom buttons (ctx is a
                            // reactive binding, so this updates the instant focus leaves the grid).
                            readonly property bool sel: !empty && slot === ch.cur && !(ch.ctx && ch.ctx.focusZone === 1)
                            x: (index % ch.cols) * ch.cellW
                            y: Math.floor(index / ch.cols) * ch.cellH
                            width: ch.cellW; height: ch.cellH
                            z: sel ? 2 : 0

                            MouseArea {
                                // Desktop/TV pointer path only — on touch the viewport MouseArea below
                                // arbitrates tap-vs-swipe itself (a per-cell area would win the grab on
                                // press and starve the swipe recognizer; that is why the round-6
                                // DragHandler never fired on the phone).
                                anchors.fill: parent; enabled: !cell.empty && !ch.mobile
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
                                // Poster cards keep a title STRIP below the artwork (text over a poster is
                                // unreadable/confusing); plain colored tiles (no image) keep the centered label.
                                // T.tileImage, not a bare item.image read: a row carrying art only under the
                                // open-ended `images` role map still gets a poster. Deliberately keyed off
                                // whether the row HAS artwork rather than T.tileNeedsTitle's live status —
                                // this drives the card's LAYOUT, and a strip that appeared as the image
                                // finished loading would resize the poster under the user. A dead url is
                                // still readable here: the strip below it carries the title either way.
                                readonly property string art: cell.empty ? "" : T.tileImage(cell.item)
                                readonly property bool hasImg: art !== ""
                                readonly property real stripH: hasImg ? Math.max(30, height * 0.24) : 0
                                Image {
                                    anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                                    height: parent.height - parent.stripH
                                    source: (parent.art !== "" && ch.host) ? ch.host.resolve(parent.art) : ""
                                    fillMode: Image.PreserveAspectCrop; visible: status === Image.Ready
                                }
                                Rectangle { // the title strip under the poster
                                    visible: parent.hasImg
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                    height: parent.stripH
                                    color: T.val(ch.card, "labelBg", "#F4F7FB")
                                    Text {
                                        anchors.fill: parent; anchors.margins: 4
                                        text: (cell.item && cell.item.title) ? cell.item.title : ""
                                        color: T.val(ch.card, "labelText", "#2A3646")
                                        // Sized off the strip (not the screen) so two wrapped lines always fit.
                                        font.pixelSize: Math.max(10, parent.parent.stripH * 0.30); font.bold: true
                                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                        wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
                                    }
                                }
                                Text { // colored tile (no artwork): label centered on the tile, as before
                                    visible: !cell.empty && !parent.hasImg
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
        z: 2  // above the touch tap/swipe MouseArea (a later sibling), so arrow clicks stay the arrows'
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
                // Page arrows PAGE the selection only — never activate. Under mobile one-tap `gotoItem` would
                // drill into the landed slot, so route through the select-only helper (falls back to gotoItem
                // on a host that predates it — desktop two-step there is still select-only for a non-current i).
                if (!ch.host) return
                var target = parent.forward ? Math.min(ch.count - 1, (ch.page + 1) * ch.perPage)
                                            : Math.max(0, (ch.page - 1) * ch.perPage)
                if (ch.host.gotoItemSelectOnly) ch.host.gotoItemSelectOnly(target)
                else if (ch.host.gotoItem) ch.host.gotoItem(target)
            }
        }
    }
    PageArrow { anchors.left: parent.left;  anchors.verticalCenter: parent.verticalCenter; forward: false; on: ch.page > 0 }
    PageArrow { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; forward: true;  on: ch.page < ch.pageCount - 1 }

    // ---- touch paging + look-ahead loading -----------------------------------------------------------------
    // Whole-page jump via the host's selection (the page follows the selection, same as the arrows).
    function flipPage(dir) {
        if (!host || !host.gotoItemSelectOnly) return
        host.gotoItemSelectOnly(Math.max(0, Math.min(count - 1, cur + dir * perPage)))
    }
    // Touch: ONE MouseArea over the viewport arbitrates tap vs swipe (the per-cell areas are desktop-only —
    // see the note there; they won the grab on press and starved the round-6 DragHandler, which is why
    // swiping never worked on the phone). A press that travels ≥ 40pt vertically (or 60 horizontally,
    // matching the pages' slide direction) flips a page on release; anything shorter is a tap, mapped to
    // its slot. Sits over the page Row but under the floating PageArrows (declaration order), so the
    // arrows still take their clicks.
    MouseArea {
        enabled: ch.mobile
        x: 0; y: ch.topPad; width: ch.vpW; height: ch.height - ch.topPad
        property real px: 0; property real py: 0
        onPressed: function(m) { px = m.x; py = m.y }
        onReleased: function(m) {
            var dx = m.x - px, dy = m.y - py
            if (dy <= -40 || dx <= -60)     { ch.flipPage(1);  return }
            if (dy >= 40  || dx >= 60)      { ch.flipPage(-1); return }
            if (Math.abs(dx) > 14 || Math.abs(dy) > 14) return   // a hesitant drag: neither tap nor swipe
            // Tap: page-local slot under the finger (the Row is translated by whole pages, so global
            // column → page + column decompose exactly; pageW == cols * cellW).
            var gx = m.x + ch.page * ch.pageW
            var pageIdx = Math.floor(gx / ch.pageW)
            var colIn   = Math.floor((gx - pageIdx * ch.pageW) / ch.cellW)
            var rowIn   = Math.floor(m.y / ch.cellH)
            if (rowIn < 0 || rowIn >= ch.rows || colIn >= ch.cols) return
            var slot = pageIdx * ch.perPage + rowIn * ch.cols + colIn
            if (slot >= 0 && slot < ch.count && ch.host && ch.host.gotoItem) ch.host.gotoItem(slot)
        }
    }
    // Ask the host for the next page of items BEFORE its slots can peek into view as empty boxes:
    // whenever the page after the current one isn't fully loaded, pull more (latched per page so a
    // source with nothing further doesn't get hammered).
    property int lastMoreReq: -1
    function maybeLoadMore() {
        if (!host || !host.nearEnd) return
        if ((page + 2) * perPage > count && page !== lastMoreReq) { lastMoreReq = page; host.nearEnd() }
    }
    onPageChanged: maybeLoadMore()
    onCountChanged: { lastMoreReq = -1; maybeLoadMore() }
    Component.onCompleted: maybeLoadMore()
}
