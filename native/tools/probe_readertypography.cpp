// Headless check of reader typography (issue #135): the pure ReaderTypography map (src/ebook/ReaderTypography.h)
// and the Settings defaults + round-trip that feed it. QtCore-only — the map is a header-only pure function and
// Settings is a QSettings wrapper — so it runs under the offscreen QPA in CI with no window and no reader.
//
// Every expected value below is HAND-WRITTEN from the design, NOT recomputed by calling the mapping a second
// time — the point is to pin the map against an independent statement of what each field should produce. The
// things most worth getting wrong, all pinned here:
//   * the FOUR theme palettes: each theme -> its exact text/background #RRGGBB pair, hand-authored, so a swapped
//     or mistyped colour is caught. TrueBlack's background must be pure #000000 (OLED), distinct from Dark.
//   * the CLAMPS at both ends: size 8..40, spacing 100..250, margin 0..25 — an out-of-range value is held to the
//     boundary, never passed through.
//   * the justify flag and the empty-family "no override" pass straight through resolve() unchanged.
//   * the Settings half: an empty ini reads the shipping defaults, values round-trip, out-of-range writes are
//     clamped by the setters, and readerTypography() gathers them so stored -> Settings -> resolve agrees with a
//     direct mapping. Size lives at the SHARED "ebook/fontSize" key (one notion of reading size).
//
// Prints READERTYPO-OK on success; any failure prints READERTYPO-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini the
// Settings half reads starts empty and is removed at exit — the defaults it asserts are a genuine absence.
#include "ReaderTypography.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "READERTYPO-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Assert a QString equals an exact, hand-written value, printing both on mismatch.
#define EXPECT_STR(got, want) do { \
    const QString g = (got); \
    if (g != QStringLiteral(want)) { \
        std::fprintf(stderr, "READERTYPO-FAIL %s -> got '%s' want '%s' (line %d)\n", \
                     #got, g.toUtf8().constData(), want, __LINE__); \
        ++failures; \
    } \
} while (0)

using RT = ReaderTypography::Theme;

// The four theme palettes, each pinned against an independently hand-authored text/background pair.
static void testThemePalettes()
{
    const ReaderTypography::Palette light = ReaderTypography::themePalette(RT::Light);
    EXPECT_STR(light.text,       "#1A1A1A");
    EXPECT_STR(light.background, "#FFFFFF");

    const ReaderTypography::Palette sepia = ReaderTypography::themePalette(RT::Sepia);
    EXPECT_STR(sepia.text,       "#5B4636");
    EXPECT_STR(sepia.background, "#F4ECD8");

    const ReaderTypography::Palette dark = ReaderTypography::themePalette(RT::Dark);
    EXPECT_STR(dark.text,        "#C8C8C8");
    EXPECT_STR(dark.background,  "#202124");

    const ReaderTypography::Palette black = ReaderTypography::themePalette(RT::TrueBlack);
    EXPECT_STR(black.text,       "#C8C8C8");
    EXPECT_STR(black.background, "#000000");   // OLED: pure black, NOT Dark's dark grey

    // The two dark themes must differ ONLY in their background — a regression that made TrueBlack == Dark would
    // silently drop the OLED look. Pin the distinction directly.
    CHECK(black.background != dark.background);
}

// Clamps at both ends, plus an in-range value passing through untouched.
static void testClamps()
{
    CHECK(ReaderTypography::clampSize(3)   == 8);    // below floor -> floor
    CHECK(ReaderTypography::clampSize(999) == 40);   // above ceil  -> ceil
    CHECK(ReaderTypography::clampSize(14)  == 14);   // in range    -> unchanged

    CHECK(ReaderTypography::clampSpacing(50)   == 100); // below floor
    CHECK(ReaderTypography::clampSpacing(999)  == 250); // above ceil
    CHECK(ReaderTypography::clampSpacing(150)  == 150); // in range

    CHECK(ReaderTypography::clampMargin(-5)  == 0);   // below floor
    CHECK(ReaderTypography::clampMargin(999) == 25);  // above ceil
    CHECK(ReaderTypography::clampMargin(6)   == 6);   // in range
}

// The stored theme int round-trips through themeFromInt/themeToInt, and an out-of-range int falls back to Light
// rather than selecting a theme that does not exist.
static void testThemeIntMapping()
{
    CHECK(ReaderTypography::themeFromInt(0) == RT::Light);
    CHECK(ReaderTypography::themeFromInt(1) == RT::Sepia);
    CHECK(ReaderTypography::themeFromInt(2) == RT::Dark);
    CHECK(ReaderTypography::themeFromInt(3) == RT::TrueBlack);
    CHECK(ReaderTypography::themeFromInt(99) == RT::Light);   // corrupt/hand-edited -> Light, never out of range
    CHECK(ReaderTypography::themeFromInt(-1) == RT::Light);

    CHECK(ReaderTypography::themeToInt(RT::Light)     == 0);
    CHECK(ReaderTypography::themeToInt(RT::Sepia)     == 1);
    CHECK(ReaderTypography::themeToInt(RT::Dark)      == 2);
    CHECK(ReaderTypography::themeToInt(RT::TrueBlack) == 3);
}

// The factory-default Settings resolve to exactly the shipping look.
static void testResolveDefaults()
{
    ReaderTypography::Settings s;   // all defaults
    const ReaderTypography::Resolved r = ReaderTypography::resolve(s);
    CHECK(r.fontFamily.isEmpty());       // no family override by default
    CHECK(r.sizePt == 14);
    CHECK(r.lineSpacingPct == 100);
    CHECK(r.marginPct == 6);
    CHECK(r.justify == false);
    EXPECT_STR(r.textColor,  "#1A1A1A");  // Light theme text
    EXPECT_STR(r.background, "#FFFFFF");  // Light theme paper
}

// resolve() clamps every numeric field, carries justify verbatim, splits the theme into its two colours, and
// trims a padded family. Each field is driven off-boundary so a mutant that drops a clamp or a colour is killed.
static void testResolveFields()
{
    ReaderTypography::Settings s;
    s.fontFamily     = QStringLiteral("  Georgia  ");   // trimmed
    s.sizePt         = 100;                              // -> 40 (clamped)
    s.lineSpacingPct = 10;                               // -> 100 (clamped)
    s.marginPct      = 40;                               // -> 25 (clamped)
    s.justify        = true;
    s.theme          = RT::Sepia;
    const ReaderTypography::Resolved r = ReaderTypography::resolve(s);
    EXPECT_STR(r.fontFamily, "Georgia");
    CHECK(r.sizePt == 40);
    CHECK(r.lineSpacingPct == 100);
    CHECK(r.marginPct == 25);
    CHECK(r.justify == true);
    EXPECT_STR(r.textColor,  "#5B4636");   // Sepia ink
    EXPECT_STR(r.background, "#F4ECD8");   // Sepia paper

    // Dark theme, and a size at the low clamp, justify off — a second independent point.
    ReaderTypography::Settings d;
    d.sizePt = 2;                                        // -> 8
    d.theme  = RT::Dark;
    const ReaderTypography::Resolved rd = ReaderTypography::resolve(d);
    CHECK(rd.sizePt == 8);
    CHECK(rd.justify == false);
    EXPECT_STR(rd.textColor,  "#C8C8C8");
    EXPECT_STR(rd.background, "#202124");
}

// The Settings half: an empty ini reads the shipping defaults, values round-trip through readerTypography(),
// out-of-range writes are clamped by the setters, and the theme enum survives the int store.
static void testSettings()
{
    // Defaults from a genuinely empty ini.
    CHECK(Settings::readerFont().isEmpty());
    CHECK(Settings::readerFontSize() == 14);
    CHECK(Settings::readerLineSpacing() == 100);
    CHECK(Settings::readerMargin() == 6);
    CHECK(Settings::readerJustify() == false);
    CHECK(Settings::readerTheme() == RT::Light);

    Settings::setReaderFont(QStringLiteral("Palatino"));
    Settings::setReaderFontSize(22);
    Settings::setReaderLineSpacing(150);
    Settings::setReaderMargin(12);
    Settings::setReaderJustify(true);
    Settings::setReaderTheme(RT::Dark);

    const ReaderTypography::Settings s = Settings::readerTypography();
    EXPECT_STR(s.fontFamily, "Palatino");
    CHECK(s.sizePt == 22);
    CHECK(s.lineSpacingPct == 150);
    CHECK(s.marginPct == 12);
    CHECK(s.justify == true);
    CHECK(s.theme == RT::Dark);

    // The gathered Settings resolve to the Dark palette end to end.
    const ReaderTypography::Resolved r = ReaderTypography::resolve(s);
    EXPECT_STR(r.textColor,  "#C8C8C8");
    EXPECT_STR(r.background, "#202124");

    // Out-of-range stored values are clamped by the setters, not passed through raw.
    Settings::setReaderFontSize(999);
    CHECK(Settings::readerFontSize() == 40);
    Settings::setReaderFontSize(1);
    CHECK(Settings::readerFontSize() == 8);
    Settings::setReaderLineSpacing(9999);
    CHECK(Settings::readerLineSpacing() == 250);
    Settings::setReaderMargin(9999);
    CHECK(Settings::readerMargin() == 25);

    // Size shares the "ebook/fontSize" key the in-reader stepper drives — one notion of reading size. Prove the
    // two accessors agree so a future split can't silently desync them.
    Settings::setReaderFontSize(18);
    CHECK(Settings::readerTypography().sizePt == 18);

    // Every theme round-trips through the int store.
    for (RT t : { RT::Light, RT::Sepia, RT::Dark, RT::TrueBlack })
    {
        Settings::setReaderTheme(t);
        CHECK(Settings::readerTheme() == t);
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testThemePalettes();
    testClamps();
    testThemeIntMapping();
    testResolveDefaults();
    testResolveFields();
    testSettings();
    if (failures == 0) std::printf("READERTYPO-OK\n");
    return failures == 0 ? 0 : 1;
}
