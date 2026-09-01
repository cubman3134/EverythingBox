// Headless check of the hardware-decode Setting (issue #67): the pure choice->mpv-option map
// (src/video/HwDecode.h) plus the Settings default. QtCore-only — the map is a header-only pure function and
// Settings is a QSettings wrapper — so it runs under the offscreen QPA in CI with no window and no player.
//
// The map is pinned with HAND-WRITTEN expected mpv strings for every (choice x platform) cell, computed by a
// human reading the design, NOT by calling mpvOption() a second time. It covers the three choices
// (off/auto/on), the "auto" default an unknown/empty value falls through to, and the two platform carve-outs:
//   - iOS is forced to "no" for EVERY choice (software QImage render path — no hardware decode possible).
//   - Android is NOT forced to software the way iOS is: its Auto is the same copy-back list every desktop
//     gets, and mediacodec-copy is the first entry in it that exists on Android — the "mediacodec via the same
//     Auto path" the issue calls for.
//
// Issue #229 added the second half of testAutoIsCopyBack(). The old expectation here was the literal string
// "auto-safe", and it was GREEN while the shipped behaviour was the opposite of the documented one: on an
// NVIDIA machine mpv resolves "auto-safe" to nvdec DIRECT (CUDA<->GL interop), not to the copy-back the design
// says Auto is for. A probe that pins a string cannot see that — only native/tools/hwdecframe, which asks a
// real libmpv what a string resolved to, can. What this probe CAN pin, and now does, is the property that made
// the old value wrong: every decoder Auto can land on is a copy-back one. So the cell assertions below are
// joined by structural ones, and a future edit that swaps the list back for a bare "auto"/"auto-safe" fails
// here rather than in someone's living room.
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

// The one string Auto asks mpv for, written out by hand here rather than read from HwDecode::autoCopyBackList()
// — the whole point of this probe is that the expectation is INDEPENDENT of the code under test.
#define AUTO_LIST "nvdec-copy,d3d11va-copy,vaapi-copy,videotoolbox-copy,mediacodec-copy,auto-copy-safe"

static void testDesktop()
{
    // Desktop: the three choices map straight through.
    EXPECT_OPT("off",  Desktop, "no");         // preserved old guaranteed-correct software decode
    EXPECT_OPT("auto", Desktop, AUTO_LIST);    // the default: copy-back decoders by name + software fallback
    EXPECT_OPT("on",   Desktop, "auto");       // full hardware, direct paths included
    // An unknown / empty value falls through to the "auto" default, never to "no".
    EXPECT_OPT("",        Desktop, AUTO_LIST);
    EXPECT_OPT("garbage", Desktop, AUTO_LIST);
}

static void testAndroid()
{
    // Android reaches mediacodec through the SAME Auto path: none of the entries before mediacodec-copy exist
    // on Android, and mpv skips a hwdec list entry it does not have. Critically Android is NOT forced to
    // software the way iOS is: Auto/On are hardware here.
    EXPECT_OPT("off",  Android, "no");
    EXPECT_OPT("auto", Android, AUTO_LIST);
    EXPECT_OPT("on",   Android, "auto");
    EXPECT_OPT("",     Android, AUTO_LIST);
    // Named explicitly because it is the whole Android carve-out: the list must CONTAIN Android's copy-back
    // decoder, or Auto on Android is a hardware mode with no hardware decoder in it.
    CHECK(HwDecode::mpvOption(QStringLiteral("auto"), HwDecode::Platform::Android)
              .split(QLatin1Char(',')).contains(QStringLiteral("mediacodec-copy")));
}

// The property the cell strings above exist to protect (issue #229): every decoder Auto can resolve to is a
// COPY-BACK one. Structural, so it survives the list being reordered or extended, and so a swap back to a bare
// "auto"/"auto-safe" - the exact regression #229 is about - fails here.
static void testAutoIsCopyBack()
{
    for (auto platform : { HwDecode::Platform::Desktop, HwDecode::Platform::Android })
    {
        const QString opt = HwDecode::mpvOption(QStringLiteral("auto"), platform);
        const QStringList entries = opt.split(QLatin1Char(','), Qt::SkipEmptyParts);
        CHECK(entries.size() >= 2);                       // a list, not a single method
        // Every entry is copy-back: either a named "<decoder>-copy" or mpv's own copy-back catch-all. A bare
        // "auto", "auto-safe", "nvdec" or "d3d11va" anywhere in here can hand the render context an interop
        // texture, which is the thing Auto exists to avoid.
        for (const QString& e : entries)
            CHECK(e.endsWith(QStringLiteral("-copy")) || e == QStringLiteral("auto-copy-safe"));
        // NVIDIA's own decoder is preferred over the D3D11VA family: measured, on the machine in #229,
        // "auto-copy-safe" alone picks d3d11va-copy, which is both the family HwDecode.h's opening paragraph
        // is a scar from and the slower of the two there.
        CHECK(entries.indexOf(QStringLiteral("nvdec-copy")) == 0);
        CHECK(entries.indexOf(QStringLiteral("nvdec-copy")) < entries.indexOf(QStringLiteral("d3d11va-copy")));
        // The catch-all is LAST: an entry after it would be unreachable on any platform where it matches.
        CHECK(entries.last() == QStringLiteral("auto-copy-safe"));
        // And the two strings this used to be, spelled out, because "it is not auto-safe any more" is the
        // whole point and a substring test would pass on "auto-copy-safe".
        CHECK(opt != QStringLiteral("auto-safe"));
        CHECK(opt != QStringLiteral("auto"));
        CHECK(!entries.contains(QStringLiteral("auto-safe")));
    }
    // On is the deliberate escape hatch BACK to full hardware, interop included - it must NOT be copy-back, or
    // the setting has two names for one behaviour.
    CHECK(HwDecode::mpvOption(QStringLiteral("on"), HwDecode::Platform::Desktop) == QStringLiteral("auto"));
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
    CHECK(HwDecode::mpvOption(Settings::hwDecode(), HwDecode::Platform::Desktop) == QStringLiteral(AUTO_LIST));
}

// #55 video hover previews: the two Settings accessors that gate the themed `video` element. Same shape as
// the hwdec default check above (this probe already links Settings.cpp + runs on an isolated, empty ini),
// so it also owns the previews' shipped defaults + the snap-volume clamp. The rendered snap, mute and BGM
// duck are QML/mpv-side and not headlessly drivable; this pins only the pure Settings contract.
static void testVideoPreviews()
{
    // Shipped defaults on an empty ini: previews ON (the intended browse experience), volume 0 (MUTED, so a
    // fresh install plays silently and never ducks the music). Both absences must read as these, not the
    // opposite — the crux of the issue's "on but muted" default.
    CHECK(Settings::videoPreviewsEnabled() == true);   // "video/previewsEnabled" absent -> ON
    CHECK(Settings::videoSnapVolume() == 0);           // "video/snapVolume" absent -> muted

    // The toggle round-trips and OFF is distinguishable from the default.
    Settings::setVideoPreviewsEnabled(false);
    CHECK(Settings::videoPreviewsEnabled() == false);
    Settings::setVideoPreviewsEnabled(true);
    CHECK(Settings::videoPreviewsEnabled() == true);

    // The snap volume round-trips and is CLAMPED to 0..100 on write, so a hand-edited ini or a stray control
    // can never drive mpv's volume property out of range.
    Settings::setVideoSnapVolume(60);
    CHECK(Settings::videoSnapVolume() == 60);
    Settings::setVideoSnapVolume(1000);   // over-max clamps to 100
    CHECK(Settings::videoSnapVolume() == 100);
    Settings::setVideoSnapVolume(-25);    // under-min clamps to 0
    CHECK(Settings::videoSnapVolume() == 0);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDesktop();
    testAndroid();
    testAutoIsCopyBack();
    testIOSCarveOut();
    testDefault();
    testVideoPreviews();
    if (failures == 0) std::printf("HWDECODE-OK\n");
    return failures == 0 ? 0 : 1;
}
