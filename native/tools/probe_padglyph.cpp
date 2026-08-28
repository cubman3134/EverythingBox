// Headless check of the pure pad-glyph translator (src/input/PadGlyphs) — the ONE place a keyboard hint
// string authored in a theme becomes the controller button a player is actually looking at. It is plain
// QtCore (no SDL, no widgets, no scene), so it runs under the offscreen QPA in CI and pins:
//
//   * verbForHint() — the eight hints the app owns map to their verb; an arrow chip and an arbitrary
//     third-party string map to None (which is what makes them pass through untranslated);
//   * retroIdForVerb() — each verb's RetroPad id, spelled out as literals here rather than read back from
//     the header, so a renumbering cannot pass by re-running the code under test;
//   * labelForSdlCode() — the full per-brand label table for every SDL code Gamepad can store, including
//     both trigger sentinels, and "" for the unbound sentinel;
//   * chip() — the composition rule: translate when the verb is known AND the code is bound, otherwise
//     return the caller's own string unchanged.
//
// Prints PADGLYPH-OK on success; any failure prints PADGLYPH-FAIL <cond> (line) and exits non-zero.
#include "PadGlyphs.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

using namespace padglyphs;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PADGLYPH-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 1. Brand names round-trip, and anything unrecognised is Generic (never a crash, never a guess).
    CHECK(brandFromName(QStringLiteral("xbox")) == Brand::Xbox);
    CHECK(brandFromName(QStringLiteral("playstation")) == Brand::PlayStation);
    CHECK(brandFromName(QStringLiteral("switch")) == Brand::Switch);
    CHECK(brandFromName(QStringLiteral("generic")) == Brand::Generic);
    CHECK(brandFromName(QStringLiteral("")) == Brand::Generic);
    CHECK(brandFromName(QStringLiteral("dreamcast")) == Brand::Generic);
    CHECK(nameForBrand(Brand::PlayStation) == QStringLiteral("playstation"));
    CHECK(nameForBrand(Brand::Generic) == QStringLiteral("generic"));
    for (Brand b : { Brand::Xbox, Brand::PlayStation, Brand::Switch, Brand::Generic })
        CHECK(brandFromName(nameForBrand(b)) == b);
    CHECK(nameForBrand(Brand::Xbox)   == QStringLiteral("xbox"));
    CHECK(nameForBrand(Brand::Switch) == QStringLiteral("switch"));

    // 2. The eight hints the app owns resolve to their verb.
    CHECK(verbForHint(QStringLiteral("Enter")) == Verb::Confirm);
    CHECK(verbForHint(QStringLiteral("Esc"))   == Verb::Back);
    CHECK(verbForHint(QStringLiteral("I"))     == Verb::Details);
    CHECK(verbForHint(QStringLiteral("/"))     == Verb::Search);
    CHECK(verbForHint(QStringLiteral("F"))     == Verb::Filter);
    CHECK(verbForHint(QStringLiteral("P"))     == Verb::Playlist);
    CHECK(verbForHint(QStringLiteral("T"))     == Verb::Theme);
    CHECK(verbForHint(QStringLiteral("S"))     == Verb::Skip);

    // 3. Everything else is None — the arrow chips every bundled theme ships, and a string only a
    //    third-party theme author knows about. None is what makes chip() hand the caller's text back.
    CHECK(verbForHint(QString::fromUtf8("\xe2\x86\x90")) == Verb::None);              // <-
    CHECK(verbForHint(QString::fromUtf8("\xe2\x86\x91\xe2\x86\x93")) == Verb::None);  // up/down
    CHECK(verbForHint(QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92")) == Verb::None);  // left/right
    CHECK(verbForHint(QStringLiteral("Ctrl+Q")) == Verb::None);
    CHECK(verbForHint(QStringLiteral("")) == Verb::None);

    // 4. Verb -> RetroPad id. Literals, not a second call into the table.
    CHECK(retroIdForVerb(Verb::Confirm)  == 0);
    CHECK(retroIdForVerb(Verb::Search)   == 1);
    CHECK(retroIdForVerb(Verb::Skip)     == 1);   // same button, different surface
    CHECK(retroIdForVerb(Verb::Theme)    == 2);
    CHECK(retroIdForVerb(Verb::Back)     == 8);
    CHECK(retroIdForVerb(Verb::Details)  == 9);
    CHECK(retroIdForVerb(Verb::Filter)   == 10);
    CHECK(retroIdForVerb(Verb::Playlist) == 11);
    CHECK(retroIdForVerb(Verb::None)     == -1);

    // 5. The label table, hand-written per brand. Generic deliberately equals Xbox.
    struct Row { int code; const char* xbox; const char* ps; const char* sw; };
    static const Row rows[] = {
        // Switch matches Xbox on the four face letters: SDL reports Nintendo face buttons by their
        // printed label (SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS defaults to "1"), so code 0 really is
        // the button marked "A". Only the positions differ between the two pads, not the letters.
        {  0, "A",    "\xe2\x9c\x95", "A"    },   // south position / cross / Nintendo-A
        {  1, "B",    "\xe2\x97\x8b", "B"    },   // east position  / circle / Nintendo-B
        {  2, "X",    "\xe2\x96\xa1", "X"    },   // west position  / square / Nintendo-X
        {  3, "Y",    "\xe2\x96\xb3", "Y"    },   // north position / triangle / Nintendo-Y
        {  4, "View", "Create",       "\xe2\x88\x92" },
        {  5, "Guide","PS",           "Home" },
        {  6, "Menu", "Options",      "+"    },
        {  7, "LS",   "L3",           "LS"   },
        {  8, "RS",   "R3",           "RS"   },
        {  9, "LB",   "L1",           "L"    },
        { 10, "RB",   "R1",           "R"    },
        { 11, "\xe2\x86\x91", "\xe2\x86\x91", "\xe2\x86\x91" },
        { 12, "\xe2\x86\x93", "\xe2\x86\x93", "\xe2\x86\x93" },
        { 13, "\xe2\x86\x90", "\xe2\x86\x90", "\xe2\x86\x90" },
        { 14, "\xe2\x86\x92", "\xe2\x86\x92", "\xe2\x86\x92" },
        { 1000, "LT", "L2", "ZL" },
        { 1001, "RT", "R2", "ZR" },
    };
    for (const Row& r : rows)
    {
        CHECK(labelForSdlCode(r.code, Brand::Xbox)        == QString::fromUtf8(r.xbox));
        CHECK(labelForSdlCode(r.code, Brand::Generic)     == QString::fromUtf8(r.xbox));
        CHECK(labelForSdlCode(r.code, Brand::PlayStation) == QString::fromUtf8(r.ps));
        CHECK(labelForSdlCode(r.code, Brand::Switch)      == QString::fromUtf8(r.sw));
    }

    // 6. Unbound and out-of-range codes have no label at all.
    CHECK(labelForSdlCode(-1, Brand::Xbox).isEmpty());
    CHECK(labelForSdlCode(99, Brand::Xbox).isEmpty());
    CHECK(labelForSdlCode(15, Brand::Switch).isEmpty());

    // 7. chip(): a known verb on a bound button renders the button.
    CHECK(chip(QStringLiteral("Enter"), Brand::Xbox, 0)        == QStringLiteral("A"));
    CHECK(chip(QStringLiteral("Enter"), Brand::PlayStation, 0) == QString::fromUtf8("\xe2\x9c\x95"));
    CHECK(chip(QStringLiteral("Esc"),   Brand::Switch, 1)      == QStringLiteral("B"));
    CHECK(chip(QStringLiteral("F"),     Brand::PlayStation, 9) == QStringLiteral("L1"));
    CHECK(chip(QStringLiteral("Enter"), Brand::Generic, 0)     == QStringLiteral("A"));

    // 8. chip(): a remapped binding renders the button the user actually mapped, not the factory one.
    CHECK(chip(QStringLiteral("Enter"), Brand::Xbox, 3) == QStringLiteral("Y"));
    CHECK(chip(QStringLiteral("/"),     Brand::Xbox, 1000) == QStringLiteral("LT"));

    // 9. chip(): pass-through. An unknown hint keeps the theme author's own text; a known verb with
    //    nothing bound to it keeps the keyboard text rather than claiming a button that does not exist.
    CHECK(chip(QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92"), Brand::Xbox, 0)
          == QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92"));
    CHECK(chip(QStringLiteral("Ctrl+Q"), Brand::Xbox, 0) == QStringLiteral("Ctrl+Q"));
    CHECK(chip(QStringLiteral("Enter"),  Brand::Xbox, -1) == QStringLiteral("Enter"));
    CHECK(chip(QStringLiteral("T"),      Brand::Xbox, 99) == QStringLiteral("T"));

    if (failures == 0) std::printf("PADGLYPH-OK\n");
    else               std::fprintf(stderr, "PADGLYPH: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
