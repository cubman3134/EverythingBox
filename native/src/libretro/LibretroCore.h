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

    // Cheats (libretro cheat API). cheatReset() clears all; cheatSet() enables/updates the code at an index
    // (Game Genie / Action Replay / raw, format depends on the core). Frontend re-applies the whole enabled
    // set after a reset whenever the list changes. No-ops if the core doesn't export the cheat functions.
    void cheatReset();
    void cheatSet(unsigned index, bool enabled, const std::string& code);
    bool supportsCheats() const { return retro_cheat_set_ != nullptr; }

    // Disk control (FDS side-flip, multi-disc PS1, ...). A core registers the interface during load; these
    // wrap it. To change a disk you must eject, set the index, then re-insert. index<count.
    bool hasDiskControl() const { return hasDisk_ && disk_.get_num_images && disk_.set_image_index; }
    unsigned diskCount() const;
    unsigned diskIndex() const;
    bool diskEjected() const;
    void setDiskEject(bool ejected);
    void setDiskIndex(unsigned index);          // takes effect only while ejected
    std::string diskLabel(unsigned index) const; // "" if the core provides no label

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

    // ---- hardware (OpenGL) rendering ----
    // A core that renders with OpenGL calls SET_HW_RENDER; we accept GL/GLES context types and hand it a
    // frontend framebuffer + proc-address resolver, both provided by the Qt/GL layer (RetroView) through the
    // two std::function hooks below. Its frames then arrive as "the FBO is ready" rather than CPU pixels:
    // usesHwRender() is true and takeHwFramePending() reports each ready HW frame for the frontend to read back.
    bool usesHwRender() const { return hwRenderRequested_; }
    const retro_hw_render_callback& hwRenderCallback() const { return hwRender_; }
    bool takeHwFramePending() { bool p = hwFramePending_; hwFramePending_ = false; return p; }
    std::function<uintptr_t()> hwGetFramebuffer;        // -> the frontend FBO id the core should render into
    std::function<void*(const char*)> hwGetProcAddress; // -> a GL entry point (Qt QOpenGLContext::getProcAddress)

    // Emulated memory access (for RetroAchievements). memoryData/Size wrap retro_get_memory_*; memoryMap()
    // returns the core's address-space descriptors (or nullptr if it didn't publish any).
    void* memoryData(unsigned id) const { return retro_get_memory_data_ ? retro_get_memory_data_(id) : nullptr; }
    size_t memorySize(unsigned id) const { return retro_get_memory_size_ ? retro_get_memory_size_(id) : 0; }
    const retro_memory_map* memoryMap() const { return memoryMap_.num_descriptors ? &memoryMap_ : nullptr; }

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
    void (*retro_cheat_reset_)() = nullptr;
    void (*retro_cheat_set_)(unsigned, bool, const char*) = nullptr;
    void* (*retro_get_memory_data_)(unsigned) = nullptr;
    size_t (*retro_get_memory_size_)(unsigned) = nullptr;

    // ---- libretro callbacks (static -> routed to current_) ----
    static LibretroCore* current_;
    static bool environmentCb(unsigned cmd, void* data);
    static void videoRefreshCb(const void* data, unsigned width, unsigned height, size_t pitch);
    static void audioSampleCb(int16_t left, int16_t right);
    static size_t audioBatchCb(const int16_t* data, size_t frames);
    static void inputPollCb();
    static int16_t inputStateCb(unsigned port, unsigned device, unsigned index, unsigned id);
    static bool rumbleSetStateCb(unsigned port, retro_rumble_effect effect, uint16_t strength);
    static uintptr_t hwGetCurrentFramebufferCb();                 // -> frontend FBO (routes to hwGetFramebuffer)
    static retro_proc_address_t hwGetProcAddressCb(const char* sym); // -> GL proc (routes to hwGetProcAddress)

    void* handle_ = nullptr;
    bool gameLoaded_ = false;
    bool crashed_ = false;
    retro_pixel_format pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_disk_control_ext_callback disk_{}; // disk-control interface (base fields are a prefix of the ext one)
    bool hasDisk_ = false;
    retro_hw_render_callback hwRender_{}; // the core's HW-render request (context_reset/destroy fns, flags)
    bool hwRenderRequested_ = false;      // true once a GL/GLES SET_HW_RENDER was accepted
    bool hwFramePending_ = false;         // videoRefreshCb saw RETRO_HW_FRAME_BUFFER_VALID: the FBO has a frame
    retro_system_info sysInfo_{};
    retro_system_av_info avInfo_{};
    std::vector<uint8_t> frame_;   // BGRA8888
    unsigned frameW_ = 0, frameH_ = 0;
    std::vector<uint8_t> gameData_; // kept alive while the game is loaded
    std::vector<retro_memory_descriptor> memDescriptors_; // copied from SET_MEMORY_MAPS (achievements)
    retro_memory_map memoryMap_{};

    std::vector<CoreOption> options_;             // definitions, in menu order
    std::map<std::string, std::string> optionValues_; // key -> current value (c_str() served to the core)
    bool optionsDirty_ = false;                   // GET_VARIABLE_UPDATE: a value changed since last poll
};
