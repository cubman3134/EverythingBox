// Shared helpers for the theme engine: reading element properties with fallbacks and resolving data
// bindings (a "a.b.c" path) against the live data context.
.pragma library

function val(el, key, def) {
    if (!el) return def
    var v = el[key]
    return (v === undefined || v === null || v === "") ? def : v
}

function num(el, key, def) { return Number(val(el, key, def)) }

// Walk a "selected.title" path against a context object.
function dig(ctx, path) {
    if (!ctx || !path) return undefined
    var parts = String(path).split(".")
    var o = ctx
    for (var i = 0; i < parts.length; i++) {
        if (o === undefined || o === null) return undefined
        o = o[parts[i]]
    }
    return o
}

// Coerce a resolved binding to display text. A binding can land on a NON-scalar — `facts` is a list of
// {label,value}, `images` is a role map — and String()-ing those paints "[object Object]" into the theme.
// A list of primitives reads as a comma list; anything else non-scalar renders as NOTHING, so a binding
// aimed at structured data is silent rather than noisy. Every panel that carries such a list also publishes
// a joined scalar beside it for themes to bind (e.g. `factsText` — see themes2/THEME_FORMAT.md).
function scalarText(v) {
    if (v === undefined || v === null) return ""
    if (Array.isArray(v)) {
        var out = []
        for (var i = 0; i < v.length; i++) {
            if (v[i] !== null && typeof v[i] === "object") return ""
            out.push(String(v[i]))
        }
        return out.join(", ")
    }
    if (typeof v === "object") return ""
    return String(v)
}

// Display text: a literal `text`, else the resolved `binding`, else "".
function textOf(el, ctx) {
    if (el && el.text !== undefined && el.text !== "") return el.text
    if (el && el.binding) return scalarText(dig(ctx, el.binding))
    return ""
}

// Source path: a literal `path`, else the resolved `binding`.
function sourceOf(el, ctx) {
    if (el && el.path) return el.path
    if (el && el.binding) { var v = dig(ctx, el.binding); return v ? String(v) : "" }
    return ""
}

// --- Extensible artwork/media roles -------------------------------------------------------------------
// Items carry an open-ended `images` map (role -> [urls], best first) plus scalar aliases (selected.logo,
// selected.box, ...) and `videos` / `audio` lists, all optional. These helpers read a role with graceful
// absence so a theme binding to art a provider didn't supply falls through to the element's default.

// All urls for an image role on the selected item: selected.images[role] (array), else the scalar alias
// selected[role] as a one-element list, else [].
function artList(ctx, role) {
    if (!ctx || !role) return []
    var sel = ctx.selected
    if (!sel) return []
    var imgs = sel.images
    if (imgs && imgs[role] && imgs[role].length) return imgs[role]
    if (sel[role]) return [String(sel[role])]
    return []
}

// The single best url for an image role, else "".
function artUrl(ctx, role) { var l = artList(ctx, role); return l.length ? l[0] : "" }

// A media list (videos / audio) on the selected item: selected[key] as an array, else [].
function mediaList(ctx, key) {
    if (!ctx) return []
    var sel = ctx.selected
    var v = sel ? sel[key] : undefined
    if (v && v.length !== undefined && typeof v !== "string") return v
    if (v) return [String(v)]
    return []
}

// --- Tile artwork (grids, carousels, channel cells, the XMB column) ------------------------------------
// Every element that draws a row as a TILE goes through the two functions below. They exist because each
// element used to answer both questions for itself and they did not agree — which is how issue #29's
// hardening half got in: a card that decided "draw the title instead" from *whether the row carries an
// image url* hid the title for exactly the rows whose url was dead, leaving a tile with nothing readable
// on it at all. One rule, one place, and no element gets to invent a second spelling of it.

// The roles a tile falls through when a row carries no scalar `image`. Poster-shaped first: a grid /
// carousel / channel cell is a portrait-ish card, so a poster or box beats a wide hero or a banner.
var kTileRoles = ["poster", "box", "thumb", "hero", "banner", "logo"]

// A catalog row's tile artwork url, or "" when the row carries none. Rows reach the themed model from
// several builders; one that publishes only the open-ended `images` role map (no scalar alias) used to
// paint a bare tile in every grid while the artwork sat right there on the row.
function tileImage(item) {
    if (!item) return ""
    if (item.image) return String(item.image)
    var imgs = item.images
    for (var i = 0; i < kTileRoles.length; i++) {
        var r = kTileRoles[i]
        if (imgs && imgs[r] && imgs[r].length) return String(imgs[r][0])
        if (item[r]) return String(item[r])
    }
    return ""
}

// Must this tile draw its title as a placeholder? Whenever no artwork is actually ON SCREEN — the row
// carries none, OR the url it does carry has not loaded / has failed. `artReady` is the element's live
// Image status (status === Image.Ready), so a dead url degrades to readable text rather than a bare tile.
function tileNeedsTitle(item, artReady) { return !artReady || tileImage(item) === "" }

// --- View selection ------------------------------------------------------------------------------------
// Which view definition renders for `name`. A theme declares the views it styles; when the host navigates
// to one the theme never declared, `theme.views[name]` is absent — and the renderer drew the background
// and NOTHING else: a navigable, selectable, entirely blank screen. That is issue #29 exactly. Triple
// ships no `browse` view (its own format doc said an xmb home does not need one), and the cross-addon
// search from the XMB root is the single route that opens `browse` on an xmb theme, so every result grid
// under Triple was one flat rectangle. No theme can be expected to declare every view the app will ever
// grow — least of all the community themes in the registry, which nobody here can fix — so the renderer
// supplies a plain legible layout for anything left unstyled instead of a blank screen.
function viewFor(theme, name) {
    var views = (theme && theme.views) ? theme.views : null
    var v = views ? views[name] : undefined
    if (v && v.elements && v.elements.length) return v
    return defaultView(views ? views["home"] : null)
}

// The built-in layout viewFor falls back to: the view's title, a grid of its items, and the help bar.
// Deliberately minimal and generic — no theme-specific styling to go stale — but it wears the theme's own
// home background (and ink derived from it), so the fallback still reads as that theme rather than as a
// foreign default screen, and is legible on a light theme as well as a dark one.
function defaultView(homeView) {
    var bg = (homeView && homeView.background) ? homeView.background : { "color": "#0F1216" }
    var ink = inkFor(bg)
    var dim = (ink === "#FFFFFF") ? "#C8CEDA" : "#4A5567"
    return {
        "background": bg,
        "elements": [
            { "type": "text", "id": "ebFallbackTitle", "pos": [0.05, 0.08], "size": [0.7, 0.06],
              "origin": [0, 0.5], "binding": "system.name", "color": ink, "fontSize": 0.042, "bold": true },
            { "type": "grid", "id": "ebFallbackGrid", "pos": [0.04, 0.16], "size": [0.92, 0.73],
              "origin": [0, 0], "columns": 5, "aspect": 1.45, "spacing": 0.008, "color": ink,
              "card": { "radius": 10, "fill": "#23272F", "label": "overlay",
                        "selectedWidth": 4, "selectedScale": 1.04 } },
            { "type": "helpsystem", "id": "ebFallbackHelp", "pos": [0.5, 0.955], "size": [1, 0.05],
              "origin": [0.5, 0.5], "color": dim, "fontSize": 0.022,
              "entries": [ { "button": "↑↓←→", "label": "Navigate" },
                           { "button": "Enter", "label": "Open" },
                           { "button": "I", "label": "Details" },
                           { "button": "/", "label": "Search" },
                           { "button": "Esc", "label": "Back" } ] }
        ]
    }
}

// Readable ink for a background block: white on a dark one, near-black on a light one. The fallback view
// has no theme palette to read, so it derives one — assuming "dark" would print white on white for the
// light themes (Channels' browse gradient starts at #EFF3F8) and swap one unreadable screen for another.
// Takes the flat `color`, else the first gradient stop, and uses the sRGB luma coefficients.
function inkFor(bg) {
    var hex = ""
    if (bg && bg.color) hex = String(bg.color)
    else if (bg && bg.gradient && bg.gradient.length) hex = String(bg.gradient[0])
    hex = hex.replace("#", "")
    if (hex.length === 8) hex = hex.substring(2)   // #AARRGGBB -> RRGGBB
    if (hex.length !== 6) return "#FFFFFF"
    var r = parseInt(hex.substring(0, 2), 16)
    var g = parseInt(hex.substring(2, 4), 16)
    var b = parseInt(hex.substring(4, 6), 16)
    if (isNaN(r) || isNaN(g) || isNaN(b)) return "#FFFFFF"
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) > 140 ? "#111820" : "#FFFFFF"
}

// Resolve an Image element's source through the role + fallback chain:
//   literal path / binding  ->  el.role (selected.images[role])  ->  el.fallback (another role, then a
//   literal/default path). Returns "" when nothing resolves (the element then shows its placeholder or,
//   if textFallback is set, the bound text).
function imageSource(el, ctx) {
    var s = sourceOf(el, ctx)
    if (s) return s
    if (el && el.role) { s = artUrl(ctx, el.role); if (s) return s }
    if (el && el.fallback) {
        var fb = artUrl(ctx, el.fallback) // treat the fallback as a role first...
        if (fb) return fb
        return el.fallback                // ...else a literal / default path (host.resolve handles it)
    }
    return ""
}
