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
    signal actionChosen(int which) // XMB: chose an inline action (0 = Play, 1 = Favorite, 2 = Add to playlist)
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

    readonly property bool xmbMode: {
        if (view && view.elements)
            for (var i = 0; i < view.elements.length; i++)
                if (view.elements[i].type === "xmb") return true
        return false
    }
    function stepCat(d) {
        var n = categories ? categories.length : 0
        if (n <= 0) return
        var ni = Math.max(0, Math.min(n - 1, catIndex + d))
        if (ni !== catIndex) { catIndex = ni; navigate(); categoryChanged() }
    }

    // Mouse parity for the elements: clicking an item navigates to it; clicking the already-selected item
    // activates it (the "click to move, click again to press" model). Clicking also gives the scene keyboard
    // focus so the arrow keys work afterwards.
    function gotoItem(i) {
        forceActiveFocus()
        if (actionsOpen) return                            // the chooser is up; clicks go to its buttons
        var n = items ? items.length : 0
        if (i < 0 || i >= n) return
        if (i === currentIndex) activated(i)               // click the focused item -> press it
        else { currentIndex = i; navigate() }              // click another -> move the selection there
    }
    function buttonAction(name) { forceActiveFocus(); actionRequested(name) } // a `button` element was pressed
    function gotoCat(i) {                                   // XMB: click a category to switch to it
        forceActiveFocus()
        if (actionsOpen) return
        var n = categories ? categories.length : 0
        if (i < 0 || i >= n) return
        if (i === catIndex) {                              // clicking the already-selected category...
            if (!items || items.length === 0) activated(currentIndex) // ...with no column (Settings) -> open it
            return
        }
        catIndex = i; navigate(); categoryChanged()
    }

    // Fire nearEnd() once the selection gets close to the end, so the host can pull the next page before the
    // user hits the bottom. Debounced by lastNearEnd so we don't spam the host while paging in.
    property int lastNearEnd: -1
    onItemsChanged: { lastNearEnd = -1; actionsOpen = false } // a new/grown set: re-arm nearEnd, drop any chooser
    onCurrentIndexChanged: {
        var n = items ? items.length : 0
        if (n > 0 && currentIndex >= n - 4 && currentIndex !== lastNearEnd) { lastNearEnd = currentIndex; nearEnd() }
        selectionMoved() // host fetches the newly-selected item's metadata for the live panel (XMB)
    }

    // Up/Down jump by a grid's column count when the view has a grid; otherwise step by one (carousels).
    property int gridCols: {
        if (view && view.elements)
            for (var i = 0; i < view.elements.length; i++)
                if (view.elements[i].type === "grid") return Math.max(1, Number(view.elements[i].columns || 4))
        return 1
    }
    function step(d) {
        var n = items ? items.length : 0
        if (n <= 0) return
        var ni = Math.max(0, Math.min(n - 1, currentIndex + d))
        if (ni !== currentIndex) { currentIndex = ni; navigate() } // only on a real move -> nav sound
    }
    // The vertical move (what Down/Up and the mouse wheel do): one item in the XMB column, one grid row else.
    function vstep(dir) { if (xmbMode) step(dir); else step(dir * gridCols) }

    // Mouse wheel navigates the vertical selection anywhere in the view (down = next, up = previous). Deltas
    // accumulate so one mouse notch = one step and a trackpad doesn't over-scroll.
    property real wheelAccum: 0
    WheelHandler {
        onWheel: function(e) {
            if (root.actionsOpen) { e.accepted = true; return } // freeze the column while the chooser is up
            root.wheelAccum += e.angleDelta.y
            while (root.wheelAccum >= 120)  { root.vstep(-1); root.wheelAccum -= 120 } // wheel up -> previous
            while (root.wheelAccum <= -120) { root.vstep(1);  root.wheelAccum += 120 } // wheel down -> next
            e.accepted = true
        }
    }
    Keys.onPressed: function(e) {
        // The XMB inline action chooser (Play / Favorite) owns the keys while it's open: Up/Down toggle the
        // two options, Enter fires the chosen one, Esc dismisses. Everything else is swallowed so the cross
        // behind it stays put.
        if (actionsOpen) {
            if (e.key === Qt.Key_Down)      { actionIndex = (actionIndex + 1) % 3; navigate() }
            else if (e.key === Qt.Key_Up)   { actionIndex = (actionIndex + 2) % 3; navigate() }
            else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Select || e.key === Qt.Key_Space)
                                            { actionChosen(actionIndex) }
            else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_Backspace) { actionsOpen = false }
            e.accepted = true
            return
        }
        // XMB cross: Left/Right switch category (the host reloads its column), Up/Down move within the column.
        if (xmbMode && (e.key === Qt.Key_Right || e.key === Qt.Key_Left)) {
            stepCat(e.key === Qt.Key_Right ? 1 : -1); e.accepted = true
        }
        else if (xmbMode && (e.key === Qt.Key_Down || e.key === Qt.Key_Up)) {
            step(e.key === Qt.Key_Down ? 1 : -1); e.accepted = true
        }
        else if (e.key === Qt.Key_Right)                        { step(1);          e.accepted = true }
        else if (e.key === Qt.Key_Left)                         { step(-1);         e.accepted = true }
        else if (e.key === Qt.Key_Down)                         { step(gridCols);   e.accepted = true }
        else if (e.key === Qt.Key_Up)                           { step(-gridCols);  e.accepted = true }
        else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Select || e.key === Qt.Key_Space)
                                                                { activated(currentIndex); e.accepted = true }
        // Info opens the theme's detail view for the focused item; Esc backs out of it to wherever it came
        // from (home or browse). Esc in home/browse asks the host to go back (it owns the home<->browse step).
        else if ((e.key === Qt.Key_I || e.key === Qt.Key_Info) && hasView("detail") && currentView !== "detail")
                                                                { detailReturn = currentView; currentView = "detail"; details(); e.accepted = true }
        else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_Backspace) {
            if (currentView === "detail") currentView = detailReturn
            else back()
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
        "selected": (items && items.length > currentIndex && currentIndex >= 0) ? items[currentIndex] : ({})
    })

    // type -> exact component filename (QML files must be capitalised; filesystems may be case-sensitive).
    readonly property var elementFiles: ({
        "text": "Text", "datetime": "DateTime", "image": "Image", "rating": "Rating",
        "grid": "Grid", "carousel": "Carousel", "video": "Video", "helpsystem": "HelpSystem",
        "particles": "Particles", "xmb": "Xmb", "wave": "Wave", "button": "Button", "panel": "Panel"
    })
    function urlFor(type) { return Qt.resolvedUrl("elements/" + (elementFiles[type] ? elementFiles[type] : type) + ".qml") }

    // Resolve an asset path: a URL as-is, an absolute path to a file URL, else relative to the theme dir.
    function resolve(p) {
        if (!p) return ""
        if (p.indexOf("://") >= 0) return p
        if (p.length > 1 && (p.charAt(0) === "/" || p.charAt(1) === ":")) return "file:///" + p
        return base + "/" + p
    }

    // The whole view fades in on first appear and re-fades whenever the active view switches (home<->browse
    // <->detail), so transitions read smoothly. Software-backend friendly (just an opacity animation).
    Item {
        id: content
        anchors.fill: parent
        opacity: 0
        Component.onCompleted: fade.restart()
        NumberAnimation { id: fade; target: content; property: "opacity"; from: 0; to: 1; duration: 220; easing.type: Easing.OutCubic }

        // --- background -------------------------------------------------------------------------------
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
    onCurrentViewChanged: fade.restart()
}
