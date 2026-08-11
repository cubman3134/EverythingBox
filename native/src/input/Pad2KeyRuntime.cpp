#include "Pad2KeyRuntime.h"
#include "Gamepad.h"
#include "libretro.h"   // RETRO_DEVICE_ID_JOYPAD_*, RETRO_DEVICE_*_ANALOG_*

#include <QTimer>
#include <QDebug>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

using namespace pad2key;

namespace {

// The couch player. pad2key is single-player (one keyboard to synthesise), so it reads port 0 — the same port
// Gamepad ships a default profile on.
constexpr unsigned kPort = 0;

// Poll cadence. 8 ms (~125 Hz) keeps synthesised keys responsive; the pad read is cheap and the emulator/game
// owns the screen, so there is no frame budget to protect here.
constexpr int kPollMs = 8;

// Map an abstract digital Control to its RetroPad button id. Stick controls are handled separately (analog).
// Returns -1 for a control that is not a plain digital button.
int retroButton(Control c)
{
    switch (c) {
        case Control::DpadUp:    return RETRO_DEVICE_ID_JOYPAD_UP;
        case Control::DpadDown:  return RETRO_DEVICE_ID_JOYPAD_DOWN;
        case Control::DpadLeft:  return RETRO_DEVICE_ID_JOYPAD_LEFT;
        case Control::DpadRight: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
        case Control::A:         return RETRO_DEVICE_ID_JOYPAD_A;
        case Control::B:         return RETRO_DEVICE_ID_JOYPAD_B;
        case Control::X:         return RETRO_DEVICE_ID_JOYPAD_X;
        case Control::Y:         return RETRO_DEVICE_ID_JOYPAD_Y;
        case Control::L1:        return RETRO_DEVICE_ID_JOYPAD_L;
        case Control::R1:        return RETRO_DEVICE_ID_JOYPAD_R;
        case Control::L2:        return RETRO_DEVICE_ID_JOYPAD_L2;
        case Control::R2:        return RETRO_DEVICE_ID_JOYPAD_R2;
        case Control::Start:     return RETRO_DEVICE_ID_JOYPAD_START;
        case Control::Select:    return RETRO_DEVICE_ID_JOYPAD_SELECT;
        default:                 return -1;   // a stick direction, or Count
    }
}

} // namespace

Pad2KeyRuntime::Pad2KeyRuntime(QObject* parent) : QObject(parent) {}
Pad2KeyRuntime::~Pad2KeyRuntime() { stop(); }

void Pad2KeyRuntime::start(Gamepad* pad, const Profile& profile)
{
    stop();                                        // release anything a previous run left, reset state
    if (!pad || !pad->available() || profile.isEmpty()) return;
    pad_ = pad;
    profile_ = profile;

    // Prime the edge state from the CURRENT frame so a button already held at launch does not read as a fresh
    // press (and a stick already deflected does not fire an arrow). prev_ starts neutral; sampling once with a
    // neutral prev resolves sticks correctly (a deflected stick engages, but we keep it as the baseline).
    pad_->poll();
    prev_ = sample(PadState{});
    active_ = true;

    if (!timer_)
    {
        timer_ = new QTimer(this);
        timer_->setInterval(kPollMs);
        connect(timer_, &QTimer::timeout, this, &Pad2KeyRuntime::tick);
    }
    timer_->start();
}

void Pad2KeyRuntime::stop()
{
    if (timer_) timer_->stop();
    releaseAllHeld();          // the footgun guard: never leave a key down
    prev_ = PadState{};
    profile_ = Profile{};
    pad_ = nullptr;
    active_ = false;
}

void Pad2KeyRuntime::tick()
{
    if (!active_ || !pad_) return;
    pad_->poll();
    const PadState cur = sample(prev_);
    inject(translate(prev_, cur, profile_));
    prev_ = cur;
}

// Read the pad into a PadState. Digital controls come straight off Gamepad::button; stick controls are resolved
// through the pure hysteresis fold (resolveStick), threading the previous frame's engaged bit so the band does
// not chatter. Only controls the profile maps are read/resolved — an unmapped control stays released.
PadState Pad2KeyRuntime::sample(const PadState& prev) const
{
    PadState s;
    if (!pad_) return s;

    for (int i = 0; i < kControlCount; ++i)
    {
        const Control c = static_cast<Control>(i);
        if (!profile_.map.contains(c)) continue;    // nothing to synthesise for this control -> leave released

        const int btn = retroButton(c);
        if (btn >= 0) { s.set(c, pad_->button(kPort, unsigned(btn))); continue; }

        // A stick direction: rectify the raw signed axis to a non-negative magnitude in this direction, then
        // apply the hysteresis fold against the previous engaged bit.
        int mag = 0;
        auto axisVal = [&](unsigned index, unsigned id) { return int(pad_->axis(kPort, index, id)); };
        switch (c) {
            case Control::LStickLeft:  mag = -axisVal(RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X); break;
            case Control::LStickRight: mag =  axisVal(RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X); break;
            case Control::LStickUp:    mag = -axisVal(RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y); break;
            case Control::LStickDown:  mag =  axisVal(RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y); break;
            case Control::RStickLeft:  mag = -axisVal(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X); break;
            case Control::RStickRight: mag =  axisVal(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X); break;
            case Control::RStickUp:    mag = -axisVal(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y); break;
            case Control::RStickDown:  mag =  axisVal(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y); break;
            default: break;
        }
        if (mag < 0) mag = 0;
        s.set(c, resolveStick(mag, prev.get(c)));
    }
    return s;
}

#if defined(Q_OS_WIN)

// Abstract Key -> Windows virtual-key. Returns 0 for Key::None / anything unmapped.
static int vkFor(Key k)
{
    if (k >= Key::A && k <= Key::Z)   return 'A' + (int(k) - int(Key::A));   // VK for letters == ASCII uppercase
    if (k >= Key::D0 && k <= Key::D9) return '0' + (int(k) - int(Key::D0));  // VK for digits  == ASCII digit
    if (k >= Key::F1 && k <= Key::F12) return VK_F1 + (int(k) - int(Key::F1));
    switch (k) {
        case Key::Up:        return VK_UP;
        case Key::Down:      return VK_DOWN;
        case Key::Left:      return VK_LEFT;
        case Key::Right:     return VK_RIGHT;
        case Key::Enter:     return VK_RETURN;
        case Key::Esc:       return VK_ESCAPE;
        case Key::Space:     return VK_SPACE;
        case Key::Tab:       return VK_TAB;
        case Key::Backspace: return VK_BACK;
        case Key::Delete:    return VK_DELETE;
        case Key::Insert:    return VK_INSERT;
        case Key::Home:      return VK_HOME;
        case Key::End:       return VK_END;
        case Key::PageUp:    return VK_PRIOR;
        case Key::PageDown:  return VK_NEXT;
        case Key::Ctrl:      return VK_CONTROL;
        case Key::Shift:     return VK_SHIFT;
        case Key::Alt:       return VK_MENU;
        default:             return 0;
    }
}

// The navigation/edit cluster and arrows are "extended" keys — SendInput needs KEYEVENTF_EXTENDEDKEY so the game
// reads the grey nav keys, not the numpad equivalents.
static bool isExtendedVk(int vk)
{
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT:
            return true;
        default:
            return false;
    }
}

static void sendVk(int vk, bool down)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = WORD(vk);
    in.ki.wScan = WORD(MapVirtualKey(UINT(vk), MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (down ? 0u : KEYEVENTF_KEYUP);
    if (isExtendedVk(vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof(INPUT));
}

void Pad2KeyRuntime::inject(const QVector<KeyEvent>& events)
{
    for (const KeyEvent& e : events)
    {
        const int vk = vkFor(e.key);
        if (vk == 0) continue;
        if (e.down)
        {
            if (heldVk_.contains(vk)) continue;   // already held (two controls -> same key): don't re-press
            heldVk_.insert(vk);
            sendVk(vk, true);
        }
        else
        {
            if (!heldVk_.remove(vk)) continue;    // we don't hold it: nothing to release
            sendVk(vk, false);
        }
    }
}

void Pad2KeyRuntime::releaseAllHeld()
{
    for (int vk : heldVk_) sendVk(vk, false);
    heldVk_.clear();
}

#else   // ---- macOS / Linux: not wired up yet. Degrade to a logged no-op, never block the build. --------------

void Pad2KeyRuntime::inject(const QVector<KeyEvent>& events)
{
    if (events.isEmpty()) return;
    if (!warnedUnsupported_)
    {
        // One line, not once per frame. macOS needs CGEventCreateKeyboardEvent; Linux needs a /dev/uinput device
        // (the user must be in the `input` udev group). Both are follow-ups; today pad2key is Windows-only.
        qWarning() << "pad2key: keystroke synthesis is not available on this platform yet — no keys will be sent";
        warnedUnsupported_ = true;
    }
    // Still track intent so heldVk_ stays coherent and release-on-stop is a no-op rather than out of sync.
    for (const KeyEvent& e : events)
    {
        if (e.down) heldVk_.insert(int(e.key));
        else        heldVk_.remove(int(e.key));
    }
}

void Pad2KeyRuntime::releaseAllHeld()
{
    heldVk_.clear();   // nothing was ever sent, so there is nothing to release
}

#endif
