#include "Gamepad.h"
#include "libretro.h" // RETRO_DEVICE_ID_JOYPAD_*, RETRO_DEVICE_*_ANALOG_*
#include "../core/Settings.h"

#ifdef MYMEDIAVAULT_HAVE_SDL
#define SDL_MAIN_HANDLED   // we never let SDL take over main()
#include <SDL.h>

static const int kStickDeadzone = 8000; // ~25% of full scale; for d-pad emulation from the left stick

Gamepad::Gamepad()
{
    loadMapping();
    SDL_SetMainReady();
    // Keep delivering controller events even when the app window isn't focused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Qt owns the Windows message loop. Without this, SDL_PumpEvents (called every poll) does its own
    // PeekMessage(NULL) and dispatches ALL of the thread's window messages — including Qt's — which fights
    // Qt's event delivery and can leave a just-created QQuickView (the themed home, rebuilt when leaving
    // Settings) painting black. Disabling SDL's message loop lets Qt dispatch; SDL still gets its device
    // notifications through Qt's dispatch, so hot-plug keeps working.
    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "0");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) == 0)
    {
        initialized_ = true;
        // Load the bundled SDL_GameControllerDB (community mappings) so uncommon / third-party pads map to the
        // standard layout, à la EmulationStation / RetroBat. Best-effort: SDL keeps its built-in defaults if
        // the file is missing, and any entry here augments/overrides them for a device.
        if (char* base = SDL_GetBasePath())
        {
            const std::string db = std::string(base) + "gamecontrollerdb.txt";
            SDL_free(base);
            SDL_GameControllerAddMappingsFromFile(db.c_str()); // -1 if absent — harmless
        }
        openControllers();
    }
}

Gamepad::~Gamepad()
{
    if (!initialized_) return;
    closeAll();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_Quit();
}

int Gamepad::connectedCount() const
{
    int n = 0;
    for (void* s : slots_) if (s) ++n;
    return n;
}

bool Gamepad::portConnected(unsigned port) const
{
    return port < kMaxPlayers && slots_[port] != nullptr;
}

int Gamepad::firstConnectedPort() const
{
    for (int p = 0; p < kMaxPlayers; ++p) if (slots_[p]) return p;
    return -1;
}

std::string Gamepad::name(unsigned port) const
{
    if (port >= kMaxPlayers || !slots_[port]) return {};
    const char* n = SDL_GameControllerName(static_cast<SDL_GameController*>(slots_[port]));
    return n ? n : "";
}

static bool instanceAlreadyOpen(const int* ids, int n, int instanceId)
{
    for (int i = 0; i < n; ++i) if (ids[i] == instanceId) return true;
    return false;
}

void Gamepad::openControllers()
{
    // Assign each not-yet-open controller to the lowest free player port (0..3).
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
    {
        if (!SDL_IsGameController(i)) continue;
        if (instanceAlreadyOpen(instanceIds_, kMaxPlayers, SDL_JoystickGetDeviceInstanceID(i))) continue;

        int slot = -1;
        for (int p = 0; p < kMaxPlayers; ++p) if (!slots_[p]) { slot = p; break; }
        if (slot < 0) break; // every player port is occupied

        SDL_GameController* gc = SDL_GameControllerOpen(i);
        if (!gc) continue;
        slots_[slot] = gc;
        instanceIds_[slot] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    }
}

void Gamepad::closeAll()
{
    for (int p = 0; p < kMaxPlayers; ++p)
        if (slots_[p])
        {
            SDL_GameControllerClose(static_cast<SDL_GameController*>(slots_[p]));
            slots_[p] = nullptr;
            instanceIds_[p] = -1;
        }
}

std::string Gamepad::phantomControllerIgnoreList() const
{
    if (!initialized_) return {};
    // Scan EVERY joystick, not just the ones we opened: a keyboard exposing a gamepad HID interface (Keychron HE)
    // is a joystick SDL2 won't map as a game controller (isGC=0), so we never open it — but an emulator's newer
    // SDL still treats it as a controller and it can steal the SDL-0 slot from the real pad. A "real" controller
    // is one SDL2 maps AND recognizes the type of (DualSense => PS5). When at least one real controller is present,
    // return the VID/PID of everything else so the emulator ignores it; otherwise suppress nothing (a lone
    // unrecognized pad must keep working).
    std::string suspects;
    bool haveRealController = false;
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; ++i)
    {
        if (SDL_IsGameController(i) && SDL_GameControllerTypeForIndex(i) != SDL_CONTROLLER_TYPE_UNKNOWN)
        { haveRealController = true; continue; }
        const Uint16 vid = SDL_JoystickGetDeviceVendor(i);
        const Uint16 pid = SDL_JoystickGetDeviceProduct(i);
        if (!vid && !pid) continue; // no identity to match on — leave it alone
        char buf[24];
        SDL_snprintf(buf, sizeof(buf), "0x%04x/0x%04x", vid, pid);
        if (!suspects.empty()) suspects += ",";
        suspects += buf;
    }
    return haveRealController ? suspects : std::string();
}

std::string Gamepad::describeControllers() const
{
    if (!initialized_) return "gamepad: SDL not initialized";
    std::string out = "gamepad: SDL2 enumerates:";
    const int n = SDL_NumJoysticks();
    if (n == 0) return out + " (no joysticks)";
    for (int i = 0; i < n; ++i)
    {
        const bool isGC = SDL_IsGameController(i);
        const char* nm = isGC ? SDL_GameControllerNameForIndex(i) : SDL_JoystickNameForIndex(i);
        const Uint16 vid = SDL_JoystickGetDeviceVendor(i);
        const Uint16 pid = SDL_JoystickGetDeviceProduct(i);
        const int type = isGC ? static_cast<int>(SDL_GameControllerTypeForIndex(i)) : -1;
        char line[256];
        SDL_snprintf(line, sizeof(line), " [%d name='%s' vid=0x%04x pid=0x%04x isGC=%d type=%d]",
                     i, nm ? nm : "?", vid, pid, isGC ? 1 : 0, type);
        out += line;
    }
    return out;
}

void Gamepad::poll()
{
    if (!initialized_) return;

    // Drain SDL's event queue so hot-plug is noticed and controller state stays fresh.
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        switch (e.type)
        {
        case SDL_CONTROLLERDEVICEADDED:
            openControllers(); // new pad -> lowest free player port
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (int p = 0; p < kMaxPlayers; ++p)
                if (instanceIds_[p] == e.cdevice.which)
                {
                    SDL_GameControllerClose(static_cast<SDL_GameController*>(slots_[p]));
                    slots_[p] = nullptr;
                    instanceIds_[p] = -1; // leave the port empty; other players keep their ports
                }
            break;
        default:
            break;
        }
    }
    SDL_GameControllerUpdate();

    // Keep rumble alive: libretro rumble persists until changed, but SDL rumble auto-expires, so re-issue
    // it each poll while non-zero, and silence it once when it drops to zero.
    for (int p = 0; p < kMaxPlayers; ++p)
    {
        if (!slots_[p]) continue;
        auto* gc = static_cast<SDL_GameController*>(slots_[p]);
        if (rumbleStrong_[p] || rumbleWeak_[p])
        {
            SDL_GameControllerRumble(gc, rumbleStrong_[p], rumbleWeak_[p], 250);
            rumbleActive_[p] = true;
        }
        else if (rumbleActive_[p])
        {
            SDL_GameControllerRumble(gc, 0, 0, 0);
            rumbleActive_[p] = false;
        }
    }
}

void Gamepad::setRumble(unsigned port, unsigned effect, uint16_t strength)
{
    if (port >= kMaxPlayers) return;
    if (effect == 0) rumbleStrong_[port] = strength; // RETRO_RUMBLE_STRONG
    else             rumbleWeak_[port]   = strength; // RETRO_RUMBLE_WEAK
}

void Gamepad::stopRumble()
{
    for (int p = 0; p < kMaxPlayers; ++p)
    {
        rumbleStrong_[p] = rumbleWeak_[p] = 0;
        if (slots_[p]) SDL_GameControllerRumble(static_cast<SDL_GameController*>(slots_[p]), 0, 0, 0);
        rumbleActive_[p] = false;
    }
}

// Is a binding code (SDL button index or trigger sentinel) currently held on this controller?
static bool codePressed(SDL_GameController* gc, int code)
{
    if (code == Gamepad::kTriggerLeft)  return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16384;
    if (code == Gamepad::kTriggerRight) return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384;
    if (code >= 0 && code < SDL_CONTROLLER_BUTTON_MAX)
        return SDL_GameControllerGetButton(gc, static_cast<SDL_GameControllerButton>(code)) != 0;
    return false;
}

bool Gamepad::button(unsigned port, unsigned retroId) const
{
    if (port >= kMaxPlayers || !slots_[port] || retroId >= kRetroPadButtons) return false;
    auto* gc = static_cast<SDL_GameController*>(slots_[port]);

    if (codePressed(gc, map_[port][retroId])) return true;

    // The left analog stick also drives the d-pad, regardless of how the d-pad is bound.
    switch (retroId)
    {
    case RETRO_DEVICE_ID_JOYPAD_UP:    return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) < -kStickDeadzone;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:  return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) >  kStickDeadzone;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:  return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) < -kStickDeadzone;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) >  kStickDeadzone;
    default:                           return false;
    }
}

int Gamepad::anyPressed(unsigned preferredPort) const
{
    // Capture from the requested port's controller when present, else from whichever is connected.
    int port = (preferredPort < kMaxPlayers && slots_[preferredPort]) ? int(preferredPort) : firstConnectedPort();
    if (port < 0) return kUnbound;
    auto* gc = static_cast<SDL_GameController*>(slots_[port]);
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
    {
        if (b == SDL_CONTROLLER_BUTTON_GUIDE) continue; // don't let the system/Guide button be bound
        if (SDL_GameControllerGetButton(gc, static_cast<SDL_GameControllerButton>(b))) return b;
    }
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16384) return kTriggerLeft;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) return kTriggerRight;
    return kUnbound;
}

int16_t Gamepad::axis(unsigned port, unsigned index, unsigned id) const
{
    if (port >= kMaxPlayers || !slots_[port]) return 0;
    auto* gc = static_cast<SDL_GameController*>(slots_[port]);

    SDL_GameControllerAxis a = SDL_CONTROLLER_AXIS_INVALID;
    if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
        a = (id == RETRO_DEVICE_ID_ANALOG_X) ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_LEFTY;
    else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
        a = (id == RETRO_DEVICE_ID_ANALOG_X) ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_RIGHTY;
    else
        return 0;

    return SDL_GameControllerGetAxis(gc, a);
}

#else // ---- SDL2 not compiled in: gamepad is inert ----

Gamepad::Gamepad() { loadMapping(); }
Gamepad::~Gamepad() {}
int Gamepad::connectedCount() const { return 0; }
bool Gamepad::portConnected(unsigned) const { return false; }
int Gamepad::firstConnectedPort() const { return -1; }
std::string Gamepad::name(unsigned) const { return {}; }
void Gamepad::openControllers() {}
void Gamepad::closeAll() {}
std::string Gamepad::phantomControllerIgnoreList() const { return {}; }
std::string Gamepad::describeControllers() const { return "gamepad: built without SDL"; }
void Gamepad::poll() {}
bool Gamepad::button(unsigned, unsigned) const { return false; }
int16_t Gamepad::axis(unsigned, unsigned, unsigned) const { return 0; }
int Gamepad::anyPressed(unsigned) const { return kUnbound; }
void Gamepad::setRumble(unsigned, unsigned, uint16_t) {}
void Gamepad::stopRumble() {}

#endif

// ---- mapping: independent of SDL (codes are plain ints; labels are fixed strings) ----

int Gamepad::defaultBinding(unsigned retroId)
{
    // SDL_CONTROLLER_BUTTON_* numeric values (stable ABI): A0 B1 X2 Y3 Back4 Start6 LStick7 RStick8
    // LShoulder9 RShoulder10 DPadUp11 Down12 Left13 Right14.
    switch (retroId)
    {
    case RETRO_DEVICE_ID_JOYPAD_B:      return 0;   // SDL A (south)
    case RETRO_DEVICE_ID_JOYPAD_A:      return 1;   // SDL B (east)
    case RETRO_DEVICE_ID_JOYPAD_Y:      return 2;   // SDL X (west)
    case RETRO_DEVICE_ID_JOYPAD_X:      return 3;   // SDL Y (north)
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return 4;   // Back
    case RETRO_DEVICE_ID_JOYPAD_START:  return 6;   // Start
    case RETRO_DEVICE_ID_JOYPAD_L3:     return 7;   // Left stick click
    case RETRO_DEVICE_ID_JOYPAD_R3:     return 8;   // Right stick click
    case RETRO_DEVICE_ID_JOYPAD_L:      return 9;   // Left shoulder
    case RETRO_DEVICE_ID_JOYPAD_R:      return 10;  // Right shoulder
    case RETRO_DEVICE_ID_JOYPAD_UP:     return 11;  // D-pad up
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return 12;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return 13;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return 14;
    case RETRO_DEVICE_ID_JOYPAD_L2:     return kTriggerLeft;
    case RETRO_DEVICE_ID_JOYPAD_R2:     return kTriggerRight;
    default:                            return kUnbound;
    }
}

std::string Gamepad::labelFor(int code)
{
    switch (code)
    {
    case kUnbound:      return "Unbound";
    case 0:             return "A (South)";
    case 1:             return "B (East)";
    case 2:             return "X (West)";
    case 3:             return "Y (North)";
    case 4:             return "Back";
    case 5:             return "Guide";
    case 6:             return "Start";
    case 7:             return "Left Stick";
    case 8:             return "Right Stick";
    case 9:             return "Left Shoulder";
    case 10:            return "Right Shoulder";
    case 11:            return "D-Pad Up";
    case 12:            return "D-Pad Down";
    case 13:            return "D-Pad Left";
    case 14:            return "D-Pad Right";
    case kTriggerLeft:  return "Left Trigger";
    case kTriggerRight: return "Right Trigger";
    default:            return "Button " + std::to_string(code);
    }
}

int Gamepad::binding(unsigned port, unsigned retroId) const
{
    return (port < kMaxPlayers && retroId < kRetroPadButtons) ? map_[port][retroId] : kUnbound;
}

void Gamepad::setBinding(unsigned port, unsigned retroId, int code)
{
    if (port >= kMaxPlayers || retroId >= kRetroPadButtons) return;
    map_[port][retroId] = code;
    Settings::setPadBinding(static_cast<int>(port), static_cast<int>(retroId), code);
}

void Gamepad::loadMapping()
{
    for (int p = 0; p < kMaxPlayers; ++p)
        for (int id = 0; id < kRetroPadButtons; ++id)
            map_[p][id] = Settings::padBinding(p, id, defaultBinding(id));
}

void Gamepad::reloadMapping() { loadMapping(); }
