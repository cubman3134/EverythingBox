// Headless check of the hardware-decode Setting (issue #67): the pure choice->mpv-option map
// (src/video/HwDecode.h) plus the Settings default. QtCore-only — the map is a header-only pure function and
// Settings is a QSettings wrapper — so it runs under the offscreen QPA in CI with no window and no player.
//
// The map is pinned with HAND-WRITTEN expected mpv strings for every (choice x platform) cell, computed by a
// human reading the design, NOT by calling mpvOption() a second time. It covers the three choices
// (off/auto/on), the "auto" default an unknown/empty value falls through to, and the two platform carve-outs:
//   - iOS is forced to "no" for EVERY choice (software QImage render path — no hardware decode possible).
//   - Android is NOT forced to software the way iOS is: its Auto is "auto-safe" (mpv negotiates
//     mediacodec-copy), the "mediacodec via the same Auto path" the issue calls for.
// The Settings half pins the stored default: an absent "video/hwdec" reads back as "auto", so the shipped
// default is Auto (auto-safe), never Off — the crux of the issue.
//
// Prints HWDECODE-OK on success; any failure prints HWDECODE-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini the
// Settings half reads starts empty and is removed at exit — the default it asserts is a genuine absence.
#include "HwDecode.h"
#include "Settings.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "HWDECODE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Assert a (choice, platform) cell maps to an exact, hand-written mpv option string.
#define EXPECT_OPT(choice, platform, want) do { \
    const QString got = HwDecode::mpvOption(QStringLiteral(choice), HwDecode::Platform::platform); \
    if (got != QStringLiteral(want)) { \
        std::fprintf(stderr, "HWDECODE-FAIL %s/%s -> got '%s' want '%s' (line %d)\n", \
                     choice, #platform, got.toUtf8().constData(), want, __LINE__); \
        ++failures; \
    } \
} while (0)

static void testDesktop()
{
    // Desktop: the three choices map straight through.
    EXPECT_OPT("off",  Desktop, "no");         // preserved old guaranteed-correct software decode
    EXPECT_OPT("auto", Desktop, "auto-safe");  // the default: safe copy-back + software fallback
    EXPECT_OPT("on",   Desktop, "auto");       // full hardware, direct paths included
    // An unknown / empty value falls through to the "auto" default, never to "no".
    EXPECT_OPT("",        Desktop, "auto-safe");
    EXPECT_OPT("garbage", Desktop, "auto-safe");
}

static void testAndroid()
{
    // Android reaches mediacodec through the SAME Auto path (auto-safe -> mediacodec-copy). Critically it is
    // NOT forced to software the way iOS is: Auto/On are hardware here.
    EXPECT_OPT("off",  Android, "no");
    EXPECT_OPT("auto", Android, "auto-safe");
    EXPECT_OPT("on",   Android, "auto");
    EXPECT_OPT("",     Android, "auto-safe");
}

static void testIOSCarveOut()
{
    // iOS keeps "no" for EVERY choice — the software-render path can't use hardware decode regardless.
    EXPECT_OPT("off",     IOS, "no");
    EXPECT_OPT("auto",    IOS, "no");
    EXPECT_OPT("on",      IOS, "no");
    EXPECT_OPT("",        IOS, "no");
    EXPECT_OPT("garbage", IOS, "no");
}

static void testDefault()
{
    // The design's default choice id, and the SHIPPED default read from an empty ini, are both "auto".
    CHECK(HwDecode::defaultChoice() == QStringLiteral("auto"));
    CHECK(Settings::hwDecode() == QStringLiteral("auto"));   // "video/hwdec" absent -> Auto, not Off

    // Round-trips through the store, and Off/On are distinguishable from the default.
    Settings::setHwDecode(QStringLiteral("off"));
    CHECK(Settings::hwDecode() == QStringLiteral("off"));
    Settings::setHwDecode(QStringLiteral("on"));
    CHECK(Settings::hwDecode() == QStringLiteral("on"));
    Settings::setHwDecode(QStringLiteral("auto"));
    CHECK(Settings::hwDecode() == QStringLiteral("auto"));

    // End-to-end: the stored choice drives the mpv option the built-in player would request on desktop.
    Settings::setHwDecode(QStringLiteral("off"));
    CHECK(HwDecode::mpvOption(Settings::hwDecode(), HwDecode::Platform::Desktop) == QStringLiteral("no"));
    Settings::setHwDecode(QStringLiteral("auto"));
    CHECK(HwDecode::mpvOption(Settings::hwDecode(), HwDecode::Platform::Desktop) == QStringLiteral("auto-safe"));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDesktop();
    testAndroid();
    testIOSCarveOut();
    testDefault();
    if (failures == 0) std::printf("HWDECODE-OK\n");
    return failures == 0 ? 0 : 1;
}
