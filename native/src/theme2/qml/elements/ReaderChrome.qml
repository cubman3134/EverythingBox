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

                // The top strip is one horizontal control ROW (the readerSettings zone) read left to right, and
                // the nav index order IS that order — so a pad moves the cursor exactly where the eye expects.
                // Exit is index 0 for every reader kind: it is the one control that means the same thing in all
                // three, so it sits in the same place in all three, at the far left where a player puts its way
                // out.
                Component {
                    id: readerCtl
                    Rectangle {
                        // NOT `required`: these are instantiated through a Loader, and a Loader cannot supply a
                        // required property — the component simply never builds and the row renders empty.
                        property var modelData: ({ i: -1, t: "" })
                        readonly property bool sel: chrome.g && chrome.g.zone === "readerSettings"
                                                    && chrome.g.index === modelData.i
                        readonly property bool on: modelData.on === true   // e.g. a comic's two-up, while unselected
                        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                        width: Math.max(Math.round(34 * chrome.ffs), ctlTxt.implicitWidth + 18)
                        height: Math.min(chrome.barH - 6, Math.round(30 * chrome.ffs)); radius: 6
                        color: sel ? chrome.accent : (on ? "#243A57" : "#1E2632")
                        border.width: sel ? 2 : 1
                        border.color: sel ? Qt.lighter(chrome.accent, 1.3) : "#2A3540"
                        Text {
                            id: ctlTxt; anchors.centerIn: parent
                            text: modelData.t; color: "#E6ECF3"; font.pixelSize: Math.round(13 * chrome.ffs)
                        }
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { if (chrome.g) { chrome.g.select("readerSettings", modelData.i); chrome.g.activate() } }
                        }
                    }
                }

                // A value the row SHOWS rather than a place the cursor stops. The font size was previously
                // changed by a control that never said what the size had become.
                Component {
                    id: readerReadout
                    Text {
                        property var modelData: ({ t: "" })   // see the note above: Loader-built, so not required
                        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                        text: modelData.t; color: "#E6ECF3"; font.bold: true
                        font.pixelSize: Math.round(14 * chrome.ffs)
                    }
                }

                // ---- Exit (index 0), far left ----
                Loader {
                    id: exitCtl
                    anchors.left: parent.left; anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    sourceComponent: readerCtl
                    onLoaded: item.modelData = ({ i: 0, t: "✕  Exit" })
                }

                Text {
                    anchors.left: exitCtl.right; anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#9AA6B2"; font.pixelSize: Math.round(13 * chrome.ffs)
                    text: chrome.br ? ("Page " + chrome.br.pageLabel) : ""  // range-aware (comic spread: "3–4 / 20")
                }

                // ---- The kind's own controls (indices 1..), right-aligned ----
                Row {
                    anchors.right: parent.right; anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    Repeater {
                        // Built in ONE place rather than declared per kind, so the index a control draws with is
                        // the index the host fires — the two cannot drift into a row that highlights one thing
                        // and does another. A `readout` entry carries no index and takes no cursor stop.
                        model: {
                            if (!chrome.br) return []
                            if (chrome.readerType === "book") {
                                var brows = [{ i: 1, t: "A −" },
                                             { readout: true, t: String(chrome.br.fontSize) },
                                             { i: 2, t: "A +" },
                                             { i: 3, t: chrome.br.themeNames[chrome.br.themeIndex] },
                                             { i: 4, t: chrome.br.fontFamilies[chrome.br.fontFamilyIndex] }]
                                // Read aloud (issue #145), indices 5..8. Gated on the ONE availability flag, so
                                // a build without the Qt TextToSpeech module (or a platform with no engine)
                                // draws the five controls this row has always had - and the host's zone count,
                                // ReadAloud::bookSettingsRowCount, agrees because it reads the same flag.
                                if (chrome.br.readAloudAvailable) {
                                    brows.push({ i: 5, t: chrome.br.readAloudActive ? "■ Stop" : "▶ Read aloud",
                                                 on: chrome.br.readAloudActive })
                                    brows.push({ i: 6, t: chrome.br.readAloudPaused ? "Resume" : "Pause",
                                                 on: chrome.br.readAloudPaused })
                                    brows.push({ i: 7, t: chrome.br.readAloudSpeedLabel })
                                    brows.push({ i: 8, t: chrome.br.readAloudVoiceLabel })
                                }
                                return brows
                            }
                            var rows = [{ i: 1, t: "−" }, { i: 2, t: "+" }, { i: 3, t: "Fit" }]
                            if (chrome.readerType === "comic")
                                rows.push({ i: 4, t: "Two-Up", on: chrome.br.twoUp })
                            return rows
                        }
                        delegate: Loader {
                            required property var modelData
                            anchors.verticalCenter: parent.verticalCenter
                            sourceComponent: modelData.readout === true ? readerReadout : readerCtl
                            onLoaded: item.modelData = modelData
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

            // Bookmark list (readerBookmarks) — the ToC's sibling panel (issue #136). Shown only while its zone
            // holds the cursor; the host grows the top strip the same way it does for the ToC, so the two panels
            // share the expanded area (only one is ever visible at once). model = the bridge's live bookmark
            // labels; activating a row fires the reader's gotoBookmark; the × affordance fires removeBookmark.
            Rectangle {
                width: parent.width
                height: parent.height - chrome.barH
                visible: height > 0 && chrome.g && chrome.g.zone === "readerBookmarks"
                color: "#0E1218"
                border.color: "#22303C"; border.width: 1

                ListView {
                    id: bmView
                    anchors.fill: parent; anchors.margins: 4
                    clip: true
                    interactive: false
                    model: chrome.br ? chrome.br.bookmarks : []
                    currentIndex: (chrome.g && chrome.g.zone === "readerBookmarks") ? chrome.g.index : -1
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: bmView.width
                        height: Math.round(30 * chrome.ffs)
                        readonly property bool sel: (chrome.g && chrome.g.zone === "readerBookmarks" && chrome.g.index === index)
                        color: sel ? Qt.rgba(0.23, 0.44, 0.69, 0.35) : "transparent"
                        radius: 5
                        // Row body: select + activate -> the host jumps to this bookmark (gotoBookmark).
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: { if (chrome.g) { chrome.g.select("readerBookmarks", index); chrome.g.activate() } }
                        }
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 10
                            anchors.right: rmBtn.left; anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData; elide: Text.ElideRight
                            color: parent.sel ? "#FFFFFF" : "#C7D0DA"; font.pixelSize: Math.round(14 * chrome.ffs)
                        }
                        // Remove affordance (×): a distinct hit target over the row body — fires removeBookmark
                        // straight on the bridge (the store change re-emits bookmarksChanged, refreshing the model
                        // and the zone count). Later sibling = higher z, so its area wins over the row MouseArea.
                        Rectangle {
                            id: rmBtn
                            anchors.right: parent.right; anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(24 * chrome.ffs); height: Math.round(24 * chrome.ffs); radius: 5
                            color: rmArea.containsMouse ? "#3A2530" : "transparent"
                            border.color: "#2A3540"; border.width: rmArea.containsMouse ? 1 : 0
                            Text {
                                anchors.centerIn: parent; text: "×"
                                color: rmArea.containsMouse ? "#F0B4B4" : "#8A97A3"
                                font.pixelSize: Math.round(16 * chrome.ffs)
                            }
                            MouseArea {
                                id: rmArea
                                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { if (chrome.br) chrome.br.removeBookmark(index) }
                            }
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
                // Scrub state: while a drag is live the fill and the label follow the POINTER, so the bar can
                // be aimed. -1 = not scrubbing, and the reader's own page drives everything again.
                property real scrubFrac: -1
                readonly property bool scrubbing: scrubFrac >= 0
                readonly property int pageTotal: chrome.br ? chrome.br.pageCount : 0
                // The page a scrub is pointing at, on the SAME 1-based scale the fill below draws. Kept in
                // step with ReaderBridge::gotoFraction by hand, because these two are one decision: the page a
                // drag shows must be the page it lands on.
                readonly property int scrubPage: pageTotal > 0
                    ? Math.max(1, Math.min(pageTotal, Math.round(scrubFrac * pageTotal))) : 0
                Rectangle {   // fill
                    anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                    anchors.margins: 2; radius: 9
                    width: {
                        var pc = progress.pageTotal
                        var p  = progress.scrubbing ? progress.scrubPage : (chrome.br ? chrome.br.page : 0)
                        return (pc > 0) ? Math.max(4, (progress.width - 4) * Math.min(1, p / pc)) : 0
                    }
                    color: Qt.rgba(0.23, 0.44, 0.69, 0.55)
                }
                Text {
                    anchors.centerIn: parent
                    // range-aware (comic spread: "3–4 / 20"); during a drag it names the DESTINATION page
                    text: progress.scrubbing ? (progress.scrubPage + " / " + progress.pageTotal)
                                             : (chrome.br ? chrome.br.pageLabel : "")
                    color: "#C7D0DA"; font.pixelSize: Math.round(12 * chrome.ffs)
                }
                // The bar is a place to GO, not just a readout: press it and the reader turns to that page,
                // drag along it to hunt. This was the one control in the strip that drew a scale and then did
                // nothing when you aimed at it. The cursor also moves onto the bar so a pad picks up where the
                // pointer left off, matching what every other control in this strip does on a click.
                MouseArea {
                    anchors.fill: parent
                    enabled: progress.pageTotal > 0
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    preventStealing: true
                    function fracAt(px) { return width > 0 ? Math.max(0, Math.min(1, px / width)) : 0 }
                    onPressed: function(mouse) {
                        if (chrome.g) chrome.g.select("readerNav", 1)
                        progress.scrubFrac = fracAt(mouse.x)
                    }
                    onPositionChanged: function(mouse) { if (pressed) progress.scrubFrac = fracAt(mouse.x) }
                    onCanceled: progress.scrubFrac = -1   // grab lost: no page turn from a gesture that died
                    onReleased: function(mouse) {
                        var f = fracAt(mouse.x)
                        progress.scrubFrac = -1
                        if (chrome.br) chrome.br.gotoFraction(f)
                    }
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
