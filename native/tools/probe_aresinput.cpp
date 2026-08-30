// Headless check of the pure ares input model (src/core/AresInput.h) — the translation from a pad's SDL
// gamepad mapping string into the ares settings.bml bindings EmulatorManager seeds on first launch. ares
// ships with NO input bindings at all (no auto-map, no keyboard default, no first-run assignment anywhere in
// desktop-ui/input/), so without this seed a fresh install boots N64 games that cannot be controlled.
//
// Qt6::Core only, no SDL, no disk. Prints ARESINPUT-OK on success; any failure prints
// ARESINPUT-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE REAL AND HAND-COMPUTED: the three mapping strings below are copied verbatim from the repo's
// own native/gamecontrollerdb.txt, and every expected assignment is derived by hand from the documented ares
// rules (group ids Axis=0 Hat=1 Trigger=2 Button=3; hat H -> inputs 2H (X, LEFT=Lo/RIGHT=Hi) and 2H+1
// (Y, UP=Lo/DOWN=Hi)) — never by re-running the function under test.
#include "AresInput.h"
#include "ControllerSeats.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "ARESINPUT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Real lines from native/gamecontrollerdb.txt.
// Hat d-pad, AXIS triggers (lefttrigger:a3, righttrigger:a4).
static const char* kPs5 =
    "030000004c050000e60c000000000000,PS5 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,misc1:b14,"
    "rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,"
    "platform:Windows,";
// BUTTON d-pad, BUTTON triggers (dpup:b10, lefttrigger:b8).
static const char* k4Play =
    "03000000d0160000040d000000000000,4Play Adapter,a:b1,b:b3,back:b4,dpdown:b11,dpleft:b12,dpright:b13,"
    "dpup:b10,leftshoulder:b6,leftstick:b14,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b15,"
    "righttrigger:b9,rightx:a3,righty:a4,start:b5,x:b0,y:b2,platform:Windows,";
// Deliberately incomplete: no right stick, no triggers, no stick clicks.
static const char* kPartial =
    "03000000ffff0000ffff000000000000,Partial Pad,a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,platform:Windows,";

static QString valueFor(const QVector<AresInput::Binding>& bs, const char* key)
{
    for (const AresInput::Binding& b : bs) if (b.key == QLatin1String(key)) return b.value;
    return QString();
}
static bool has(const QVector<AresInput::Binding>& bs, const char* key)
{
    for (const AresInput::Binding& b : bs) if (b.key == QLatin1String(key)) return true;
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. Hat d-pad + axis triggers (PS5). ------------------------------------------------------------
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("030000004c050000e60c000000000000");
        p.name = QStringLiteral("PS5 Controller");
        p.sdlMapping = QLatin1String(kPs5);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p);
        const QString g = p.guid + QStringLiteral("/0/");

        // Face buttons: SDL a:b1 -> ares "A..South" on Button (group 3) index 1.
        CHECK(valueFor(b, "A..South") == g + QStringLiteral("3/1"));
        CHECK(valueFor(b, "B..East")  == g + QStringLiteral("3/2"));
        CHECK(valueFor(b, "X..West")  == g + QStringLiteral("3/0"));
        CHECK(valueFor(b, "Y..North") == g + QStringLiteral("3/3"));
        CHECK(valueFor(b, "Select")   == g + QStringLiteral("3/8"));
        CHECK(valueFor(b, "Start")    == g + QStringLiteral("3/9"));
        CHECK(valueFor(b, "L-Bumper") == g + QStringLiteral("3/4"));
        CHECK(valueFor(b, "R-Bumper") == g + QStringLiteral("3/5"));
        CHECK(valueFor(b, "L-Stick..Click") == g + QStringLiteral("3/10"));
        CHECK(valueFor(b, "R-Stick..Click") == g + QStringLiteral("3/11"));

        // D-pad on hat 0: X is ares hat input 0 (LEFT=Lo, RIGHT=Hi), Y is input 1 (UP=Lo, DOWN=Hi).
        CHECK(valueFor(b, "Pad.Up")    == g + QStringLiteral("1/1/Lo"));
        CHECK(valueFor(b, "Pad.Down")  == g + QStringLiteral("1/1/Hi"));
        CHECK(valueFor(b, "Pad.Left")  == g + QStringLiteral("1/0/Lo"));
        CHECK(valueFor(b, "Pad.Right") == g + QStringLiteral("1/0/Hi"));

        // Axis triggers: a digital control bound to an axis takes the Hi half.
        CHECK(valueFor(b, "L-Trigger") == g + QStringLiteral("0/3/Hi"));
        CHECK(valueFor(b, "R-Trigger") == g + QStringLiteral("0/4/Hi"));

        // Sticks: SDL X negative = left, Y negative = up.
        CHECK(valueFor(b, "L-Left")  == g + QStringLiteral("0/0/Lo"));
        CHECK(valueFor(b, "L-Right") == g + QStringLiteral("0/0/Hi"));
        CHECK(valueFor(b, "L-Up")    == g + QStringLiteral("0/1/Lo"));
        CHECK(valueFor(b, "L-Down")  == g + QStringLiteral("0/1/Hi"));
        CHECK(valueFor(b, "R-Left")  == g + QStringLiteral("0/2/Lo"));
        CHECK(valueFor(b, "R-Right") == g + QStringLiteral("0/2/Hi"));
        CHECK(valueFor(b, "R-Up")    == g + QStringLiteral("0/5/Lo"));
        CHECK(valueFor(b, "R-Down")  == g + QStringLiteral("0/5/Hi"));
    }

    // ---- 2. Button d-pad + button triggers (4Play). Proves indices come from the mapping string, not an
    //         assumed layout: this pad's A is b1 and its X is b0, the opposite way round from an Xbox pad.
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("03000000d0160000040d000000000000");
        p.sdlMapping = QLatin1String(k4Play);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p);
        const QString g = p.guid + QStringLiteral("/0/");

        CHECK(valueFor(b, "Pad.Up")    == g + QStringLiteral("3/10"));
        CHECK(valueFor(b, "Pad.Down")  == g + QStringLiteral("3/11"));
        CHECK(valueFor(b, "Pad.Left")  == g + QStringLiteral("3/12"));
        CHECK(valueFor(b, "Pad.Right") == g + QStringLiteral("3/13"));
        CHECK(valueFor(b, "L-Trigger") == g + QStringLiteral("3/8"));
        CHECK(valueFor(b, "R-Trigger") == g + QStringLiteral("3/9"));
        CHECK(valueFor(b, "R-Up")      == g + QStringLiteral("0/4/Lo"));
        CHECK(valueFor(b, "R-Right")   == g + QStringLiteral("0/3/Hi"));
    }

    // ---- 3. A pad missing controls: those keys are OMITTED, never guessed. ------------------------------
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("03000000ffff0000ffff000000000000");
        p.sdlMapping = QLatin1String(kPartial);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p);
        CHECK(has(b, "Pad.Up"));
        CHECK(has(b, "L-Up"));
        CHECK(!has(b, "R-Up"));
        CHECK(!has(b, "R-Down"));
        CHECK(!has(b, "L-Trigger"));
        CHECK(!has(b, "R-Trigger"));
        CHECK(!has(b, "L-Stick..Click"));
        // An empty mapping string yields NOTHING at all — degrade, never guess.
        ControllerSeats::PadInfo none;
        none.guid = QStringLiteral("03000000ffff0000ffff000000000000");
        CHECK(AresInput::bindingsFor(none).isEmpty());
    }

    // ---- 4. settingsBml: BML shape (two-space indent, "name: value", LF) and VirtualPad numbering. ------
    {
        ControllerSeats::PadInfo p1;
        p1.index = 0;
        p1.guid = QStringLiteral("030000004c050000e60c000000000000");
        p1.sdlMapping = QLatin1String(kPs5);
        ControllerSeats::PadInfo p2;
        p2.index = 1;
        p2.guid = QStringLiteral("03000000d0160000040d000000000000");
        p2.sdlMapping = QLatin1String(k4Play);
        const QVector<ControllerSeats::Seat> seats = ControllerSeats::assignSeats({ p1, p2 });
        CHECK(seats.size() == 2);
        const QByteArray bml = AresInput::settingsBml(seats);

        CHECK(bml.contains("VirtualPad1\n"));
        CHECK(bml.contains("VirtualPad2\n"));
        CHECK(bml.contains("  A..South: 030000004c050000e60c000000000000/0/3/1\n"));
        CHECK(bml.contains("  Pad.Up: 030000004c050000e60c000000000000/0/1/1/Lo\n"));
        CHECK(bml.contains("  A..South: 03000000d0160000040d000000000000/0/3/1\n"));
        CHECK(!bml.contains("\r"));                       // LF only, as BML::serialize writes
        CHECK(!bml.contains("VirtualPad3"));              // only seated pads are written
        // No pads at all -> nothing to write, so the caller seeds no file.
        CHECK(AresInput::settingsBml({}).isEmpty());
    }

    // ---- 5. needsSeed: only a file with NO VirtualPad assignment is seeded. -----------------------------
    {
        CHECK(AresInput::needsSeed(QByteArray()));                       // absent / empty file
        CHECK(AresInput::needsSeed("Video\n  Driver: Direct3D 11\n"));   // ares ran, never mapped
        // ares writes every VirtualPad key with an empty value when nothing is assigned.
        CHECK(AresInput::needsSeed("VirtualPad1\n  Pad.Up:\n  A..South:\n"));
        // A user's own mapping is never touched.
        CHECK(!AresInput::needsSeed("VirtualPad1\n  Pad.Up: 0300/0/1/1/Lo\n"));
        CHECK(!AresInput::needsSeed("Video\n  Driver: Direct3D 11\nVirtualPad2\n  Start: 0300/0/3/9\n"));
    }

    // ---- 6. mergeSettingsBml: exactly ONE VirtualPadN block survives. ares resolves a settings path with
    //         _find(path)[0] — the FIRST match — so a seed appended after ares' own empty block would be
    //         silently ignored on load AND overwritten on save. Every pre-existing VirtualPad block must go.
    {
        const QByteArray seed = "VirtualPad1\n  Pad.Up: 0300/0/1/1/Lo\n";

        // Absent file: the seed IS the file.
        CHECK(AresInput::mergeSettingsBml(QByteArray(), seed) == seed);

        // ares ran once and wrote an unmapped file: its VirtualPad block is replaced, not duplicated, and
        // every non-pad setting it wrote survives untouched.
        const QByteArray existing =
            "Video\n"
            "  Driver: Direct3D 11\n"
            "VirtualPad1\n"
            "  Pad.Up:\n"
            "  A..South:\n"
            "Audio\n"
            "  Driver: WASAPI\n";
        const QByteArray merged = AresInput::mergeSettingsBml(existing, seed);
        CHECK(merged.count("VirtualPad1") == 1);
        CHECK(merged.contains("  Pad.Up: 0300/0/1/1/Lo\n"));
        CHECK(!merged.contains("  A..South:\n"));           // the stale empty key is gone with its block
        CHECK(merged.contains("Video\n  Driver: Direct3D 11\n"));
        CHECK(merged.contains("Audio\n  Driver: WASAPI\n"));

        // A stale block for a pad we are NOT seeding this time is also removed, so a settings.bml cannot
        // accumulate bindings for a controller that is no longer attached.
        const QByteArray twoPads =
            "VirtualPad1\n  Pad.Up:\n"
            "VirtualPad2\n  Pad.Up:\n"
            "Video\n  Driver: Direct3D 11\n";
        const QByteArray merged2 = AresInput::mergeSettingsBml(twoPads, seed);
        CHECK(!merged2.contains("VirtualPad2"));
        CHECK(merged2.count("VirtualPad1") == 1);
        CHECK(merged2.contains("Video\n  Driver: Direct3D 11\n"));
    }

    if (failures == 0) std::printf("ARESINPUT-OK\n");
    else               std::fprintf(stderr, "ARESINPUT: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
