// Headless check of HDR output handling (issue #68): the pure (Mode, Platform) -> mpv option map
// (src/video/HdrOutput.h, header-only), the id<->Mode round-trip the settings builders use, and the Settings
// default + round-trip that feed it. QtCore-only — the map is a header-only pure function and Settings is a
// QSettings wrapper — so it runs under the offscreen QPA in CI with no window and no player. Settings.cpp pulls
// FormFactor.cpp (videoRefreshSync()'s default resolves against it), same as probe_refreshsync.
//
// Every expected mpv option value below is HAND-WRITTEN from the design (the issue's two-way switch), NOT
// recomputed by calling the map a second time — the point is to pin the map against an independent statement of
// what each mode should emit. The things most worth getting wrong, all pinned here:
//   * TONE-MAP TO SDR (default): tone-mapping=bt.2446a + hdr-compute-peak=yes, and it must NOT set
//     target-colorspace-hint=yes (setting the passthrough hint here would defeat the whole tone-map). It emits
//     target-colorspace-hint=no — mpv's default — so a live flip back FROM passthrough clears the hint.
//   * PASSTHROUGH WHEN SUPPORTED: target-colorspace-hint=yes + hdr-compute-peak=yes, and it resets the SDR curve
//     by emitting tone-mapping=auto (mpv's default), so a live flip back FROM tone-map does not leave bt.2446a set.
//   * the RESET DISCIPLINE: BOTH modes emit all three keys (tone-mapping, hdr-compute-peak, target-colorspace-hint)
//     so a live re-apply after a mode flip resets the other mode's key rather than leaving it stuck on the context.
//   * the iOS CARVE-OUT: iOS has no HDR swapchain (software QImage blit), so passthrough is forced to tone-map —
//     BOTH stored modes resolve to the tone-map option set on iOS.
//
// Prints HDROUTPUT-OK on success; any failure prints HDROUTPUT-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini the
// Settings half reads starts empty and is removed at exit — the default it asserts is a genuine absence.
#include "HdrOutput.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QVector>
#include <QPair>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "HDROUTPUT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Look a single option name up in the emitted pair list. Returns the value, or a sentinel that can never be a
// real value so "missing" and "wrong" are distinguishable (target-colorspace-hint=no is a real value, so a
// missing key must not read as "no").
static QString optValue(const QVector<QPair<QString, QString>>& opts, const QString& name)
{
    for (const auto& o : opts) if (o.first == name) return o.second;
    return QStringLiteral("<ABSENT>");
}

// Assert an option maps to an exact, hand-written value for a given (mode, platform).
#define EXPECT_OPT(opts, name, want) do { \
    const QString got = optValue(opts, QStringLiteral(name)); \
    if (got != QStringLiteral(want)) { \
        std::fprintf(stderr, "HDROUTPUT-FAIL %s -> got '%s' want '%s' (line %d)\n", \
                     name, got.toUtf8().constData(), want, __LINE__); \
        ++failures; \
    } \
} while (0)

// The default (Tone-map to SDR) maps to exactly these strings, on both platforms that have a real render surface.
static void testToneMapMode()
{
    for (HdrOutput::Platform plat : { HdrOutput::Platform::Desktop, HdrOutput::Platform::Android })
    {
        const auto m = HdrOutput::optionsFor(HdrOutput::Mode::ToneMapSdr, plat);
        EXPECT_OPT(m, "tone-mapping",           "bt.2446a"); // map HDR down to the SDR gamut (fixes washed-out)
        EXPECT_OPT(m, "hdr-compute-peak",       "yes");      // measure per-scene peak so the roll-off is right
        EXPECT_OPT(m, "target-colorspace-hint", "no");       // mpv default: the passthrough hint is OFF here
        // Tripwire: tone-map must NEVER signal HDR10 to the swapchain — that would defeat the tone-map on an HDR
        // panel. This absence-of-behaviour check is exactly what the issue's "washed-out on SDR" fix depends on.
        CHECK(optValue(m, QStringLiteral("target-colorspace-hint")) != QStringLiteral("yes"));
        // All three keys are emitted (present, not absent) so a live flip resets the other mode's key.
        CHECK(optValue(m, QStringLiteral("tone-mapping"))           != QStringLiteral("<ABSENT>"));
        CHECK(optValue(m, QStringLiteral("hdr-compute-peak"))       != QStringLiteral("<ABSENT>"));
        CHECK(optValue(m, QStringLiteral("target-colorspace-hint")) != QStringLiteral("<ABSENT>"));
    }
}

// Passthrough-when-supported signals HDR10 to the swapchain and resets the SDR curve to mpv's default.
static void testPassthroughMode()
{
    for (HdrOutput::Platform plat : { HdrOutput::Platform::Desktop, HdrOutput::Platform::Android })
    {
        const auto m = HdrOutput::optionsFor(HdrOutput::Mode::PassthroughWhenSupported, plat);
        EXPECT_OPT(m, "target-colorspace-hint", "yes");      // signal HDR10 metadata to the swapchain
        EXPECT_OPT(m, "hdr-compute-peak",       "yes");      // kept for the tone-map fallback where unsupported
        EXPECT_OPT(m, "tone-mapping",           "auto");     // mpv default: the SDR curve (bt.2446a) is reset
        // Passthrough must NOT leave the SDR-specific curve set — a live flip from tone-map has to clear it.
        CHECK(optValue(m, QStringLiteral("tone-mapping")) != QStringLiteral("bt.2446a"));
        // All three keys emitted so the flip is a full reset either direction.
        CHECK(optValue(m, QStringLiteral("tone-mapping"))           != QStringLiteral("<ABSENT>"));
        CHECK(optValue(m, QStringLiteral("hdr-compute-peak"))       != QStringLiteral("<ABSENT>"));
        CHECK(optValue(m, QStringLiteral("target-colorspace-hint")) != QStringLiteral("<ABSENT>"));
    }
}

// The iOS carve-out: no HDR swapchain, so BOTH stored modes resolve to the tone-map option set — passthrough is
// forced off there regardless of what the user chose.
static void testIOSCarveOut()
{
    for (HdrOutput::Mode mode : { HdrOutput::Mode::ToneMapSdr, HdrOutput::Mode::PassthroughWhenSupported })
    {
        const auto m = HdrOutput::optionsFor(mode, HdrOutput::Platform::IOS);
        EXPECT_OPT(m, "tone-mapping",           "bt.2446a");
        EXPECT_OPT(m, "hdr-compute-peak",       "yes");
        EXPECT_OPT(m, "target-colorspace-hint", "no");
        // The crux of the carve-out: even with Passthrough stored, iOS never signals HDR10 to a swapchain it hasn't.
        CHECK(optValue(m, QStringLiteral("target-colorspace-hint")) != QStringLiteral("yes"));
    }
}

// The id <-> Mode round-trip the two settings builders and Settings use. Independent hand-written expectations.
static void testIdRoundTrip()
{
    CHECK(HdrOutput::defaultModeId() == QStringLiteral("tonemap"));
    CHECK(HdrOutput::idForMode(HdrOutput::Mode::ToneMapSdr)               == QStringLiteral("tonemap"));
    CHECK(HdrOutput::idForMode(HdrOutput::Mode::PassthroughWhenSupported) == QStringLiteral("passthrough"));
    CHECK(HdrOutput::modeFromId(QStringLiteral("tonemap"))     == HdrOutput::Mode::ToneMapSdr);
    CHECK(HdrOutput::modeFromId(QStringLiteral("passthrough")) == HdrOutput::Mode::PassthroughWhenSupported);
    // A blank / hand-edited garbage / absent value degrades to the safe default, never to passthrough.
    CHECK(HdrOutput::modeFromId(QString())                     == HdrOutput::Mode::ToneMapSdr);
    CHECK(HdrOutput::modeFromId(QStringLiteral("nonsense"))    == HdrOutput::Mode::ToneMapSdr);
    // Trimmed, so a hand-edited "  passthrough  " still resolves.
    CHECK(HdrOutput::modeFromId(QStringLiteral("  passthrough  ")) == HdrOutput::Mode::PassthroughWhenSupported);
}

// The Settings half: an empty ini reads the ToneMapSdr default, the id round-trips, and the stored mode drives
// the mpv option set the built-in player would request.
static void testSettings()
{
    // Absent "video/hdr" -> the shipped default (Tone-map to SDR).
    CHECK(Settings::hdrOutput() == HdrOutput::Mode::ToneMapSdr);

    // Round-trips through the store, and both modes are distinguishable.
    Settings::setHdrOutput(QStringLiteral("passthrough"));
    CHECK(Settings::hdrOutput() == HdrOutput::Mode::PassthroughWhenSupported);
    Settings::setHdrOutput(QStringLiteral("tonemap"));
    CHECK(Settings::hdrOutput() == HdrOutput::Mode::ToneMapSdr);
    // A garbage stored value degrades to the default rather than an undefined mode.
    Settings::setHdrOutput(QStringLiteral("nonsense"));
    CHECK(Settings::hdrOutput() == HdrOutput::Mode::ToneMapSdr);

    // End-to-end: the stored mode drives the exact mpv option set on desktop.
    Settings::setHdrOutput(QStringLiteral("passthrough"));
    {
        const auto m = HdrOutput::optionsFor(Settings::hdrOutput(), HdrOutput::Platform::Desktop);
        EXPECT_OPT(m, "target-colorspace-hint", "yes");
        EXPECT_OPT(m, "tone-mapping",           "auto");
    }
    Settings::setHdrOutput(QStringLiteral("tonemap"));
    {
        const auto m = HdrOutput::optionsFor(Settings::hdrOutput(), HdrOutput::Platform::Desktop);
        EXPECT_OPT(m, "tone-mapping",           "bt.2446a");
        EXPECT_OPT(m, "target-colorspace-hint", "no");
    }
    // And the iOS carve-out holds end-to-end: even with Passthrough stored, iOS resolves to the tone-map set.
    Settings::setHdrOutput(QStringLiteral("passthrough"));
    {
        const auto m = HdrOutput::optionsFor(Settings::hdrOutput(), HdrOutput::Platform::IOS);
        EXPECT_OPT(m, "target-colorspace-hint", "no");
        EXPECT_OPT(m, "tone-mapping",           "bt.2446a");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testToneMapMode();
    testPassthroughMode();
    testIOSCarveOut();
    testIdRoundTrip();
    testSettings();
    if (failures == 0) std::printf("HDROUTPUT-OK\n");
    return failures == 0 ? 0 : 1;
}
