// Physical game-controller input via SDL2, mapped to the libretro RetroPad. One player (port 0) for now.
// Hot-plug aware: a controller connected after launch is picked up automatically. When SDL2 isn't
// compiled in (EVERYTHINGBOX_HAVE_SDL undefined), every method is a no-op and available() is false, so the
// rest of the app needs no #ifdefs.
#pragma once
#include <cstdint>
#include <string>

class Gamepad
{
public:
    // Binding codes stored per RetroPad button. Non-negative codes are SDL_GameControllerButton values
    // (A=0, B=1, X=2, Y=3, Back=4, Guide=5, Start=6, LStick=7, RStick=8, LShoulder=9, RShoulder=10,
    // DPadUp=11, Down=12, Left=13, Right=14). The two triggers are analog axes, so they get sentinels.
    static constexpr int kUnbound      = -1;
    static constexpr int kTriggerLeft  = 1000;
    static constexpr int kTriggerRight = 1001;
    static constexpr int kRetroPadButtons = 16; // RETRO_DEVICE_ID_JOYPAD_* are 0..15
    static constexpr int kMaxPlayers = 4;        // controllers map to player ports 0..3

    Gamepad();
    ~Gamepad();
    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;

    // SDL initialised OK. This becomes true ASYNCHRONOUSLY, shortly after construction — see the input-thread
    // note below. Callers must already cope with "no controller yet" (that is the hot-plug case), so a pad that
    // arrives a moment late is the same situation as one plugged in a moment late.
    bool available() const;
    bool connected() const { return connectedCount() > 0; }
    bool portConnected(unsigned port) const;           // a controller occupies this player port
    int  connectedCount() const;                       // number of controllers currently open
    std::string name(unsigned port = 0) const;         // a port's controller name (for the remap UI)

    void poll();   // call once per frame, before reading state (also handles connect/disconnect)

    // Some HID keyboards (e.g. Keychron HE) expose a game-controller interface, so SDL treats the keyboard as a
    // controller and it can grab the first device slot — an emulator bound to "SDL-0" then listens to the keyboard
    // instead of the real pad. Returns the VID/PID list (SDL *_IGNORE_DEVICES format, e.g. "0x3434/0x0e20") of
    // connected "controllers" SDL can't identify as a real type, but only when a properly recognized controller is
    // also present, so a lone unrecognized third-party pad is never suppressed. Empty when there's nothing to skip.
    std::string phantomControllerIgnoreList() const;

    // Diagnostic: one line describing every joystick SDL currently enumerates (index, name, vid/pid, whether SDL
    // treats it as a game controller, and its controller type). Logged at startup to debug device-mapping issues.
    std::string describeControllers() const;

    // How the controller on this port spells its buttons: "xbox" | "playstation" | "switch" | "generic".
    // Derived from SDL_GameControllerGetType; an unrecognised or absent pad is "generic" (Xbox spelling,
    // the de-facto lingua franca in frontends). Used by InputMode to label on-screen hints.
    std::string brand(unsigned port = 0) const;

    // Digital RetroPad button for a player port. id is a RETRO_DEVICE_ID_JOYPAD_* value. The d-pad also
    // responds to the left analog stick past a deadzone (stickAsDpad, default true) so stick-only pads still
    // drive d-pad games. Pass stickAsDpad=false for cores where the main stick is a SEPARATE analog control
    // (GameCube/N64): otherwise a deflected stick spuriously presses the d-pad (e.g. Melee up-taunt on stick-up).
    bool button(unsigned port, unsigned retroId, bool stickAsDpad = true) const;

    // Analog stick for a player port: index is RETRO_DEVICE_INDEX_ANALOG_LEFT/RIGHT, id is
    // RETRO_DEVICE_ID_ANALOG_X/Y. Returns the SDL axis value (-32768..32767), already in libretro's range.
    int16_t axis(unsigned port, unsigned index, unsigned id) const;

    // Rumble: effect 0 = strong (low-freq) motor, 1 = weak (high-freq); strength 0..65535. Persists until
    // changed (refreshed in poll()). stopRumble() silences all motors (call when emulation stops).
    void setRumble(unsigned port, unsigned effect, uint16_t strength);
    void stopRumble();

    // ---- remapping (per player port: each port has its own button profile) ----
    int  binding(unsigned port, unsigned retroId) const;          // current code for a RetroPad button
    void setBinding(unsigned port, unsigned retroId, int code);   // update live + persist to Settings
    static int defaultBinding(unsigned retroId);                  // factory mapping (same for every port)
    void reloadMapping();                                         // re-read all bindings from Settings
    int  anyPressed(unsigned preferredPort = 0) const;            // a button/trigger held now (preferring
                                                                  // that port's controller), or kUnbound
    static std::string labelFor(int code);                        // human label for a binding code

private:
    int  firstConnectedPort() const;     // lowest port with a controller, or -1
    void loadMapping();

    // Every SDL call lives on a dedicated input thread owned by Impl, and the accessors above read a snapshot
    // it publishes. The reason is not tidiness: SDL_Init(SDL_INIT_GAMECONTROLLER) enumerates HID devices, and
    // one that does not answer blocks for TENS OF SECONDS. It used to run in this constructor, on the GUI
    // thread, before the window was shown, and turned a 1.4s startup into 41s (measured: startup.settings
    // 178ms -> 30,121ms, with the process burning 0.00s of CPU throughout, i.e. a pure blocking wait).
    // Construction is now instant, and Gamepad.cpp's run() also keeps the pad itself from arriving late by
    // trying SDL's modern backends before its slow legacy one.
    struct Impl;
    Impl* impl_ = nullptr;               // owns the input thread + the published snapshot; null without SDL

    int map_[kMaxPlayers][kRetroPadButtons]; // [port][RetroPad id] -> binding code (one profile per player)
};
