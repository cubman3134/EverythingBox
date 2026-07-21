// ReaderChrome — the themed strip content for a HOSTED raster reader (ReaderChromeHost), per the B1 VARIANT A
// composition decision: an OPAQUE strip raised over the reader. ONE component, parameterised by `region`:
//
//   * region "top"    — a compact bar (page-of + the reader-settings row) plus, when the chapter list is
//                       focused (nav.zone === "readerToc"), an expanding chapter panel below it.
//   * region "bottom" — the page-navigation bar: ‹ Prev · progress + "page x / y" · Next ›.
//
// It reads two context properties the host installs: `nav` (the reader's NavGraph — same shared model the
// probe asserts) and `readerBridge` (page/chapter/font/toc + the reader commands). Leaf highlight is driven by
// nav.zone / nav.index; clicks route through nav.select + nav.activate so mouse and controller share ONE
// dispatch in the host. Font size is a `ThemedChoice` in EXTERNAL-edit mode — the strips are Qt::NoFocus (the
// reader keeps key focus, spike constraint 1), so an inline focus-grabbing picker can't receive keys; instead
// activation cycles to the next size through the host bridge (the Task-2 externalEdit contract). Each region
// is behind a Loader gated on `region`, so the OTHER region's items (incl. the ThemedChoice zone) are never
// instantiated in this strip. Book zones only this task.
import QtQuick

Rectangle {
    id: chrome
    // region + barHeight arrive as context properties set BEFORE the QML loads (see ReaderChromeHost::
    // buildStrips) so the region Loaders below resolve correctly at creation and the inactive region's items
    // (incl. the font ThemedChoice zone) are never even instantiated in this strip.
    property string region: (typeof chromeRegion !== "undefined") ? chromeRegion : "top"
    property int    barHeight: (typeof chromeBarHeight !== "undefined") ? chromeBarHeight : 40
    // Form-factor UI scale (subsystem D): the strip's fonts + fixed controls grow with uiScale (the host scales the
    // strip's OUTER geometry in ReaderChromeHost::layoutStrips). Desktop uiScale is 1.0 (identity) — every
    // Math.round(literal * 1) is a no-op. typeof-guarded so a strip loaded without `form` renders unchanged.
    readonly property real ffs: (typeof form !== "undefined" && form) ? form.uiScale : 1
    readonly property int  barH: Math.round(barHeight * ffs)   // the scaled inner settings-bar height
    readonly property color accent: "#3A6FB0"

    color: "#0E1218"                     // OPAQUE — Variant A (no translucency dependency)
    readonly property var br: (typeof readerBridge !== "undefined") ? readerBridge : null
    readonly property var g:  (typeof nav !== "undefined") ? nav : null
    readonly property string readerType: br ? br.readerType : "book"   // "book" | "pdf" | "comic"

    Loader {
        anchors.fill: parent
        active: chrome.region === "top"
        sourceComponent: topComponent
    }
    Loader {
        anchors.fill: parent
        active: chrome.region === "bottom"
        sourceComponent: bottomComponent
    }

    // ---------------------------------------------------------------- TOP: settings bar + chapter panel -----
    Component {
        id: topComponent
        Column {
            // Compact settings bar (aligned to the reader's reserved top inset).
            Rectangle {
                width: parent.width; height: chrome.barH
                color: "#141A22"
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#9AA6B2"; font.pixelSize: Math.round(13 * chrome.ffs)
                    text: chrome.br ? ("Page " + chrome.br.pageLabel) : ""  // range-aware (comic spread: "3–4 / 20")
                }
                // The reader-settings zone. Book: a font-size ThemedChoice. Pdf/Comic: a row of plain buttons
                // (zoom out / in / fit, + two-up for a comic) the host activates by index. Each variant is behind
                // a Loader gated on the reader kind so the OTHER kind's items — crucially the font ThemedChoice,
                // whose onDestruction removes the shared readerSettings zone — are never instantiated here.
                Row {
                    anchors.right: parent.right; anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    // ---- Book: font size ----
                    Loader {
                        anchors.verticalCenter: parent.verticalCenter
                        active: chrome.readerType === "book"
                        sourceComponent: Row {
                            spacing: 10
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#9AA6B2"; font.pixelSize: Math.round(13 * chrome.ffs); text: "Font"
                            }
                            // externalEdit -> activation cycles to the next size via the host bridge (no inline
                            // focus grab; the strip is NoFocus). The host also feeds the zone's count, so
                            // navigation to it never depends on this component's registration timing.
                            ThemedChoice {
                                id: fontChoice
                                anchors.verticalCenter: parent.verticalCenter
                                navZone: "readerSettings"; navRow: 1; navCol: 0
                                externalEdit: true
                                accent: chrome.accent
                                width: Math.round(108 * chrome.ffs); height: Math.min(chrome.barH - 6, Math.round(34 * chrome.ffs))
                                options: chrome.br ? chrome.br.fontOptions : []
                                currentOption: chrome.br ? chrome.br.fontIndex : 0
                                onEditRequested: (zone) => {
                                    if (!chrome.br) { finishEdit(false); return }
                                    var n = chrome.br.fontOptions.length
                                    if (n > 0) chrome.br.chooseFont((chrome.br.fontIndex + 1) % n) // next size
                                    finishEdit(false)   // applied via the bridge; don't double-fire chosen()
                                }
                            }
                        }
                    }

                    // ---- Pdf/Comic: zoom out(0) / zoom in(1) / fit(2) [/ two-up(3) for a comic] ----
                    Loader {
                        anchors.verticalCenter: parent.verticalCenter
                        active: chrome.readerType !== "book"
                        sourceComponent: Row {
                            spacing: 8
                            Repeater {
                                model: chrome.readerType === "comic"
                                       ? [{t: "−", i: 0}, {t: "+", i: 1}, {t: "Fit", i: 2}, {t: "Two-Up", i: 3}]
                                       : [{t: "−", i: 0}, {t: "+", i: 1}, {t: "Fit", i: 2}]
                                delegate: Rectangle {
                                    required property var modelData
                                    // highlighted when the nav cursor is on this settings row
                                    readonly property bool sel: chrome.g && chrome.g.zone === "readerSettings"
                                                                && chrome.g.index === modelData.i
                                    // the two-up toggle also shows its ON state (pressed look) from the bridge
                                    readonly property bool active2: modelData.i === 3 && chrome.br && chrome.br.twoUp
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.max(Math.round(34 * chrome.ffs), btnTxt.implicitWidth + 18)
                                    height: Math.min(chrome.barH - 6, Math.round(30 * chrome.ffs)); radius: 6
                                    color: sel ? chrome.accent : (active2 ? "#243A57" : "#1E2632")
                                    border.width: sel ? 2 : 1
                                    border.color: sel ? Qt.lighter(chrome.accent, 1.3) : "#2A3540"
                                    Text {
                                        id: btnTxt; anchors.centerIn: parent
                                        text: modelData.t; color: "#E6ECF3"; font.pixelSize: Math.round(13 * chrome.ffs)
                                    }
                                    MouseArea {
                                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                        onClicked: { if (chrome.g) { chrome.g.select("readerSettings", modelData.i); chrome.g.activate() } }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Chapter list (readerToc). Shown only while its zone holds the cursor — the host grows the top
            // strip to reveal it, so it overlays the page like the classic contents panel.
            Rectangle {
                width: parent.width
                height: parent.height - chrome.barH
                visible: height > 0 && chrome.g && chrome.g.zone === "readerToc"
                color: "#0E1218"
                border.color: "#22303C"; border.width: 1

                ListView {
                    id: tocView
                    anchors.fill: parent; anchors.margins: 4
                    clip: true
                    interactive: false
                    model: chrome.br ? chrome.br.toc : []
                    currentIndex: (chrome.g && chrome.g.zone === "readerToc") ? chrome.g.index : -1
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: tocView.width
                        height: Math.round(30 * chrome.ffs)
                        readonly property bool sel: (chrome.g && chrome.g.zone === "readerToc" && chrome.g.index === index)
                        color: sel ? Qt.rgba(0.23, 0.44, 0.69, 0.35) : "transparent"
                        radius: 5
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 10
                            anchors.right: parent.right; anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData; elide: Text.ElideRight
                            color: parent.sel ? "#FFFFFF" : "#C7D0DA"; font.pixelSize: Math.round(14 * chrome.ffs)
                        }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { if (chrome.g) { chrome.g.select("readerToc", index); chrome.g.activate() } }
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- BOTTOM: page navigation bar -----------
    Component {
        id: bottomComponent
        Item {
            id: navbar
            // prev(0) / progress(1) / next(2) — the readerNav zone (Horizontal, wraps). Highlight from nav.index.
            function navSel(i) { return chrome.g && chrome.g.zone === "readerNav" && chrome.g.index === i }
            function fire(i)   { if (chrome.g) { chrome.g.select("readerNav", i); chrome.g.activate() } }

            Rectangle { anchors.fill: parent; color: "#141A22" }

            Rectangle {   // ‹ Prev
                id: prevBtn
                anchors.left: parent.left; anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(96 * chrome.ffs); height: parent.height - 12; radius: 8
                color: navbar.navSel(0) ? chrome.accent : "#1E2632"
                border.width: navbar.navSel(0) ? 2 : 1
                border.color: navbar.navSel(0) ? Qt.lighter(chrome.accent, 1.3) : "#2A3540"
                Text { anchors.centerIn: parent; text: "‹ Prev"; color: "#E6ECF3"; font.pixelSize: Math.round(14 * chrome.ffs) }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: navbar.fire(0) }
            }

            Rectangle {   // progress + page x / y
                id: progress
                anchors.left: prevBtn.right; anchors.right: nextBtn.left; anchors.margins: 14
                anchors.verticalCenter: parent.verticalCenter
                height: Math.round(22 * chrome.ffs); radius: 11
                color: navbar.navSel(1) ? "#223042" : "#1A222C"
                border.width: navbar.navSel(1) ? 2 : 1
                border.color: navbar.navSel(1) ? chrome.accent : "#2A3540"
                Rectangle {   // fill
                    anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                    anchors.margins: 2; radius: 9
                    width: {
                        var pc = chrome.br ? chrome.br.pageCount : 0
                        var p  = chrome.br ? chrome.br.page : 0
                        return (pc > 0) ? Math.max(4, (progress.width - 4) * Math.min(1, p / pc)) : 0
                    }
                    color: Qt.rgba(0.23, 0.44, 0.69, 0.55)
                }
                Text {
                    anchors.centerIn: parent
                    text: chrome.br ? chrome.br.pageLabel : ""  // range-aware (comic spread: "3–4 / 20")
                    color: "#C7D0DA"; font.pixelSize: Math.round(12 * chrome.ffs)
                }
            }

            Rectangle {   // Next ›
                id: nextBtn
                anchors.right: parent.right; anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(96 * chrome.ffs); height: parent.height - 12; radius: 8
                color: navbar.navSel(2) ? chrome.accent : "#1E2632"
                border.width: navbar.navSel(2) ? 2 : 1
                border.color: navbar.navSel(2) ? Qt.lighter(chrome.accent, 1.3) : "#2A3540"
                Text { anchors.centerIn: parent; text: "Next ›"; color: "#E6ECF3"; font.pixelSize: Math.round(14 * chrome.ffs) }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: navbar.fire(2) }
            }
        }
    }
}
