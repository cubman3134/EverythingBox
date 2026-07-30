// Headless test for the theme renderer's PURE decisions — the functions in src/theme2/qml/Theme.js that
// answer, for a given theme and a given catalog row, "what actually gets painted?". They are plain JS with
// no QML, no scene and no host, so this probe evaluates the SHIPPED file out of the theme2 qrc (the same
// bytes ThemeView.qml imports) in a QJSEngine and pins the answers directly.
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
#include <QCoreApplication>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "THEMEVIEW-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

// Load the shipped Theme.js out of the qrc into a bare JS engine. `.pragma library` is a QML-JS directive
// that QJSEngine does not parse, so it is stripped — it only tells the QML engine to share one instance of
// the file, which has no meaning here and no effect on any function below.
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
    QCoreApplication app(argc, argv);
    QJSEngine eng;
    if (!loadThemeJs(eng)) return 2;

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
    CHECK(evalStr(eng, "inkFor({ color: 'nonsense' })") == QStringLiteral("#FFFFFF"), "inkFor: junk -> white, never NaN");

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

    if (failures == 0) std::printf("THEMEVIEW-OK\n");
    else std::fprintf(stderr, "THEMEVIEW: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
