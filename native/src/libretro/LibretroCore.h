// A minimal cross-platform libretro frontend: loads a core, runs frames, exposes the video frame.
// No Qt / no engine dependency - the UI layer (mpv window / Qt) consumes frame() each tick.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <functional>
#include "libretro.h"

// A single user-tunable core setting (resolution, BIOS, region, ...), harvested from whichever
// libretro options API the core uses (SET_VARIABLES / SET_CORE_OPTIONS / *_V2 / *_INTL).
struct CoreOption
{
    std::string key;          // stable id the core queries via GET_VARIABLE
    std::string desc;         // human-readable label
    std::string info;         // optional help / tooltip
    std::vector<std::pair<std::string, std::string>> values; // (value, label); values[0] order = menu order
    std::string defaultValue; // one of values[].first
};

class LibretroCore
{
public:
    ~LibretroCore();

    // Load the core .dll/.so/.dylib and resolve its libretro entry points.
    bool loadCore(const std::string& corePath, std::string* error = nullptr);
    // Load content (a ROM). Pass empty for cores that start without content.
    bool loadGame(const std::string& gamePath, std::string* error = nullptr);
    // Advance one frame; the latest video frame is then available via frame()/frameWidth()/...
    void runFrame();
    void unload();

    // Tell the core what device occupies a player port (RETRO_DEVICE_JOYPAD / RETRO_DEVICE_NONE).
    // Used to enable ports 1..3 for multiplayer. No-op if the core doesn't export it.
    void setControllerPortDevice(unsigned port, unsigned device);

    // Save states (libretro serialize API). saveState() fills out with the current state; loadState()
    // restores it. Both return false if the core doesn't support serialization or the call fails.
    bool saveState(std::vector<uint8_t>& out);
    bool loadState(const uint8_t* data, size_t size);

    bool coreLoaded() const { return handle_ != nullptr; }
    bool gameLoaded() const { return gameLoaded_; }
    bool crashed() const { return crashed_; } // a core hard-faulted during runFrame(); stop using it

    // Latest video frame (converted to 32-bit BGRA for easy texture upload).
    const uint8_t* frameBGRA() const { return frame_.empty() ? nullptr : frame_.data(); }
    unsigned frameWidth() const { return frameW_; }
    unsigned frameHeight() const { return frameH_; }
    bool hasFrame() const { return frameW_ != 0 && frameH_ != 0; }

    const retro_system_info& systemInfo() const { return sysInfo_; }
    const retro_system_av_info& avInfo() const { return avInfo_; }

    // Core options the loaded core exposes. Populated during loadCore() for cores that register
    // options in retro_set_environment/retro_init (most of them) - so this is usable headlessly,
    // without a game, to drive a settings UI.
    const std::vector<CoreOption>& options() const { return options_; }
    std::string optionValue(const std::string& key) const;       // current value (or its default)
    void setOptionValue(const std::string& key, const std::string& value); // flags the core to re-read

    // Host directories the core may request.
    std::string systemDir = ".";
    std::string saveDir = ".";
    // Pushed audio (interleaved S16 stereo) - the UI layer drains this to an audio sink.
    std::function<void(const int16_t*, size_t /*frames*/)> onAudio;
    // Polled input - return non-zero when the button id is pressed (RETRO_DEVICE_ID_JOYPAD_*).
    std::function<int16_t(unsigned /*port*/, unsigned /*device*/, unsigned /*index*/, unsigned /*id*/)> onInput;
    // Rumble request from the core: effect is RETRO_RUMBLE_STRONG/WEAK, strength 0..65535 (persists until changed).
    std::function<void(unsigned /*port*/, unsigned /*effect*/, uint16_t /*strength*/)> onRumble;

private:
    void resolve(const char* name, void** fn);

    // ---- core-option registration (one per libretro options API variant) ----
    void addOption(const std::string& key, const std::string& desc, const std::string& info,
                   std::vector<std::pair<std::string, std::string>> values, const std::string& def);
    void registerVariablesLegacy(const retro_variable* vars);                   // SET_VARIABLES
    void registerOptionDefs(const retro_core_option_definition* defs);          // SET_CORE_OPTIONS(_INTL)
    void registerOptionDefsV2(const retro_core_option_v2_definition* defs);     // SET_CORE_OPTIONS_V2(_INTL)

    // ---- core entry points ----
    void (*retro_init_)() = nullptr;
    void (*retro_deinit_)() = nullptr;
    unsigned (*retro_api_version_)() = nullptr;
    void (*retro_get_system_info_)(retro_system_info*) = nullptr;
    void (*retro_get_system_av_info_)(retro_system_av_info*) = nullptr;
    void (*retro_set_environment_)(retro_environment_t) = nullptr;
    void (*retro_set_video_refresh_)(retro_video_refresh_t) = nullptr;
    void (*retro_set_audio_sample_)(retro_audio_sample_t) = nullptr;
    void (*retro_set_audio_sample_batch_)(retro_audio_sample_batch_t) = nullptr;
    void (*retro_set_input_poll_)(retro_input_poll_t) = nullptr;
    void (*retro_set_input_state_)(retro_input_state_t) = nullptr;
    void (*retro_run_)() = nullptr;
    bool (*retro_load_game_)(const retro_game_info*) = nullptr;
    void (*retro_unload_game_)() = nullptr;
    void (*retro_set_controller_port_device_)(unsigned, unsigned) = nullptr;
    size_t (*retro_serialize_size_)() = nullptr;
    bool (*retro_serialize_)(void*, size_t) = nullptr;
    bool (*retro_unserialize_)(const void*, size_t) = nullptr;

    // ---- libretro callbacks (static -> routed to current_) ----
    static LibretroCore* current_;
    static bool environmentCb(unsigned cmd, void* data);
    static void videoRefreshCb(const void* data, unsigned width, unsigned height, size_t pitch);
    static void audioSampleCb(int16_t left, int16_t right);
    static size_t audioBatchCb(const int16_t* data, size_t frames);
    static void inputPollCb();
    static int16_t inputStateCb(unsigned port, unsigned device, unsigned index, unsigned id);
    static bool rumbleSetStateCb(unsigned port, retro_rumble_effect effect, uint16_t strength);

    void* handle_ = nullptr;
    bool gameLoaded_ = false;
    bool crashed_ = false;
    retro_pixel_format pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_system_info sysInfo_{};
    retro_system_av_info avInfo_{};
    std::vector<uint8_t> frame_;   // BGRA8888
    unsigned frameW_ = 0, frameH_ = 0;
    std::vector<uint8_t> gameData_; // kept alive while the game is loaded

    std::vector<CoreOption> options_;             // definitions, in menu order
    std::map<std::string, std::string> optionValues_; // key -> current value (c_str() served to the core)
    bool optionsDirty_ = false;                   // GET_VARIABLE_UPDATE: a value changed since last poll
};
