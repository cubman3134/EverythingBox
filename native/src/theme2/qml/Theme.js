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

// Display text: a literal `text`, else the resolved `binding`, else "".
function textOf(el, ctx) {
    if (el && el.text !== undefined && el.text !== "") return el.text
    if (el && el.binding) { var v = dig(ctx, el.binding); return (v === undefined || v === null) ? "" : String(v) }
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
