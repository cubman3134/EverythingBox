// Headless check of the pure ares input model (src/core/AresInput.h) — the translation from a pad's SDL
// gamepad mapping string into the ares settings.bml bindings EmulatorManager seeds on first launch. ares
// ships with NO input bindings at all (no auto-map, no keyboard default, no first-run assignment anywhere in
// desktop-ui/input/), so without this seed a fresh install boots N64 games that cannot be controlled.
//
// Qt6::Core only, no SDL, no disk. Prints ARESINPUT-OK on success; any failure prints
// ARESINPUT-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE REAL AND HAND-COMPUTED: every mapping string below except kCombined, kPartial and kNoName is
// copied verbatim from the repo's own native/gamecontrollerdb.txt (the three exceptions are hand-authored
// because no shipped line has that shape), and every expected assignment is derived by hand from the ares
// rules — never by re-running the function under test. The rules, in full:
//   * an assignment is "<guid>/<slot>/<group>/<input>[/<qualifier>]", groups Axis=0 Hat=1 Trigger=2 Button=3;
//   * hat H -> inputs 2H (X, LEFT=Lo/RIGHT=Hi) and 2H+1 (Y, UP=Lo/DOWN=Hi);
//   * an axis-backed control is Lo when exactly one of (SDL "-" half-axis, SDL "~" inversion) holds, else Hi;
//     for a stick direction the "negative" direction (left/up) plays the role of the "-" half;
//   * slot is the pad's ordinal among EARLIER seats reporting the same GUID.
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
// HALF-AXIS d-pad (dpup:-a1,dpdown:+a1,dpleft:-a0,dpright:+a0) — the shape 64 of the shipped db's
// platform:Windows entries use. Emitting Hi for all four leaves up and left dead and makes down fire up and
// down at once, which is why the half-axis sign must survive onto the qualifier.
static const char* kNes30 =
    "030000003512000012ab000000000000,8BitDo NES30,a:b2,b:b1,back:b6,dpdown:+a1,dpleft:-a0,dpright:+a0,"
    "dpup:-a1,leftshoulder:b4,rightshoulder:b5,start:b7,x:b3,y:b0,platform:Windows,";
// INVERTED axis triggers (lefttrigger:a3~, righttrigger:a4~): they rest at +32767 and FALL when pressed, so
// reading them as Hi would be "permanently held from boot".
static const char* kPs3 =
    "030000004c0500006802000000000000,PS3 Controller,a:b2,b:b1,back:b9,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b10,lefttrigger:a3~,leftx:a0,lefty:a1,rightshoulder:b7,"
    "rightstick:b11,righttrigger:a4~,rightx:a2,righty:a5,start:b8,x:b3,y:b0,platform:Windows,";
// INVERTED stick axes (lefty:a1~, righty:a3~): pushing up drives the raw axis POSITIVE, so up is Hi.
static const char* kSwitch2 =
    "030000007e0500006920000000000000,Nintendo Switch 2 Pro Controller,a:b0,b:b1,back:b14,dpdown:b8,"
    "dpleft:b10,dpright:b9,dpup:b11,guide:b16,leftshoulder:b12,leftstick:b15,lefttrigger:b13,leftx:a0,"
    "lefty:a1~,misc1:b17,misc2:b20,paddle1:b18,paddle2:b19,rightshoulder:b4,rightstick:b7,righttrigger:b5,"
    "rightx:a2,righty:a3~,start:b6,x:b2,y:b3,platform:Windows,";
// Two identical Xbox 360 pads — the commonest 2-player couch setup — report the SAME GUID and are told apart
// by ares ONLY by the slot term.
static const char* kX360 =
    "03000000380700001647000000000000,Xbox 360 Controller,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,"
    "dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b8,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,"
    "rightstick:b9,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,platform:Windows,";
// HAND-AUTHORED: no shipped line carries BOTH decorations on one value, but SDL's parser composes them (the
// half-axis narrows the range, "~" swaps its ends), so "-a1~" presses Hi and "+a1~" presses Lo — the exact
// inverse of kNes30's d-pad.
static const char* kCombined =
    "03000000ffff0000aaaa000000000000,Combined Decoration Pad,a:b0,dpdown:+a1~,dpleft:-a0,dpright:+a0,"
    "dpup:-a1~,lefttrigger:a2~,leftx:a0,lefty:a1~,platform:Windows,";
// HAND-AUTHORED: an EMPTY device-name field, which SDL permits ("<guid>,,a:b0,…"). The first key:value pair
// therefore sits where a named pad's name sits, so a split that drops empty fields would swallow "a:b0".
static const char* kNoName =
    "03000000ffff0000cccc000000000000,,a:b0,b:b1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftx:a0,"
    "lefty:a1,platform:Windows,";
// HAND-AUTHORED, deliberately incomplete: no right stick, no triggers, no stick clicks.
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
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p, 0);
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

        // Axis triggers, undecorated: they rest low and rise, so the digital control takes the Hi half.
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
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p, 0);
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
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p, 0);
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
        CHECK(AresInput::bindingsFor(none, 0).isEmpty());
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
        // THREE SLOTS, one per identity variant. EverythingBox links SDL2 and ares v148 links SDL3, and the
        // two libraries disagree about the GUID's leading BUS field for the same physical pad: SDL3's Windows
        // HIDAPI backend reports a Bluetooth DualSense as BUS_BLUETOOTH (0x05) where SDL2 says BUS_USB (0x03),
        // so an SDL2-keyed binding addresses a device ares never sees and the pad is dead. ares gives every
        // input three binding slots and ORs them, so seed the SDL2 identity plus its bus variants and let
        // whichever matches the live device win; the others are inert. (Measured on a real DualSense: SDL2
        // 030057564c05...6800 vs SDL3 050057564c05...6800 — identical but for those two leading bytes.)
        CHECK(bml.contains("  A..South: 030000004c050000e60c000000000000/0/3/1"
                           ";050000004c050000e60c000000000000/0/3/1;\n"));
        CHECK(bml.contains("  Pad.Up: 030000004c050000e60c000000000000/0/1/1/Lo"
                           ";050000004c050000e60c000000000000/0/1/1/Lo;\n"));
        CHECK(bml.contains("  A..South: 03000000d0160000040d000000000000/0/3/1"
                           ";05000000d0160000040d000000000000/0/3/1;\n"));
        CHECK(!bml.contains("\r"));                       // LF only, as BML::serialize writes
        CHECK(!bml.contains("VirtualPad3"));              // only seated pads are written
        // No pads at all -> nothing to write, so the caller seeds no file.
        CHECK(AresInput::settingsBml({}).isEmpty());
    }

    // ---- 5. HALF-AXIS d-pad (8BitDo NES30). The SDL "-"/"+" prefix says WHICH END of the raw axis presses
    //         the control, so up/left are Lo and down/right are Hi. Reading every axis as Hi (the pre-fix
    //         behaviour) left up and left dead and made down press up and down simultaneously.
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("030000003512000012ab000000000000");
        p.name = QStringLiteral("8BitDo NES30");
        p.sdlMapping = QLatin1String(kNes30);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p, 0);
        const QString g = p.guid + QStringLiteral("/0/");

        CHECK(valueFor(b, "Pad.Up")    == g + QStringLiteral("0/1/Lo"));   // dpup:-a1
        CHECK(valueFor(b, "Pad.Down")  == g + QStringLiteral("0/1/Hi"));   // dpdown:+a1
        CHECK(valueFor(b, "Pad.Left")  == g + QStringLiteral("0/0/Lo"));   // dpleft:-a0
        CHECK(valueFor(b, "Pad.Right") == g + QStringLiteral("0/0/Hi"));   // dpright:+a0
        // The four are pairwise distinct — the exact property the pre-fix code broke.
        CHECK(valueFor(b, "Pad.Up") != valueFor(b, "Pad.Down"));
        CHECK(valueFor(b, "Pad.Left") != valueFor(b, "Pad.Right"));
        // This pad declares no stick and no trigger, so it gets neither.
        CHECK(valueFor(b, "A..South") == g + QStringLiteral("3/2"));
        CHECK(!has(b, "L-Left"));
        CHECK(!has(b, "L-Trigger"));
    }

    // ---- 6. INVERTED axis (PS3). "a3~" rests at +32767 and falls when pressed, so the digital control is
    //         Lo; read as Hi it would be held down from boot. The pad's UNdecorated axes are unaffected.
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("030000004c0500006802000000000000");
        p.name = QStringLiteral("PS3 Controller");
        p.sdlMapping = QLatin1String(kPs3);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p, 0);
        const QString g = p.guid + QStringLiteral("/0/");

        CHECK(valueFor(b, "L-Trigger") == g + QStringLiteral("0/3/Lo"));   // lefttrigger:a3~
        CHECK(valueFor(b, "R-Trigger") == g + QStringLiteral("0/4/Lo"));   // righttrigger:a4~
        CHECK(valueFor(b, "L-Left")    == g + QStringLiteral("0/0/Lo"));   // leftx:a0, undecorated
        CHECK(valueFor(b, "L-Up")      == g + QStringLiteral("0/1/Lo"));   // lefty:a1, undecorated
        CHECK(valueFor(b, "R-Up")      == g + QStringLiteral("0/5/Lo"));   // righty:a5, undecorated
        CHECK(valueFor(b, "Pad.Up")    == g + QStringLiteral("1/1/Lo"));   // hat, untouched by inversion
    }

    // ---- 7. INVERTED STICK axes (Switch 2 Pro). "lefty:a1~" means pushing up drives the raw axis POSITIVE,
    //         so L-Up is Hi and L-Down is Lo — the inversion swaps the two directions of the stick.
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("030000007e0500006920000000000000");
        p.name = QStringLiteral("Nintendo Switch 2 Pro Controller");
        p.sdlMapping = QLatin1String(kSwitch2);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p, 0);
        const QString g = p.guid + QStringLiteral("/0/");

        CHECK(valueFor(b, "L-Up")    == g + QStringLiteral("0/1/Hi"));     // lefty:a1~
        CHECK(valueFor(b, "L-Down")  == g + QStringLiteral("0/1/Lo"));
        CHECK(valueFor(b, "R-Up")    == g + QStringLiteral("0/3/Hi"));     // righty:a3~
        CHECK(valueFor(b, "R-Down")  == g + QStringLiteral("0/3/Lo"));
        CHECK(valueFor(b, "L-Left")  == g + QStringLiteral("0/0/Lo"));     // leftx:a0, NOT inverted
        CHECK(valueFor(b, "L-Right") == g + QStringLiteral("0/0/Hi"));
        CHECK(valueFor(b, "R-Left")  == g + QStringLiteral("0/2/Lo"));     // rightx:a2, NOT inverted
        CHECK(valueFor(b, "R-Right") == g + QStringLiteral("0/2/Hi"));
        CHECK(valueFor(b, "Pad.Up")  == g + QStringLiteral("3/11"));       // button d-pad, no qualifier
    }

    // ---- 8. The two decorations COMPOSE. A value carrying both a half-axis sign and an inversion is Lo
    //         when exactly one of them holds: "-a1~" presses Hi and "+a1~" presses Lo, the exact inverse of
    //         the plain half-axis d-pad in check 5.
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("03000000ffff0000aaaa000000000000");
        p.sdlMapping = QLatin1String(kCombined);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p, 0);
        const QString g = p.guid + QStringLiteral("/0/");

        CHECK(valueFor(b, "Pad.Up")    == g + QStringLiteral("0/1/Hi"));   // dpup:-a1~   (negative + swapped)
        CHECK(valueFor(b, "Pad.Down")  == g + QStringLiteral("0/1/Lo"));   // dpdown:+a1~ (positive + swapped)
        CHECK(valueFor(b, "Pad.Left")  == g + QStringLiteral("0/0/Lo"));   // dpleft:-a0  (sign only)
        CHECK(valueFor(b, "Pad.Right") == g + QStringLiteral("0/0/Hi"));   // dpright:+a0 (sign only)
        CHECK(valueFor(b, "L-Trigger") == g + QStringLiteral("0/2/Lo"));   // lefttrigger:a2~ (inversion only)
        CHECK(valueFor(b, "L-Left")    == g + QStringLiteral("0/0/Lo"));   // leftx:a0, undecorated
        CHECK(valueFor(b, "L-Up")      == g + QStringLiteral("0/1/Hi"));   // lefty:a1~, inverted stick
        CHECK(valueFor(b, "L-Down")    == g + QStringLiteral("0/1/Lo"));
    }

    // ---- 9. SLOT. Two physically identical pads report the SAME SDL GUID, and ares' SDL driver tells them
    //         apart only by the slot term (identifier = identity + "/" + slot). Emitting slot 0 for both
    //         wrote byte-identical assignments into VirtualPad1 and VirtualPad2, so player 1's pad drove
    //         both and player 2's drove nothing.
    {
        const QString gx = QStringLiteral("03000000380700001647000000000000");   // two Xbox 360 pads
        const QString gp = QStringLiteral("030000004c050000e60c000000000000");   // one PS5 pad

        // The single-pad entry point takes the slot explicitly, so it is directly testable.
        ControllerSeats::PadInfo x;
        x.index = 0;
        x.guid = gx;
        x.sdlMapping = QLatin1String(kX360);
        CHECK(valueFor(AresInput::bindingsFor(x, 0), "A..South") == gx + QStringLiteral("/0/3/0"));
        CHECK(valueFor(AresInput::bindingsFor(x, 1), "A..South") == gx + QStringLiteral("/1/3/0"));
        CHECK(valueFor(AresInput::bindingsFor(x, 1), "Pad.Up")   == gx + QStringLiteral("/1/1/1/Lo"));
        CHECK(valueFor(AresInput::bindingsFor(x, 1), "L-Up")     == gx + QStringLiteral("/1/0/1/Lo"));

        // Two seats, same GUID -> slots 0 and 1, and the two VirtualPad blocks are NOT identical.
        ControllerSeats::PadInfo x2 = x; x2.index = 1;
        const QByteArray bml = AresInput::settingsBml(ControllerSeats::assignSeats({ x, x2 }));
        CHECK(bml.contains("VirtualPad1\n  Pad.Up: " + (gx + QStringLiteral("/0/1/1/Lo;")).toUtf8()));
        CHECK(bml.contains("VirtualPad2\n  Pad.Up: " + (gx + QStringLiteral("/1/1/1/Lo;")).toUtf8()));
        CHECK(bml.count((gx + QStringLiteral("/0/3/0")).toUtf8()) == 1);
        CHECK(bml.count((gx + QStringLiteral("/1/3/0")).toUtf8()) == 1);
        // The slot ordinal is carried into EVERY variant, not just the first — otherwise two identical pads
        // would fall back onto player 1's device the moment the bus variant is the one that matches.
        CHECK(bml.count(QByteArray("050") + gx.mid(3).toUtf8() + "/1/3/0") == 1);

        // A DIFFERENT GUID starts again at 0: seats {X, PS5, X} -> slots 0, 0, 1. Slot is the ordinal among
        // earlier seats sharing the GUID, NOT the seat number.
        ControllerSeats::PadInfo ps5;
        ps5.index = 1;
        ps5.guid = gp;
        ps5.sdlMapping = QLatin1String(kPs5);
        ControllerSeats::PadInfo x3 = x; x3.index = 2;
        const QByteArray bml3 = AresInput::settingsBml(ControllerSeats::assignSeats({ x, ps5, x3 }));
        CHECK(bml3.contains("VirtualPad1\n  Pad.Up: " + (gx + QStringLiteral("/0/1/1/Lo;")).toUtf8()));
        CHECK(bml3.contains("VirtualPad2\n  Pad.Up: " + (gp + QStringLiteral("/0/1/1/Lo;")).toUtf8()));
        CHECK(bml3.contains("VirtualPad3\n  Pad.Up: " + (gx + QStringLiteral("/1/1/1/Lo;")).toUtf8()));

        // A seat we contribute NO bindings for still consumes its slot: ares enumerates that device either
        // way, so the next same-GUID pad is its slot 1, not slot 0.
        ControllerSeats::PadInfo blank;
        blank.index = 0;
        blank.guid = gx;                 // same GUID, but SDL has no gamepad mapping for it
        const QByteArray bml4 = AresInput::settingsBml(ControllerSeats::assignSeats({ blank, x2 }));
        CHECK(!bml4.contains("VirtualPad1"));
        CHECK(bml4.contains("VirtualPad2\n  Pad.Up: " + (gx + QStringLiteral("/1/1/1/Lo;")).toUtf8()));
    }

    // ---- 10. needsSeed: only a file with NO VirtualPad assignment is seeded. ----------------------------
    {
        CHECK(AresInput::needsSeed(QByteArray()));                       // absent / empty file
        CHECK(AresInput::needsSeed("Video\n  Driver: Direct3D 11\n"));   // ares ran, never mapped
        // ares writes every VirtualPad key with an empty value when nothing is assigned.
        CHECK(AresInput::needsSeed("VirtualPad1\n  Pad.Up:\n  A..South:\n"));
        // A value of nothing but whitespace is still "unassigned" — it must not block the seed.
        CHECK(AresInput::needsSeed("VirtualPad1\n  Pad.Up:   \n  A..South: \t \n"));
        // THE FORM REAL ares ACTUALLY WRITES. An InputMapping holds up to three bindings and serializes
        // them joined by ';', so an UNMAPPED input persists as the separators ALONE — ares v148 saves
        // "Pad.Up: ;;", not "Pad.Up:". Read as an assignment this permanently blocks the seed: ares saves
        // settings.bml on exit, so one unmapped run leaves a file that never gets seeded again and the pad
        // stays dead for ever. Verified against a settings.bml ares wrote itself.
        CHECK(AresInput::needsSeed("VirtualPad1\n  Pad.Up: ;;\n  A..South: ;;\n"));
        CHECK(AresInput::needsSeed("VirtualPad1\n  Pad.Up: ;\n"));            // two empty slots
        CHECK(AresInput::needsSeed("VirtualPad1\n  Pad.Up:  ;  ;  \n"));      // padded separators
        // ...but a real binding in ANY of the three slots is still a real mapping.
        CHECK(!AresInput::needsSeed("VirtualPad1\n  Pad.Up: 0300/0/1/1/Lo;;\n"));
        CHECK(!AresInput::needsSeed("VirtualPad1\n  Pad.Up: ;;0300/0/3/9\n"));
        // A user's own mapping is never touched.
        CHECK(!AresInput::needsSeed("VirtualPad1\n  Pad.Up: 0300/0/1/1/Lo\n"));
        CHECK(!AresInput::needsSeed("Video\n  Driver: Direct3D 11\nVirtualPad2\n  Start: 0300/0/3/9\n"));
        // An INDENTED key that merely begins with "VirtualPad" is a child of some other node, not a pad
        // block, so its value does not count as an assignment.
        CHECK(AresInput::needsSeed("Paths\n  VirtualPadProfiles: C:/pads\n"));
    }

    // ---- 10b. identity variants: the SDL2/SDL3 bus disagreement, and its edges. ---------------------------
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.sdlMapping = QLatin1String(kX360);

        // A USB-bus (0x03) identity gains the Bluetooth (0x05) variant; the third slot stays empty.
        p.guid = QStringLiteral("030000005e040000e002000000000000");
        QByteArray b = AresInput::settingsBml(ControllerSeats::assignSeats({ p }));
        CHECK(b.contains("  A..South: 030000005e040000e002000000000000/0/3/0"
                         ";050000005e040000e002000000000000/0/3/0;\n"));

        // ...and symmetrically, a Bluetooth identity gains the USB one. A pad moved between transports keeps
        // working without a re-seed, which matters because the seed only ever runs once.
        p.guid = QStringLiteral("050000005e040000e002000000000000");
        b = AresInput::settingsBml(ControllerSeats::assignSeats({ p }));
        CHECK(b.contains("  A..South: 050000005e040000e002000000000000/0/3/0"
                         ";030000005e040000e002000000000000/0/3/0;\n"));

        // A bus we do not recognise keeps its own identity FIRST and still gets both variants offered.
        p.guid = QStringLiteral("0a0000005e040000e002000000000000");
        b = AresInput::settingsBml(ControllerSeats::assignSeats({ p }));
        CHECK(b.contains("  A..South: 0a0000005e040000e002000000000000/0/3/0"
                         ";050000005e040000e002000000000000/0/3/0"
                         ";030000005e040000e002000000000000/0/3/0\n"));

        // A GUID that is not 32 hex chars is left ALONE rather than sliced: we would be inventing an identity.
        p.guid = QStringLiteral("0300");
        b = AresInput::settingsBml(ControllerSeats::assignSeats({ p }));
        CHECK(b.contains("  A..South: 0300/0/3/0;;\n"));

        // Whatever the variants, the line still reads as a REAL assignment — it must not re-open the seed gate.
        CHECK(!AresInput::needsSeed(b));
    }

    // ---- 11. mergeSettingsBml: exactly ONE VirtualPadN block survives. ares resolves a settings path with
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

        // CRLF input (a file touched by a Windows editor): the pad block is still recognised and dropped,
        // and every kept line keeps its own \r — we rewrite nobody else's line endings.
        const QByteArray crlf =
            "Video\r\n  Driver: Direct3D 11\r\nVirtualPad1\r\n  Pad.Up:\r\nAudio\r\n  Driver: WASAPI\r\n";
        const QByteArray merged3 = AresInput::mergeSettingsBml(crlf, seed);
        CHECK(merged3 == QByteArray("Video\r\n  Driver: Direct3D 11\r\nAudio\r\n  Driver: WASAPI\r\n") + seed);
        CHECK(merged3.count("VirtualPad1") == 1);           // only the seed's

        // No trailing newline on the last line: the seed must start on its OWN line, never glued onto it.
        const QByteArray noEol = "Video\n  Driver: Direct3D 11";
        CHECK(AresInput::mergeSettingsBml(noEol, seed) == noEol + "\n" + seed);

        // An INDENTED child line that merely begins with "VirtualPad" belongs to another node and must
        // survive: only a TOP-LEVEL VirtualPad line opens a block to drop.
        const QByteArray childLine =
            "Paths\n  VirtualPadProfiles: C:/pads\nVirtualPad1\n  Pad.Up:\nAudio\n  Driver: WASAPI\n";
        const QByteArray merged4 = AresInput::mergeSettingsBml(childLine, seed);
        CHECK(merged4.contains("Paths\n  VirtualPadProfiles: C:/pads\n"));
        CHECK(merged4.contains("Audio\n  Driver: WASAPI\n"));
        CHECK(!merged4.contains("VirtualPad1\n  Pad.Up:\n"));
        CHECK(merged4.count("VirtualPad1") == 1);           // only the seed's
    }

    // ---- 12. AN EMPTY NAME FIELD. SDL allows the device-name field to be blank ("<guid>,,a:b0,…"), and
    //          parseMapping splits with KeepEmptyParts precisely so the two leading non-pair fields stay at
    //          indices 0 and 1. Under SkipEmptyParts the blank name collapses, every pair shifts down one, and
    //          the FIRST real binding is eaten as the name — silently, with the other fifteen still correct, so
    //          nothing but this fixture would go red. "a:b0" is that first pair here, so A..South is the canary.
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("03000000ffff0000cccc000000000000");
        p.name = QString();                                  // the pad SDL gave no name for
        p.sdlMapping = QLatin1String(kNoName);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p, 0);
        const QString g = p.guid + QStringLiteral("/0/");

        CHECK(valueFor(b, "A..South") == g + QStringLiteral("3/0"));   // the pair a SkipEmptyParts split eats
        CHECK(valueFor(b, "B..East")  == g + QStringLiteral("3/1"));
        CHECK(valueFor(b, "Pad.Up")   == g + QStringLiteral("1/1/Lo"));
        CHECK(valueFor(b, "L-Left")   == g + QStringLiteral("0/0/Lo"));
        CHECK(valueFor(b, "L-Right")  == g + QStringLiteral("0/0/Hi"));
    }

    if (failures == 0) std::printf("ARESINPUT-OK\n");
    else               std::fprintf(stderr, "ARESINPUT: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
