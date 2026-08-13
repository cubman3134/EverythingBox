// Pure, header-only mapping of EverythingBox input (keyboard + physical RetroPad) to the VK-code bytes the
// RetroPark FCEUmm libretro shim reads out of rp_input_state.keys[]. Kept header-only and free of <windows.h>
// and any RetroPark include so it unit-tests in probe_retropark_input with no runtime, GPU, or DLL dependency —
// this is the load-bearing logic of Slice 2b's input feed, so it is the piece that gets asserted + mutation-killed.
//
// The shim's NES button<-key map is fixed in external/RetroPark/cores/libretro_shim/LibretroShim.cpp
// input_state_cb() (verified there, Slice 2b):
//   JOYPAD_UP   = keys[VK_UP]    JOYPAD_DOWN = keys[VK_DOWN]  JOYPAD_LEFT  = keys[VK_LEFT]  JOYPAD_RIGHT = keys[VK_RIGHT]
//   JOYPAD_A    = keys['X']      JOYPAD_B    = keys['Z']      JOYPAD_START = keys[VK_RETURN] JOYPAD_SELECT = keys[VK_SHIFT]
// so the NES A button is the X key and B is the Z key (the usual Z=B / X=A NES layout). The numeric values below
// are the Win32 <windows.h> virtual-key codes the shim indexes keys[] with; we hardcode the numbers (not include
// <windows.h>) so this header stays platform-neutral and testable off-Windows.
#pragma once
#include <QtCore/qnamespace.h>

namespace rpinput {

// Win32 virtual-key codes the shim reads out of rp_input_state.keys[] (values match <windows.h>).
enum : int {
    kVkReturn = 0x0D,   // VK_RETURN  -> NES Start
    kVkShift  = 0x10,   // VK_SHIFT   -> NES Select
    kVkLeft   = 0x25,   // VK_LEFT
    kVkUp     = 0x26,   // VK_UP
    kVkRight  = 0x27,   // VK_RIGHT
    kVkDown   = 0x28,   // VK_DOWN
    kVkX      = 0x58,   // 'X'        -> NES A
    kVkZ      = 0x5A,   // 'Z'        -> NES B
};

// libretro RetroPad button ids (RETRO_DEVICE_ID_JOYPAD_*), spelled out here so this header needs no libretro.h.
enum : unsigned {
    kJoyB      = 0,
    kJoySelect = 2,
    kJoyStart  = 3,
    kJoyUp     = 4,
    kJoyDown   = 5,
    kJoyLeft   = 6,
    kJoyRight  = 7,
    kJoyA      = 8,
};

// Map a Qt key code to the shim VK byte, if it is one of the NES controls. Returns false (vkOut untouched) for
// any other key, so callers leave the pause-menu / system keys (Esc, Back, arrows-in-menu, ...) alone.
inline bool nesVkForQtKey(int qtKey, int& vkOut) {
    switch (qtKey) {
        case Qt::Key_Up:     vkOut = kVkUp;     return true;
        case Qt::Key_Down:   vkOut = kVkDown;   return true;
        case Qt::Key_Left:   vkOut = kVkLeft;   return true;
        case Qt::Key_Right:  vkOut = kVkRight;  return true;
        case Qt::Key_X:      vkOut = kVkX;      return true;  // NES A
        case Qt::Key_Z:      vkOut = kVkZ;      return true;  // NES B
        case Qt::Key_Return:                                  // main Enter ...
        case Qt::Key_Enter:  vkOut = kVkReturn; return true;  // ... and keypad Enter -> Start
        case Qt::Key_Shift:  vkOut = kVkShift;  return true;  // Select
        default:             return false;
    }
}

// Map a physical RetroPad button id (RETRO_DEVICE_ID_JOYPAD_*, from Gamepad::button) to the same shim VK byte,
// so a controller drives the identical NES controls as the keyboard. Returns false for ids the NES lacks.
inline bool nesVkForRetroPad(unsigned retroId, int& vkOut) {
    switch (retroId) {
        case kJoyUp:     vkOut = kVkUp;     return true;
        case kJoyDown:   vkOut = kVkDown;   return true;
        case kJoyLeft:   vkOut = kVkLeft;   return true;
        case kJoyRight:  vkOut = kVkRight;  return true;
        case kJoyA:      vkOut = kVkX;      return true;  // NES A -> 'X'
        case kJoyB:      vkOut = kVkZ;      return true;  // NES B -> 'Z'
        case kJoyStart:  vkOut = kVkReturn; return true;
        case kJoySelect: vkOut = kVkShift;  return true;
        default:         return false;
    }
}

} // namespace rpinput
