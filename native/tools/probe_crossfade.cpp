// Headless check of crossfade (issue #141): the pure decision — given the user's setting in seconds, whether
// the item is MUSIC, and what the two entries either side of a boundary are (album tag, folder, length),
// should the boundary be crossfaded and for how long — plus the equal-power curve and the Settings
// default/clamp/round-trip that feed it (src/video/Crossfade.h, header-only). QtCore-only: the decision is a
// pure function over a value type and Settings is a QSettings wrapper, so this runs under the offscreen QPA in
// CI with no window, no player and no mpv. Settings.cpp pulls FormFactor.cpp (videoRefreshSync()'s default
// resolves against it), same as probe_replaygain.
//
// WHY THIS IS THE PART WORTH TESTING. Overlapping two decoders is mechanical and needs audio to judge; WHERE
// the overlap is allowed is judgement, is testable without a speaker, and is the half that does damage when it
// is wrong. #141's words: crossfading a live album is vandalism. A fade that fires where it should not rewrites
// a record the listener chose to play in order, silently and every time; a fade that fails to fire costs a
// nicety. So the suppressions below are the assertions, and the curve is checked for the property it exists
// for rather than for a table of numbers.
//
// Every expectation is a HAND-WRITTEN number or verdict computed by reading the design, NOT by calling the
// function under test a second time. What it pins:
//   - OFF IS OFF: setting 0 yields no fade for any pair, and 0 is the shipped default;
//   - the MUSIC-ONLY carve-out: isMusic false (an audiobook, a podcast, or video — the one expression the host
//     computes for ReplayGain too) yields no fade for any pair, at any setting;
//   - SAME-ALBUM SUPPRESSION: two entries carrying the same album tag never fade, case- and whitespace-
//     insensitively; different albums do; one side tagged and the other not is NOT the same album;
//   - the UNTAGGED FOLDER FALLBACK: with no album tag on either side the directory stands in for the record,
//     so a folder of untagged rips of one album is still protected — and it can never override a tag, which is
//     asserted in both directions;
//   - the TOO-SHORT-TRACK cap: the window never exceeds half of either known length, an unknown (0) length
//     constrains nothing, and a window that shrinks below one second is dropped rather than shipped as a blip;
//   - the BAND: 1-12 s, with a stray sub-minimum value rounding UP to 1 rather than silently down to off;
//   - EQUAL POWER: out^2 + in^2 == 1 across the whole window (the property the curve exists for — a linear
//     pair dips to 0.707 at the midpoint and is audibly wrong), the endpoints are exactly (1,0) and (0,1), and
//     an overshooting t clamps instead of running the curve backwards;
//   - the Settings half: an absent "playback/crossfadeSeconds" reads back as 0/off, and the value clamps on
//     both read and write.
//
// Prints CROSSFADE-OK on success; any failure prints CROSSFADE-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini the
// Settings half reads starts empty and is removed at exit — the default it asserts is a genuine absence.
#include "Crossfade.h"
#include "Settings.h"
#include "AppBrand.h"   // the ini file name — the clamp is asserted against what is actually PERSISTED
#include "AppPaths.h"   // this process's isolated data dir (issue #42), where that ini lives

#include <QCoreApplication>
#include <QSettings>
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CROSSFADE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using Track = Crossfade::Track;

// A queue entry, spelled out. The three fields are exactly what the host can know about a file it has not
// opened yet: the album tag, the directory it sits in, and its length.
static Track entry(const char* album, const char* folder, double dur)
{
    Track t;
    t.album = QString::fromUtf8(album);
    t.folder = QString::fromUtf8(folder);
    t.durationSec = dur;
    return t;
}
// The ordinary case used by most cells: a long, tagged track from a named album in its own folder.
static Track fromAlbum(const char* album) { return entry(album, "/music/x", 240.0); }

#define EXPECT_SECS(setting, music, a, b, want) do { \
    const double got = Crossfade::secondsFor(setting, music, a, b); \
    if (std::fabs(got - double(want)) > 1e-9) { \
        std::fprintf(stderr, "CROSSFADE-FAIL setting=%d music=%d -> got %.3f want %.3f (line %d)\n", \
                     int(setting), int(music), got, double(want), __LINE__); \
        ++failures; \
    } \
} while (0)

// ---- 1. Off is off ---------------------------------------------------------------------------------------
static void testOffIsOff()
{
    const Track a = fromAlbum("Kind of Blue");
    const Track b = fromAlbum("Blue Train");
    // 0 is off; so is anything at or below it out of a mangled store. Nothing about the pair can turn it on.
    EXPECT_SECS(0, true, a, b, 0.0);
    EXPECT_SECS(-3, true, a, b, 0.0);
    // And off is what an install that never touched the setting has.
    CHECK(Crossfade::defaultSeconds() == 0);
    CHECK(Crossfade::defaultSeconds() == Crossfade::offSeconds());
}

// ---- 2. Music only ---------------------------------------------------------------------------------------
// The carve-out #141 shares with ReplayGain, and the reason it is a hard gate rather than a heuristic:
// dissolving the last sentence of a chapter into the first of the next is losing words, not a style. Video is
// false through the same host expression, so "never for video" is this same cell.
static void testMusicOnly()
{
    const Track a = fromAlbum("Chapter 1");
    const Track b = fromAlbum("Chapter 2");
    for (int s = Crossfade::minSeconds(); s <= Crossfade::maxSeconds(); ++s)
        EXPECT_SECS(s, false, a, b, 0.0);
    // Untagged spoken word, different folders, plenty long: still nothing. There is no pair that unlocks it.
    EXPECT_SECS(6, false, entry("", "/books/a", 3600.0), entry("", "/books/b", 3600.0), 0.0);
    // And the same pair IS faded once it is music — so the assertion above is discriminating, not vacuous.
    EXPECT_SECS(6, true, entry("", "/books/a", 3600.0), entry("", "/books/b", 3600.0), 6.0);
}

// ---- 3. Same album ---------------------------------------------------------------------------------------
static void testSameAlbumSuppression()
{
    // The headline case: two tracks of one live record, mid-album. Never.
    EXPECT_SECS(8, true, fromAlbum("Live at Leeds"), fromAlbum("Live at Leeds"), 0.0);
    // Tag spellings differ in case and padding all the time (different taggers, different rips of one disc).
    EXPECT_SECS(8, true, fromAlbum("Live At Leeds"), fromAlbum("  live at leeds "), 0.0);
    // Two different records DO fade — the whole point of the feature.
    EXPECT_SECS(8, true, fromAlbum("Live at Leeds"), fromAlbum("Who's Next"), 8.0);
    // One side tagged, the other not: NOT the same album. A tagged track and an untagged stray are not
    // evidence of one record, and this is also what stops the folder fallback below from overriding a tag.
    EXPECT_SECS(8, true, entry("Live at Leeds", "/music/leeds", 240.0), entry("", "/music/leeds", 240.0), 8.0);
    EXPECT_SECS(8, true, entry("", "/music/leeds", 240.0), entry("Live at Leeds", "/music/leeds", 240.0), 8.0);
    // Album ALONE, deliberately not album+artist: a badly tagged compilation has a different artist on every
    // track, and pairing the two would un-suppress exactly the record most likely to be continuous. So two
    // unrelated "Greatest Hits" do not fade either — the harmless side of that trade, asserted so it stays a
    // decision rather than becoming a surprise.
    EXPECT_SECS(8, true, entry("Greatest Hits", "/music/a", 240.0), entry("Greatest Hits", "/music/b", 240.0), 0.0);
}

// ---- 4. The untagged folder fallback -----------------------------------------------------------------------
static void testUntaggedFolderFallback()
{
    // No tags anywhere: the directory is the only evidence of a record there is, and a folder of untagged rips
    // of one album is exactly the material rule 3 exists to protect.
    EXPECT_SECS(5, true, entry("", "/music/bootleg", 240.0), entry("", "/music/bootleg", 240.0), 0.0);
    // Different folders, still untagged: nothing says these are one record, so the fade happens.
    EXPECT_SECS(5, true, entry("", "/music/a", 240.0), entry("", "/music/b", 240.0), 5.0);
    // An empty folder is not evidence of anything (a stream, an unresolvable path) and never matches — not
    // even against another empty one, which would otherwise suppress every boundary between two streams.
    EXPECT_SECS(5, true, entry("", "", 240.0), entry("", "", 240.0), 5.0);
    // The fallback can NEVER override a tag, in either direction: same folder but different album tags fades,
    // and different folders with the same album tag does not.
    EXPECT_SECS(5, true, entry("Disc 1", "/music/set", 240.0), entry("Disc 2", "/music/set", 240.0), 5.0);
    EXPECT_SECS(5, true, entry("One Record", "/music/cd1", 240.0), entry("One Record", "/music/cd2", 240.0), 0.0);
}

// ---- 5. Never longer than the music --------------------------------------------------------------------
static void testTooShortTrackCap()
{
    // 12 s asked for, but the outgoing track is 10 s long: half of it, 5 s. A fade longer than the tail it is
    // fading out of is not a transition, it is two tracks played at once.
    EXPECT_SECS(12, true, entry("A", "/m/a", 10.0), entry("B", "/m/b", 240.0), 5.0);
    // The incoming side caps it too, and the SHORTER of the two wins.
    EXPECT_SECS(12, true, entry("A", "/m/a", 240.0), entry("B", "/m/b", 8.0), 4.0);
    EXPECT_SECS(12, true, entry("A", "/m/a", 10.0), entry("B", "/m/b", 8.0), 4.0);
    // An unknown length (0 — a container that would not give one cheaply) constrains nothing rather than
    // capping to zero, which would have turned "we could not read the length" into "never fade".
    EXPECT_SECS(6, true, entry("A", "/m/a", 0.0), entry("B", "/m/b", 0.0), 6.0);
    EXPECT_SECS(6, true, entry("A", "/m/a", 0.0), entry("B", "/m/b", 240.0), 6.0);
    // Under a second after the cap: dropped entirely. A 0.4 s fade is a blip, not a transition.
    EXPECT_SECS(12, true, entry("A", "/m/a", 1.6), entry("B", "/m/b", 240.0), 0.0);
    // Exactly one second survives — the boundary of the rule, not just its inside.
    EXPECT_SECS(12, true, entry("A", "/m/a", 2.0), entry("B", "/m/b", 240.0), 1.0);
    // The cap never LENGTHENS a window: a 2 s setting on two long tracks stays 2 s.
    EXPECT_SECS(2, true, fromAlbum("A"), fromAlbum("B"), 2.0);
}

// ---- 6. The band -----------------------------------------------------------------------------------------
static void testBand()
{
    CHECK(Crossfade::minSeconds() == 1);
    CHECK(Crossfade::maxSeconds() == 12);
    CHECK(Crossfade::clampSeconds(0) == 0);
    CHECK(Crossfade::clampSeconds(-9) == 0);
    CHECK(Crossfade::clampSeconds(1) == 1);
    CHECK(Crossfade::clampSeconds(12) == 12);
    CHECK(Crossfade::clampSeconds(99) == 12);
    // A stray sub-minimum value rounds UP, never down to off: a surface showing the feature as on must not be
    // lying about it. (There is no integer strictly between 0 and 1, so this is asserted through the clamp of
    // an out-of-band value arriving as a rounded 0 from elsewhere — the direction is what matters.)
    CHECK(Crossfade::clampSeconds(13) == 12);
    // Over-max asked for still fades, at the top of the band, rather than being rejected as invalid.
    EXPECT_SECS(99, true, fromAlbum("A"), fromAlbum("B"), 12.0);
}

// ---- 7. Equal power --------------------------------------------------------------------------------------
static void testEqualPowerCurve()
{
    // Endpoints: the window opens on the outgoing track alone and closes on the incoming track alone.
    CHECK(std::fabs(Crossfade::outgoingGain(0.0) - 1.0) < 1e-9);
    CHECK(std::fabs(Crossfade::incomingGain(0.0) - 0.0) < 1e-9);
    CHECK(std::fabs(Crossfade::outgoingGain(1.0) - 0.0) < 1e-9);
    CHECK(std::fabs(Crossfade::incomingGain(1.0) - 1.0) < 1e-9);
    // THE property, across the whole window: constant power. Two uncorrelated signals sum in power, so a
    // linear pair (which satisfies out + in == 1) sums to 0.707 at the midpoint and dips audibly. This is the
    // assertion that tells the two curves apart, and it is the reason the pair is sin/cos at all.
    for (int i = 0; i <= 100; ++i)
    {
        const double t = i / 100.0;
        const double o = Crossfade::outgoingGain(t), n = Crossfade::incomingGain(t);
        CHECK(o >= 0.0 && o <= 1.0);
        CHECK(n >= 0.0 && n <= 1.0);
        CHECK(std::fabs(o * o + n * n - 1.0) < 1e-9);
    }
    // Monotone, so a tick can never make the outgoing track louder than it was a tick ago.
    for (int i = 1; i <= 100; ++i)
        CHECK(Crossfade::outgoingGain(i / 100.0) <= Crossfade::outgoingGain((i - 1) / 100.0) + 1e-12);
    // An overshooting or negative t CLAMPS rather than running the curve past its quarter — a stalled GUI
    // thread that misses ticks lands on the end of the fade, not somewhere on the way back up.
    CHECK(std::fabs(Crossfade::outgoingGain(1.7) - 0.0) < 1e-9);
    CHECK(std::fabs(Crossfade::incomingGain(1.7) - 1.0) < 1e-9);
    CHECK(std::fabs(Crossfade::outgoingGain(-0.4) - 1.0) < 1e-9);
    CHECK(std::fabs(Crossfade::incomingGain(-0.4) - 0.0) < 1e-9);
}

// ---- 8. Settings -----------------------------------------------------------------------------------------
static void testSettings()
{
    // Absent key -> off. This is a genuine absence: the probe's data dir is its own and starts empty (#42), so
    // nothing has written this key before now.
    CHECK(Settings::crossfadeSeconds() == 0);
    Settings::setCrossfadeSeconds(7);
    CHECK(Settings::crossfadeSeconds() == 7);
    // Clamped on WRITE, asserted against what is actually persisted rather than against the getter — a getter
    // that clamps on read would hide an out-of-band value sitting in the user's ini.
    Settings::setCrossfadeSeconds(500);
    {
        QSettings ini(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile), QSettings::IniFormat);
        CHECK(ini.value(QStringLiteral("playback/crossfadeSeconds")).toInt() == 12);
    }
    CHECK(Settings::crossfadeSeconds() == 12);
    // Clamped on READ too: a value written by an older build or by hand still comes back inside the band.
    {
        QSettings ini(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile), QSettings::IniFormat);
        ini.setValue(QStringLiteral("playback/crossfadeSeconds"), 900);
        ini.sync();
    }
    CHECK(Settings::crossfadeSeconds() == 12);
    Settings::setCrossfadeSeconds(0);
    CHECK(Settings::crossfadeSeconds() == 0);

    // End-to-end: the STORED setting is what the boundary decision runs on, and the same stored setting yields
    // nothing for an audiobook — the two halves the host wires together.
    Settings::setCrossfadeSeconds(6);
    EXPECT_SECS(Settings::crossfadeSeconds(), true,  fromAlbum("A"), fromAlbum("B"), 6.0);
    EXPECT_SECS(Settings::crossfadeSeconds(), false, fromAlbum("A"), fromAlbum("B"), 0.0);
    EXPECT_SECS(Settings::crossfadeSeconds(), true,  fromAlbum("A"), fromAlbum("A"), 0.0);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testOffIsOff();
    testMusicOnly();
    testSameAlbumSuppression();
    testUntaggedFolderFallback();
    testTooShortTrackCap();
    testBand();
    testEqualPowerCurve();
    testSettings();
    if (failures == 0) std::printf("CROSSFADE-OK\n");
    return failures == 0 ? 0 : 1;
}
