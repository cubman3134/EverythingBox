// Headless check of ReplayGain (issue #141): the pure decision — given the user's mode, whether the item is
// MUSIC, and which REPLAYGAIN_* gain tags the file actually carries, which mpv options get applied — plus the
// Settings default/clamp/round-trip that feed it (src/video/ReplayGain.h, header-only). QtCore-only: the map is
// a pure function over a value type and Settings is a QSettings wrapper, so this runs under the offscreen QPA
// in CI with no window, no player and no mpv. Settings.cpp pulls FormFactor.cpp (videoRefreshSync()'s default
// resolves against it), same as probe_refreshsync.
//
// Every expectation is a HAND-WRITTEN mpv string / enum computed by reading the design, NOT by calling the
// function under test a second time. What it pins, and why each one is worth a test:
//   - the MUSIC-ONLY carve-out: an audiobook/podcast (isMusic false) resolves to Off in EVERY mode, so a book
//     is never levelled and never inherits the previous album's gain;
//   - the UNTAGGED case: neither gain tag present -> Off, in every mode. #141 is explicit that untagged
//     material plays unmodified — no analysis, no invented gain;
//   - PRESENCE IS NOT VALUE: a gain tagged "0.00 dB" is a real, already-normalised track and must resolve to a
//     real mode, not to the untagged Off. This is the exact confusion AudioTags::GainValue's `present` flag
//     exists to prevent, and the only way to get it wrong is to test the number instead of the flag;
//   - the one-sided-tag fallbacks: Album asked for with only a track gain -> Track, and the mirror;
//   - the emitted OPTION SET: all four options, every time, with `replaygain-clip` pinned yes (clipping
//     prevention is deliberately not a setting) and `replaygain-fallback` pinned 0 in EVERY cell of the matrix
//     — that option is mpv's "gain for files with no tags", and a non-zero value there would be exactly the
//     invented gain the issue forbids;
//   - the preamp: clamped to the ±15 dB band, and forced to 0 whenever the answer is Off (a preamp on material
//     we just decided not to touch is a hidden volume change, not ReplayGain);
//   - the Settings half: an absent "playback/replayGain" reads back as ALBUM (the shipped default), a garbage
//     id also reads as Album rather than silently Off, all three modes round-trip, and the preamp clamps on
//     both read and write.
//
// Prints REPLAYGAIN-OK on success; any failure prints REPLAYGAIN-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini the
// Settings half reads starts empty and is removed at exit — the default it asserts is a genuine absence.
#include "ReplayGain.h"
#include "Settings.h"
#include "AppBrand.h"   // the ini file name — the preamp clamp is asserted against what is actually PERSISTED
#include "AppPaths.h"   // this process's isolated data dir (issue #42), where that ini lives

#include <QCoreApplication>
#include <QSettings>
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "REPLAYGAIN-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using Mode = ReplayGain::Mode;
using Gain = AudioTags::GainValue;

// The two tag states that matter, plus the one that is easy to get wrong.
static Gain absent()            { Gain g; return g; }                                  // present == false
static Gain tagged(double db)   { Gain g; g.present = true; g.value = db; return g; }   // a real tag
static Gain taggedZero()        { return tagged(0.0); }  // "already normalised" — present, value 0.00 dB

static const char* modeName(Mode m)
{
    switch (m) { case Mode::Off: return "Off"; case Mode::Track: return "Track"; case Mode::Album: return "Album"; }
    return "?";
}

// Assert a (setting, isMusic, trackGain, albumGain) cell resolves to an exact, hand-written effective mode.
#define EXPECT_MODE(setting, music, tg, ag, want) do { \
    const Mode got = ReplayGain::effectiveMode(Mode::setting, music, tg, ag); \
    if (got != Mode::want) { \
        std::fprintf(stderr, "REPLAYGAIN-FAIL %s/music=%d -> got %s want %s (line %d)\n", \
                     #setting, int(music), modeName(got), #want, __LINE__); \
        ++failures; \
    } \
} while (0)

// Assert one option name carries an exact value in the emitted set (and that the name is emitted at all).
static void expectOption(const QVector<QPair<QString, QString>>& opts, const char* name, const char* want,
                         int line)
{
    for (const auto& o : opts)
    {
        if (o.first == QString::fromLatin1(name))
        {
            if (o.second != QString::fromLatin1(want))
            {
                std::fprintf(stderr, "REPLAYGAIN-FAIL %s -> got '%s' want '%s' (line %d)\n",
                             name, o.second.toUtf8().constData(), want, line);
                ++failures;
            }
            return;
        }
    }
    std::fprintf(stderr, "REPLAYGAIN-FAIL option '%s' not emitted at all (line %d)\n", name, line);
    ++failures;
}
#define EXPECT_OPT(opts, name, want) expectOption(opts, name, want, __LINE__)

static void testIdRoundTrip()
{
    // The stored ids are the ini's words; both settings builders map their displayed option back through them.
    CHECK(ReplayGain::idForMode(Mode::Off)   == QStringLiteral("off"));
    CHECK(ReplayGain::idForMode(Mode::Track) == QStringLiteral("track"));
    CHECK(ReplayGain::idForMode(Mode::Album) == QStringLiteral("album"));
    CHECK(ReplayGain::modeFromId(QStringLiteral("off"))   == Mode::Off);
    CHECK(ReplayGain::modeFromId(QStringLiteral("track")) == Mode::Track);
    CHECK(ReplayGain::modeFromId(QStringLiteral("album")) == Mode::Album);
    // Case and whitespace are tolerated — a hand-edited ini should not silently change behaviour.
    CHECK(ReplayGain::modeFromId(QStringLiteral(" Album ")) == Mode::Album);
    CHECK(ReplayGain::modeFromId(QStringLiteral("TRACK"))   == Mode::Track);
    // Absent / unknown resolves to the SHIPPED DEFAULT, never to Off: a value from a newer build, an empty
    // string or corruption must degrade to what the app ships with, not to "levelling silently stopped".
    CHECK(ReplayGain::defaultMode() == Mode::Album);
    CHECK(ReplayGain::modeFromId(QString())                  == Mode::Album);
    CHECK(ReplayGain::modeFromId(QStringLiteral("loudness")) == Mode::Album);
}

static void testPreampClamp()
{
    CHECK(ReplayGain::defaultPreampDb() == 0.0);
    CHECK(ReplayGain::minPreampDb() == -15.0);
    CHECK(ReplayGain::maxPreampDb() ==  15.0);
    CHECK(ReplayGain::clampPreamp(0.0)    ==  0.0);
    CHECK(ReplayGain::clampPreamp(6.0)    ==  6.0);
    CHECK(ReplayGain::clampPreamp(-6.0)   == -6.0);
    CHECK(ReplayGain::clampPreamp(15.0)   == 15.0);   // the band is inclusive at both ends
    CHECK(ReplayGain::clampPreamp(-15.0)  == -15.0);
    CHECK(ReplayGain::clampPreamp(150.0)  == 15.0);   // mpv would accept +150 dB; a user must never reach it
    CHECK(ReplayGain::clampPreamp(-150.0) == -15.0);
    // NaN out of a corrupt ini is garbage, not a quiet zero — it must not propagate into an mpv option string.
    CHECK(ReplayGain::clampPreamp(std::nan("")) == 0.0);
}

static void testMusicCarveOut()
{
    // An audiobook / podcast / video (isMusic false) is NEVER levelled, whatever the setting says and however
    // well tagged the file happens to be. This is the #140 carve-out, applied before anything else.
    EXPECT_MODE(Album, false, tagged(-7.0), tagged(-6.0), Off);
    EXPECT_MODE(Track, false, tagged(-7.0), tagged(-6.0), Off);
    EXPECT_MODE(Off,   false, tagged(-7.0), tagged(-6.0), Off);
    // ... and music with the same tags is levelled, so the carve-out is the thing making the difference and
    // not some other reason the answer happened to be Off.
    EXPECT_MODE(Album, true, tagged(-7.0), tagged(-6.0), Album);
    EXPECT_MODE(Track, true, tagged(-7.0), tagged(-6.0), Track);
}

static void testUserOff()
{
    // "Off" is a real choice: a user with a hand-levelled library gets nothing touched, tags or no tags.
    EXPECT_MODE(Off, true, tagged(-7.0), tagged(-6.0), Off);
    EXPECT_MODE(Off, true, absent(),     absent(),     Off);
}

static void testUntaggedPlaysUnmodified()
{
    // No gain tags at all -> Off in every mode. Nothing is analysed and nothing is invented.
    EXPECT_MODE(Album, true, absent(), absent(), Off);
    EXPECT_MODE(Track, true, absent(), absent(), Off);
    EXPECT_MODE(Off,   true, absent(), absent(), Off);
}

static void testPresenceIsNotValue()
{
    // A track tagged "0.00 dB" IS tagged — it is a track the tagger measured and found already at reference.
    // Treating it as untagged (the mistake a `gain != 0` test makes) would drop it out of levelling entirely,
    // which is worse than it sounds: on an album where every other track is being pulled down, the one at 0
    // would be the only one left loud. AudioTags::GainValue::present exists for exactly this.
    EXPECT_MODE(Album, true, taggedZero(), taggedZero(), Album);
    EXPECT_MODE(Track, true, taggedZero(), taggedZero(), Track);
    // And a zero on ONE side still counts as that side being present, so no fallback fires.
    EXPECT_MODE(Album, true, tagged(-7.0), taggedZero(), Album);
    EXPECT_MODE(Track, true, taggedZero(), tagged(-6.0), Track);

    // The option set proves it end to end: an all-zero-tagged album still asks mpv for album mode, not "no".
    const auto opts = ReplayGain::toMpvOptions(Mode::Album, true, 0.0, taggedZero(), taggedZero());
    EXPECT_OPT(opts, "replaygain", "album");
}

static void testOneSidedTagFallback()
{
    // A single ripped without album tags is still worth levelling: Album falls back to the track gain.
    EXPECT_MODE(Album, true, tagged(-7.0), absent(), Track);
    // The mirror: a file carrying only an album gain still levels when the user asked for per-track.
    EXPECT_MODE(Track, true, absent(), tagged(-6.0), Album);
    // The carve-out still wins over both fallbacks — a one-sided-tagged audiobook is still not levelled.
    EXPECT_MODE(Album, false, tagged(-7.0), absent(), Off);
}

static void testOptionSet()
{
    // The full, ordinary case: a music track with both tags, per album, +3 dB preamp.
    const auto album = ReplayGain::toMpvOptions(Mode::Album, true, 3.0, tagged(-7.0), tagged(-6.0));
    CHECK(album.size() == 4);                      // all four, every time — nothing is left to a stale value
    EXPECT_OPT(album, "replaygain",          "album");
    EXPECT_OPT(album, "replaygain-preamp",   "3");
    EXPECT_OPT(album, "replaygain-clip",     "yes");
    EXPECT_OPT(album, "replaygain-fallback", "0");

    // Per track, negative preamp.
    const auto track = ReplayGain::toMpvOptions(Mode::Track, true, -4.5, tagged(-7.0), tagged(-6.0));
    EXPECT_OPT(track, "replaygain",        "track");
    EXPECT_OPT(track, "replaygain-preamp", "-4.5");

    // Out-of-band preamp is clamped before it ever reaches mpv.
    const auto hot = ReplayGain::toMpvOptions(Mode::Album, true, 99.0, tagged(-7.0), tagged(-6.0));
    EXPECT_OPT(hot, "replaygain-preamp", "15");

    // OFF is a full RESET, not an absence: mpv's own defaults get written back, including a preamp of 0 even
    // though the user has one configured. Otherwise an audiobook after an album would keep the album's boost.
    const auto book = ReplayGain::toMpvOptions(Mode::Album, false, 6.0, tagged(-7.0), tagged(-6.0));
    CHECK(book.size() == 4);
    EXPECT_OPT(book, "replaygain",          "no");
    EXPECT_OPT(book, "replaygain-preamp",   "0");
    EXPECT_OPT(book, "replaygain-clip",     "yes");
    EXPECT_OPT(book, "replaygain-fallback", "0");

    // An untagged music file is the same full reset — the preamp does not leak onto it either.
    const auto untagged = ReplayGain::toMpvOptions(Mode::Album, true, 6.0, absent(), absent());
    EXPECT_OPT(untagged, "replaygain",        "no");
    EXPECT_OPT(untagged, "replaygain-preamp", "0");
}

static void testInvariantsOverWholeMatrix()
{
    // Two properties must hold in EVERY cell, because each is a way the feature could quietly become something
    // the issue forbids:
    //   * replaygain-clip == "yes" — clipping prevention is on, always, and is not a setting;
    //   * replaygain-fallback == "0" — mpv's "apply this gain to files with NO tags" knob stays disabled, which
    //     is what "untagged material plays unmodified" means once mpv is the one doing the work.
    const Mode modes[] = { Mode::Off, Mode::Track, Mode::Album };
    const Gain gains[] = { absent(), taggedZero(), tagged(-9.5) };
    const double preamps[] = { -20.0, -3.0, 0.0, 3.0, 20.0 };
    int cells = 0;
    for (Mode m : modes)
        for (bool music : { false, true })
            for (const Gain& tg : gains)
                for (const Gain& ag : gains)
                    for (double pre : preamps)
                    {
                        const auto opts = ReplayGain::toMpvOptions(m, music, pre, tg, ag);
                        ++cells;
                        if (opts.size() != 4) { std::fprintf(stderr, "REPLAYGAIN-FAIL matrix cell emitted %d options\n", int(opts.size())); ++failures; continue; }
                        EXPECT_OPT(opts, "replaygain-clip",     "yes");
                        EXPECT_OPT(opts, "replaygain-fallback", "0");
                        // And a non-music cell is Off in every one of them.
                        if (!music) EXPECT_OPT(opts, "replaygain", "no");
                    }
    CHECK(cells == 3 * 2 * 3 * 3 * 5);   // the matrix was actually walked, not silently empty
}

static void testSettings()
{
    // The SHIPPED default read from an empty ini: ALBUM. (Absent key -> modeFromId("") -> the default.)
    CHECK(Settings::replayGainMode() == Mode::Album);
    CHECK(Settings::replayGainPreamp() == 0.0);

    // All three modes round-trip, and each is distinguishable from the others.
    Settings::setReplayGainMode(Mode::Off);
    CHECK(Settings::replayGainMode() == Mode::Off);
    Settings::setReplayGainMode(Mode::Track);
    CHECK(Settings::replayGainMode() == Mode::Track);
    Settings::setReplayGainMode(Mode::Album);
    CHECK(Settings::replayGainMode() == Mode::Album);

    // The preamp round-trips and clamps on WRITE as well as read (house style, cf. defaultPlaybackSpeed).
    Settings::setReplayGainPreamp(4.0);
    CHECK(Settings::replayGainPreamp() == 4.0);
    Settings::setReplayGainPreamp(-4.0);
    CHECK(Settings::replayGainPreamp() == -4.0);
    Settings::setReplayGainPreamp(500.0);
    CHECK(Settings::replayGainPreamp() == 15.0);
    Settings::setReplayGainPreamp(-500.0);
    CHECK(Settings::replayGainPreamp() == -15.0);

    // Clamped on WRITE as well, asserted against what is actually PERSISTED rather than against the read path.
    // A read-side clamp alone would satisfy the two checks above while leaving "+500" sitting in the ini for
    // every other reader to find and believe — a later build, a support dump, or the user's own text editor.
    Settings::setReplayGainPreamp(500.0);
    {
        QSettings ini(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                      QSettings::IniFormat);
        CHECK(ini.value(QStringLiteral("playback/replayGainPreamp")).toDouble() == 15.0);
    }
    Settings::setReplayGainPreamp(0.0);

    // End-to-end: the stored setting drives the mpv option the built-in player would request for a tagged
    // music track, and the SAME stored setting yields the reset for an audiobook.
    Settings::setReplayGainMode(Mode::Album);
    Settings::setReplayGainPreamp(2.0);
    const auto music = ReplayGain::toMpvOptions(Settings::replayGainMode(), true, Settings::replayGainPreamp(),
                                                tagged(-8.0), tagged(-7.0));
    EXPECT_OPT(music, "replaygain",        "album");
    EXPECT_OPT(music, "replaygain-preamp", "2");
    const auto book = ReplayGain::toMpvOptions(Settings::replayGainMode(), false, Settings::replayGainPreamp(),
                                               tagged(-8.0), tagged(-7.0));
    EXPECT_OPT(book, "replaygain",        "no");
    EXPECT_OPT(book, "replaygain-preamp", "0");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testIdRoundTrip();
    testPreampClamp();
    testMusicCarveOut();
    testUserOff();
    testUntaggedPlaysUnmodified();
    testPresenceIsNotValue();
    testOneSidedTagFallback();
    testOptionSet();
    testInvariantsOverWholeMatrix();
    testSettings();
    if (failures == 0) std::printf("REPLAYGAIN-OK\n");
    return failures == 0 ? 0 : 1;
}
