// Headless check of refresh-rate matching Tier 1 (issue #70): the pure toggle->mpv-`video-sync` map plus the
// form-factor default (src/video/RefreshSync.h) and the Settings round-trip. QtCore-only — the map is a
// header-only pure function and Settings is a QSettings wrapper — so it runs under the offscreen QPA in CI with
// no window and no player. Settings.cpp pulls FormFactor.cpp (videoRefreshSync()'s default resolves against it),
// same as probe_hwdecode.
//
// Every expectation is a HAND-WRITTEN mpv string / bool computed by reading the design, NOT by calling the
// function under test a second time. It pins:
//   - videoSyncFor: ON (non-iOS) -> "display-resync"; OFF -> "audio"; iOS -> "audio" for BOTH toggle states
//     (software QImage render path has no display clock to resync to — forced off regardless).
//   - defaultEnabled: ON for Desktop and Tv (a big-screen pan is exactly what this fixes), OFF for a Mobile
//     handheld — the shipped per-form-factor default the toggle takes when the user has never set it.
//   - the Settings half: an absent "video/refreshSync" reads back as the form-factor default (Desktop under the
//     offscreen CI build => ON), and set true/false round-trips.
//
// Prints REFRESHSYNC-OK on success; any failure prints REFRESHSYNC-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini the
// Settings half reads starts empty and is removed at exit — the default it asserts is a genuine absence.
#include "RefreshSync.h"
#include "Settings.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "REFRESHSYNC-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Assert a (enabled, platform) cell maps to an exact, hand-written mpv `video-sync` value.
#define EXPECT_SYNC(enabled, platform, want) do { \
    const QString got = RefreshSync::videoSyncFor(enabled, RefreshSync::Platform::platform); \
    if (got != QStringLiteral(want)) { \
        std::fprintf(stderr, "REFRESHSYNC-FAIL %s/%s -> got '%s' want '%s' (line %d)\n", \
                     #enabled, #platform, got.toUtf8().constData(), want, __LINE__); \
        ++failures; \
    } \
} while (0)

static void testDesktopMap()
{
    // Desktop: the toggle straight-maps. ON locks video to the display, OFF is mpv's own audio-clock default.
    EXPECT_SYNC(true,  Desktop, "display-resync");
    EXPECT_SYNC(false, Desktop, "audio");
}

static void testAndroidMap()
{
    // Android renders through a real GL surface (NOT the iOS software blit), so display-resync applies there too
    // when the user has it on — it is NOT forced off the way iOS is.
    EXPECT_SYNC(true,  Android, "display-resync");
    EXPECT_SYNC(false, Android, "audio");
}

static void testIOSCarveOut()
{
    // iOS keeps "audio" for BOTH toggle states — the software-render path has no display clock to resync to, so
    // the toggle cannot reach display-resync there. This is the force-off the issue calls for.
    EXPECT_SYNC(true,  IOS, "audio");
    EXPECT_SYNC(false, IOS, "audio");
}

static void testFormFactorDefault()
{
    // The shipped default per form factor: ON for desktop and TV, OFF for a mobile handheld.
    CHECK(RefreshSync::defaultEnabled(RefreshSync::FormFactor::Desktop) == true);
    CHECK(RefreshSync::defaultEnabled(RefreshSync::FormFactor::Tv)      == true);
    CHECK(RefreshSync::defaultEnabled(RefreshSync::FormFactor::Mobile)  == false);
}

static void testSettings()
{
    // The SHIPPED default read from an empty ini: the offscreen CI build resolves the Desktop form factor, whose
    // default is ON. ("video/refreshSync" absent -> form-factor default, not a hard-coded false.)
    CHECK(Settings::videoRefreshSync() == true);

    // Round-trips through the store, and both states are distinguishable from each other.
    Settings::setVideoRefreshSync(false);
    CHECK(Settings::videoRefreshSync() == false);
    Settings::setVideoRefreshSync(true);
    CHECK(Settings::videoRefreshSync() == true);

    // End-to-end: the stored toggle drives the mpv option the built-in player would request on desktop.
    Settings::setVideoRefreshSync(true);
    CHECK(RefreshSync::videoSyncFor(Settings::videoRefreshSync(), RefreshSync::Platform::Desktop)
          == QStringLiteral("display-resync"));
    Settings::setVideoRefreshSync(false);
    CHECK(RefreshSync::videoSyncFor(Settings::videoRefreshSync(), RefreshSync::Platform::Desktop)
          == QStringLiteral("audio"));
    // And the iOS carve-out holds end-to-end: even with the toggle stored ON, iOS resolves to "audio".
    Settings::setVideoRefreshSync(true);
    CHECK(RefreshSync::videoSyncFor(Settings::videoRefreshSync(), RefreshSync::Platform::IOS)
          == QStringLiteral("audio"));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDesktopMap();
    testAndroidMap();
    testIOSCarveOut();
    testFormFactorDefault();
    testSettings();
    if (failures == 0) std::printf("REFRESHSYNC-OK\n");
    return failures == 0 ? 0 : 1;
}
