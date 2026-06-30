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
    property var view: ({})        // one view of the theme: { background:{...}, elements:[...] }
    property var items: []         // the catalog rows for this view
    property var system: ({})      // view-level info (name, counts, ...)
    property int currentIndex: 0   // the selected row
    property string base: ""       // theme directory as a file:// URL, for resolving relative asset paths

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
        "grid": "Grid", "carousel": "Carousel", "video": "Video", "helpsystem": "HelpSystem"
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
    Rectangle { anchors.fill: parent; color: T.val(root.view ? root.view.background : null, "color", "#0F1216") }
    Image {
        anchors.fill: parent
        source: root.resolve(T.val(root.view ? root.view.background : null, "image", ""))
        visible: source != "" && status === Image.Ready
        fillMode: Image.PreserveAspectCrop
    }
    Rectangle { anchors.fill: parent; color: "black"; opacity: Number(T.val(root.view ? root.view.background : null, "dim", 0)) }

    // --- elements -------------------------------------------------------------------------------------
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
