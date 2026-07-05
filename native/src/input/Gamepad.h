// Physical game-controller input via SDL2, mapped to the libretro RetroPad. One player (port 0) for now.
// Hot-plug aware: a controller connected after launch is picked up automatically. When SDL2 isn't
// compiled in (MYMEDIAVAULT_HAVE_SDL undefined), every method is a no-op and available() is false, so the
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

    bool available() const { return initialized_; }    // SDL initialised OK
    bool connected() const { return connectedCount() > 0; }
    bool portConnected(unsigned port) const;           // a controller occupies this player port
    int  connectedCount() const;                       // number of controllers currently open
    std::string name(unsigned port = 0) const;         // a port's controller name (for the remap UI)

    void poll();   // call once per frame, before reading state (also handles connect/disconnect)

    // Digital RetroPad button for a player port. id is a RETRO_DEVICE_ID_JOYPAD_* value. The d-pad also
    // responds to the left analog stick past a deadzone, so stick-only pads still drive d-pad games.
    bool button(unsigned port, unsigned retroId) const;

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
    void openControllers();              // fill empty player slots from attached controllers
    void closeAll();
    int  firstConnectedPort() const;     // lowest port with a controller, or -1
    void loadMapping();

    bool initialized_ = false;
    void* slots_[kMaxPlayers] = { nullptr, nullptr, nullptr, nullptr }; // SDL_GameController* per port
    int instanceIds_[kMaxPlayers] = { -1, -1, -1, -1 };                 // SDL instance id per port
    int map_[kMaxPlayers][kRetroPadButtons]; // [port][RetroPad id] -> binding code (one profile per player)

    uint16_t rumbleStrong_[kMaxPlayers] = { 0, 0, 0, 0 }; // current low-freq motor target per port
    uint16_t rumbleWeak_[kMaxPlayers]   = { 0, 0, 0, 0 }; // current high-freq motor target per port
    bool rumbleActive_[kMaxPlayers]     = { false, false, false, false };
};
