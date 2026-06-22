#include "Keymap.h"
#include "libretro.h" // RETRO_DEVICE_ID_JOYPAD_*
#include "../core/Settings.h"
#include <QKeySequence>

Keymap::Keymap() { load(); }

int Keymap::defaultKey(unsigned port, unsigned retroId)
{
    if (port != 0) return kUnbound; // only player 1 gets a default keyboard layout
    // Matches the original hardcoded RetroArch-style layout. L2/R2/L3/R3 are unbound by default.
    switch (retroId)
    {
    case RETRO_DEVICE_ID_JOYPAD_UP:     return Qt::Key_Up;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return Qt::Key_Down;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return Qt::Key_Left;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return Qt::Key_Right;
    case RETRO_DEVICE_ID_JOYPAD_B:      return Qt::Key_Z;
    case RETRO_DEVICE_ID_JOYPAD_A:      return Qt::Key_X;
    case RETRO_DEVICE_ID_JOYPAD_Y:      return Qt::Key_A;
    case RETRO_DEVICE_ID_JOYPAD_X:      return Qt::Key_S;
    case RETRO_DEVICE_ID_JOYPAD_L:      return Qt::Key_Q;
    case RETRO_DEVICE_ID_JOYPAD_R:      return Qt::Key_W;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return Qt::Key_Backspace;
    case RETRO_DEVICE_ID_JOYPAD_START:  return Qt::Key_Return;
    default:                            return kUnbound;
    }
}

void Keymap::load()
{
    for (int p = 0; p < kPlayers; ++p)
        for (int id = 0; id < kRetroPadButtons; ++id)
            map_[p][id] = Settings::keyBinding(p, id, defaultKey(p, id));
}

void Keymap::reload() { load(); }

int Keymap::key(unsigned port, unsigned retroId) const
{
    return (port < kPlayers && retroId < kRetroPadButtons) ? map_[port][retroId] : kUnbound;
}

void Keymap::setKey(unsigned port, unsigned retroId, int qtKey)
{
    if (port >= kPlayers || retroId >= kRetroPadButtons) return;
    // A key can only drive one button within a given player's profile: clear it from any other first.
    if (qtKey != kUnbound)
        for (int id = 0; id < kRetroPadButtons; ++id)
            if (map_[port][id] == qtKey && static_cast<unsigned>(id) != retroId)
            {
                map_[port][id] = kUnbound;
                Settings::setKeyBinding(port, id, kUnbound);
            }
    map_[port][retroId] = qtKey;
    Settings::setKeyBinding(static_cast<int>(port), static_cast<int>(retroId), qtKey);
}

QString Keymap::labelFor(int qtKey)
{
    if (qtKey == kUnbound) return QStringLiteral("Unbound");
    const QString s = QKeySequence(qtKey).toString(QKeySequence::NativeText);
    return s.isEmpty() ? QStringLiteral("Key %1").arg(qtKey) : s;
}
