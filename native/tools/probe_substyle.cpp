// Headless check of subtitle styling (issue #71): the pure Style -> mpv option map (src/video/SubtitleStyle.h)
// and the Settings defaults + round-trip that feed it. QtCore-only — the map is a header-only pure function and
// Settings is a QSettings wrapper — so it runs under the offscreen QPA in CI with no window and no mpv.
//
// Every expected mpv option value below is HAND-WRITTEN from the design, NOT recomputed by calling the mapping
// a second time — the point is to pin the map against an independent statement of what each field should emit.
// The three things most worth getting wrong, all pinned here:
//   * the background-box ALPHA ENCODING: box off => a fully transparent sub-back-color (#00000000, no box
//     regardless of the stored opacity); box on => the opacity percentage on the 0..255 alpha channel.
//   * the ASS-OVERRIDE gate: the DEFAULT (toggle off) must emit NO `force`, so a styled ASS/SSA sub keeps its
//     own typography; only the toggle on emits sub-ass-override=force. This is the correctness crux of the
//     issue — get it backwards and every anime fansub renders in the user's plain SRT look.
//   * size maps to sub-scale (the existing size notion), never a second sub-font-size knob.
//
// Prints SUBSTYLE-OK on success; any failure prints SUBSTYLE-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini the
// Settings half reads starts empty and is removed at exit — the defaults it asserts are a genuine absence.
#include "SubtitleStyle.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QVector>
#include <QPair>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SUBSTYLE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Look a single option name up in the emitted pair list. Returns the value, or a sentinel that can never be a
// real value so "missing" and "wrong" are distinguishable.
static QString optValue(const QVector<QPair<QString, QString>>& opts, const QString& name)
{
    for (const auto& o : opts) if (o.first == name) return o.second;
    return QStringLiteral("<ABSENT>");
}
static bool hasPair(const QVector<QPair<QString, QString>>& opts, const QString& name, const QString& value)
{
    for (const auto& o : opts) if (o.first == name && o.second == value) return true;
    return false;
}

// Assert an option maps to an exact, hand-written value for a given Style.
#define EXPECT_OPT(opts, name, want) do { \
    const QString got = optValue(opts, QStringLiteral(name)); \
    if (got != QStringLiteral(want)) { \
        std::fprintf(stderr, "SUBSTYLE-FAIL %s -> got '%s' want '%s' (line %d)\n", \
                     name, got.toUtf8().constData(), want, __LINE__); \
        ++failures; \
    } \
} while (0)

// The factory-default Style (mirrors mpv's own no-options-set look) maps to exactly these strings.
static void testDefaults()
{
    SubtitleStyle::Style s;   // all defaults
    const auto o = SubtitleStyle::toMpvOptions(s);
    EXPECT_OPT(o, "sub-font",        "sans-serif");  // empty family falls back to mpv's default family
    EXPECT_OPT(o, "sub-scale",       "1");           // 100% -> 1.0, formatted "1"
    EXPECT_OPT(o, "sub-color",       "#FFFFFF");
    EXPECT_OPT(o, "sub-border-size", "3");
    EXPECT_OPT(o, "sub-border-color","#000000");
    EXPECT_OPT(o, "sub-back-color",  "#00000000");   // box off -> fully transparent, i.e. no box
    EXPECT_OPT(o, "sub-pos",         "100");         // bottom
    EXPECT_OPT(o, "sub-bold",        "no");
    // The crux: the default path emits sub-ass-override=scale, and NEVER force.
    EXPECT_OPT(o, "sub-ass-override","scale");
    CHECK(!hasPair(o, QStringLiteral("sub-ass-override"), QStringLiteral("force")));
}

// Non-default fields each map through, and the numeric formatting is C-locale.
static void testFieldMapping()
{
    SubtitleStyle::Style s;
    s.fontFamily  = QStringLiteral("Verdana");
    s.sizePercent = 125;
    s.textColor   = QStringLiteral("#FFFF00");
    s.borderSize  = 5;
    s.borderColor = QStringLiteral("#112233");
    s.position    = 25;
    s.bold        = true;
    const auto o = SubtitleStyle::toMpvOptions(s);
    EXPECT_OPT(o, "sub-font",        "Verdana");
    EXPECT_OPT(o, "sub-scale",       "1.25");
    EXPECT_OPT(o, "sub-color",       "#FFFF00");
    EXPECT_OPT(o, "sub-border-size", "5");
    EXPECT_OPT(o, "sub-border-color","#112233");
    EXPECT_OPT(o, "sub-pos",         "25");
    EXPECT_OPT(o, "sub-bold",        "yes");

    // A fractional scale that is not a round percentage still formats without a locale comma.
    SubtitleStyle::Style s90; s90.sizePercent = 90;
    EXPECT_OPT(SubtitleStyle::toMpvOptions(s90), "sub-scale", "0.9");
}

// The background-box alpha encoding, pinned at several opacities and the off case.
static void testBoxAlpha()
{
    SubtitleStyle::Style off;   off.boxEnabled = false; off.boxOpacityPercent = 100;   // opacity ignored when off
    EXPECT_OPT(SubtitleStyle::toMpvOptions(off), "sub-back-color", "#00000000");

    SubtitleStyle::Style full;  full.boxEnabled = true;  full.boxOpacityPercent = 100;
    EXPECT_OPT(SubtitleStyle::toMpvOptions(full), "sub-back-color", "#FF000000");

    SubtitleStyle::Style half;  half.boxEnabled = true;  half.boxOpacityPercent = 50;   // round(127.5) -> 128 -> 0x80
    EXPECT_OPT(SubtitleStyle::toMpvOptions(half), "sub-back-color", "#80000000");

    SubtitleStyle::Style q;     q.boxEnabled = true;     q.boxOpacityPercent = 25;       // round(63.75) -> 64 -> 0x40
    EXPECT_OPT(SubtitleStyle::toMpvOptions(q), "sub-back-color", "#40000000");

    SubtitleStyle::Style d;     d.boxEnabled = true;     d.boxOpacityPercent = 75;       // round(191.25) -> 191 -> 0xBF
    EXPECT_OPT(SubtitleStyle::toMpvOptions(d), "sub-back-color", "#BF000000");
}

// The ASS-override gate, both directions. Only the toggle ON emits force; nothing else does.
static void testAssOverride()
{
    SubtitleStyle::Style offStyle;  offStyle.overrideStyled = false;
    const auto offOpts = SubtitleStyle::toMpvOptions(offStyle);
    CHECK(!hasPair(offOpts, QStringLiteral("sub-ass-override"), QStringLiteral("force")));
    EXPECT_OPT(offOpts, "sub-ass-override", "scale");

    SubtitleStyle::Style onStyle;   onStyle.overrideStyled = true;
    const auto onOpts = SubtitleStyle::toMpvOptions(onStyle);
    CHECK(hasPair(onOpts, QStringLiteral("sub-ass-override"), QStringLiteral("force")));

    // Turning on EVERY other field but leaving override off must STILL not emit force — styling a plain sub
    // must never reach a styled one by default.
    SubtitleStyle::Style loud;
    loud.fontFamily = QStringLiteral("Impact"); loud.sizePercent = 300; loud.textColor = QStringLiteral("#FF0000");
    loud.borderSize = 6; loud.boxEnabled = true; loud.boxOpacityPercent = 100; loud.bold = true;
    loud.overrideStyled = false;
    CHECK(!hasPair(SubtitleStyle::toMpvOptions(loud), QStringLiteral("sub-ass-override"), QStringLiteral("force")));
}

// The Settings half: an empty ini reads the mpv-matching defaults, values round-trip, and subtitleStyle()
// gathers them so the end-to-end (stored -> Style -> mpv options) agrees with a direct mapping.
static void testSettings()
{
    CHECK(Settings::subtitleFont().isEmpty());
    CHECK(Settings::subtitleSizePercent() == 100);
    CHECK(Settings::subtitleColor() == QStringLiteral("#FFFFFF"));
    CHECK(Settings::subtitleBorderSize() == 3);
    CHECK(Settings::subtitleBorderColor() == QStringLiteral("#000000"));
    CHECK(Settings::subtitleBox() == false);
    CHECK(Settings::subtitleBoxOpacity() == 75);
    CHECK(Settings::subtitlePosition() == 100);
    CHECK(Settings::subtitleBold() == false);
    CHECK(Settings::subtitleOverrideStyled() == false);   // the shipped default leaves ASS alone

    Settings::setSubtitleFont(QStringLiteral("Arial"));
    Settings::setSubtitleSizePercent(150);
    Settings::setSubtitleColor(QStringLiteral("#00FF00"));
    Settings::setSubtitleBorderSize(4);
    Settings::setSubtitleBorderColor(QStringLiteral("#0000FF"));
    Settings::setSubtitleBox(true);
    Settings::setSubtitleBoxOpacity(50);
    Settings::setSubtitlePosition(0);
    Settings::setSubtitleBold(true);
    Settings::setSubtitleOverrideStyled(true);

    const SubtitleStyle::Style s = Settings::subtitleStyle();
    CHECK(s.fontFamily == QStringLiteral("Arial"));
    CHECK(s.sizePercent == 150);
    CHECK(s.textColor == QStringLiteral("#00FF00"));
    CHECK(s.borderSize == 4);
    CHECK(s.borderColor == QStringLiteral("#0000FF"));
    CHECK(s.boxEnabled == true);
    CHECK(s.boxOpacityPercent == 50);
    CHECK(s.position == 0);
    CHECK(s.bold == true);
    CHECK(s.overrideStyled == true);

    const auto o = SubtitleStyle::toMpvOptions(s);
    EXPECT_OPT(o, "sub-font",        "Arial");
    EXPECT_OPT(o, "sub-scale",       "1.5");
    EXPECT_OPT(o, "sub-back-color",  "#80000000");   // box on, 50% opacity
    EXPECT_OPT(o, "sub-pos",         "0");
    EXPECT_OPT(o, "sub-ass-override","force");        // override now on

    // Out-of-range stored values are clamped by the setters, not passed through raw.
    Settings::setSubtitleSizePercent(99999);
    CHECK(Settings::subtitleSizePercent() == 1000);
    Settings::setSubtitleBoxOpacity(999);
    CHECK(Settings::subtitleBoxOpacity() == 100);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDefaults();
    testFieldMapping();
    testBoxAlpha();
    testAssOverride();
    testSettings();
    if (failures == 0) std::printf("SUBSTYLE-OK\n");
    return failures == 0 ? 0 : 1;
}
