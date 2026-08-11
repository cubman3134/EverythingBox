// Multi-seat controller propagation into standalone emulators (issue #104) — the PURE heart. The standalone
// twin of #103's write-config-on-launch, applied to CONTROLLERS. EmulatorManager::prepareControllerConfig used
// to map exactly ONE standard pad as Player 1 per emulator; this model extends that to MULTIPLE pads with seat
// assignment — the pad in seat N becomes player N in the emulator's own config, so four pads on the couch get
// players 1..4 seeded in Dolphin/PCSX2/DuckStation/Cemu without anyone opening an input dialog.
//
// PURE, header-only, QtCore-only, NO SDL, NO disk, NO EmulatorManager. Two pure functions:
//   * assignSeats(pads)          — order a connected-pad list into seats 0..kMaxSeats-1 (stable, deterministic).
//   * controllerEdits(id, N, pad)— the exact per-player config writes for emulator `id`, player seat N.
// The write-on-launch side (EmulatorManager) enumerates the live pads via SDL, calls assignSeats, then applies
// each ConfigEdit with the SAME only-when-absent merge idioms it already uses (appendIniSectionIfAbsent /
// seedFileIfAbsent) — never a whole-file clobber, so a user's own hand-built mapping is preserved untouched.
// Keeping this pure is what lets probe_seats pin the seat model and the exact per-emulator player-N edits against
// hand-authored fixtures, with no live emulator and no disk.
//
// SEAT 0 == TODAY, EXACTLY. controllerEdits(id, 0, pad) reproduces the byte-for-byte P1 blocks the code shipped
// before this issue (verified in probe_seats against the shipped literals as an independent fixture), so the
// single-pad couch case is a strict no-op change. Seat N>0 parameterises the SAME blocks by the player index:
// Dolphin GCPad{N+1} + SIDevice{N}; PCSX2 [Pad{N+1}] on SDL-{N}; DuckStation [Pad{N+1}] on SDL-{N}; Cemu
// controller{N}.xml with <uuid>{N}</uuid>.
//
// PER-EMULATOR PLAYER-INDEX CONVENTIONS (documented, each verified against the emulator's config format, NOT
// against a live run — the #103 confidence posture):
//   * Dolphin — GCPadNew.ini sections are 1-based ([GCPad1]..[GCPad4]); Dolphin.ini [Core] SIDevice keys are
//     0-based (SIDevice0..SIDevice3). Device string XInput/{index}/Gamepad keys on the 0-based XInput slot.
//   * PCSX2 — inis/PCSX2.ini pad sections are 1-based ([Pad1]..); the SDL device is SDL-{index} (0-based).
//   * DuckStation — settings.ini pad sections are 1-based ([Pad1]..); device SDL-{index} (0-based).
//   * Cemu — controllerProfiles/controller{index}.xml is 0-based; <uuid>{index}</uuid> binds that device slot.
// An unset seat (out of range) or unknown emulator yields NO edit — degrade to "open the emulator", never guess.
//
// DEFERRED (issue stays open — Refs #104, not Fixes): hotkey-combo propagation; expanding past these four
// emulators (the brand matrix); and live-GUID PINNING — keeping a given physical pad on the same seat across a
// replug. PadInfo carries a `guid` field reserved for that pinning, but this landing keys purely on the
// connection-order index (the identity today's XInput/N and SDL-N device strings already use).
//
// NOTE ON MULTITAP (PCSX2 / DuckStation): the PS1/PS2 hardware has two physical controller ports; players 3-4
// require the emulator's multitap to be enabled. This model emits [Pad3]/[Pad4] faithfully, but does not toggle
// multitap — so on those two emulators a 3rd/4th seat is written but only takes effect once multitap is on. The
// benign failure mode ("this seat does nothing until multitap") mirrors #103's "this one setting does nothing".
// Dolphin and Cemu seat four players natively.
#pragma once
#include <QString>
#include <QByteArray>
#include <QVector>

namespace ControllerSeats
{
    static constexpr int kMaxSeats = 4; // players 1..4 (RetroPad ports 0..3), the in-process tier's cap

    // A physical pad as the app enumerates it. `index` is its connection-order index among game controllers
    // (0-based) — the identity today's per-emulator device strings key on (XInput/N, SDL-N). `guid`/`name` are
    // RESERVED: a future version pins "this physical pad = P2" across replug via the SDL joystick GUID (the same
    // identity SDL_gamecontrollerdb keys on); this landing does not consult them (live-GUID pinning is DEFERRED).
    struct PadInfo
    {
        int     index = 0;   // connection-order index among game controllers (0..)
        QString guid;        // SDL joystick GUID — reserved for future GUID pinning (unused this landing)
        QString name;        // human controller name — reserved (diagnostics / future seat UI)
    };

    // One assigned seat: player `index` (0-based; player index+1) driven by `pad`.
    struct Seat
    {
        int     index = 0;
        PadInfo pad;
        bool operator==(const Seat& o) const { return index == o.index && pad.index == o.pad.index
                                                    && pad.guid == o.pad.guid && pad.name == o.pad.name; }
    };

    // One config write for a player. When `marker` is NON-empty: append `body` to `file` if `marker` is not
    // already present in the file (merge, never clobber) — the appendIniSectionIfAbsent idiom. When `marker` is
    // EMPTY: `body` is the WHOLE file, seeded only if the file is absent — the seedFileIfAbsent idiom (Cemu's
    // per-controller XML). `file` is relative to the emulator's install dir. Modelling both write idioms here is
    // what lets seat 0 reproduce today's P1 write byte-for-byte.
    struct ConfigEdit
    {
        QString    file;
        QString    marker;   // append-if-absent marker; empty => whole-file seed-if-absent
        QByteArray body;
        bool operator==(const ConfigEdit& o) const { return file == o.file && marker == o.marker && body == o.body; }
        bool operator!=(const ConfigEdit& o) const { return !(*this == o); }
    };

    // ---- pure: order connected pads into seats 0..kMaxSeats-1 ------------------------------------------------
    // Stable and deterministic: seat i gets pads[i], in the order given (the app hands pads in connection order).
    // Pads past kMaxSeats get no seat. GUID pinning (holding a physical pad on one seat across replug) is
    // deferred — the GUID rides along on each Seat's PadInfo but is not consulted here.
    inline QVector<Seat> assignSeats(const QVector<PadInfo>& pads)
    {
        QVector<Seat> seats;
        for (int i = 0; i < pads.size() && i < kMaxSeats; ++i)
            seats.push_back(Seat{ i, pads[i] });
        return seats;
    }

    // ---- pure per-emulator body builders (device index == seat index this landing) --------------------------
    // Each returns the EXACT bytes today's P1 block used at index 0, parameterised by the player/device index.

    // Dolphin GCPadNew.ini [GCPad{n+1}] — standard XInput pad on slot n. Byte-identical to the shipped P1 block
    // at n==0; only the section number and the XInput slot move with n.
    inline QByteArray dolphinGcPadBody(int n)
    {
        return "[GCPad" + QByteArray::number(n + 1) + "]\nDevice = XInput/" + QByteArray::number(n) + "/Gamepad\n"
               "Buttons/A = `Button B`\nButtons/B = `Button A`\nButtons/X = `Button Y`\nButtons/Y = `Button X`\n"
               "Buttons/Z = `Shoulder R`\nButtons/Start = `Start`\n"
               "Main Stick/Up = `Left Y+`\nMain Stick/Down = `Left Y-`\nMain Stick/Left = `Left X-`\n"
               "Main Stick/Right = `Left X+`\nC-Stick/Up = `Right Y+`\nC-Stick/Down = `Right Y-`\n"
               "C-Stick/Left = `Right X-`\nC-Stick/Right = `Right X+`\n"
               "Triggers/L = `Trigger L`\nTriggers/R = `Trigger R`\nTriggers/L-Analog = `Trigger L`\n"
               "Triggers/R-Analog = `Trigger R`\nD-Pad/Up = `Pad N`\nD-Pad/Down = `Pad S`\nD-Pad/Left = `Pad W`\n"
               "D-Pad/Right = `Pad E`\nMain Stick/Dead Zone = 15.0\nC-Stick/Dead Zone = 15.0\n"
               "Rumble/Motor = `Motor L`|`Motor R`\n";
    }

    // PCSX2 [Pad{n+1}] on SDL-{n}. The [InputSources] preamble is shared (emitted once, for seat 0) so seat 0's
    // two edits concatenate to exactly today's single P1 append.
    inline QByteArray pcsx2PadBody(int n)
    {
        const QByteArray d = "SDL-" + QByteArray::number(n);
        return "\n[Pad" + QByteArray::number(n + 1) + "]\nType = DualShock2\n"
               "Up = " + d + "/DPadUp\nRight = " + d + "/DPadRight\nDown = " + d + "/DPadDown\nLeft = " + d + "/DPadLeft\n"
               "Triangle = " + d + "/FaceNorth\nCircle = " + d + "/FaceEast\nCross = " + d + "/FaceSouth\nSquare = " + d + "/FaceWest\n"
               "Select = " + d + "/Back\nStart = " + d + "/Start\nL1 = " + d + "/LeftShoulder\nR1 = " + d + "/RightShoulder\n"
               "L2 = " + d + "/+LeftTrigger\nR2 = " + d + "/+RightTrigger\nL3 = " + d + "/LeftStick\nR3 = " + d + "/RightStick\n"
               "Analog = " + d + "/Guide\nLUp = " + d + "/-LeftY\nLRight = " + d + "/+LeftX\nLDown = " + d + "/+LeftY\n"
               "LLeft = " + d + "/-LeftX\nRUp = " + d + "/-RightY\nRRight = " + d + "/+RightX\nRDown = " + d + "/+RightY\n"
               "RLeft = " + d + "/-RightX\nLargeMotor = " + d + "/LargeMotor\nSmallMotor = " + d + "/SmallMotor\n";
    }

    // DuckStation [Pad{n+1}] on SDL-{n}. The [ControllerPorts]+[InputSources] preamble is shared (seat 0 only).
    inline QByteArray duckPadBody(int n)
    {
        const QByteArray d = "SDL-" + QByteArray::number(n);
        return "\n[Pad" + QByteArray::number(n + 1) + "]\nType = AnalogController\n"
               "Up = " + d + "/DPadUp\nDown = " + d + "/DPadDown\nLeft = " + d + "/DPadLeft\nRight = " + d + "/DPadRight\n"
               "Triangle = " + d + "/Y\nCircle = " + d + "/B\nCross = " + d + "/A\nSquare = " + d + "/X\n"
               "Select = " + d + "/Back\nStart = " + d + "/Start\nL1 = " + d + "/LeftShoulder\nR1 = " + d + "/RightShoulder\n"
               "L2 = " + d + "/+LeftTrigger\nR2 = " + d + "/+RightTrigger\nL3 = " + d + "/LeftStick\nR3 = " + d + "/RightStick\n"
               "Analog = " + d + "/Guide\nLLeft = " + d + "/-LeftX\nLRight = " + d + "/+LeftX\nLDown = " + d + "/+LeftY\n"
               "LUp = " + d + "/-LeftY\nRLeft = " + d + "/-RightX\nRRight = " + d + "/+RightX\nRDown = " + d + "/+RightY\n"
               "RUp = " + d + "/-RightY\n";
    }

    // Cemu controller{n}.xml. Seat 0 is the Wii U GamePad (byte-identical to the shipped P1 XML); seats 1..3 are
    // Wii U Pro Controllers (the Wii U has one GamePad; extra players are Pro Controllers). <uuid>{n}</uuid> binds
    // the n-th device slot generically (XInput api, no per-device GUID — GUID pinning is deferred). The Pro
    // Controller shares the A/B/X/Y/L/R/ZL/ZR/±/dpad/L3/R3/stick layout, so the mapping-id block is reused.
    inline QByteArray cemuControllerBody(int n)
    {
        const bool gamepad = (n == 0);
        const QByteArray type = gamepad ? "Wii U GamePad" : "Wii U Pro Controller";
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<emulated_controller>\n\t<type>" + type + "</type>\n"
               "\t<controller>\n\t\t<api>XInput</api>\n\t\t<uuid>" + QByteArray::number(n) + "</uuid>\n"
               "\t\t<display_name>Controller " + QByteArray::number(n) + "</display_name>\n"
               "\t\t<rumble>0</rumble>\n\t\t<axis><deadzone>0.25</deadzone><range>1</range></axis>\n"
               "\t\t<rotation><deadzone>0.25</deadzone><range>1</range></rotation>\n"
               "\t\t<trigger><deadzone>0.25</deadzone><range>1</range></trigger>\n\t\t<mappings>\n"
               "\t\t\t<entry><mapping>1</mapping><button>13</button></entry>\n"   // A  <- XInput A
               "\t\t\t<entry><mapping>2</mapping><button>12</button></entry>\n"   // B  <- XInput B
               "\t\t\t<entry><mapping>3</mapping><button>15</button></entry>\n"   // X
               "\t\t\t<entry><mapping>4</mapping><button>14</button></entry>\n"   // Y
               "\t\t\t<entry><mapping>5</mapping><button>8</button></entry>\n"    // L
               "\t\t\t<entry><mapping>6</mapping><button>9</button></entry>\n"    // R
               "\t\t\t<entry><mapping>7</mapping><button>42</button></entry>\n"   // ZL (left trigger)
               "\t\t\t<entry><mapping>8</mapping><button>43</button></entry>\n"   // ZR (right trigger)
               "\t\t\t<entry><mapping>9</mapping><button>4</button></entry>\n"    // + (start)
               "\t\t\t<entry><mapping>10</mapping><button>5</button></entry>\n"   // - (back)
               "\t\t\t<entry><mapping>11</mapping><button>0</button></entry>\n"   // dpad up
               "\t\t\t<entry><mapping>12</mapping><button>1</button></entry>\n"   // dpad down
               "\t\t\t<entry><mapping>13</mapping><button>2</button></entry>\n"   // dpad left
               "\t\t\t<entry><mapping>14</mapping><button>3</button></entry>\n"   // dpad right
               "\t\t\t<entry><mapping>15</mapping><button>6</button></entry>\n"   // L3
               "\t\t\t<entry><mapping>16</mapping><button>7</button></entry>\n"   // R3
               "\t\t\t<entry><mapping>17</mapping><button>39</button></entry>\n"  // left stick up
               "\t\t\t<entry><mapping>18</mapping><button>45</button></entry>\n"  // left stick down
               "\t\t\t<entry><mapping>19</mapping><button>44</button></entry>\n"  // left stick left
               "\t\t\t<entry><mapping>20</mapping><button>38</button></entry>\n"  // left stick right
               "\t\t\t<entry><mapping>21</mapping><button>41</button></entry>\n"  // right stick up
               "\t\t\t<entry><mapping>22</mapping><button>47</button></entry>\n"  // right stick down
               "\t\t\t<entry><mapping>23</mapping><button>46</button></entry>\n"  // right stick left
               "\t\t\t<entry><mapping>24</mapping><button>40</button></entry>\n"  // right stick right
               "\t\t</mappings>\n\t</controller>\n</emulated_controller>\n";
    }

    // ---- pure: the per-emulator player-N config mapping — the mutation-tested core -------------------------
    // The exact config writes to seat player `seatIndex` (0-based) in emulator `emulatorId`. An out-of-range seat
    // or an unknown/unhandled emulator yields NO edit. `pad` is reserved (GUID pinning, deferred) and unused.
    inline QVector<ConfigEdit> controllerEdits(const QString& emulatorId, int seatIndex, const PadInfo& pad)
    {
        (void)pad; // reserved for future GUID pinning; this landing keys on seatIndex only
        QVector<ConfigEdit> out;
        if (seatIndex < 0 || seatIndex >= kMaxSeats) return out; // out-of-range seat -> no edit
        const int n = seatIndex;                                 // device index == seat index this landing

        if (emulatorId == QLatin1String("dolphin"))
        {
            out.push_back(ConfigEdit{ QStringLiteral("User/Config/GCPadNew.ini"),
                                      QStringLiteral("[GCPad%1]").arg(n + 1), dolphinGcPadBody(n) });
            out.push_back(ConfigEdit{ QStringLiteral("User/Config/Dolphin.ini"),
                                      QStringLiteral("SIDevice%1").arg(n),
                                      QByteArray("\n[Core]\nSIDevice") + QByteArray::number(n) + " = 6\n" });
        }
        else if (emulatorId == QLatin1String("pcsx2"))
        {
            const QString ini = QStringLiteral("inis/PCSX2.ini");
            if (n == 0) // shared SDL input-source preamble, once
                out.push_back(ConfigEdit{ ini, QStringLiteral("[InputSources]"),
                    "\n[InputSources]\nSDL = true\nSDLControllerEnhancedMode = false\nSDLRawInput = true\n"
                    "XInput = false\nDInput = false\n" });
            out.push_back(ConfigEdit{ ini, QStringLiteral("[Pad%1]").arg(n + 1), pcsx2PadBody(n) });
        }
        else if (emulatorId == QLatin1String("duckstation"))
        {
            const QString ini = QStringLiteral("settings.ini");
            if (n == 0) // shared controller-ports + input-source preamble, once
                out.push_back(ConfigEdit{ ini, QStringLiteral("[ControllerPorts]"),
                    "\n[ControllerPorts]\nMultitapMode = Disabled\nControllerSettingsMigrated = true\n\n"
                    "[InputSources]\nSDL = true\nSDLControllerEnhancedMode = false\nXInput = false\nDInput = false\n" });
            out.push_back(ConfigEdit{ ini, QStringLiteral("[Pad%1]").arg(n + 1), duckPadBody(n) });
        }
        else if (emulatorId == QLatin1String("cemu"))
        {
            out.push_back(ConfigEdit{ QStringLiteral("controllerProfiles/controller%1.xml").arg(n),
                                      QString(), cemuControllerBody(n) }); // empty marker => whole-file seed
        }
        // Any other emulator id yields no edit — degrade to "open the emulator", never guess.
        return out;
    }
}
