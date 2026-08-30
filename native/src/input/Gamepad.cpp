#include "Gamepad.h"
#include "libretro.h" // RETRO_DEVICE_ID_JOYPAD_*, RETRO_DEVICE_*_ANALOG_*
#include "../core/Settings.h"

#ifdef EVERYTHINGBOX_HAVE_SDL
#define SDL_MAIN_HANDLED   // we never let SDL take over main()
#include <SDL.h>

#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <atomic>
#include <cstring>

static const int kStickDeadzone = 8000; // ~25% of full scale; for d-pad emulation from the left stick

// ----------------------------------------------------------------------------------------------------------
// The input thread. SDL is initialised, polled and torn down HERE and nowhere else; no other thread ever calls
// into SDL. Two reasons, one of them load-bearing:
//
//  * SDL_Init(SDL_INIT_GAMECONTROLLER) enumerates HID devices, and an unresponsive one blocks for TENS OF
//    SECONDS. This used to run in the Gamepad constructor — i.e. on the GUI thread, inside MainWindow's
//    constructor, before the window was ever shown. A paired-but-idle Bluetooth DualSense turned a 1.4s
//    startup into 41s (startup.settings 178ms -> 30,121ms, with the process burning 0.00s of CPU the whole
//    time: a pure blocking wait). Off the GUI thread, that same dead device costs only a late-arriving pad.
//
//  * It lets SDL pump its OWN Win32 message queue again. On the GUI thread we must set
//    SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP=0, because SDL's PeekMessage(NULL) there dispatches Qt's messages
//    too and fights Qt's event delivery (it could leave a just-created QQuickView painting black). On a thread
//    SDL has to itself there is no Qt loop to fight, so the hint goes back to "1" and SDL receives its
//    device-arrival notifications directly instead of depending on Qt happening to dispatch for it.
//
// Consumers never touch SDL: the thread publishes a plain snapshot struct, guarded by one mutex. At 60fps
// that is a few hundred uncontended lock/unlock pairs a second, far cheaper than the SDL calls it replaces.
// ----------------------------------------------------------------------------------------------------------
struct Gamepad::Impl : QThread
{
    struct Snapshot
    {
        bool        connected[kMaxPlayers] = {};
        uint32_t    buttons[kMaxPlayers]   = {};   // bit N set = SDL_CONTROLLER_BUTTON_N held
        int16_t     axes[kMaxPlayers][SDL_CONTROLLER_AXIS_MAX] = {};
        std::string names[kMaxPlayers];
        int         count = 0;
        std::string describe = "gamepad: SDL not initialized"; // until the first publish()
        std::string phantomIgnore;
    };

    mutable QMutex m;
    Snapshot snap;                              // guarded by m
    uint16_t rumbleStrong[kMaxPlayers] = {};    // guarded by m: written by callers, read by the thread
    uint16_t rumbleWeak[kMaxPlayers]   = {};

    std::atomic<bool> sdlOk{false};             // SDL_Init succeeded (published before ready)
    std::atomic<bool> stopping{false};

    // Touched ONLY by the input thread — never under the mutex, never from a caller.
    SDL_GameController* pads_[kMaxPlayers] = {};   // NB: not "slots" — Qt's moc keywords #define that away
    int  instanceIds[kMaxPlayers]  = { -1, -1, -1, -1 };
    bool rumbleActive[kMaxPlayers] = {};

    void run() override;
    void openControllers();
    void closeAll();
    void publish();

    // ---- snapshot readers (any thread). Deliberately narrow: no accessor copies the whole Snapshot, so a
    // per-frame button read never allocates.
    bool connectedAt(unsigned p) const
    { QMutexLocker lock(&m); return p < kMaxPlayers && snap.connected[p]; }
    int count() const
    { QMutexLocker lock(&m); return snap.count; }
    uint32_t buttonsAt(unsigned p) const
    { QMutexLocker lock(&m); return p < kMaxPlayers ? snap.buttons[p] : 0u; }
    int16_t axisAt(unsigned p, int a) const
    {
        QMutexLocker lock(&m);
        return (p < kMaxPlayers && a >= 0 && a < SDL_CONTROLLER_AXIS_MAX) ? snap.axes[p][a] : int16_t(0);
    }
    std::string nameAt(unsigned p) const
    { QMutexLocker lock(&m); return p < kMaxPlayers ? snap.names[p] : std::string(); }
    std::string describeStr() const
    { QMutexLocker lock(&m); return snap.describe; }
    std::string phantomStr() const
    { QMutexLocker lock(&m); return snap.phantomIgnore; }
    int firstConnected() const
    {
        QMutexLocker lock(&m);
        for (int p = 0; p < kMaxPlayers; ++p) if (snap.connected[p]) return p;
        return -1;
    }

    // Is a binding code (SDL button index or trigger sentinel) currently held, per the published snapshot?
    bool codePressed(unsigned port, uint32_t bits, int code) const
    {
        if (code == Gamepad::kTriggerLeft)  return axisAt(port, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16384;
        if (code == Gamepad::kTriggerRight) return axisAt(port, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384;
        if (code >= 0 && code < SDL_CONTROLLER_BUTTON_MAX) return ((bits >> code) & 1u) != 0;
        return false;
    }
};

void Gamepad::Impl::openControllers()
{
    // Assign each not-yet-open controller to the lowest free player port (0..3).
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
    {
        if (!SDL_IsGameController(i)) continue;
        const int inst = SDL_JoystickGetDeviceInstanceID(i);
        bool alreadyOpen = false;
        for (int p = 0; p < kMaxPlayers; ++p) if (instanceIds[p] == inst) { alreadyOpen = true; break; }
        if (alreadyOpen) continue;

        int slot = -1;
        for (int p = 0; p < kMaxPlayers; ++p) if (!pads_[p]) { slot = p; break; }
        if (slot < 0) break; // every player port is occupied

        SDL_GameController* gc = SDL_GameControllerOpen(i);
        if (!gc) continue;
        pads_[slot] = gc;
        instanceIds[slot] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    }
}

void Gamepad::Impl::closeAll()
{
    for (int p = 0; p < kMaxPlayers; ++p)
        if (pads_[p])
        {
            SDL_GameControllerClose(pads_[p]);
            pads_[p] = nullptr;
            instanceIds[p] = -1;
        }
}

// Rebuild the snapshot from SDL and publish it. Enumeration-derived strings (the diagnostic line and the
// phantom-controller ignore list) are computed HERE, on the thread that owns SDL, so their accessors are
// plain reads rather than cross-thread SDL calls.
void Gamepad::Impl::publish()
{
    Snapshot s;

    s.describe = "gamepad: SDL2 enumerates:";
    const int n = SDL_NumJoysticks();
    if (n == 0) s.describe += " (no joysticks)";

    // A "real" controller is one SDL2 maps AND recognizes the type of (DualSense => PS5). When at least one is
    // present, everything else is a suspect: a keyboard exposing a game-controller HID interface (Keychron HE)
    // is a joystick SDL2 won't map as a game controller, so we never open it — but an emulator's newer SDL can
    // treat it as one and let it steal the SDL-0 slot from the real pad. With no real controller present we
    // suppress nothing, so a lone unrecognized third-party pad keeps working.
    bool haveRealController = false;
    std::string suspects;
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
        s.describe += line;

        if (isGC && SDL_GameControllerTypeForIndex(i) != SDL_CONTROLLER_TYPE_UNKNOWN)
        { haveRealController = true; continue; }
        if (!vid && !pid) continue; // no identity to match on — leave it alone
        char buf[24];
        SDL_snprintf(buf, sizeof(buf), "0x%04x/0x%04x", vid, pid);
        if (!suspects.empty()) suspects += ",";
        suspects += buf;
    }
    s.phantomIgnore = haveRealController ? suspects : std::string();

    for (int p = 0; p < kMaxPlayers; ++p)
    {
        if (!pads_[p]) continue;
        s.connected[p] = true;
        ++s.count;
        const char* nm = SDL_GameControllerName(pads_[p]);
        s.names[p] = nm ? nm : "";

        uint32_t bits = 0;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (SDL_GameControllerGetButton(pads_[p], static_cast<SDL_GameControllerButton>(b)))
                bits |= (1u << b);
        s.buttons[p] = bits;

        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; ++a)
            s.axes[p][a] = SDL_GameControllerGetAxis(pads_[p], static_cast<SDL_GameControllerAxis>(a));
    }

    QMutexLocker lock(&m);
    snap = std::move(s); // rumble targets live outside the snapshot, so they survive a publish
}

void Gamepad::Impl::run()
{
    SDL_SetMainReady();
    // Keep delivering controller events even when the app window isn't focused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // SDL owns this thread, so let it pump its own message queue — there is no Qt event loop here for its
    // PeekMessage to steal from. (See the header comment: on the GUI thread this MUST stay off.)
    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "1");

    // The call that used to freeze startup. Everything below only runs once it returns.
    const bool init = (SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    if (init)
    {
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
        publish();
    }
    sdlOk.store(init, std::memory_order_release); // after publish(): available() must not race ahead of state
    if (!init) return;

    while (!stopping.load(std::memory_order_acquire))
    {
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
                    if (instanceIds[p] == e.cdevice.which)
                    {
                        SDL_GameControllerClose(pads_[p]);
                        pads_[p] = nullptr;
                        instanceIds[p] = -1; // leave the port empty; other players keep their ports
                    }
                break;
            default:
                break;
            }
        }
        SDL_GameControllerUpdate();

        // Keep rumble alive: libretro rumble persists until changed, but SDL rumble auto-expires, so re-issue
        // it each tick while non-zero, and silence it once when it drops to zero.
        uint16_t strong[kMaxPlayers], weak[kMaxPlayers];
        {
            QMutexLocker lock(&m);
            std::memcpy(strong, rumbleStrong, sizeof(strong));
            std::memcpy(weak,   rumbleWeak,   sizeof(weak));
        }
        for (int p = 0; p < kMaxPlayers; ++p)
        {
            if (!pads_[p]) continue;
            if (strong[p] || weak[p])
            {
                SDL_GameControllerRumble(pads_[p], strong[p], weak[p], 250);
                rumbleActive[p] = true;
            }
            else if (rumbleActive[p])
            {
                SDL_GameControllerRumble(pads_[p], 0, 0, 0);
                rumbleActive[p] = false;
            }
        }

        publish();
        SDL_Delay(4); // ~250Hz: finer than any core's frame rate, and far too cheap to notice
    }

    closeAll();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_Quit();
}

// ---- Gamepad: a thin façade over the input thread's snapshot ----

Gamepad::Gamepad()
{
    loadMapping();
    impl_ = new Impl;
    impl_->start();
}

Gamepad::~Gamepad()
{
    if (!impl_) return;
    impl_->stopping.store(true, std::memory_order_release);
    // The thread may still be inside the slow SDL_Init, which cannot be interrupted. Bound the wait so
    // teardown can never hang on a dead device; if it is still stuck, DELIBERATELY leak the Impl rather than
    // free memory the thread is about to touch (or terminate() it while it holds HID handles). The leak is one
    // object, once, on a path that only runs when a device is already misbehaving.
    if (impl_->wait(3000)) delete impl_;
    impl_ = nullptr;
}

bool Gamepad::available() const { return impl_ && impl_->sdlOk.load(std::memory_order_acquire); }

int Gamepad::connectedCount() const { return impl_ ? impl_->count() : 0; }

bool Gamepad::portConnected(unsigned port) const { return impl_ && impl_->connectedAt(port); }

int Gamepad::firstConnectedPort() const { return impl_ ? impl_->firstConnected() : -1; }

std::string Gamepad::name(unsigned port) const { return impl_ ? impl_->nameAt(port) : std::string(); }

std::string Gamepad::phantomControllerIgnoreList() const
{
    return impl_ ? impl_->phantomStr() : std::string();
}

std::string Gamepad::describeControllers() const
{
    return impl_ ? impl_->describeStr() : std::string("gamepad: SDL not initialized");
}

// The input thread polls continuously, so there is nothing left to do here. Kept (rather than removed) so the
// ~10 per-frame call sites in RetroView / RetroParkView / Pad2KeyRuntime / the remap UI need no change, and so
// a caller that polls before reading still reads fresh state.
void Gamepad::poll() {}

void Gamepad::setRumble(unsigned port, unsigned effect, uint16_t strength)
{
    if (!impl_ || port >= kMaxPlayers) return;
    QMutexLocker lock(&impl_->m);
    if (effect == 0) impl_->rumbleStrong[port] = strength; // RETRO_RUMBLE_STRONG
    else             impl_->rumbleWeak[port]   = strength; // RETRO_RUMBLE_WEAK
}

void Gamepad::stopRumble()
{
    if (!impl_) return;
    QMutexLocker lock(&impl_->m);
    for (int p = 0; p < kMaxPlayers; ++p) impl_->rumbleStrong[p] = impl_->rumbleWeak[p] = 0;
    // The motors are actually silenced by the input thread on its next tick (~4ms).
}

bool Gamepad::button(unsigned port, unsigned retroId, bool stickAsDpad) const
{
    if (!impl_ || port >= kMaxPlayers || retroId >= kRetroPadButtons) return false;
    if (!impl_->connectedAt(port)) return false;

    if (impl_->codePressed(port, impl_->buttonsAt(port), map_[port][retroId])) return true;

    // The left analog stick also drives the d-pad, regardless of how the d-pad is bound — but ONLY for cores
    // where the stick and d-pad are one control (NES-style). Callers with a separate main stick (GameCube/N64)
    // pass stickAsDpad=false so a deflected stick doesn't phantom-press the d-pad.
    if (!stickAsDpad) return false;
    switch (retroId)
    {
    case RETRO_DEVICE_ID_JOYPAD_UP:    return impl_->axisAt(port, SDL_CONTROLLER_AXIS_LEFTY) < -kStickDeadzone;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:  return impl_->axisAt(port, SDL_CONTROLLER_AXIS_LEFTY) >  kStickDeadzone;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:  return impl_->axisAt(port, SDL_CONTROLLER_AXIS_LEFTX) < -kStickDeadzone;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return impl_->axisAt(port, SDL_CONTROLLER_AXIS_LEFTX) >  kStickDeadzone;
    default:                           return false;
    }
}

int Gamepad::anyPressed(unsigned preferredPort) const
{
    if (!impl_) return kUnbound;
    // Capture from the requested port's controller when present, else from whichever is connected.
    const int port = (preferredPort < kMaxPlayers && impl_->connectedAt(preferredPort))
                   ? int(preferredPort) : firstConnectedPort();
    if (port < 0) return kUnbound;

    const uint32_t bits = impl_->buttonsAt(unsigned(port));
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
    {
        if (b == SDL_CONTROLLER_BUTTON_GUIDE) continue; // don't let the system/Guide button be bound
        if ((bits >> b) & 1u) return b;
    }
    if (impl_->axisAt(unsigned(port), SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16384) return kTriggerLeft;
    if (impl_->axisAt(unsigned(port), SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) return kTriggerRight;
    return kUnbound;
}

int16_t Gamepad::axis(unsigned port, unsigned index, unsigned id) const
{
    if (!impl_ || port >= kMaxPlayers || !impl_->connectedAt(port)) return 0;

    SDL_GameControllerAxis a = SDL_CONTROLLER_AXIS_INVALID;
    if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
        a = (id == RETRO_DEVICE_ID_ANALOG_X) ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_LEFTY;
    else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
        a = (id == RETRO_DEVICE_ID_ANALOG_X) ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_RIGHTY;
    else
        return 0;

    return impl_->axisAt(port, a);
}

#else // ---- SDL2 not compiled in: gamepad is inert ----

Gamepad::Gamepad() { loadMapping(); }
Gamepad::~Gamepad() {}
bool Gamepad::available() const { return false; }
int Gamepad::connectedCount() const { return 0; }
bool Gamepad::portConnected(unsigned) const { return false; }
int Gamepad::firstConnectedPort() const { return -1; }
std::string Gamepad::name(unsigned) const { return {}; }
std::string Gamepad::phantomControllerIgnoreList() const { return {}; }
std::string Gamepad::describeControllers() const { return "gamepad: built without SDL"; }
void Gamepad::poll() {}
bool Gamepad::button(unsigned, unsigned, bool) const { return false; }
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
