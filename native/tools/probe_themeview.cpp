// Headless test for the theme renderer's PURE decisions — the functions in src/theme2/qml/Theme.js that
// answer, for a given theme and a given catalog row, "what actually gets painted?". They are plain JS with
// no scene and no host, so this probe evaluates the SHIPPED file out of the theme2 qrc (the same bytes
// ThemeView.qml imports) in a QML JS engine, with no window and no scene, and pins the answers directly.
//
// Two decisions live here, and both of them were, until issue #29, made implicitly in a way that could
// render a screen the user can navigate but cannot see:
//
//   * viewFor(theme, name) — which view definition renders. A theme declares the views it styles; the
//     renderer used to resolve an UNDECLARED one to `({})`, and an empty definition draws the background
//     and nothing else. Triple ships no `browse` view and the cross-addon search from the XMB root is the
//     one route that opens `browse`, so every result grid under Triple was a single flat rectangle. The
//     fallback layout (and the ink inkFor() derives for it) is what makes that case legible instead.
//
//   * tileImage(item) / tileNeedsTitle(item, artReady) — a tile's artwork, and whether it must fall back to
//     its title. Each tile element used to answer both for itself and they disagreed: a card that decided
//     "draw the title instead" from *whether the row carries an image url* hid the title for exactly the
//     rows whose url was dead, leaving nothing readable on the tile at all.
//
// The RENDERER'S USE of viewFor is a separate claim — asserted end to end by probe_navqml §22, which drives
// the real ThemeEngine::buildView on a browse-less theme. This probe pins the decisions; that one pins that
// the scene actually asks.
//
// Prints THEMEVIEW-OK on success; any failure prints THEMEVIEW-FAIL <what> and exits non-zero.
#include <QFile>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "THEMEVIEW-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

// Load the shipped Theme.js out of the qrc into the JS engine. `.pragma library` is a QML-JS directive that
// the raw evaluator does not parse, so it is stripped — it only tells the QML engine to share one instance
// of the file, which has no meaning here and no effect on any function below.
//
// The engine is a QQmlEngine, and the target links QtQuick, for ONE reason: inkFor resolves a background
// colour by asking `Qt.color` — the very parser a `Rectangle { color: … }` uses — instead of hand-reading
// hex. `Qt` is put on the JS global by the QML engine, and the colour PROVIDER behind Qt.color is installed
// by the QtQuick module. Against a bare QJSEngine there is no `Qt` at all; against Core+Qml alone Qt.color
// throws for every input. Either way inkFor would take its unresolvable branch every time and this file
// would pin a function the app never runs. (`Qt` really is reachable from a `.pragma library` script —
// verified against a live QML engine, and pinned end to end by probe_navqml §22, which reads the ink and
// the outline off a REAL rendered fallback view rather than off a returned object.)
static bool loadThemeJs(QJSEngine& eng)
{
    QFile f(QStringLiteral(":/theme2/Theme.js"));
    if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "THEMEVIEW-FAIL cannot open :/theme2/Theme.js\n"); return false; }
    QString src = QString::fromUtf8(f.readAll());
    const QStringList lines = src.split(QLatin1Char('\n'));
    QStringList kept;
    kept.reserve(lines.size());
    for (const QString& l : lines)
        kept << (l.trimmed().startsWith(QStringLiteral(".pragma")) || l.trimmed().startsWith(QStringLiteral(".import"))
                 ? QString() : l); // blank it, don't drop it — keeps reported line numbers honest
    const QJSValue r = eng.evaluate(kept.join(QLatin1Char('\n')), QStringLiteral("Theme.js"));
    if (r.isError())
    {
        std::fprintf(stderr, "THEMEVIEW-FAIL Theme.js did not evaluate: %s (line %d)\n",
                     qPrintable(r.toString()), r.property(QStringLiteral("lineNumber")).toInt());
        return false;
    }
    return true;
}

// Evaluate an expression against the loaded Theme.js and hand back the result. A THROWN expression is a
// failure in its own right, counted here rather than left to the caller's CHECK: a QJSValue holding an
// Error object is truthy and stringifies to the message, so a bare toBool()/toString() comparison can pass
// straight through a TypeError — which is exactly how a null-safety assertion goes quietly inert.
static QJSValue evalOr(QJSEngine& eng, const char* expr)
{
    const QJSValue v = eng.evaluate(QString::fromLatin1(expr));
    if (v.isError())
    {
        std::fprintf(stderr, "THEMEVIEW-FAIL expression threw: %s -> %s\n", expr, qPrintable(v.toString()));
        ++failures;
    }
    return v;
}

static QString evalStr(QJSEngine& eng, const char* expr) { return evalOr(eng, expr).toString(); }
static bool    evalBool(QJSEngine& eng, const char* expr) { return evalOr(eng, expr).toBool(); }
static int     evalInt(QJSEngine& eng, const char* expr) { return evalOr(eng, expr).toInt(); }

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");   // no scene is built, but QtQuick wants a platform
    QGuiApplication app(argc, argv);
    // Touch QtQuick so the linker cannot drop the module whose static init installs the colour provider
    // that Qt.color needs. Nothing is rendered — this probe has no window and no scene.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQmlEngine eng;
    if (!loadThemeJs(eng)) return 2;
    // The load-bearing precondition for every inkFor assertion below: if `Qt` were missing, inkFor would
    // answer "#FFFFFF" for everything and the light-background cases would fail for a reason that has
    // nothing to do with the rule they are testing. Say so once, up front, rather than 12 times obliquely.
    CHECK(evalStr(eng, "typeof Qt") == QStringLiteral("object"),
          "engine: the QML `Qt` object is on the global — inkFor resolves colours through Qt.color");
    CHECK(evalStr(eng, "typeof Qt.color") == QStringLiteral("function"),
          "engine: …and Qt.color is callable (the colour PROVIDER behind it comes from the QtQuick module)");

    // Fixtures, defined once in the engine so each assertion below reads as one question.
    //   themed   — a theme that styles `home` and `browse` (what a complete theme looks like).
    //   xmbOnly  — the shape that produced issue #29: a dark xmb `home`, and no `browse` at all.
    //   lightOne — a LIGHT home background, for the ink derivation.
    //   emptyEls — declares `browse`, but with an empty element list: the same blank screen, so the same
    //              fallback must apply. A "declared" check that only tested for the KEY would miss this.
    evalOr(eng,
        "var themed = { views: { home: { background: { color: '#0A1326' },"
        "                               elements: [ { type: 'xmb', id: 'cross' } ] },"
        "                       browse: { background: { color: '#123456' },"
        "                                 elements: [ { type: 'grid', id: 'themeOwnGrid' } ] } } };"
        "var xmbOnly = { views: { home: { background: { color: '#0A1326' },"
        "                                elements: [ { type: 'xmb', id: 'cross' } ] } } };"
        "var lightOne = { views: { home: { background: { gradient: ['#EFF3F8', '#C4D2E4'] },"
        "                                 elements: [ { type: 'grid' } ] } } };"
        "var emptyEls = { views: { home: { background: { color: '#0A1326' }, elements: [ { type: 'xmb' } ] },"
        "                          browse: { background: { color: '#0A1326' }, elements: [] } } };"
        "function elTypes(v) { return v.elements.map(function (e) { return e.type }).join(',') }"
        "function elIds(v) { return v.elements.map(function (e) { return e.id }).join(',') }");

    // ---- 1. viewFor: a theme that STYLES the view always wins -----------------------------------------
    // The fallback is a safety net, never a competitor: if the theme declared the view, its own definition
    // comes back verbatim, identity included.
    CHECK(evalStr(eng, "elIds(viewFor(themed, 'browse'))") == QStringLiteral("themeOwnGrid"),
          "viewFor: a declared view is returned as the theme wrote it");
    CHECK(evalStr(eng, "viewFor(themed, 'browse').background.color") == QStringLiteral("#123456"),
          "viewFor: …with its own background, not the home one");
    CHECK(evalStr(eng, "elIds(viewFor(themed, 'home'))") == QStringLiteral("cross"),
          "viewFor: the home view is likewise untouched (the xmb cross still renders)");

    // ---- 2. viewFor: an UNDECLARED view falls back to something that can be seen -----------------------
    // This is issue #29's whole shape: `browse` absent -> the renderer used to get `({})` -> background only.
    CHECK(evalInt(eng, "viewFor(xmbOnly, 'browse').elements.length") > 0,
          "viewFor: an undeclared view renders ELEMENTS, not a bare background (issue #29)");
    // …and specifically a surface that shows the ITEMS. A fallback that drew only a title bar would be just
    // as blank where it matters, so name the element that carries the rows.
    CHECK(evalBool(eng, "elTypes(viewFor(xmbOnly, 'browse')).indexOf('grid') >= 0"),
          "viewFor: the fallback includes a grid — the items are what the screen is FOR");
    CHECK(evalBool(eng, "viewFor(xmbOnly, 'browse').elements.some(function (e)"
                        " { return e.type === 'text' && e.binding === 'system.name' })"),
          "viewFor: …and a title bound to system.name, so the screen says what it is showing");

    // A view DECLARED but empty is the same blank screen as an absent one, and must fall back too.
    CHECK(evalInt(eng, "viewFor(emptyEls, 'browse').elements.length") > 0,
          "viewFor: a declared-but-empty element list falls back as well (a key is not a layout)");

    // Degenerate themes must not blank either — a theme with no views at all, and no theme object.
    CHECK(evalInt(eng, "viewFor({}, 'browse').elements.length") > 0,
          "viewFor: a theme with no views still renders the fallback");
    CHECK(evalInt(eng, "viewFor(null, 'home').elements.length") > 0,
          "viewFor: no theme at all still renders the fallback (never a null deref, never a blank)");

    // ---- 3. the fallback wears the THEME, not a foreign default ---------------------------------------
    // It inherits the theme's own home background, so a user who hits it sees their theme's colour rather
    // than a generic slab, and the ink is derived from that colour rather than assumed.
    CHECK(evalStr(eng, "viewFor(xmbOnly, 'browse').background.color") == QStringLiteral("#0A1326"),
          "fallback: inherits the theme's HOME background (Triple's navy, not a stock grey)");
    CHECK(evalStr(eng, "viewFor(xmbOnly, 'browse').elements[0].color") == QStringLiteral("#FFFFFF"),
          "fallback: white title ink over a dark home background");
    CHECK(evalStr(eng, "viewFor(lightOne, 'browse').elements[0].color") != QStringLiteral("#FFFFFF"),
          "fallback: a LIGHT home background gets dark ink — white-on-white is the same bug again");
    CHECK(evalBool(eng, "JSON.stringify(viewFor(lightOne, 'browse').background.gradient)"
                        " === JSON.stringify(['#EFF3F8','#C4D2E4'])"),
          "fallback: a gradient home background is inherited whole, not flattened");

    // ---- 4. inkFor: the luminance rule the fallback leans on -------------------------------------------
    CHECK(evalStr(eng, "inkFor({ color: '#000000' })") == QStringLiteral("#FFFFFF"), "inkFor: black -> white ink");
    CHECK(evalStr(eng, "inkFor({ color: '#FFFFFF' })") != QStringLiteral("#FFFFFF"), "inkFor: white -> dark ink");
    CHECK(evalStr(eng, "inkFor({ color: '#0A1326' })") == QStringLiteral("#FFFFFF"), "inkFor: Triple's navy -> white ink");
    CHECK(evalStr(eng, "inkFor({ color: '#EFF3F8' })") != QStringLiteral("#FFFFFF"), "inkFor: Channels' near-white -> dark ink");
    // Green dominates the luma sum and blue barely counts — a naive average would call both of these the
    // same brightness and get one of them wrong.
    CHECK(evalStr(eng, "inkFor({ color: '#00FF00' })") != QStringLiteral("#FFFFFF"),
          "inkFor: saturated green is a LIGHT background (0.7152 luma weight)");
    CHECK(evalStr(eng, "inkFor({ color: '#0000FF' })") == QStringLiteral("#FFFFFF"),
          "inkFor: saturated blue is a DARK one (0.0722 luma weight)");
    // The other shapes a background can take.
    CHECK(evalStr(eng, "inkFor({ gradient: ['#EFF3F8', '#111111'] })") != QStringLiteral("#FFFFFF"),
          "inkFor: reads the FIRST gradient stop (the top of the screen)");
    CHECK(evalStr(eng, "inkFor({ color: '#FFEFF3F8' })") != QStringLiteral("#FFFFFF"),
          "inkFor: an #AARRGGBB colour drops the alpha instead of misreading the channels");
    CHECK(evalStr(eng, "inkFor(null)") == QStringLiteral("#FFFFFF"), "inkFor: no background -> white (safe default)");

    // ---- 4b. inkFor: every colour form a THEME can legally write ---------------------------------------
    // `background.color` goes to a QML Rectangle, so anything QColor reads is already legal and in the wild.
    // A reader that understood only 6 and 8 hex digits called all of the rest "dark" and printed WHITE on
    // them — issue #29 rebuilt on the layer added to fix #29, and specifically on the shared layer whose
    // job is protecting community-registry themes nobody here can edit. THE case: a registry theme whose
    // home background is `#EEE` and which declares no `browse`.
    CHECK(evalStr(eng, "inkFor({ color: '#EEE' })") != QStringLiteral("#FFFFFF"),
          "inkFor: a 3-digit #RGB near-white is LIGHT — the registry-theme case that must not print white on white");
    CHECK(evalStr(eng, "inkFor({ color: '#123' })") == QStringLiteral("#FFFFFF"),
          "inkFor: …and a 3-digit #RGB dark navy is still dark (the expansion is per-nibble, not a truncation)");
    CHECK(evalStr(eng, "inkFor({ gradient: ['#EEE', '#111'] })") != QStringLiteral("#FFFFFF"),
          "inkFor: a 3-digit gradient stop resolves too, not just a flat 3-digit colour");
    // Named colours: ~150 of them are legal, and both tones must land correctly.
    CHECK(evalStr(eng, "inkFor({ color: 'whitesmoke' })") != QStringLiteral("#FFFFFF"),
          "inkFor: the NAME 'whitesmoke' is a light background (#F5F5F5), not an unreadable string");
    CHECK(evalStr(eng, "inkFor({ color: 'darkslategray' })") == QStringLiteral("#FFFFFF"),
          "inkFor: …and the NAME 'darkslategray' is a dark one (#2F4F4F) — names are read, not defaulted");
    CHECK(evalStr(eng, "inkFor({ color: 'WhiteSmoke' })") != QStringLiteral("#FFFFFF"),
          "inkFor: a name is matched case-insensitively, exactly as the Rectangle matches it");
    // The 9- and 12-digit forms QColor also accepts; nothing in the docs forbids them.
    CHECK(evalStr(eng, "inkFor({ color: '#EEEFFF888' })") != QStringLiteral("#FFFFFF"),
          "inkFor: the 9-digit #RRRGGGBBB form resolves (delegation, not a digit-count whitelist)");
    // A string QColor CANNOT read is not a mystery and must not be treated as one: a Rectangle handed it
    // paints BLACK. So the honest answer is the black one — white ink — and it is exact, not a fallback.
    // "#FFF8" belongs here and NOT with the short forms above: Qt has no 3-digit-plus-alpha colour, so it
    // is a typo that renders as a black box, and expanding it to a near-white would print dark ink on black.
    CHECK(evalStr(eng, "inkFor({ color: 'nonsense' })") == QStringLiteral("#FFFFFF"),
          "inkFor: a string QColor cannot read paints BLACK, so it earns white ink");
    CHECK(evalStr(eng, "inkFor({ color: '#FFF8' })") == QStringLiteral("#FFFFFF"),
          "inkFor: '#FFF8' is NOT 50% white — Qt has no 4-digit form, the Rectangle paints it black");
    CHECK(evalStr(eng, "inkFor({ color: '#GGGGGG' })") == QStringLiteral("#FFFFFF"),
          "inkFor: six NON-hex digits are junk too — a length check alone would have read them as a colour");
    // Alpha is not decoration: the backdrop composites over the window's near-black clear colour, so a
    // half-transparent white paints as mid-grey. Reading #80FFFFFF as "white" would print dark ink on it.
    CHECK(evalStr(eng, "inkFor({ color: 'transparent' })") == QStringLiteral("#FFFFFF"),
          "inkFor: a fully transparent background shows the near-black window behind it -> white ink");
    CHECK(evalStr(eng, "inkFor({ color: '#80FFFFFF' })") == QStringLiteral("#FFFFFF"),
          "inkFor: a half-transparent white paints as mid-grey, and mid-grey takes white ink");
    // The genuinely unknowable case, and the ONLY one delegation leaves: a background with no colour at all.
    // An image can be any brightness, so no ink is right on its own — which is why this answer is the one
    // defaultView pairs with a black halo (§7 below). White-with-a-dark-halo reads over anything; assuming
    // the LIGHT end here would print near-black on a dark photograph with a near-black outline round it.
    CHECK(evalStr(eng, "inkFor({ image: 'bg.jpg', dim: 0.3 })") == QStringLiteral("#FFFFFF"),
          "inkFor: an image-only background takes the dark end -> white ink, which defaultView outlines in black");
    CHECK(evalStr(eng, "inkFor({})") == QStringLiteral("#FFFFFF") &&
          evalStr(eng, "inkFor({ color: '' })") == QStringLiteral("#FFFFFF"),
          "inkFor: an empty background block, and an empty colour string, land there too");

    // ---- 5. tileImage: where a tile's artwork comes from -----------------------------------------------
    // The scalar `image` is what HomeView::browseItems() puts on every row, so it wins outright.
    CHECK(evalStr(eng, "tileImage({ image: 'a.jpg', images: { poster: ['b.jpg'] } })") == QStringLiteral("a.jpg"),
          "tileImage: the scalar `image` wins (it is what the browse builder publishes)");
    // A row that carries art ONLY under the open-ended role map still gets a poster instead of a bare tile.
    CHECK(evalStr(eng, "tileImage({ images: { poster: ['p.jpg', 'p2.jpg'] } })") == QStringLiteral("p.jpg"),
          "tileImage: falls through to images[role][0] — best first — when there is no scalar");
    CHECK(evalStr(eng, "tileImage({ poster: 'alias.jpg' })") == QStringLiteral("alias.jpg"),
          "tileImage: …and to the scalar role alias (selected.poster) when there is no list");
    // Role ORDER is load-bearing: a card is portrait-ish, so a poster must beat a wide hero/banner.
    CHECK(evalStr(eng, "tileImage({ images: { hero: ['h.jpg'], poster: ['p.jpg'] } })") == QStringLiteral("p.jpg"),
          "tileImage: poster beats hero — a tile is portrait-shaped, not 16:9");
    CHECK(evalStr(eng, "tileImage({ images: { logo: ['l.jpg'], box: ['b.jpg'] } })") == QStringLiteral("b.jpg"),
          "tileImage: box beats logo (a clear logo is not cover art)");
    CHECK(evalStr(eng, "tileImage({ images: { banner: ['bn.jpg'] } })") == QStringLiteral("bn.jpg"),
          "tileImage: a role with nothing above it is still used rather than dropped");
    // Absence, in every shape a row can be absent.
    CHECK(evalStr(eng, "tileImage({ title: 'No art' })").isEmpty(), "tileImage: a row with no artwork yields \"\"");
    CHECK(evalStr(eng, "tileImage({ images: { poster: [] } })").isEmpty(), "tileImage: an EMPTY role list is not artwork");
    CHECK(evalStr(eng, "tileImage(null)").isEmpty(), "tileImage: null row -> \"\" (delegates run before the model lands)");
    CHECK(evalStr(eng, "tileImage(undefined)").isEmpty(), "tileImage: undefined row -> \"\"");

    // ---- 6. tileNeedsTitle: the shared "nothing readable on this tile" rule ----------------------------
    // The regression this exists for: a row that HAS a url whose image failed. Keying off the url alone
    // (the old per-element spelling) said "art present, hide the title" and left a blank card.
    CHECK(evalBool(eng, "tileNeedsTitle({ image: 'dead.jpg', title: 'X' }, false)"),
          "tileNeedsTitle: a url that has not loaded / failed still needs the title (the #29 hardening)");
    CHECK(!evalBool(eng, "tileNeedsTitle({ image: 'good.jpg', title: 'X' }, true)"),
          "tileNeedsTitle: artwork actually on screen does NOT get a title over it");
    CHECK(evalBool(eng, "tileNeedsTitle({ title: 'X' }, false)"),
          "tileNeedsTitle: a row with no artwork at all needs the title");
    // artReady can only be true for a row that HAS art, but assert the belt-and-braces case anyway: a stale
    // Ready status from a previous source must not suppress the placeholder for an art-less row.
    CHECK(evalBool(eng, "tileNeedsTitle({ title: 'X' }, true)"),
          "tileNeedsTitle: no artwork wins over a stale Ready status");
    // artReady TRUE deliberately: with it false the rule short-circuits before it ever looks at the row, so
    // `tileNeedsTitle(null, false)` would assert nothing about null-safety at all. This is the call that
    // actually reaches the row — delegates do run before the model lands.
    CHECK(evalBool(eng, "tileNeedsTitle(null, true)"), "tileNeedsTitle: a null row needs the title, not a crash");

    // ---- 7. the fallback outlines its text, because a background can be a PICTURE ----------------------
    // inkFor can only answer for a background it can read a colour off. `{ "image": …, "dim": … }` is legal
    // and gives it nothing, and white over a bright photo is issue #29 again in a shape no luminance rule
    // can see. So every string the fallback paints carries a halo in the OPPOSITE tone — the same hardening
    // the `label: "none"` tile placeholder already had. (That the elements actually DRAW it is a separate
    // claim, pinned in the real scene by probe_navqml §22: a knob no element reads would be inert here.)
    evalOr(eng, "var imgOnly = { views: { home: { background: { image: 'bg.jpg', dim: 0.3 },"
                "                                elements: [ { type: 'grid' } ] } } };"
                "function elById(v, id) { var f = v.elements.filter(function (e) { return e.id === id });"
                "                         return f.length ? f[0] : ({}) }");
    CHECK(evalStr(eng, "elById(viewFor(imgOnly, 'browse'), 'ebFallbackTitle').outline") == QStringLiteral("#000000"),
          "fallback: white ink over an unreadable background is outlined in BLACK, so it reads over any image");
    CHECK(evalStr(eng, "elById(viewFor(imgOnly, 'browse'), 'ebFallbackHelp').outline") == QStringLiteral("#000000"),
          "fallback: …and so is the help bar, which had neither outline nor scrim");
    // The outline tracks the ink rather than being a constant: on a light theme the ink is dark, so a BLACK
    // halo would be invisible and a black-on-light title would keep no safety margin at all.
    CHECK(evalStr(eng, "elById(viewFor(lightOne, 'browse'), 'ebFallbackTitle').outline") == QStringLiteral("#FFFFFF"),
          "fallback: dark ink on a light theme is outlined in WHITE — the halo is the opposite tone, not a constant");
    CHECK(evalStr(eng, "elById(viewFor(xmbOnly, 'browse'), 'ebFallbackTitle').outline") == QStringLiteral("#000000"),
          "fallback: white ink on a dark theme is outlined in black");

    // ---- 8. declaresView: the gates and the renderer must mean the same thing by "declared" ------------
    // The HOST asks "does the theme style this view?" before it OFFERS a route into one ("I" only opens
    // `detail` on a theme that has one). That question used to be answered by testing the KEY, while
    // viewFor answered "is there a layout here" by testing the ELEMENT LIST. A theme shipping
    // `"detail": { "elements": [] }` fell straight through the gap: it passed the gate, so the host entered
    // detail mode, and then the renderer drew viewFor's fallback — an item GRID bound to the browse rows —
    // while the key router moved an invisible action cursor and fired play/download verbs at it.
    CHECK(evalBool(eng, "declaresView(themed, 'browse')"),
          "declaresView: a view with elements IS declared (the route is offered)");
    CHECK(!evalBool(eng, "declaresView(xmbOnly, 'browse')"),
          "declaresView: an absent view is not declared");
    CHECK(!evalBool(eng, "declaresView(emptyEls, 'browse')"),
          "declaresView: a declared-but-EMPTY view is not declared either — the exact gap the gates fell through");
    CHECK(!evalBool(eng, "declaresView({}, 'browse')") && !evalBool(eng, "declaresView(null, 'browse')"),
          "declaresView: no views / no theme is not declared, and does not throw");
    // The two answers are one answer: whatever declaresView calls declared is what viewFor renders verbatim,
    // and whatever it does not is what viewFor replaces. Assert the correspondence itself, so the pair
    // cannot drift apart again the way key-presence and element-emptiness did.
    CHECK(evalBool(eng, "['browse','detail','home','nowplayingAudio'].every(function (n) {"
                        "  return [themed, xmbOnly, emptyEls, lightOne, {}].every(function (t) {"
                        "    var declared = declaresView(t, n);"
                        "    var isOwn = viewFor(t, n) === (t && t.views ? t.views[n] : undefined);"
                        "    return declared === isOwn }) })"),
          "declaresView/viewFor: declared <=> the theme's OWN object renders; undeclared <=> the fallback does");

    if (failures == 0) std::printf("THEMEVIEW-OK\n");
    else std::fprintf(stderr, "THEMEVIEW: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
