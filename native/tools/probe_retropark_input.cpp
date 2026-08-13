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

    if (failures == 0) {
        std::printf("PROBE probe_retropark_input PASSED\n");
        std::printf("RETROPARK-INPUT-OK\n");
        return 0;
    }
    std::fprintf(stderr, "PROBE probe_retropark_input FAILED (%d checks)\n", failures);
    return 1;
}
