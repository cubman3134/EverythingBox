// Headless check of the pure RetroPark input mapper (src/emu/RetroParkInput.h) — Slice 2b, Task 3.
//
// RetroParkView feeds live keyboard/pad input to the driven FCEUmm shim by setting the VK-code bytes of
// rp_input_state.keys[] the shim reads (LibretroShim.cpp input_state_cb). The Qt-key -> VK and RetroPad-id -> VK
// translation is the load-bearing logic of that feed, so it is pinned here rather than left to the live run: a
// wrong VK number silently sends the NES the wrong (or no) button and nothing crashes to tell you.
//
// Every expected VK number is an INDEPENDENT oracle — the Win32 <windows.h> virtual-key code (VK_UP=0x26,
// VK_RETURN=0x0D, ...) and the ASCII of 'X'/'Z' — hand-written here, never read back from the header's own enum,
// so the assertions cannot be a fixed point of the mapper. Likewise the RetroPad ids are the libretro
// RETRO_DEVICE_ID_JOYPAD_* constants (UP=4, A=8, B=0, START=3, SELECT=2, ...) written by hand.
//
// RetroParkInput.h is header-only (only <QtCore/qnamespace.h> for the Qt::Key enum) and does no I/O, so this
// probe needs no QCoreApplication. Prints RETROPARK-INPUT-OK on success; any failure prints RPINPUT-FAIL <cond>
// (line) and exits non-zero.
#include "RetroParkInput.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "RPINPUT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Assert a Qt key maps (returns true) to exactly the given VK; the vk is seeded to a poison value first so a
// mapper that returns true without writing vkOut is caught.
static void expectQt(int qtKey, int wantVk) {
    int vk = -12345;
    const bool ok = rpinput::nesVkForQtKey(qtKey, vk);
    CHECK(ok);
    CHECK(vk == wantVk);
}
static void expectQtUnmapped(int qtKey) {
    int vk = -12345;
    const bool ok = rpinput::nesVkForQtKey(qtKey, vk);
    CHECK(!ok);
    CHECK(vk == -12345);   // vkOut left untouched for a non-NES key
}
static void expectPad(unsigned retroId, int wantVk) {
    int vk = -12345;
    const bool ok = rpinput::nesVkForRetroPad(retroId, vk);
    CHECK(ok);
    CHECK(vk == wantVk);
}

// ---- GameCube abstract-pad mapper (Slice 3b) oracles. -------------------------------------------------------
// The wanted bit index is the ABI's documented RP_PAD_* value (A=0,B=1,X=2,Y=3,L=4,R=5,SELECT=6,START=7,L3=8,
// R3=9,DPAD_UP/DOWN/LEFT/RIGHT=10-13), hand-written here — NEVER read back from the header's kPad* enum — so the
// assertion cannot be a fixed point of the mapper. wantBit is the index; (1u<<wantBit) is the mask a held control
// ORs into pad_buttons.
static void expectGcKey(int qtKey, int wantBit) {
    CHECK(rpinput::gcPadButtonForQtKey(qtKey) == wantBit);
}
static void expectGcKeyUnmapped(int qtKey) {
    CHECK(rpinput::gcPadButtonForQtKey(qtKey) == -1);
}
static void expectGcPad(unsigned retroId, int wantBit) {
    CHECK(rpinput::gcPadButtonForRetroPad(retroId) == wantBit);
}
static void expectGcAxis(int qtKey, int wantAxis, int wantVal) {
    int ax = -777, val = -777;
    const bool ok = rpinput::gcPadAxisForQtKey(qtKey, ax, val);
    CHECK(ok);
    CHECK(ax == wantAxis);
    CHECK(val == wantVal);
}

int main()
{
    // ---- 1. Keyboard: Qt::Key -> shim VK. Oracles are the Win32 VK codes / ASCII, hand-written. -----------
    expectQt(Qt::Key_Up,    0x26);   // VK_UP
    expectQt(Qt::Key_Down,  0x28);   // VK_DOWN
    expectQt(Qt::Key_Left,  0x25);   // VK_LEFT
    expectQt(Qt::Key_Right, 0x27);   // VK_RIGHT
    expectQt(Qt::Key_X,     0x58);   // 'X' -> NES A
    expectQt(Qt::Key_Z,     0x5A);   // 'Z' -> NES B
    expectQt(Qt::Key_Return, 0x0D);  // VK_RETURN -> Start
    expectQt(Qt::Key_Enter,  0x0D);  // keypad Enter is Start too
    expectQt(Qt::Key_Shift,  0x10);  // VK_SHIFT -> Select

    // The NES A/B are the X/Z keys, NOT swapped: X must be A (0x58) and Z must be B (0x5A). Pin the direction so
    // an accidental swap in the header is a hard failure.
    { int vk = 0; CHECK(rpinput::nesVkForQtKey(Qt::Key_X, vk) && vk == 0x58); }
    { int vk = 0; CHECK(rpinput::nesVkForQtKey(Qt::Key_Z, vk) && vk == 0x5A); }

    // ---- 2. Non-NES keys are left alone (menu/system keys must not leak into the game feed). ---------------
    expectQtUnmapped(Qt::Key_Escape);   // opens the pause menu — must NOT map to a NES button
    expectQtUnmapped(Qt::Key_Back);     // TV-remote Back — same
    expectQtUnmapped(Qt::Key_A);        // an arbitrary unmapped letter
    expectQtUnmapped(Qt::Key_Space);

    // ---- 3. Physical RetroPad: RETRO_DEVICE_ID_JOYPAD_* -> the SAME shim VK, so a pad plays identically. ----
    expectPad(4, 0x26);   // JOYPAD_UP    -> VK_UP
    expectPad(5, 0x28);   // JOYPAD_DOWN  -> VK_DOWN
    expectPad(6, 0x25);   // JOYPAD_LEFT  -> VK_LEFT
    expectPad(7, 0x27);   // JOYPAD_RIGHT -> VK_RIGHT
    expectPad(8, 0x58);   // JOYPAD_A     -> 'X'
    expectPad(0, 0x5A);   // JOYPAD_B     -> 'Z'
    expectPad(3, 0x0D);   // JOYPAD_START -> VK_RETURN
    expectPad(2, 0x10);   // JOYPAD_SELECT-> VK_SHIFT

    // A RetroPad id the NES has no button for (e.g. X/Y/L/R = 9/1/10/11) must not map.
    { int vk = -99; CHECK(!rpinput::nesVkForRetroPad(9, vk)  && vk == -99); }   // JOYPAD_X
    { int vk = -99; CHECK(!rpinput::nesVkForRetroPad(1, vk)  && vk == -99); }   // JOYPAD_Y
    { int vk = -99; CHECK(!rpinput::nesVkForRetroPad(11, vk) && vk == -99); }   // JOYPAD_R

    // ==== GameCube abstract-pad mapper (Slice 3b). Oracles are the ABI's documented RP_PAD_*/RP_AXIS_* indices. ==

    // ---- 4. Keyboard -> abstract-pad BIT index. -------------------------------------------------------------
    expectGcKey(Qt::Key_X,      0);   // -> RP_PAD_A  (GC A)
    expectGcKey(Qt::Key_Z,      1);   // -> RP_PAD_B
    expectGcKey(Qt::Key_S,      2);   // -> RP_PAD_X
    expectGcKey(Qt::Key_A,      3);   // -> RP_PAD_Y
    expectGcKey(Qt::Key_C,      6);   // -> RP_PAD_SELECT (GC "Z")
    expectGcKey(Qt::Key_Q,      4);   // -> RP_PAD_L
    expectGcKey(Qt::Key_E,      5);   // -> RP_PAD_R
    expectGcKey(Qt::Key_Return, 7);   // -> RP_PAD_START
    expectGcKey(Qt::Key_Enter,  7);   // keypad Enter -> Start too
    expectGcKey(Qt::Key_T,     10);   // -> RP_PAD_DPAD_UP
    expectGcKey(Qt::Key_F,     12);   // -> RP_PAD_DPAD_LEFT
    expectGcKey(Qt::Key_G,     11);   // -> RP_PAD_DPAD_DOWN
    expectGcKey(Qt::Key_H,     13);   // -> RP_PAD_DPAD_RIGHT

    // GC A/B are the X/Z keys, NOT swapped (X=A=bit0, Z=B=bit1). Pin the direction so an accidental swap fails.
    CHECK(rpinput::gcPadButtonForQtKey(Qt::Key_X) == 0);
    CHECK(rpinput::gcPadButtonForQtKey(Qt::Key_Z) == 1);

    // Arrows are NOT buttons (they drive the analog left stick); reserved/system keys are unmapped.
    expectGcKeyUnmapped(Qt::Key_Up);
    expectGcKeyUnmapped(Qt::Key_Down);
    expectGcKeyUnmapped(Qt::Key_Escape);   // pause menu — must not become a GC button
    expectGcKeyUnmapped(Qt::Key_R);        // rewind key
    expectGcKeyUnmapped(Qt::Key_Space);

    // ---- 5. Keyboard arrows -> analog LEFT stick (axis index + full-deflection value). Y is UP-positive. -----
    expectGcAxis(Qt::Key_Up,     1,  32767);   // RP_AXIS_LEFT_Y = +full (up)
    expectGcAxis(Qt::Key_Down,   1, -32767);   // RP_AXIS_LEFT_Y = -full (down)
    expectGcAxis(Qt::Key_Left,   0, -32767);   // RP_AXIS_LEFT_X = -full (left)
    expectGcAxis(Qt::Key_Right,  0,  32767);   // RP_AXIS_LEFT_X = +full (right)
    { int ax = -777, val = -777; CHECK(!rpinput::gcPadAxisForQtKey(Qt::Key_X, ax, val)   // a non-arrow is not an axis
                                       && ax == -777 && val == -777); }

    // ---- 6. Physical RetroPad -> abstract-pad BIT index (RETRO_DEVICE_ID_JOYPAD_* hand-written). --------------
    expectGcPad(8,  0);   // JOYPAD_A      -> RP_PAD_A
    expectGcPad(0,  1);   // JOYPAD_B      -> RP_PAD_B
    expectGcPad(9,  2);   // JOYPAD_X      -> RP_PAD_X
    expectGcPad(1,  3);   // JOYPAD_Y      -> RP_PAD_Y
    expectGcPad(10, 4);   // JOYPAD_L      -> RP_PAD_L
    expectGcPad(11, 5);   // JOYPAD_R      -> RP_PAD_R
    expectGcPad(2,  6);   // JOYPAD_SELECT -> RP_PAD_SELECT (GC "Z")
    expectGcPad(3,  7);   // JOYPAD_START  -> RP_PAD_START
    expectGcPad(14, 8);   // JOYPAD_L3     -> RP_PAD_L3
    expectGcPad(15, 9);   // JOYPAD_R3     -> RP_PAD_R3
    expectGcPad(4, 10);   // JOYPAD_UP     -> RP_PAD_DPAD_UP
    expectGcPad(5, 11);   // JOYPAD_DOWN   -> RP_PAD_DPAD_DOWN
    expectGcPad(6, 12);   // JOYPAD_LEFT   -> RP_PAD_DPAD_LEFT
    expectGcPad(7, 13);   // JOYPAD_RIGHT  -> RP_PAD_DPAD_RIGHT
    // The analog triggers (L2=12/R2=13) are NOT digital pad bits — feedInput turns them into RP_AXIS_*_TRIGGER.
    CHECK(rpinput::gcPadButtonForRetroPad(12) == -1);   // JOYPAD_L2
    CHECK(rpinput::gcPadButtonForRetroPad(13) == -1);   // JOYPAD_R2

    // ---- 7. Composite bitmask: holding X + Z + Enter -> pad_buttons = (1<<0)|(1<<1)|(1<<7) = 0x83 (hand-comp). -
    {
        unsigned mask = 0;
        for (int k : { Qt::Key_X, Qt::Key_Z, Qt::Key_Return }) {
            const int bit = rpinput::gcPadButtonForQtKey(k);
            CHECK(bit >= 0);
            mask |= (1u << bit);
        }
        CHECK(mask == 0x83u);   // 1 | 2 | 128
    }

    // ---- 8. Stick-Y convention flip: SDL down-positive -> ABI up-positive (X passes through elsewhere). --------
    CHECK(rpinput::gcStickY(1000)   == -1000);
    CHECK(rpinput::gcStickY(-1000)  ==  1000);
    CHECK(rpinput::gcStickY(0)      ==  0);
    CHECK(rpinput::gcStickY(-32768) ==  32767);   // clamp: -(-32768) would overflow int16, pinned to +32767
    CHECK(rpinput::gcStickY(32767)  == -32767);

    // ---- 9. Start+Select exit-combo rising-edge / debounce. Fires ONCE per press; hand-traced truth table, so the
    // expected sequence is an independent oracle (never read back from the function under test). Each step also
    // checks prevHeld was updated to the current "both held" state. ----------------------------------------------
    {
        bool prev = false;
        CHECK(!rpinput::exitComboRising(false, prev)); CHECK(prev == false);  // idle: not held -> no fire
        CHECK( rpinput::exitComboRising(true,  prev)); CHECK(prev == true);   // press: rising edge -> fires once
        CHECK(!rpinput::exitComboRising(true,  prev)); CHECK(prev == true);   // held:  debounced -> no re-fire
        CHECK(!rpinput::exitComboRising(true,  prev)); CHECK(prev == true);   // still held -> no re-fire
        CHECK(!rpinput::exitComboRising(false, prev)); CHECK(prev == false);  // release -> no fire, state clears
        CHECK( rpinput::exitComboRising(true,  prev)); CHECK(prev == true);   // re-press -> fires again
    }
    // One button alone (bothHeld=false) never fires and leaves prevHeld cleared, even from a held prior state.
    { bool prev = true;  CHECK(!rpinput::exitComboRising(false, prev)); CHECK(prev == false); }
    { bool prev = false; CHECK(!rpinput::exitComboRising(false, prev)); CHECK(prev == false); }

    if (failures == 0) {
        std::printf("PROBE probe_retropark_input PASSED\n");
        std::printf("RETROPARK-INPUT-OK\n");
        return 0;
    }
    std::fprintf(stderr, "PROBE probe_retropark_input FAILED (%d checks)\n", failures);
    return 1;
}
