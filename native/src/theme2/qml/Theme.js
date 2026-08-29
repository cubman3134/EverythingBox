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
// Does the THEME style this view? This is the question the HOST asks before it OFFERS a route into a view
// ("I" only opens `detail` on a theme that has one), and it must be answered the same way viewFor answers
// "is there a layout here" — an `elements` list that is EMPTY is not a layout. When the two disagreed, a
// theme shipping `"detail": { "elements": [] }` passed the gate on key PRESENCE and then rendered viewFor's
// fallback, which is an item GRID bound to ctx.items: the key router sat in detail mode over a screen
// showing something else entirely, arrows moving an invisible action cursor and Enter firing play/download
// verbs at it. One definition of "declared", asked in both places (ThemeView.hasView, MainWindow's gates).
function declaresView(theme, name) {
    var views = (theme && theme.views) ? theme.views : null
    var v = views ? views[name] : undefined
    return !!(v && v.elements && v.elements.length)
}

// Which view definition renders for `name`. A theme declares the views it styles; when the host navigates
// to one the theme never declared, `theme.views[name]` is absent — and the renderer drew the background
// and NOTHING else: a navigable, selectable, entirely blank screen. That is issue #29 exactly. Triple
// ships no `browse` view (its own format doc said an xmb home does not need one), and the cross-addon
// search from the XMB root is the single route that opens `browse` on an xmb theme, so every result grid
// under Triple was one flat rectangle. No theme can be expected to declare every view the app will ever
// grow — least of all the community themes in the registry, which nobody here can fix — so the renderer
// supplies a plain legible layout for anything left unstyled instead of a blank screen.
function viewFor(theme, name) {
    if (declaresView(theme, name)) return theme.views[name]
    return defaultView((theme && theme.views) ? theme.views["home"] : null)
}

// The built-in layout viewFor falls back to: the view's title, a grid of its items, and the help bar.
// Deliberately minimal and generic — no theme-specific styling to go stale — but it wears the theme's own
// home background (and ink derived from it), so the fallback still reads as that theme rather than as a
// foreign default screen, and is legible on a light theme as well as a dark one.
function defaultView(homeView) {
    var bg = (homeView && homeView.background) ? homeView.background : { "color": "#0F1216" }
    var ink = inkFor(bg)
    var dim = (ink === "#FFFFFF") ? "#C8CEDA" : "#4A5567"
    // A background need not be a colour at all: `{ "image": ..., "dim": ... }` is legal, and then NO ink
    // choice is safe on its own — white over a bright photo is issue #29 again, in a shape no luminance
    // rule can see. Every string this layout paints therefore also carries an outline in the opposite tone,
    // the same hardening the `label: "none"` tile placeholder already uses, so the fallback stays readable
    // over artwork it cannot inspect as well as over a colour it can.
    var edge = (ink === "#FFFFFF") ? "#000000" : "#FFFFFF"
    return {
        "background": bg,
        "elements": [
            { "type": "text", "id": "ebFallbackTitle", "pos": [0.05, 0.08], "size": [0.7, 0.06],
              "origin": [0, 0.5], "binding": "system.name", "color": ink, "outline": edge,
              "fontSize": 0.042, "bold": true },
            { "type": "grid", "id": "ebFallbackGrid", "pos": [0.04, 0.16], "size": [0.92, 0.73],
              "origin": [0, 0], "columns": 5, "aspect": 1.45, "spacing": 0.008, "color": ink,
              "card": { "radius": 10, "fill": "#23272F", "label": "overlay",
                        "selectedWidth": 4, "selectedScale": 1.04 } },
            { "type": "helpsystem", "id": "ebFallbackHelp", "pos": [0.5, 0.955], "size": [1, 0.05],
              "origin": [0.5, 0.5], "color": dim, "outline": edge, "fontSize": 0.022,
              "entries": [ { "button": "↑↓←→", "label": "Navigate" },
                           { "button": "Enter", "label": "Open" },
                           { "button": "I", "label": "Details" },
                           { "button": "/", "label": "Search" },
                           { "button": "M", "label": "Options" },
                           { "button": "Esc", "label": "Back" } ] }
        ]
    }
}

// The luminance the renderer will ACTUALLY PAINT for a background block, on 0..255.
//
// It does not parse the colour itself. `background.color` is handed straight to a QML `Rectangle.color`
// (ThemeView's backdrop), and THEME_FORMAT.md promises only "hex" — so every form QColor accepts is already
// legal and in the wild: `#RGB`, `#RRGGBB`, `#AARRGGBB`, the 9- and 12-digit forms, and the ~150 SVG colour
// NAMES ("white", "whitesmoke", "darkslategray"). A hand-rolled hex reader that recognised only 6 and 8
// digits called every one of the others "dark" and printed WHITE on them, which is issue #29 rebuilt on the
// very layer added to fix it: a registry theme with `"home": { "background": { "color": "#EEE" } }` and no
// `browse` rendered the cross-add-on search as a near-white screen with white text on it.
//
// So it asks Qt.color — the SAME parser the Rectangle uses — and is exact by construction rather than by
// approximation. Its failure mode is exact too: Qt.color THROWS on a string QColor cannot read, and a
// Rectangle handed that same string paints BLACK (verified against a live QML engine: "#FFF8", "nonsense"
// and "#GGGGGG" all render #000000). An unreadable colour is therefore not a guess — the screen really is
// black — so it scores 0 and earns white ink. Alpha counts, because the backdrop composites over the
// window's near-black clear colour: a half-transparent white paints as mid-grey, not as white.
// Any colour a theme may legally write, normalised to canonical "aarrggbb" — or "" when Qt cannot read it.
// The normalising is Qt's own: Qt.color() parses the string and STRINGIFIES back to "#rrggbb" (or
// "#aarrggbb" when it is translucent), so "#EEE", "WhiteSmoke", "#EEEFFF888" and "#FFEFF3F8" all arrive
// here as eight plain hex digits and only one very boring reader is needed below. The round-trip is used
// rather than the colour's r/g/b properties on purpose: those accessors come from a QML value type that is
// only registered once a scene exists, while the string form is available anywhere the engine is.
function normalizeColor(s) {
    var c = null
    try { if (typeof Qt !== "undefined" && Qt && Qt.color) c = Qt.color(s) } catch (e) { return "" }
    if (c === undefined || c === null) return ""
    var h = String(c).replace("#", "")
    if (h.length === 6) h = "ff" + h                       // opaque: Qt omits the alpha pair
    // Deliberately UNREACHABLE today, and kept: a valid QColor stringifies to "#rrggbb"/"#aarrggbb" and
    // nothing else, so no test can currently distinguish this from a bare length check (weakening it to one
    // survives mutation). It stays because it is the only thing standing between an outside library's
    // output format and the parseInt calls in bgLuma, which are written to assume eight hex digits — not
    // because a branch below it would otherwise mop up the failure. That is the difference between this and
    // the isNaN guard that used to sit at the end of inkFor, which could never change an answer at all.
    if (!/^[0-9a-fA-F]{8}$/.test(h)) return ""
    return h
}

function bgLuma(bg) {
    var s = ""
    if (bg && bg.color) s = String(bg.color)
    else if (bg && bg.gradient && bg.gradient.length) s = String(bg.gradient[0])
    // Two ways to get nothing, and they deliberately share one answer rather than a sentinel each. A block
    // with no colour at all (`{ "image": …, "dim": … }`) is genuinely unknowable, and a string QColor
    // cannot read paints black — and BOTH want the dark end of the rule, because that is the tone
    // defaultView haloes in black, the pairing that reads over a photograph as well as over a black box.
    // Distinguishing them would add a branch that could never change an answer.
    var h = normalizeColor(s)
    if (h === "") return 0
    var a = parseInt(h.substring(0, 2), 16) / 255
    var r = parseInt(h.substring(2, 4), 16)
    var g = parseInt(h.substring(4, 6), 16)
    var b = parseInt(h.substring(6, 8), 16)
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) * a
}

// Readable ink for a background block: white on a dark one, near-black on a light one. The fallback view
// has no theme palette to read, so it derives one — assuming "dark" would print white on white for the
// light themes (Channels' browse gradient starts at #EFF3F8) and swap one unreadable screen for another.
// Takes the flat `color`, else the first gradient stop, and uses the sRGB luma coefficients.
//
// One comparison is the whole rule. There is no NaN guard: normalizeColor establishes that all eight digits
// are hex before bgLuma reads any of them, so parseInt cannot hand one back — and a guard whose only effect
// would be to return the value the comparison already returns is not a guard, it is unreachable prose.
function inkFor(bg) { return bgLuma(bg) > 140 ? "#111820" : "#FFFFFF" }

// Resolve an Image element's source through the role + fallback chain:
//   literal path / binding  ->  el.role (selected.images[role])  ->  el.fallback (another role, then a
//   literal/default path). Returns "" when nothing resolves (the element then shows its placeholder or,
//   if textFallback is set, the bound text).
//
// This answers WHICH STRING WINS and nothing else. It cannot be handed to a resolver, because the four
// branches do not come from the same place: `path` and a literal `fallback` are written by the THEME, while
// `binding`, `role` and a `fallback` that names a role are whatever the provider supplied. imageUrl() below
// walks the same branches and applies the right rule to each; keep the two orders identical.
function imageSource(el, ctx) {
    var s = sourceOf(el, ctx)
    if (s) return s
    if (el && el.role) { s = artUrl(ctx, el.role); if (s) return s }
    if (el && el.fallback) {
        var fb = artUrl(ctx, el.fallback) // treat the fallback as a role first...
        if (fb) return fb
        return el.fallback                // ...else a literal / default path (themeAsset judges it)
    }
    return ""
}

// --- Where a theme's asset path is allowed to point ----------------------------------------------------
// Two rules, because the renderer resolves strings from two DIFFERENT places and only one of them is
// untrusted. A theme.json arrives from a public, third-party-writable registry; a poster url arrives from
// whichever addon the user chose to install. Collapsing them into one permissive resolver (which is what
// the single `resolve()` these replace did) meant a manifest could name anything a provider could.
//
// themeAsset() is the JS twin of ThemeAssetPath::resolve in src/core/ThemeAssetPath.h — same policy, same
// case table (probe_theme §8 pins the C++ half, probe_themeview §9 pins this one). It is deliberately a
// STRICT SUBSET on one axis: the C++ half judges an ABSOLUTE path by containment (one pointing inside the
// folder is the same file spelled the long way), this one refuses absolutes outright. The QML side holds the
// theme folder as a file:// URL rather than a path, so it cannot do path-space containment without a second
// property to keep in sync — and an absolute path in a manifest is unportable by construction, so no theme
// that ships to another machine can be using one. Keep that difference here, and keep it pinned.
//
// THIS FILE HAS A CONSUMER OUTSIDE THIS REPO. The community theme registry
// (github.com/cubman3134/everythingbox-themes) gates submissions by DOWNLOADING this file from `main` and
// running it under node — `.pragma`/`.import` blanked, then `themeAsset(base, path)` called by name — so
// that the registry rejects a theme the app would refuse to render, without keeping a third copy of the rule
// that could drift permissive. Two consequences, neither visible from here:
//
//   * RENAMING OR MOVING themeAsset() breaks that gate. It fails closed (its CI exits 2 and judges no
//     theme, rather than passing everything), so nothing unsafe is published — but submissions stop being
//     checked until someone updates tools/rule-shim.js there. Renaming it is fine; doing so silently is not.
//   * A change to the RULE takes effect for the registry as soon as it lands on `main`, with no version pin.
//     That is intended. It also means tightening the rule can turn an already-published theme into a
//     rejected one, which is a thing to decide deliberately rather than discover from a contributor.
//
// (contentUrl has no such consumer — the registry only judges manifest paths.)

// Fold "." and ".." segments of a RELATIVE path. Returns "" if it climbs above the root, or lands on the
// root itself. Anchored per segment, so a sibling whose name merely EXTENDS this folder ("…/NightMare"
// beside "…/Night") can never come out of it: this walks segments, it does not compare string prefixes.
function cleanRelPath(p) {
    var parts = String(p).split("/")
    var out = []
    for (var i = 0; i < parts.length; i++) {
        var seg = parts[i]
        if (seg === "" || seg === ".") continue     // "a//b" and "a/./b" are just "a/b"
        if (seg === "..") { if (out.length === 0) return ""; out.pop(); continue }
        out.push(seg)
    }
    return out.length ? out.join("/") : ""
}

// A path the THEME's manifest named (background.image, fontFile, a category icon, a literal `path` /
// `fallback`). Confined to the theme's own folder. REJECT, DO NOT SANITIZE — a refused path yields "" and
// the element draws its placeholder, because trimming a bad path back inside the folder would invent a file
// the theme never asked for and paint art nobody chose.
function themeAsset(base, p) {
    if (!base || !p) return ""
    p = String(p)
    // No remote assets in a manifest. THEME_FORMAT.md has only ever documented these as paths relative to
    // the theme folder, so nothing legitimate is refused here — while a registry-installed theme.json
    // naming http://… would beacon the viewer's IP on every render, let its author change what is painted
    // after the theme was reviewed, and break offline use. A provider's url is a different question and
    // keeps its scheme; see contentUrl.
    if (p.indexOf("://") >= 0) return ""
    // Refused on EVERY platform, not just the one where the OS would act on them: a backslash is a separator
    // on Windows and a colon makes a path drive-relative there. A manifest travels between machines, so a
    // path that escapes on Windows must not resolve to a merely odd filename elsewhere.
    if (p.indexOf("\\") >= 0 || p.indexOf(":") >= 0) return ""
    if (p.charAt(0) === "/") return ""              // absolute — see the note above on the C++ twin
    var rel = cleanRelPath(p)
    if (rel === "") return ""                       // climbed out of the folder, or named the folder itself
    return base + "/" + rel
}

// A url the CONTENT supplied (a provider's poster, a scraped image, a local-library thumb). Deliberately
// permissive, and deliberately unchanged from the resolver this pair replaces: providers serve artwork over
// https, and MetaCache/LocalLibrary hand back ABSOLUTE LOCAL PATHS (the offline image cache, an NFO's
// <thumb>) as the url for a tile. Containment-checking this would blank every cached poster; refusing a
// scheme would blank every catalog. The relative branch is vestigial — no provider produces one — but is
// kept so this is a move rather than a second change riding along.
function contentUrl(base, p) {
    if (!p) return ""
    p = String(p)
    if (p.indexOf("://") >= 0) return p
    if (p.length > 1 && (p.charAt(0) === "/" || p.charAt(1) === ":")) return "file:///" + p
    return base ? base + "/" + p : p
}

// Resolve an Image/Video element's source, applying the right rule to whichever branch wins. Same branch
// ORDER as imageSource above — this is that function with a resolver attached to each arm, so the two can
// never disagree about precedence. `host` supplies themeAsset/contentUrl (ThemeView.qml).
//
// A refused manifest path stops here rather than falling through to the next branch: falling through would
// paint the role's art in place of the path the theme asked for, which is the sanitising behaviour the whole
// rule exists to refuse.
function imageUrl(el, ctx, host) {
    if (!host) return ""
    if (el && el.path) return host.themeAsset(el.path)
    if (el && el.binding) { var v = dig(ctx, el.binding); if (v) return host.contentUrl(String(v)) }
    if (el && el.role) { var s = artUrl(ctx, el.role); if (s) return host.contentUrl(s) }
    if (el && el.fallback) {
        var fb = artUrl(ctx, el.fallback)           // treat the fallback as a role first...
        if (fb) return host.contentUrl(fb)
        // ...else it is a literal path the THEME wrote — but only if it actually looks like one (has a
        // separator or an extension). A bare role word ("poster") whose role simply has no art used to fall
        // through here and become a bogus theme-file load (".../themes2/Triple/poster"): one failed async
        // load + one logged warning PER SELECTION STEP, forever. No art is the honest answer for that case.
        if (el.fallback.indexOf("/") >= 0 || el.fallback.indexOf(".") >= 0)
            return host.themeAsset(el.fallback)
    }
    return ""
}

// The Gallery element's already-resolved image list: the selected item's art for `role` (content), else the
// single `fallback` (another role, else a literal path the theme wrote). Resolved HERE rather than in the
// element for the same reason as imageUrl — the element cannot see which branch produced each string. A
// refused entry is dropped, so the reel cycles what it may show instead of stalling on a blank frame.
function galleryUrls(el, ctx, host) {
    if (!host) return []
    var list = artList(ctx, val(el, "role", "screenshot"))
    if (list.length > 0) {
        var out = []
        for (var i = 0; i < list.length; i++) { var u = host.contentUrl(list[i]); if (u) out.push(u) }
        return out
    }
    if (el && el.fallback) {
        var fb = artUrl(ctx, el.fallback)
        var one = fb ? host.contentUrl(fb) : host.themeAsset(el.fallback)
        if (one) return [one]
    }
    return []
}
