// Headless check of the multi-seat controller model (src/core/ControllerSeats.h, issue #104). QtCore-only — the
// model is PURE (no SDL, no disk, no live emulator) — so it runs under the offscreen QPA in CI. Prints SEATS-OK
// on success; any failure prints SEATS-FAIL <cond> (line) and exits non-zero.
//
// WHAT IT PINS:
//   * assignSeats gives 2/3/4 pads distinct seats 0..N-1 in STABLE (connection) order, carrying each pad's
//     payload; a 5th pad gets no seat (capped at kMaxSeats); an empty list -> no seats.
//   * controllerEdits(id, 0, pad) reproduces the shipped Player-1 block byte-for-byte for PCSX2/DuckStation/Cemu,
//     and for Dolphin the NEW SDL profile (SDL/{n}/{name}, tokens hand-authored from Dolphin's own SDL source) —
//     the fix for a DualSense / any DirectInput pad feeding no input under the old XInput-only device string;
//   * controllerEdits(id, 1, pad) produces the correctly-incremented Player-2 block: Dolphin [GCPad2] on
//     SDL/1/{name} + Dolphin.ini SIDevice1; PCSX2 [Pad2] on SDL-1 with NO repeated [InputSources]; DuckStation
//     [Pad2] on SDL-1 with NO repeated [ControllerPorts]; Cemu controller1.xml with <uuid>1</uuid>;
//   * the Dolphin MIGRATION replaces EB's OWN old XInput seed with the SDL body but leaves a user's custom
//     mapping untouched (byte-identity predicate; mutation-killed);
//   * an out-of-range seat (-1, kMaxSeats) yields NO edit; an unknown emulator yields NO edit.
//
// FIXTURES ARE HAND-AUTHORED, INDEPENDENT OF THE CODE UNDER TEST: the expected seat-0 blocks below are the
// literal Player-1 config text the app shipped BEFORE this issue (copied from the pre-#104 prepareControllerConfig
// write side), NOT produced by running controllerEdits. An assertion therefore cannot pass merely because it
// re-ran the function it checks. controllerEdits is the new code under test; the shipped P1 text is the oracle.
#include "ControllerSeats.h"

#include <QCoreApplication>
#include <cstdio>

using namespace ControllerSeats;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SEATS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The one edit for `file` whose marker is `marker` (or a null edit if absent). Hand-driven lookup so each
// assertion names exactly the (file, marker) it expects.
static ConfigEdit editFor(const QVector<ConfigEdit>& edits, const QString& file, const QString& marker)
{
    for (const ConfigEdit& e : edits)
        if (e.file == file && e.marker == marker) return e;
    return ConfigEdit{};
}

// The full config bytes seat 0 must write for an emulator = the ordered concatenation of every edit's body. For
// seat 0 this must equal today's shipped P1 append/seed byte-for-byte.
static QByteArray concatBodies(const QVector<ConfigEdit>& edits)
{
    QByteArray b;
    for (const ConfigEdit& e : edits) b += e.body;
    return b;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- assignSeats: stable order, payload carried, capped at kMaxSeats ------------------------------------
    auto pad = [](int idx, const char* guid, const char* name) {
        PadInfo p; p.index = idx; p.guid = QString::fromLatin1(guid); p.name = QString::fromLatin1(name); return p;
    };

    CHECK(assignSeats(QVector<PadInfo>{}).isEmpty());                       // no pads -> no seats

    {   // two pads -> seats 0,1 in order, each carrying its own pad
        const QVector<Seat> s = assignSeats({ pad(0, "GA", "Xbox"), pad(1, "GB", "DualSense") });
        CHECK(s.size() == 2);
        CHECK(s[0].index == 0 && s[0].pad.guid == QLatin1String("GA") && s[0].pad.name == QLatin1String("Xbox"));
        CHECK(s[1].index == 1 && s[1].pad.guid == QLatin1String("GB") && s[1].pad.name == QLatin1String("DualSense"));
    }
    {   // three pads -> seats 0,1,2, distinct and in order (kills a "seat = 0" or "pad = pads[0]" mutation)
        const QVector<Seat> s = assignSeats({ pad(0, "A", "p0"), pad(1, "B", "p1"), pad(2, "C", "p2") });
        CHECK(s.size() == 3);
        CHECK(s[0].index == 0 && s[1].index == 1 && s[2].index == 2);
        CHECK(s[0].pad.guid == QLatin1String("A") && s[1].pad.guid == QLatin1String("B") && s[2].pad.guid == QLatin1String("C"));
    }
    {   // four pads -> a full couch, seats 0..3
        const QVector<Seat> s = assignSeats({ pad(0, "A", ""), pad(1, "B", ""), pad(2, "C", ""), pad(3, "D", "") });
        CHECK(s.size() == 4);
        CHECK(s[3].index == 3 && s[3].pad.guid == QLatin1String("D"));
    }
    {   // five pads -> capped at kMaxSeats (kills an off-by-one in the cap)
        const QVector<Seat> s = assignSeats({ pad(0,"A",""), pad(1,"B",""), pad(2,"C",""), pad(3,"D",""), pad(4,"E","") });
        CHECK(s.size() == kMaxSeats);
        CHECK(s.size() == 4);
        for (const Seat& seat : s) CHECK(seat.pad.guid != QLatin1String("E")); // the 5th pad never got a seat
    }

    const PadInfo any = pad(0, "X", "any");

    // ================= SEAT 0 == TODAY, byte-for-byte (independent shipped-literal fixtures) =================

    // ---- Dolphin P1: GCPadNew.ini [GCPad1] + Dolphin.ini [Core] SIDevice0 = 6 -----------------------------
    // Oracle hand-authored from Dolphin's SDL ControllerInterface source (issue: DualSense/any DirectInput pad got
    // no input under the old XInput-only device string). Device = SDL/{n}/{name}; face buttons Button S/E/W/N per
    // s_sdl_button_names (South/East/West/North) at the SAME physical positions the old XInput seed used
    // (A=East, B=South, X=North, Y=West); dpad/shoulders/Start/sticks/triggers/rumble tokens are name-identical
    // between Dolphin's XInput and SDL backends, so only the device line and the four face buttons move. `any`'s
    // name is "any" -> SDL/0/any.
    {
        static const QByteArray kGcPad1 =
            "[GCPad1]\nDevice = SDL/0/any\n"
            "Buttons/A = `Button E`\nButtons/B = `Button S`\nButtons/X = `Button N`\nButtons/Y = `Button W`\n"
            "Buttons/Z = `Shoulder R`\nButtons/Start = `Start`\n"
            "Main Stick/Up = `Left Y+`\nMain Stick/Down = `Left Y-`\nMain Stick/Left = `Left X-`\n"
            "Main Stick/Right = `Left X+`\nC-Stick/Up = `Right Y+`\nC-Stick/Down = `Right Y-`\n"
            "C-Stick/Left = `Right X-`\nC-Stick/Right = `Right X+`\n"
            "Triggers/L = `Trigger L`\nTriggers/R = `Trigger R`\nTriggers/L-Analog = `Trigger L`\n"
            "Triggers/R-Analog = `Trigger R`\nD-Pad/Up = `Pad N`\nD-Pad/Down = `Pad S`\nD-Pad/Left = `Pad W`\n"
            "D-Pad/Right = `Pad E`\nMain Stick/Dead Zone = 15.0\nC-Stick/Dead Zone = 15.0\n"
            "Rumble/Motor = `Motor L`|`Motor R`\n";
        static const QByteArray kSiDevice0 = "\n[Core]\nSIDevice0 = 6\n";
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("dolphin"), 0, any);
        CHECK(e.size() == 2);
        const ConfigEdit gc = editFor(e, QStringLiteral("User/Config/GCPadNew.ini"), QStringLiteral("[GCPad1]"));
        CHECK(gc.body == kGcPad1);
        CHECK(!gc.body.contains("XInput"));                     // the whole point: no XInput device anymore
        const ConfigEdit si = editFor(e, QStringLiteral("User/Config/Dolphin.ini"), QStringLiteral("SIDevice0"));
        CHECK(si.body == kSiDevice0);
    }

    // ---- Dolphin: the SDL device string carries the live pad NAME (SDL/{n}/{name}) ------------------------
    // A distinct name proves pad.name is threaded through (not a hardcoded string) — kills a "drop the name" or
    // "still XInput" mutation. DualSense reports "PS5 Controller" to SDL; whatever the name, it lands verbatim.
    {
        PadInfo ds; ds.index = 0; ds.name = QStringLiteral("PS5 Controller");
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("dolphin"), 0, ds);
        const ConfigEdit gc = editFor(e, QStringLiteral("User/Config/GCPadNew.ini"), QStringLiteral("[GCPad1]"));
        CHECK(gc.body.startsWith("[GCPad1]\nDevice = SDL/0/PS5 Controller\n"));
        CHECK(!gc.body.contains("SDL/0/any"));                  // the name is the pad's, not a fixed literal
    }

    // ---- PCSX2 P1: [InputSources] preamble + [Pad1] on SDL-0, concatenating to today's single append -------
    {
        static const QByteArray kPcsx2P1 =
            "\n[InputSources]\nSDL = true\nSDLControllerEnhancedMode = false\nSDLRawInput = true\n"
            "XInput = false\nDInput = false\n\n[Pad1]\nType = DualShock2\n"
            "Up = SDL-0/DPadUp\nRight = SDL-0/DPadRight\nDown = SDL-0/DPadDown\nLeft = SDL-0/DPadLeft\n"
            "Triangle = SDL-0/FaceNorth\nCircle = SDL-0/FaceEast\nCross = SDL-0/FaceSouth\nSquare = SDL-0/FaceWest\n"
            "Select = SDL-0/Back\nStart = SDL-0/Start\nL1 = SDL-0/LeftShoulder\nR1 = SDL-0/RightShoulder\n"
            "L2 = SDL-0/+LeftTrigger\nR2 = SDL-0/+RightTrigger\nL3 = SDL-0/LeftStick\nR3 = SDL-0/RightStick\n"
            "Analog = SDL-0/Guide\nLUp = SDL-0/-LeftY\nLRight = SDL-0/+LeftX\nLDown = SDL-0/+LeftY\n"
            "LLeft = SDL-0/-LeftX\nRUp = SDL-0/-RightY\nRRight = SDL-0/+RightX\nRDown = SDL-0/+RightY\n"
            "RLeft = SDL-0/-RightX\nLargeMotor = SDL-0/LargeMotor\nSmallMotor = SDL-0/SmallMotor\n";
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("pcsx2"), 0, any);
        CHECK(e.size() == 2); // preamble + Pad1
        for (const ConfigEdit& c : e) CHECK(c.file == QLatin1String("inis/PCSX2.ini"));
        CHECK(concatBodies(e) == kPcsx2P1);
    }

    // ---- DuckStation P1: [ControllerPorts]+[InputSources] preamble + [Pad1] on SDL-0 ----------------------
    {
        static const QByteArray kDuckP1 =
            "\n[ControllerPorts]\nMultitapMode = Disabled\nControllerSettingsMigrated = true\n\n"
            "[InputSources]\nSDL = true\nSDLControllerEnhancedMode = false\nXInput = false\nDInput = false\n\n"
            "[Pad1]\nType = AnalogController\n"
            "Up = SDL-0/DPadUp\nDown = SDL-0/DPadDown\nLeft = SDL-0/DPadLeft\nRight = SDL-0/DPadRight\n"
            "Triangle = SDL-0/Y\nCircle = SDL-0/B\nCross = SDL-0/A\nSquare = SDL-0/X\n"
            "Select = SDL-0/Back\nStart = SDL-0/Start\nL1 = SDL-0/LeftShoulder\nR1 = SDL-0/RightShoulder\n"
            "L2 = SDL-0/+LeftTrigger\nR2 = SDL-0/+RightTrigger\nL3 = SDL-0/LeftStick\nR3 = SDL-0/RightStick\n"
            "Analog = SDL-0/Guide\nLLeft = SDL-0/-LeftX\nLRight = SDL-0/+LeftX\nLDown = SDL-0/+LeftY\n"
            "LUp = SDL-0/-LeftY\nRLeft = SDL-0/-RightX\nRRight = SDL-0/+RightX\nRDown = SDL-0/+RightY\n"
            "RUp = SDL-0/-RightY\n";
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("duckstation"), 0, any);
        CHECK(e.size() == 2);
        for (const ConfigEdit& c : e) CHECK(c.file == QLatin1String("settings.ini"));
        CHECK(concatBodies(e) == kDuckP1);
    }

    // ---- Cemu P1: controllerProfiles/controller0.xml (whole-file seed; empty marker) ----------------------
    {
        static const QByteArray kCemu0 =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<emulated_controller>\n\t<type>Wii U GamePad</type>\n"
            "\t<controller>\n\t\t<api>XInput</api>\n\t\t<uuid>0</uuid>\n\t\t<display_name>Controller 0</display_name>\n"
            "\t\t<rumble>0</rumble>\n\t\t<axis><deadzone>0.25</deadzone><range>1</range></axis>\n"
            "\t\t<rotation><deadzone>0.25</deadzone><range>1</range></rotation>\n"
            "\t\t<trigger><deadzone>0.25</deadzone><range>1</range></trigger>\n\t\t<mappings>\n"
            "\t\t\t<entry><mapping>1</mapping><button>13</button></entry>\n"
            "\t\t\t<entry><mapping>2</mapping><button>12</button></entry>\n"
            "\t\t\t<entry><mapping>3</mapping><button>15</button></entry>\n"
            "\t\t\t<entry><mapping>4</mapping><button>14</button></entry>\n"
            "\t\t\t<entry><mapping>5</mapping><button>8</button></entry>\n"
            "\t\t\t<entry><mapping>6</mapping><button>9</button></entry>\n"
            "\t\t\t<entry><mapping>7</mapping><button>42</button></entry>\n"
            "\t\t\t<entry><mapping>8</mapping><button>43</button></entry>\n"
            "\t\t\t<entry><mapping>9</mapping><button>4</button></entry>\n"
            "\t\t\t<entry><mapping>10</mapping><button>5</button></entry>\n"
            "\t\t\t<entry><mapping>11</mapping><button>0</button></entry>\n"
            "\t\t\t<entry><mapping>12</mapping><button>1</button></entry>\n"
            "\t\t\t<entry><mapping>13</mapping><button>2</button></entry>\n"
            "\t\t\t<entry><mapping>14</mapping><button>3</button></entry>\n"
            "\t\t\t<entry><mapping>15</mapping><button>6</button></entry>\n"
            "\t\t\t<entry><mapping>16</mapping><button>7</button></entry>\n"
            "\t\t\t<entry><mapping>17</mapping><button>39</button></entry>\n"
            "\t\t\t<entry><mapping>18</mapping><button>45</button></entry>\n"
            "\t\t\t<entry><mapping>19</mapping><button>44</button></entry>\n"
            "\t\t\t<entry><mapping>20</mapping><button>38</button></entry>\n"
            "\t\t\t<entry><mapping>21</mapping><button>41</button></entry>\n"
            "\t\t\t<entry><mapping>22</mapping><button>47</button></entry>\n"
            "\t\t\t<entry><mapping>23</mapping><button>46</button></entry>\n"
            "\t\t\t<entry><mapping>24</mapping><button>40</button></entry>\n"
            "\t\t</mappings>\n\t</controller>\n</emulated_controller>\n";
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("cemu"), 0, any);
        CHECK(e.size() == 1);
        CHECK(e[0].file == QLatin1String("controllerProfiles/controller0.xml"));
        CHECK(e[0].marker.isEmpty());          // whole-file seed idiom
        CHECK(e[0].body == kCemu0);
    }

    // ================= SEAT 1: the correctly-incremented Player-2 block =====================================

    // Dolphin P2: [GCPad2] on SDL/1/{name}, Dolphin.ini SIDevice1 = 6 (GCPad 1-based, SIDevice 0-based).
    {
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("dolphin"), 1, any);
        CHECK(e.size() == 2);
        const ConfigEdit gc = editFor(e, QStringLiteral("User/Config/GCPadNew.ini"), QStringLiteral("[GCPad2]"));
        CHECK(gc.body.startsWith("[GCPad2]\nDevice = SDL/1/any\n"));
        CHECK(gc.body.contains("Rumble/Motor = `Motor L`|`Motor R`\n"));   // full block, not a stub
        CHECK(!gc.body.contains("SDL/0/"));                                // not still P1's device index
        CHECK(!gc.body.contains("XInput"));                                // and never XInput
        const ConfigEdit si = editFor(e, QStringLiteral("User/Config/Dolphin.ini"), QStringLiteral("SIDevice1"));
        CHECK(si.body == QByteArray("\n[Core]\nSIDevice1 = 6\n"));         // 0-based
        CHECK(editFor(e, QStringLiteral("User/Config/Dolphin.ini"), QStringLiteral("SIDevice2")).body.isEmpty());
    }

    // PCSX2 P2: exactly one edit — [Pad2] on SDL-1, and NO repeated [InputSources] preamble.
    {
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("pcsx2"), 1, any);
        CHECK(e.size() == 1);
        CHECK(e[0].marker == QLatin1String("[Pad2]"));
        CHECK(e[0].body.startsWith("\n[Pad2]\nType = DualShock2\n"));
        CHECK(e[0].body.contains("Up = SDL-1/DPadUp\n"));
        CHECK(!e[0].body.contains("SDL-0/"));
        CHECK(!e[0].body.contains("[InputSources]"));                      // preamble is seat-0 only
    }

    // DuckStation P2: exactly one edit — [Pad2] on SDL-1, no repeated [ControllerPorts]/[InputSources].
    {
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("duckstation"), 1, any);
        CHECK(e.size() == 1);
        CHECK(e[0].marker == QLatin1String("[Pad2]"));
        CHECK(e[0].body.startsWith("\n[Pad2]\nType = AnalogController\n"));
        CHECK(e[0].body.contains("Up = SDL-1/DPadUp\n"));
        CHECK(!e[0].body.contains("SDL-0/"));
        CHECK(!e[0].body.contains("[ControllerPorts]"));
        CHECK(!e[0].body.contains("[InputSources]"));
    }

    // Cemu P2: controller1.xml with <uuid>1</uuid>, display "Controller 1", Pro Controller type.
    {
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("cemu"), 1, any);
        CHECK(e.size() == 1);
        CHECK(e[0].file == QLatin1String("controllerProfiles/controller1.xml"));
        CHECK(e[0].marker.isEmpty());
        CHECK(e[0].body.contains("<uuid>1</uuid>"));
        CHECK(e[0].body.contains("<display_name>Controller 1</display_name>"));
        CHECK(!e[0].body.contains("<uuid>0</uuid>"));
        CHECK(e[0].body.contains("<type>Wii U Pro Controller</type>"));    // extra players are Pro Controllers
    }

    // Seat 3 (the 4th player) for Dolphin: [GCPad4] / SIDevice3.
    {
        const QVector<ConfigEdit> e = controllerEdits(QStringLiteral("dolphin"), 3, any);
        CHECK(!editFor(e, QStringLiteral("User/Config/GCPadNew.ini"), QStringLiteral("[GCPad4]")).body.isEmpty());
        CHECK(editFor(e, QStringLiteral("User/Config/Dolphin.ini"), QStringLiteral("SIDevice3")).body
              == QByteArray("\n[Core]\nSIDevice3 = 6\n"));
    }

    // ================= Dolphin migration: recognize EB's OWN old XInput seed, never a user's mapping ========
    // Pin dolphinGcPadBodyLegacyXInput against the hand-authored OLD seed literal (the exact text EB shipped
    // before the SDL migration), so the recognizer keys on the real pre-SDL shape — independent of any builder.
    {
        static const QByteArray kOldGcPad1 =
            "[GCPad1]\nDevice = XInput/0/Gamepad\n"
            "Buttons/A = `Button B`\nButtons/B = `Button A`\nButtons/X = `Button Y`\nButtons/Y = `Button X`\n"
            "Buttons/Z = `Shoulder R`\nButtons/Start = `Start`\n"
            "Main Stick/Up = `Left Y+`\nMain Stick/Down = `Left Y-`\nMain Stick/Left = `Left X-`\n"
            "Main Stick/Right = `Left X+`\nC-Stick/Up = `Right Y+`\nC-Stick/Down = `Right Y-`\n"
            "C-Stick/Left = `Right X-`\nC-Stick/Right = `Right X+`\n"
            "Triggers/L = `Trigger L`\nTriggers/R = `Trigger R`\nTriggers/L-Analog = `Trigger L`\n"
            "Triggers/R-Analog = `Trigger R`\nD-Pad/Up = `Pad N`\nD-Pad/Down = `Pad S`\nD-Pad/Left = `Pad W`\n"
            "D-Pad/Right = `Pad E`\nMain Stick/Dead Zone = 15.0\nC-Stick/Dead Zone = 15.0\n"
            "Rumble/Motor = `Motor L`|`Motor R`\n";
        CHECK(dolphinGcPadBodyLegacyXInput(0) == kOldGcPad1);
    }

    // The replace-if-EB-seed predicate, on independent hand-authored fixtures (NOT built by the functions under
    // test): a tiny "legacy" section and its SDL replacement. The predicate is byte-identity of the WHOLE section.
    {
        static const QByteArray legacy = "[GCPad1]\nDevice = XInput/0/Gamepad\nButtons/A = `Button B`\n";
        static const QByteArray sdl    = "[GCPad1]\nDevice = SDL/0/Pad\nButtons/A = `Button E`\n";

        // (a) a file that IS exactly EB's old seed -> replaced with the SDL body.
        CHECK(replaceDolphinGcPadSectionIfEbSeed(legacy, 0, legacy, sdl) == sdl);

        // (b) a user-custom section (one byte differs: A remapped) -> LEFT UNTOUCHED. This is the mutation-kill for
        //     a too-greedy recognizer: any header-only / prefix match would clobber this hand-built mapping.
        static const QByteArray custom = "[GCPad1]\nDevice = XInput/0/Gamepad\nButtons/A = `Button X`\n";
        CHECK(replaceDolphinGcPadSectionIfEbSeed(custom, 0, legacy, sdl) == custom);

        // (c) no [GCPad1] section present -> unchanged.
        static const QByteArray other = "[GCPad2]\nDevice = XInput/1/Gamepad\n";
        CHECK(replaceDolphinGcPadSectionIfEbSeed(other, 0, legacy, sdl) == other);

        // (d) EB seed FOLLOWED by another section: only the EB section is replaced; the trailing one is preserved.
        static const QByteArray legacyPlus = legacy + "[GCPad2]\nDevice = XInput/1/Gamepad\nButtons/A = `Button B`\n";
        static const QByteArray sdlPlus    = sdl    + "[GCPad2]\nDevice = XInput/1/Gamepad\nButtons/A = `Button B`\n";
        CHECK(replaceDolphinGcPadSectionIfEbSeed(legacyPlus, 0, legacy, sdl) == sdlPlus);

        // (e) [GCPad1] must NOT match [GCPad10] (the trailing ']' disambiguates).
        static const QByteArray ten = "[GCPad10]\nDevice = XInput/9/Gamepad\n";
        CHECK(replaceDolphinGcPadSectionIfEbSeed(ten, 0, legacy, sdl) == ten);
    }

    // Tie-in with the REAL builders as EmulatorManager wires them: a file that is EB's real old XInput seed is
    // migrated to the real SDL body (with the live pad name); a user-custom file is left exactly as found.
    {
        const QByteArray realOld = dolphinGcPadBodyLegacyXInput(0);
        const QByteArray migrated = replaceDolphinGcPadSectionIfEbSeed(
            realOld, 0, dolphinGcPadBodyLegacyXInput(0), dolphinGcPadBody(0, QStringLiteral("Xbox 360 Controller")));
        CHECK(migrated == dolphinGcPadBody(0, QStringLiteral("Xbox 360 Controller")));
        CHECK(migrated.startsWith("[GCPad1]\nDevice = SDL/0/Xbox 360 Controller\n"));
        CHECK(!migrated.contains("XInput"));

        // A user who hand-edited even one byte of the old seed keeps their file untouched.
        QByteArray userEdited = dolphinGcPadBodyLegacyXInput(0);
        userEdited.replace("Buttons/A = `Button B`", "Buttons/A = `Button Y`");
        CHECK(replaceDolphinGcPadSectionIfEbSeed(
                  userEdited, 0, dolphinGcPadBodyLegacyXInput(0),
                  dolphinGcPadBody(0, QStringLiteral("Xbox"))) == userEdited);
    }

    // ================= out-of-range seats and unknown emulators yield NO edit ===============================
    CHECK(controllerEdits(QStringLiteral("dolphin"), -1, any).isEmpty());          // negative seat
    CHECK(controllerEdits(QStringLiteral("dolphin"), kMaxSeats, any).isEmpty());   // seat == cap (0-based) -> none
    CHECK(controllerEdits(QStringLiteral("pcsx2"), 4, any).isEmpty());
    CHECK(controllerEdits(QStringLiteral("melonds"), 0, any).isEmpty());           // unhandled emulator
    CHECK(controllerEdits(QStringLiteral("nonsuch"), 0, any).isEmpty());           // unknown emulator

    if (failures == 0) std::puts("SEATS-OK");
    return failures == 0 ? 0 : 1;
}
