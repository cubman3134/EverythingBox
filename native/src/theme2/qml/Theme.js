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
