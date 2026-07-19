// Generic theme-driven view renderer. Given a `view` (background + an `elements` array) and the live data
// (`items`, `system`, `currentIndex`), it positions each element by fractional pos/size/origin/zIndex and
// loads the matching element component. The look is 100% in the theme - this file knows nothing about it.
import QtQuick
import "Theme.js" as T

Item {
    id: root
    width: 1280
    height: 720

    // --- injected from C++ / the host -----------------------------------------------------------------
    property var theme: ({})       // the whole theme: { name, views: { home:{...}, browse:{...}, detail:{...} } }
    property string currentView: "home"
    property string detailReturn: "home" // where Esc returns to after the detail view
    property var items: []         // the catalog rows for this view
    property var system: ({})      // view-level info (name, counts, ...)
    property int currentIndex: 0   // the selected row
    property string base: ""       // theme directory as a file:// URL, for resolving relative asset paths

    // The active view's definition (background + elements). Switching currentView re-renders everything.
    readonly property var view: (theme && theme.views && theme.views[currentView]) ? theme.views[currentView] : ({})
    function hasView(name) { return !!(theme && theme.views && theme.views[name]) }
    // Optional vertical background gradient: background.gradient = ["#top", "#bottom"] (else a flat colour).
    readonly property var bgGradient: (view && view.background && view.background.gradient) ? view.background.gradient : null

    focus: true
    signal activated(int index)    // Enter on the selected row (host decides what to open)
    signal back()                  // Esc / Back at the root (home) view
    signal cycleTheme()            // T: next theme (the host swaps the theme file)
    signal searchRequested()       // "/" or search key: the host prompts for a query and runs it
    signal nearEnd()               // selection is within a few items of the end: the host pulls the next page
    signal navigate()              // the selection actually moved (for the host's navigation sound)
    signal details()               // entered the detail view (for the host's "open details" sound)
    signal categoryChanged()       // XMB: moved to another category (host loads its column); read catIndex
    signal selectionMoved()        // XMB: the column selection moved (host fetches that item's metadata)
    signal actionChosen(int which) // XMB: chose an inline action (0 = Play, 1 = Favorite, 2 = Add to playlist, 3 = Download)
    signal addToPlaylistRequested() // XMB: "P" on the highlighted item -> host adds it to a playlist
    signal actionRequested(string name) // a `button` element was clicked -> host runs the named action

    // XMB (cross) state. categories = the horizontal axis; items = the active category's column; catIndex /
    // currentIndex are the two cursors. xmbMode is on when the active view contains an `xmb` element, which
    // flips Left/Right to category navigation and Up/Down to moving within the column.
    property var categories: []
    property int catIndex: 0

    // XMB live-meta + inline actions (host-driven; the Triple theme reads these). selectedMeta is the data for
    // the metadata panel beside the cross. actionsOpen shows a Play/Favorite chooser over the selected leaf;
    // actionIndex is its cursor (0 = Play, 1 = Favorite), actionFav reflects the item's current favourite
    // state, actionItem is the browse index being acted on (so the host knows which row when an action fires).
    property var selectedMeta: ({})
    property bool actionsOpen: false
    property int actionIndex: 0
    property bool actionFav: false
    property int actionItem: -1

    property string nowPlaying: "" // current background-music track name (host-set; the "nowplaying" element reads it)
    property bool catLoading: false // host-set: the selected category's column is fetching (XMB shows a spinner)
    // Shared UI motion duration for theme transitions (XMB category slide / drill push). Host-fed from
    // kUiFadeMs in native/src/ui/FeedbackPolicy.h — that header owns the canonical value; this default only
    // covers hosts that don't set it (probes, previews).
    property int uiMotionMs: 150

    // Bottom-bar buttons (e.g. the Channels theme's Settings/Profile corner buttons) join keyboard/controller
    // navigation: pressing Down at the bottom of the grid moves focus into this "button zone". buttonList is
    // the view's `button` actions left-to-right; buttonIndex is the cursor; focusedButtonAction is what a
    // `button` element checks to draw its focus ring (empty string = the grid still has focus).
    property int focusZone: 0   // 0 = main content (grid/carousel), 1 = the bottom buttons
    property int buttonIndex: 0
    readonly property var buttonList: {
        var out = []
        if (view && view.elements) {
            var arr = []
            for (var i = 0; i < view.elements.length; i++) {
                var el = view.elements[i]
                if (el.type === "button" && el.action)
                    arr.push({ x: (el.pos && el.pos.length) ? Number(el.pos[0]) : 0, action: el.action })
            }
            arr.sort(function(a, b) { return a.x - b.x })
            for (var j = 0; j < arr.length; j++) out.push(arr[j].action)
        }
        return out
    }
    readonly property string focusedButtonAction:
        (focusZone === 1 && buttonIndex >= 0 && buttonIndex < buttonList.length) ? buttonList[buttonIndex] : ""

    // UI-test introspection: expose the current selection as plain strings, computed in QML (the items /
    // categories are native JS arrays here). The C++ UiTestServer reads these directly, instead of
    // marshaling the whole `var` arrays across the boundary.
    readonly property string uitestSelection:
        (items && currentIndex >= 0 && currentIndex < items.length && items[currentIndex]
         && items[currentIndex].title) ? items[currentIndex].title : ""
    readonly property string uitestCategory:
        (xmbMode && categories && catIndex >= 0 && catIndex < categories.length && categories[catIndex]
         && categories[catIndex].title) ? categories[catIndex].title : ""

    readonly property bool xmbMode: {
        if (view && view.elements)
            for (var i = 0; i < view.elements.length; i++)
                if (view.elements[i].type === "xmb") return true
        return false
    }
    // Navigation is arbitrated by the NavGraph selection model exposed as the `nav` context object. Arrow
    // keys, the wheel and mouse clicks all become REQUESTS to `nav`; the C++ bridge writes the resolved
    // selection back into catIndex / currentIndex / buttonIndex / actionIndex (and focusZone) — the props
    // every element binds. This file never assigns those selection props directly: the model owns the
    // clamp + divider-skip arbitration, so one resolver is the single source of truth.
    //
    // ZONE TRANSITIONS ARE DECLARED IN THE MODEL (ThemeEngine::buildView registers the zones AND their
    // declared edges: items<->categories two-cursor switching with the fused step, items->buttons on the
    // grid's bottom row, buttons->items back up). Keys therefore call nav.move()/nav.activate()/nav.back()
    // only — reachability lives in the graph, which validates its own connectivity (Invariant 2), not in
    // this key handler. The two prop-sync handlers below (onCurrentIndexChanged / onCatIndexChanged) keep
    // the model aligned when the HOST writes a selection prop directly (column reload, browse restore).
    // What legitimately remains here is the grid's 2-D geometry (gridCols row math + which row is the
    // bottom): the model's strips are 1-D, so grid arrows compute the target index here and hand it to
    // nav.select — a request, not an arbitration (the model still snaps/clamps it).

    // Horizontal move (Left/Right). XMB: one press switches to + steps the category axis via the declared
    // items->categories edge (fused step; the host reloads the column). Grid/carousel: one item along the
    // row. `d` is +1 (right) / -1 (left).
    function navHorizontal(d) {
        if (xmbMode) {
            if (nav.move(d > 0 ? Qt.Key_Right : Qt.Key_Left)) { navigate(); categoryChanged() }
        } else {
            gridSelect(currentIndex + d, d)                    // one item along the strip
        }
    }
    // Vertical move (Up/Down key, wheel). XMB: step the item column (divider-skipping lives in the model;
    // from the category cursor the declared categories->items edge switches + steps in one press).
    // Grid: jump a full row; carousel: one item. `d` is +1 (down) / -1 (up).
    function navVertical(d) {
        if (xmbMode) {
            if (nav.move(d > 0 ? Qt.Key_Down : Qt.Key_Up)) navigate()
        } else {
            gridSelect(currentIndex + d * gridCols, d)
        }
    }
    // Grid/carousel step: seek to the nearest selectable at/after `target` travelling `dir` (skipping
    // dividers, continuing the travel direction), then ask `nav` to select it (the model no-op-snaps the
    // already-selectable index). Only a real move plays the navigation sound.
    function gridSelect(target, dir) {
        var n = items ? items.length : 0
        if (n <= 0) return
        var t = Math.max(0, Math.min(n - 1, target))
        var ni = seekSelectable(t, dir >= 0 ? 1 : -1)
        if (ni < 0) ni = seekSelectable(currentIndex, dir >= 0 ? -1 : 1)
        if (ni >= 0 && ni !== currentIndex) { nav.select("items", ni); navigate() }
    }
    // The divider (header:true) rows as an index list, fed to the model as `items`'s unselectable set.
    function headerIndices() {
        var out = []
        if (items) for (var i = 0; i < items.length; i++) if (isHeader(i)) out.push(i)
        return out
    }

    // Mouse parity for the elements: clicking an item navigates to it; clicking the already-selected item
    // activates it (the "click to move, click again to press" model). Clicking also gives the scene keyboard
    // focus so the arrow keys work afterwards. Selection goes through `nav`; the bridge writes the props.
    function gotoItem(i) {
        forceActiveFocus()
        if (actionsOpen) return                            // the chooser is up; clicks go to its buttons
        var n = items ? items.length : 0
        if (i < 0 || i >= n || isHeader(i)) return         // dividers aren't clickable
        if (i === currentIndex) { nav.select("items", i); nav.activate() } // click the focused item -> press it
        else { nav.select("items", i); navigate() }        // click another -> move the selection there
    }
    function buttonAction(name) { forceActiveFocus(); actionRequested(name) } // a `button` element was pressed
    function gotoCat(i) {                                   // XMB: click a category to switch to it
        forceActiveFocus()
        if (actionsOpen) return
        var n = categories ? categories.length : 0
        if (i < 0 || i >= n) return
        if (i === catIndex) {                              // clicking the already-selected category...
            if (!items || items.length === 0) { nav.select("items", currentIndex); nav.activate() } // no column -> open it
            return
        }
        nav.select("categories", i); navigate(); categoryChanged()
    }

    // Fire nearEnd() once the selection gets close to the end, so the host can pull the next page before the
    // user hits the bottom. Debounced by lastNearEnd so we don't spam the host while paging in.
    property int lastNearEnd: -1
    // New item set: re-arm near-end paging and drop the chooser (a QML render-state reset; the bridge's
    // actionsOpenChanged handler drops the model's `actions` zone). Focus returns to the grid via the model:
    // resizing the `items` zone (and the host re-selecting into it) writes focusZone=0. The divider auto-hop
    // that used to live in onCurrentIndexChanged is gone — the model owns it via the unselectable set below.
    onItemsChanged: {
        lastNearEnd = -1; actionsOpen = false
        nav.setZoneCount("items", items ? items.length : 0)  // resize the item zone (the model reassigns/snaps)
        nav.setDividers("items", headerIndices())            // section dividers are non-selectable in the model
    }
    // Keep the model's zone counts in step with the data the theme renders (single-sourced from the QML, so
    // every producer of these arrays stays in sync without per-call-site pairing).
    onCategoriesChanged: nav.setZoneCount("categories", categories ? categories.length : 0)
    onButtonListChanged: nav.setZoneCount("buttons", buttonList.length)
    Component.onCompleted: {
        nav.setZoneCount("items", items ? items.length : 0)
        nav.setDividers("items", headerIndices())
        nav.setZoneCount("categories", categories ? categories.length : 0)
        nav.setZoneCount("buttons", buttonList.length)
    }
    onCurrentIndexChanged: {
        // Prop -> model sync: the HOST writes currentIndex directly (column reload, browse restore); mirror
        // it into the model so nav.move steps from the right place. A bridge-originated write is a no-op
        // here (same zone+index). Skipped while the chooser is open (its parked selection must not be
        // yanked; syncActionsZone re-syncs on close) — and a select onto a hidden zone is refused anyway.
        if (!actionsOpen) nav.select("items", currentIndex)
        var n = items ? items.length : 0
        if (n > 0 && currentIndex >= n - 4 && currentIndex !== lastNearEnd) { lastNearEnd = currentIndex; nearEnd() }
        selectionMoved() // host fetches the newly-selected item's metadata for the live panel (XMB)
    }
    // Same sync for the category cursor (the host seeds catIndex at build/restore).
    onCatIndexChanged: if (!actionsOpen) nav.select("categories", catIndex)

    // Up/Down jump by a grid's column count when the view has a grid; otherwise step by one (carousels).
    property int gridCols: {
        if (view && view.elements)
            for (var i = 0; i < view.elements.length; i++)
                if (view.elements[i].type === "grid" || view.elements[i].type === "channels")
                    return Math.max(1, Number(view.elements[i].columns || 4))
        return 1
    }
    // Section dividers ({header:true}) are non-selectable: navigation steps over them.
    function isHeader(i) { return !!(items && items[i] && items[i].header) }
    function seekSelectable(from, dir) { // nearest non-header at/after `from` travelling `dir`; -1 if none
        var n = items ? items.length : 0
        for (var i = from; i >= 0 && i < n; i += dir) if (!isHeader(i)) return i
        return -1
    }
    // (step/vstep/stepCat were folded into navVertical/navHorizontal/gridSelect above — all selection now
    //  flows through `nav`, so there are no direct index mutations left here.)

    // Mouse wheel navigates the vertical selection anywhere in the view (down = next, up = previous). Deltas
    // accumulate so one mouse notch = one step and a trackpad doesn't over-scroll. Scrolling returns focus to
    // the grid via the model (navVertical selects the `items` zone, and the bridge writes focusZone=0).
    property real wheelAccum: 0
    WheelHandler {
        onWheel: function(e) {
            if (root.actionsOpen) { e.accepted = true; return } // freeze the column while the chooser is up
            root.wheelAccum += e.angleDelta.y
            while (root.wheelAccum >= 120)  { root.navVertical(-1); root.wheelAccum -= 120 } // wheel up -> previous
            while (root.wheelAccum <= -120) { root.navVertical(1);  root.wheelAccum += 120 } // wheel down -> next
            e.accepted = true
        }
    }
    Keys.onPressed: function(e) {
        // The XMB inline action chooser (Play / Favorite / …) owns the keys while it's open: the model's
        // selection is parked in the wrapping `actions` zone, so Up/Down step it, Enter fires it, Esc dismisses
        // (which flips actionsOpen — the bridge then drops the zone and restores the item cursor).
        if (actionsOpen) {
            if (e.key === Qt.Key_Down)      { if (nav.move(Qt.Key_Down)) navigate() }
            else if (e.key === Qt.Key_Up)   { if (nav.move(Qt.Key_Up))   navigate() }
            else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Select || e.key === Qt.Key_Space)
                                            { nav.activate() }
            else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_Backspace) { actionsOpen = false }
            e.accepted = true
            return
        }
        // The bottom button bar has focus (the model's selection sits in the `buttons` zone): Left/Right step
        // it, Up leaves via the declared buttons->items edge (the grid cursor comes back from zone memory),
        // Enter fires the focused button, Esc leaves like Up (silently — no move sound on a cancel).
        if (focusZone === 1) {
            if (e.key === Qt.Key_Left)       { if (nav.move(Qt.Key_Left))  navigate() }
            else if (e.key === Qt.Key_Right) { if (nav.move(Qt.Key_Right)) navigate() }
            else if (e.key === Qt.Key_Up)    { nav.move(Qt.Key_Up); navigate() }   // edge -> back to the grid
            else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Select || e.key === Qt.Key_Space)
                                             { nav.activate() }
            else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_Backspace) { nav.move(Qt.Key_Up) }
            e.accepted = true
            return
        }
        // XMB cross: Left/Right switch category (the host reloads its column), Up/Down move within the column.
        if (xmbMode && (e.key === Qt.Key_Right || e.key === Qt.Key_Left)) {
            navHorizontal(e.key === Qt.Key_Right ? 1 : -1); e.accepted = true
        }
        else if (xmbMode && (e.key === Qt.Key_Down || e.key === Qt.Key_Up)) {
            navVertical(e.key === Qt.Key_Down ? 1 : -1); e.accepted = true
        }
        else if (e.key === Qt.Key_Right)                        { navHorizontal(1);  e.accepted = true }
        else if (e.key === Qt.Key_Left)                         { navHorizontal(-1); e.accepted = true }
        else if (e.key === Qt.Key_Down) {
            // Move to the row below if there is one; at the bottom row, drop focus to the bottom buttons via
            // the declared items->buttons edge (entered at the bar's remembered button). The QML decides WHEN
            // (it owns the gridCols row geometry); the model owns WHERE the focus lands.
            var n = items ? items.length : 0
            if (currentIndex + gridCols < n) navVertical(1)
            else if (buttonList.length > 0) { nav.move(Qt.Key_Down); navigate() }
            e.accepted = true
        }
        else if (e.key === Qt.Key_Up)                           { navVertical(-1);   e.accepted = true }
        else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Select || e.key === Qt.Key_Space)
                                                                { if (!isHeader(currentIndex)) nav.activate(); e.accepted = true }
        // Info opens the theme's detail view for the focused item; Esc backs out of it to wherever it came
        // from (home or browse). Esc in home/browse asks the host to go back (it owns the home<->browse step).
        else if ((e.key === Qt.Key_I || e.key === Qt.Key_Info) && hasView("detail") && currentView !== "detail")
                                                                { detailReturn = currentView; currentView = "detail"; details(); e.accepted = true }
        else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_Backspace) {
            if (currentView === "detail") currentView = detailReturn
            else nav.back()   // empty level stack -> rootBack -> the host's themed back() (drill up / pause menu)
            e.accepted = true
        }
        else if (e.key === Qt.Key_T)                            { cycleTheme();     e.accepted = true }
        // "P" adds the highlighted item to a playlist (the host ignores it off a real media row).
        else if (e.key === Qt.Key_P)                            { addToPlaylistRequested(); e.accepted = true }
        // "/" (or the dedicated Search key) asks the host to prompt for a query. Not in the detail view.
        else if ((e.key === Qt.Key_Slash || e.key === Qt.Key_Search) && currentView !== "detail")
                                                                { searchRequested(); e.accepted = true }
    }

    // The data context bindings resolve against. Recomputed when the selection changes.
    readonly property var dataCtx: ({
        "system": system,
        "items": items,
        "index": currentIndex,
        "count": items ? items.length : 0,
        "selected": (items && items.length > currentIndex && currentIndex >= 0) ? items[currentIndex] : ({}),
        "focusZone": focusZone // 1 = focus has left the grid for the bottom buttons (grid drops its selection)
    })

    // type -> exact component filename (QML files must be capitalised; filesystems may be case-sensitive).
    readonly property var elementFiles: ({
        "text": "Text", "datetime": "DateTime", "image": "Image", "rating": "Rating",
        "grid": "Grid", "carousel": "Carousel", "video": "Video", "helpsystem": "HelpSystem",
        "particles": "Particles", "xmb": "Xmb", "wave": "Wave", "button": "Button", "panel": "Panel",
        "channels": "Channels", "clock": "Clock", "nowplaying": "NowPlaying",
        "gallery": "Gallery"
    })
    function urlFor(type) { return Qt.resolvedUrl("elements/" + (elementFiles[type] ? elementFiles[type] : type) + ".qml") }

    // Resolve an asset path: a URL as-is, an absolute path to a file URL, else relative to the theme dir.
    function resolve(p) {
        if (!p) return ""
        if (p.indexOf("://") >= 0) return p
        if (p.length > 1 && (p.charAt(0) === "/" || p.charAt(1) === ":")) return "file:///" + p
        return base + "/" + p
    }

    // --- background -----------------------------------------------------------------------------------
    // OUTSIDE the fading `content` below and always fully opaque, so switching views never drops to the
    // window's near-black clear colour (which read as a black screen in full screen). Only the foreground
    // elements fade; the backdrop stays solid the whole time.
    Rectangle { anchors.fill: parent; color: T.val(root.view ? root.view.background : null, "color", "#0F1216") }
    Rectangle { // optional vertical gradient (top -> bottom) over the flat colour
        anchors.fill: parent
        visible: !!root.bgGradient
        gradient: Gradient {
            GradientStop { position: 0.0; color: (root.bgGradient && root.bgGradient.length > 0) ? root.bgGradient[0] : "transparent" }
            GradientStop { position: 1.0; color: (root.bgGradient && root.bgGradient.length > 1) ? root.bgGradient[1] : "transparent" }
        }
    }
    Image {
        anchors.fill: parent
        source: root.resolve(T.val(root.view ? root.view.background : null, "image", ""))
        visible: source != "" && status === Image.Ready
        fillMode: Image.PreserveAspectCrop
    }
    Rectangle { anchors.fill: parent; color: "black"; opacity: Number(T.val(root.view ? root.view.background : null, "dim", 0)) }

    // The foreground elements fade in on first appear and re-fade whenever the active view switches
    // (home<->browse<->detail), so transitions read smoothly. Software-backend friendly (an opacity animation).
    Item {
        id: content
        anchors.fill: parent
        opacity: 0
        Component.onCompleted: fade.restart()
        NumberAnimation { id: fade; target: content; property: "opacity"; from: 0; to: 1; duration: 220; easing.type: Easing.OutCubic }

        // --- elements ---------------------------------------------------------------------------------
        Repeater {
            model: (root.view && root.view.elements) ? root.view.elements : []
            delegate: Item {
                id: cell
                required property var modelData
                property var el: modelData
                property var p: el.pos || [0, 0]
                property var s: el.size || [0.1, 0.1]
                property var o: el.origin || [0, 0]
                width:  Number(s[0]) * root.width
                height: Number(s[1]) * root.height
                x: Number(p[0]) * root.width  - Number(o[0]) * width
                y: Number(p[1]) * root.height - Number(o[1]) * height
                z: Number(el.zIndex || 0)
                opacity: el.opacity !== undefined ? Number(el.opacity) : 1
                Loader {
                    anchors.fill: parent
                    source: root.urlFor(cell.el.type)
                    onLoaded: {
                        if (!item) return
                        item.el = cell.el
                        item.host = root
                        item.ctx = Qt.binding(function() { return root.dataCtx })
                    }
                }
            }
        }
    }

    // Re-run the fade when the active view changes (e.g. opening/closing the detail view).
    // A view switch (home<->browse<->detail) drops any bottom-button focus back to the content: re-select the
    // `items` zone through the model (the bridge writes focusZone=0) instead of poking the prop directly.
    onCurrentViewChanged: { nav.select("items", currentIndex); fade.restart() }
}
