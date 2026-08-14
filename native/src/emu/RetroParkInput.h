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
// The NES mapper uses only a subset; the full set is listed so the GameCube abstract-pad mapper below can map
// the whole face/shoulder/stick-click cluster a GC pad has.
enum : unsigned {
    kJoyB      = 0,
    kJoyY      = 1,
    kJoySelect = 2,
    kJoyStart  = 3,
    kJoyUp     = 4,
    kJoyDown   = 5,
    kJoyLeft   = 6,
    kJoyRight  = 7,
    kJoyA      = 8,
    kJoyX      = 9,
    kJoyL      = 10,
    kJoyR      = 11,
    kJoyL2     = 12,
    kJoyR2     = 13,
    kJoyL3     = 14,
    kJoyR3     = 15,
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

// ===================================================================================================
// GameCube abstract-pad mapper (Slice 3b) — a PURE analog of the NES keys[] mapper above, for the DOLPHIN
// PRESENTING core. Unlike the NES driven shim (which reads keys[]), Dolphin reads the ABSTRACT PAD out of
// rp_input_state.pad_buttons (a bitmask, bit = 1u<<RP_PAD_x) + pad_axes[] (int16 sticks/triggers). So GC input
// is mapped to those bit indices / axis values, NOT to keys[]. Kept header-only + retropark-ABI-free (the bit
// indices are mirrored here as plain ints) so it unit-tests in probe_retropark_input with no runtime/DLL.
//
// Dolphin's producer maps this abstract pad to a GC controller in
//   external/RetroPark/external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp (SetInputOverrideFunction):
//     Buttons:  A<-RP_PAD_A  B<-RP_PAD_B  X<-RP_PAD_X  Y<-RP_PAD_Y  Z<-RP_PAD_SELECT  Start<-RP_PAD_START
//     Main Stick (analog): X<-pad_axes[RP_AXIS_LEFT_X]   Y<-pad_axes[RP_AXIS_LEFT_Y]   (value/32767, Y UP = +)
//     C-Stick    (analog): X<-pad_axes[RP_AXIS_RIGHT_X]  Y<-pad_axes[RP_AXIS_RIGHT_Y]
//     Triggers:  L<-(RP_PAD_L || LEFT_TRIGGER>0.5)  R<-(RP_PAD_R || RIGHT_TRIGGER>0.5)  +analog L/R
//     D-Pad:     Up/Down/Left/Right <- RP_PAD_DPAD_UP/DOWN/LEFT/RIGHT
// so the GC main stick is the LEFT analog stick (NOT the D-pad) and GC "Z" is RP_PAD_SELECT. Matched exactly here.

// rp_input_state.pad_buttons bit indices (mirror of retropark_abi.h RP_PAD_*). A held button ORs 1u<<index.
enum : int {
    kPadA         = 0,
    kPadB         = 1,
    kPadX         = 2,
    kPadY         = 3,
    kPadL         = 4,   // left shoulder (digital)
    kPadR         = 5,   // right shoulder (digital)
    kPadSelect    = 6,   // -> GC "Z" button
    kPadStart     = 7,
    kPadL3        = 8,
    kPadR3        = 9,
    kPadDpadUp    = 10,
    kPadDpadDown  = 11,
    kPadDpadLeft  = 12,
    kPadDpadRight = 13,
    kPadGuide     = 14,
};
// rp_input_state.pad_axes[] indices (mirror of retropark_abi.h RP_AXIS_*).
enum : int {
    kAxisLeftX        = 0,
    kAxisLeftY        = 1,
    kAxisRightX       = 2,
    kAxisRightY       = 3,
    kAxisLeftTrigger  = 4,
    kAxisRightTrigger = 5,
};
// Full analog deflection. 32767 (not 32768) so Dolphin's value/32767 lands exactly on ±1.0 with no overflow.
enum : int { kAxisFull = 32767 };

// Keyboard scheme for GameCube (documented, refined by feel in Task 6). Arrow keys are NOT here — they drive the
// analog LEFT stick (see gcPadAxisForQtKey); this covers the face/shoulder/start/d-pad DIGITAL controls.
//   X->A  Z->B  S->X  A->Y   (GC face diamond; X=A/Z=B keeps the NES muscle-memory)
//   C->Z(RP_PAD_SELECT)   Q->L   E->R   Enter->Start
//   T/F/G/H -> D-Pad Up/Left/Down/Right  (a non-conflicting cross; the analog stick is on the arrows)
// Returns the abstract-pad BIT INDEX (kPad*), or -1 for any key this scheme does not use (so menu/system keys
// and the arrow keys are left for their own handlers).
inline int gcPadButtonForQtKey(int qtKey) {
    switch (qtKey) {
        case Qt::Key_X:      return kPadA;
        case Qt::Key_Z:      return kPadB;
        case Qt::Key_S:      return kPadX;
        case Qt::Key_A:      return kPadY;
        case Qt::Key_C:      return kPadSelect;   // GC "Z"
        case Qt::Key_Q:      return kPadL;
        case Qt::Key_E:      return kPadR;
        case Qt::Key_Return:                       // main Enter ...
        case Qt::Key_Enter:  return kPadStart;     // ... and keypad Enter -> Start
        case Qt::Key_T:      return kPadDpadUp;
        case Qt::Key_F:      return kPadDpadLeft;
        case Qt::Key_G:      return kPadDpadDown;
        case Qt::Key_H:      return kPadDpadRight;
        default:             return -1;
    }
}

// Map a Qt arrow key to a full-deflection analog LEFT-stick axis+value (GC main stick from the keyboard). Returns
// false for non-arrow keys (axisOut/valueOut untouched). Y is UP-positive per the ABI: Up = +full, Down = -full.
inline bool gcPadAxisForQtKey(int qtKey, int& axisOut, int& valueOut) {
    switch (qtKey) {
        case Qt::Key_Up:    axisOut = kAxisLeftY; valueOut =  kAxisFull; return true;
        case Qt::Key_Down:  axisOut = kAxisLeftY; valueOut = -kAxisFull; return true;
        case Qt::Key_Left:  axisOut = kAxisLeftX; valueOut = -kAxisFull; return true;
        case Qt::Key_Right: axisOut = kAxisLeftX; valueOut =  kAxisFull; return true;
        default:            return false;
    }
}

// Map a physical RetroPad button id (RETRO_DEVICE_ID_JOYPAD_*, from Gamepad::button) to the abstract-pad BIT
// INDEX (kPad*), or -1 for ids the GC pad has no button for. The analog sticks + triggers are handled by axis
// reads in feedInput(), not here (this covers the DIGITAL buttons: face, shoulders, start, "Z", stick-clicks,
// d-pad). RetroPad SELECT -> GC "Z" (RP_PAD_SELECT), matching Dolphin's override.
inline int gcPadButtonForRetroPad(unsigned retroId) {
    switch (retroId) {
        case kJoyA:      return kPadA;
        case kJoyB:      return kPadB;
        case kJoyX:      return kPadX;
        case kJoyY:      return kPadY;
        case kJoyL:      return kPadL;
        case kJoyR:      return kPadR;
        case kJoySelect: return kPadSelect;   // -> GC "Z"
        case kJoyStart:  return kPadStart;
        case kJoyL3:     return kPadL3;
        case kJoyR3:     return kPadR3;
        case kJoyUp:     return kPadDpadUp;
        case kJoyDown:   return kPadDpadDown;
        case kJoyLeft:   return kPadDpadLeft;
        case kJoyRight:  return kPadDpadRight;
        default:         return -1;
    }
}

// Convert a stick Y from the SDL/libretro convention (DOWN = positive, as Gamepad::axis returns) to the abstract
// pad's convention (UP = positive, per the ABI). X passes through unchanged; only Y is negated. -32768 is clamped
// to +32767 so the negation never overflows int16.
inline int gcStickY(int sdlY) { return sdlY <= -32768 ? 32767 : -sdlY; }

// Rising-edge (debounce) detector for the Start+Select "exit combo" — the pad-only route to the pause menu (so a
// controller player can reach Exit with no keyboard). Given whether BOTH buttons are held THIS tick and the
// caller's remembered previous held-state (updated in place), it returns true exactly on the transition from
// not-both-held to both-held, so the combo fires ONCE per press and never every frame it is held down. Pure, with
// the persistent state passed by reference, so it unit-tests with no runtime (probe_retropark_input, section 9).
inline bool exitComboRising(bool bothHeldNow, bool& prevHeld) {
    const bool rising = bothHeldNow && !prevHeld;
    prevHeld = bothHeldNow;
    return rising;
}

// ===================================================================================================
// Pause-menu selection navigation (RetroPark menu pad, mirroring RetroView). Pure index math so the
// controller can walk the pause menu's button list: advance the selection by ONE step over `count`
// entries, wrapping at both ends and skipping any entry for which isEnabled(index) is false (a greyed-out
// button, e.g. Save/Load on the no-ROM refcore fallback). `cur` is the current selection index; `down`==true
// moves to the NEXT entry (d-pad Down), false to the PREVIOUS (d-pad Up). Returns the new index, or `cur`
// unchanged when nothing else is selectable (count<=0, or every OTHER entry disabled). isEnabled is any
// callable int->bool, so this stays header-only and free of Qt/runtime — unit-tested + mutation-killed in
// probe_retropark_input.
template <class EnabledFn>
inline int nextMenuIndex(int cur, int count, bool down, EnabledFn isEnabled) {
    if (count <= 0) return cur;
    const int step = down ? 1 : count - 1;   // (idx + count-1) % count == the previous index (no negatives)
    int idx = cur;
    for (int hops = 0; hops < count; ++hops) {
        idx = (idx + step) % count;
        if (isEnabled(idx)) return idx;      // first ENABLED entry in the step direction
    }
    return cur;                              // no other selectable entry — stay put
}

} // namespace rpinput
