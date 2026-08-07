// Headless check of audio output options (issue #69): the pure Output -> mpv option map (src/video/AudioOutput.h),
// the Settings defaults + round-trip that feed it, and — the correctness crux of this issue — that the audio keys
// are classified DEVICE-LOCAL by CloudSync's carve-out, NOT synced. QtCore-only for the map + Settings; the
// classification is asserted against the REAL CloudSync::isDeviceLocalKey (linked in), never re-derived here.
//
// Every expected mpv option value below is HAND-WRITTEN from the design, NOT recomputed by calling the mapping a
// second time — the point is to pin the map against an independent statement of what each field should emit. The
// three things most worth getting wrong, all pinned here:
//   * the AUTO device case: an empty stored device id must map to "auto" (mpv's own auto-select), never to an
//     empty audio-device that would leave mpv with no output selected.
//   * the PASSTHROUGH string: on => exactly "ac3,eac3,dts,dts-hd,truehd" on audio-spdif; off => an EMPTY string
//     (mpv's default, spdif disabled), emitted unconditionally so a live re-apply after turning it off clears it.
//   * the DEVICE-LOCAL classification: audio/device, audio/passthrough and audio/exclusive are device-local (an
//     audio-device id is meaningless on another machine), so they must NOT ride the synced settings bundle —
//     the exact OPPOSITE of #71's subs/* keys, which DO sync. Getting this backwards would point device B's
//     player at device A's sound card.
//
// Prints AUDIOOUT-OK on success; any failure prints AUDIOOUT-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini the
// Settings half reads starts empty and is removed at exit — the defaults it asserts are a genuine absence.
#include "AudioOutput.h"
#include "Settings.h"
#include "CloudSync.h"   // the REAL device-local carve-out (isDeviceLocalKey) — asserted, not re-derived

#include <QCoreApplication>
#include <QVector>
#include <QPair>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "AUDIOOUT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Look a single option name up in the emitted pair list. Returns the value, or a sentinel that can never be a
// real value so "missing" and "wrong" are distinguishable. (audio-spdif off is a real EMPTY value, so absence
// must not read as empty — hence the distinct <ABSENT> sentinel.)
static QString optValue(const QVector<QPair<QString, QString>>& opts, const QString& name)
{
    for (const auto& o : opts) if (o.first == name) return o.second;
    return QStringLiteral("<ABSENT>");
}

// Assert an option maps to an exact, hand-written value for a given Output.
#define EXPECT_OPT(opts, name, want) do { \
    const QString got = optValue(opts, QStringLiteral(name)); \
    if (got != QStringLiteral(want)) { \
        std::fprintf(stderr, "AUDIOOUT-FAIL %s -> got '%s' want '%s' (line %d)\n", \
                     name, got.toUtf8().constData(), want, __LINE__); \
        ++failures; \
    } \
} while (0)

// The factory-default Output (mirrors mpv's own no-options-set behaviour) maps to exactly these strings.
static void testDefaults()
{
    AudioOutput::Output o;   // all defaults
    const auto m = AudioOutput::toMpvOptions(o);
    EXPECT_OPT(m, "audio-device",    "auto");   // empty stored id -> mpv's auto-select
    EXPECT_OPT(m, "audio-spdif",     "");        // passthrough off -> empty, i.e. spdif disabled
    EXPECT_OPT(m, "audio-exclusive", "no");      // shared mode
    // audio-spdif is present-but-empty when off, not absent — the reset-on-reapply guarantee depends on it.
    CHECK(optValue(m, QStringLiteral("audio-spdif")) != QStringLiteral("<ABSENT>"));
}

// Each non-default field maps through to its exact mpv value.
static void testFieldMapping()
{
    AudioOutput::Output dev;   dev.device = QStringLiteral("wasapi/{0.0.0.00000000}.{abc}");
    EXPECT_OPT(AudioOutput::toMpvOptions(dev), "audio-device", "wasapi/{0.0.0.00000000}.{abc}");

    AudioOutput::Output pass;  pass.passthrough = true;
    EXPECT_OPT(AudioOutput::toMpvOptions(pass), "audio-spdif", "ac3,eac3,dts,dts-hd,truehd");

    AudioOutput::Output excl;  excl.exclusive = true;
    EXPECT_OPT(AudioOutput::toMpvOptions(excl), "audio-exclusive", "yes");

    // Everything at once, and the passthrough string is exactly the documented list (independently of the
    // helper — the hand-written expectation is the oracle).
    AudioOutput::Output all; all.device = QStringLiteral("coreaudio/BuiltInSpeaker");
    all.passthrough = true; all.exclusive = true;
    const auto m = AudioOutput::toMpvOptions(all);
    EXPECT_OPT(m, "audio-device",    "coreaudio/BuiltInSpeaker");
    EXPECT_OPT(m, "audio-spdif",     "ac3,eac3,dts,dts-hd,truehd");
    EXPECT_OPT(m, "audio-exclusive", "yes");
}

// The Auto case and the empty-when-off passthrough case, pinned explicitly (they are the two the issue calls out).
static void testAutoAndOff()
{
    AudioOutput::Output empty;  // device "" -> "auto"
    EXPECT_OPT(AudioOutput::toMpvOptions(empty), "audio-device", "auto");

    AudioOutput::Output spaces; spaces.device = QStringLiteral("   ");   // a hand-edited blank must still be Auto
    EXPECT_OPT(AudioOutput::toMpvOptions(spaces), "audio-device", "auto");

    // A stored id with stray surrounding whitespace is trimmed, not turned into a device nobody has.
    AudioOutput::Output padded; padded.device = QStringLiteral("  pulse/alsa_output.pci  ");
    EXPECT_OPT(AudioOutput::toMpvOptions(padded), "audio-device", "pulse/alsa_output.pci");

    // Passthrough off => audio-spdif is exactly empty, never the codec list.
    AudioOutput::Output off; off.passthrough = false;
    EXPECT_OPT(AudioOutput::toMpvOptions(off), "audio-spdif", "");
    CHECK(optValue(AudioOutput::toMpvOptions(off), QStringLiteral("audio-spdif"))
          != AudioOutput::passthroughCodecs());
}

// The Settings half: an empty ini reads the mpv-matching defaults, values round-trip, and audioOutput() gathers
// them so the end-to-end (stored -> Output -> mpv options) agrees with a direct mapping.
static void testSettings()
{
    CHECK(Settings::audioDevice().isEmpty());          // Auto by default (no stored id)
    CHECK(Settings::audioPassthrough() == false);      // decode to PCM by default
    CHECK(Settings::audioExclusive() == false);        // shared mode by default

    // The gathered default Output maps to the mpv defaults (the same strings testDefaults pinned).
    {
        const AudioOutput::Output d = Settings::audioOutput();
        const auto m = AudioOutput::toMpvOptions(d);
        EXPECT_OPT(m, "audio-device",    "auto");
        EXPECT_OPT(m, "audio-spdif",     "");
        EXPECT_OPT(m, "audio-exclusive", "no");
    }

    Settings::setAudioDevice(QStringLiteral("wasapi/RECEIVER"));
    Settings::setAudioPassthrough(true);
    Settings::setAudioExclusive(true);

    CHECK(Settings::audioDevice() == QStringLiteral("wasapi/RECEIVER"));
    CHECK(Settings::audioPassthrough() == true);
    CHECK(Settings::audioExclusive() == true);

    const AudioOutput::Output o = Settings::audioOutput();
    CHECK(o.device == QStringLiteral("wasapi/RECEIVER"));
    CHECK(o.passthrough == true);
    CHECK(o.exclusive == true);

    const auto m = AudioOutput::toMpvOptions(o);
    EXPECT_OPT(m, "audio-device",    "wasapi/RECEIVER");
    EXPECT_OPT(m, "audio-spdif",     "ac3,eac3,dts,dts-hd,truehd");
    EXPECT_OPT(m, "audio-exclusive", "yes");

    // The setter trims, so a hand-typed device with surrounding whitespace does not persist a phantom space.
    Settings::setAudioDevice(QStringLiteral("  trimmed-id  "));
    CHECK(Settings::audioDevice() == QStringLiteral("trimmed-id"));
}

// The correctness crux: the audio keys are DEVICE-LOCAL (excluded from sync), the exact opposite of subs/* which
// syncs. Asserted against the REAL CloudSync carve-out, not a copy of the rule.
static void testDeviceLocal()
{
    // Every audio/* key is device-local: an audio-device id names a sound card on THIS machine, and passthrough
    // / exclusive depend on what THIS box is wired to.
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("audio/device")) == true);
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("audio/passthrough")) == true);
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("audio/exclusive")) == true);

    // The deliberate contrast (issue #71): a subtitle-look key SYNCS — a chosen look follows the account across
    // devices. If this ever reads true, the two groups have been conflated.
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("subs/font")) == false);
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("subs/assOverride")) == false);

    // The prefix is exactly "audio/" WITH the slash: a key that merely starts with the letters "audio" but is a
    // different group ("audiobook/…") must NOT be swept into the carve-out.
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("audiobook/lastPos")) == false);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDefaults();
    testFieldMapping();
    testAutoAndOff();
    testSettings();
    testDeviceLocal();
    if (failures == 0) std::printf("AUDIOOUT-OK\n");
    return failures == 0 ? 0 : 1;
}
