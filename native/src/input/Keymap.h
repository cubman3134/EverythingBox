#pragma once
#include <QString>

// Keyboard -> RetroPad mapping, per player port, persisted via Settings (keys "kbd/<port>/<retroId>" =
// Qt key code). Mirrors Gamepad's per-port remappable model so the same UI can edit both. A binding of
// kUnbound (0) means "no key". Player 1 (port 0) ships with a default layout; ports 1-3 start unbound.
class Keymap
{
public:
    static constexpr int kPlayers = 4;
    static constexpr int kRetroPadButtons = 16; // RETRO_DEVICE_ID_JOYPAD_* are 0..15
    static constexpr int kUnbound = 0;           // Qt::Key 0 == no key

    Keymap();

    int  key(unsigned port, unsigned retroId) const;          // Qt key bound to (port, button)
    void setKey(unsigned port, unsigned retroId, int qtKey);   // update live + persist
    static int defaultKey(unsigned port, unsigned retroId);    // factory binding (only port 0 has defaults)
    void reload();                                             // re-read from Settings
    static QString labelFor(int qtKey);                        // human label for a key code

private:
    void load();
    int map_[kPlayers][kRetroPadButtons]; // [port][retroId] -> Qt key code
};
