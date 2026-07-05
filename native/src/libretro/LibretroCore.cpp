#include "LibretroCore.h"
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
  #include <windows.h>
  static void* dl_open(const char* p) { return (void*)LoadLibraryA(p); }
  static void* dl_sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
  static void  dl_close(void* h) { if (h) FreeLibrary((HMODULE)h); }
#else
  #include <dlfcn.h>
  static void* dl_open(const char* p) { return dlopen(p, RTLD_LAZY | RTLD_LOCAL); }
  static void* dl_sym(void* h, const char* n) { return dlsym(h, n); }
  static void  dl_close(void* h) { if (h) dlclose(h); }
#endif

LibretroCore* LibretroCore::current_ = nullptr;

static void core_log(enum retro_log_level level, const char* fmt, ...)
{
    if (level < RETRO_LOG_WARN) return; // keep the console quiet; surface warnings/errors
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

// Run a call into a core under structured exception handling, so a hard fault (access violation, etc.)
// inside a misbehaving core becomes a recoverable failure instead of crashing the whole app. Kept in
// its own helper with no C++ objects that need unwinding (a requirement for __try). fn must be trivially
// destructible (our call sites pass lambdas capturing only `this`/pointers). Returns false on a fault.
#ifdef _WIN32
template <class Fn>
static bool guardedCall(Fn&& fn)
{
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#else
template <class Fn>
static bool guardedCall(Fn&& fn) { fn(); return true; } // POSIX: a future SIGSEGV handler could harden this
#endif

LibretroCore::~LibretroCore() { unload(); }

void LibretroCore::resolve(const char* name, void** fn)
{
    *fn = dl_sym(handle_, name);
}

bool LibretroCore::loadCore(const std::string& corePath, std::string* error)
{
    handle_ = dl_open(corePath.c_str());
    if (!handle_) { if (error) *error = "could not load core: " + corePath; return false; }
    current_ = this;
    options_.clear();
    optionValues_.clear();
    optionsDirty_ = false;
    crashed_ = false;

    resolve("retro_init", (void**)&retro_init_);
    resolve("retro_deinit", (void**)&retro_deinit_);
    resolve("retro_api_version", (void**)&retro_api_version_);
    resolve("retro_get_system_info", (void**)&retro_get_system_info_);
    resolve("retro_get_system_av_info", (void**)&retro_get_system_av_info_);
    resolve("retro_set_environment", (void**)&retro_set_environment_);
    resolve("retro_set_video_refresh", (void**)&retro_set_video_refresh_);
    resolve("retro_set_audio_sample", (void**)&retro_set_audio_sample_);
    resolve("retro_set_audio_sample_batch", (void**)&retro_set_audio_sample_batch_);
    resolve("retro_set_input_poll", (void**)&retro_set_input_poll_);
    resolve("retro_set_input_state", (void**)&retro_set_input_state_);
    resolve("retro_run", (void**)&retro_run_);
    resolve("retro_load_game", (void**)&retro_load_game_);
    resolve("retro_unload_game", (void**)&retro_unload_game_);
    resolve("retro_set_controller_port_device", (void**)&retro_set_controller_port_device_);
    resolve("retro_serialize_size", (void**)&retro_serialize_size_);
    resolve("retro_serialize", (void**)&retro_serialize_);
    resolve("retro_unserialize", (void**)&retro_unserialize_);
    resolve("retro_get_memory_data", (void**)&retro_get_memory_data_); // for RetroAchievements memory reads
    resolve("retro_get_memory_size", (void**)&retro_get_memory_size_);
    resolve("retro_cheat_reset", (void**)&retro_cheat_reset_);
    resolve("retro_cheat_set", (void**)&retro_cheat_set_);

    if (!retro_init_ || !retro_run_ || !retro_load_game_ || !retro_get_system_info_)
    { if (error) *error = "core is missing required libretro exports"; unload(); return false; }

    retro_get_system_info_(&sysInfo_);

    // The environment/setup handshake + init, guarded: some cores fault here in a minimal frontend
    // (e.g. Mesen). A fault becomes a clean failure the UI can report rather than a process crash.
    const bool ok = guardedCall([&] {
        retro_set_environment_(environmentCb);
        retro_set_video_refresh_(videoRefreshCb);
        retro_set_audio_sample_(audioSampleCb);
        retro_set_audio_sample_batch_(audioBatchCb);
        retro_set_input_poll_(inputPollCb);
        retro_set_input_state_(inputStateCb);
        retro_init_();
    });
    if (!ok)
    {
        if (error) *error = "core crashed during initialization (incompatible build?): " + corePath;
        unload();
        return false;
    }
    return true;
}

bool LibretroCore::loadGame(const std::string& gamePath, std::string* error)
{
    retro_game_info info{};
    info.path = gamePath.empty() ? nullptr : gamePath.c_str();
    if (!gamePath.empty() && !sysInfo_.need_fullpath)
    {
        std::ifstream f(gamePath, std::ios::binary | std::ios::ate);
        if (!f) { if (error) *error = "could not read game: " + gamePath; return false; }
        gameData_.resize((size_t)f.tellg());
        f.seekg(0); f.read((char*)gameData_.data(), (std::streamsize)gameData_.size());
        info.data = gameData_.data();
        info.size = gameData_.size();
    }
    bool loaded = false;
    if (!guardedCall([&] { loaded = retro_load_game_(&info); }))
    { if (error) *error = "core crashed while loading the game"; return false; }
    if (!loaded) { if (error) *error = "core rejected the game"; return false; }
    gameLoaded_ = true;
    retro_get_system_av_info_(&avInfo_);
    frame_.assign((size_t)avInfo_.geometry.max_width * avInfo_.geometry.max_height * 4, 0);
    return true;
}

void LibretroCore::runFrame()
{
    if (!retro_run_ || crashed_) return;
    if (!guardedCall([&] { retro_run_(); }))
        crashed_ = true; // the UI polls crashed() and stops, rather than faulting every frame
}

void LibretroCore::setControllerPortDevice(unsigned port, unsigned device)
{
    if (!retro_set_controller_port_device_ || crashed_) return;
    guardedCall([&] { retro_set_controller_port_device_(port, device); });
}

bool LibretroCore::saveState(std::vector<uint8_t>& out)
{
    if (!gameLoaded_ || !retro_serialize_size_ || !retro_serialize_) return false;
    const size_t size = retro_serialize_size_();
    if (size == 0) return false; // core doesn't support save states for this content
    out.resize(size);
    return retro_serialize_(out.data(), size);
}

bool LibretroCore::loadState(const uint8_t* data, size_t size)
{
    if (!gameLoaded_ || !retro_unserialize_ || !data || size == 0) return false;
    // Some cores grow their state between save and load; only sizes >= current are guaranteed loadable.
    if (retro_serialize_size_ && size > retro_serialize_size_()) return false;
    return retro_unserialize_(data, size);
}

void LibretroCore::cheatReset()
{
    if (!gameLoaded_ || !retro_cheat_reset_) return;
    guardedCall([&] { retro_cheat_reset_(); });
}

void LibretroCore::cheatSet(unsigned index, bool enabled, const std::string& code)
{
    if (!gameLoaded_ || !retro_cheat_set_ || code.empty()) return;
    guardedCall([&] { retro_cheat_set_(index, enabled, code.c_str()); });
}

unsigned LibretroCore::diskCount() const { return (hasDisk_ && disk_.get_num_images) ? disk_.get_num_images() : 0; }
unsigned LibretroCore::diskIndex() const { return (hasDisk_ && disk_.get_image_index) ? disk_.get_image_index() : 0; }
bool LibretroCore::diskEjected() const { return (hasDisk_ && disk_.get_eject_state) ? disk_.get_eject_state() : false; }
void LibretroCore::setDiskEject(bool ejected) { if (hasDisk_ && disk_.set_eject_state) guardedCall([&] { disk_.set_eject_state(ejected); }); }
void LibretroCore::setDiskIndex(unsigned index) { if (hasDisk_ && disk_.set_image_index) guardedCall([&] { disk_.set_image_index(index); }); }
std::string LibretroCore::diskLabel(unsigned index) const
{
    if (hasDisk_ && disk_.get_image_label)
    {
        char buf[256] = { 0 };
        if (disk_.get_image_label(index, buf, sizeof buf) && buf[0]) return std::string(buf);
    }
    return {};
}

void LibretroCore::unload()
{
    if (gameLoaded_ && retro_unload_game_) retro_unload_game_();
    if (handle_ && retro_deinit_) retro_deinit_();
    if (handle_) dl_close(handle_);
    handle_ = nullptr; gameLoaded_ = false;
    if (current_ == this) current_ = nullptr;
}

std::string LibretroCore::optionValue(const std::string& key) const
{
    auto it = optionValues_.find(key);
    return it != optionValues_.end() ? it->second : std::string();
}

void LibretroCore::setOptionValue(const std::string& key, const std::string& value)
{
    auto it = optionValues_.find(key);
    if (it == optionValues_.end() || it->second != value)
    {
        optionValues_[key] = value;
        optionsDirty_ = true; // core re-reads on its next GET_VARIABLE_UPDATE poll
    }
}

void LibretroCore::addOption(const std::string& key, const std::string& desc, const std::string& info,
                             std::vector<std::pair<std::string, std::string>> values, const std::string& def)
{
    if (key.empty()) return;
    CoreOption o;
    o.key = key;
    o.desc = desc.empty() ? key : desc;
    o.info = info;
    o.values = std::move(values);
    o.defaultValue = def;
    if (o.defaultValue.empty() && !o.values.empty())
        o.defaultValue = o.values.front().first;
    options_.push_back(std::move(o));
    // Seed the live value with the default the first time we see this key (don't clobber a value
    // the frontend may have applied earlier).
    const CoreOption& added = options_.back();
    if (optionValues_.find(added.key) == optionValues_.end())
        optionValues_[added.key] = added.defaultValue;
}

// SET_VARIABLES (legacy): value string is "Label; v1|v2|v3" with v1 as the default.
void LibretroCore::registerVariablesLegacy(const retro_variable* v)
{
    for (; v && v->key; ++v)
    {
        const std::string str = v->value ? v->value : "";
        std::string desc;
        std::vector<std::pair<std::string, std::string>> vals;
        const size_t semi = str.find(';');
        if (semi != std::string::npos)
        {
            desc = str.substr(0, semi);
            size_t i = semi + 1;
            while (i < str.size() && str[i] == ' ') ++i; // skip the single space after ';'
            for (size_t start = i; start <= str.size(); )
            {
                const size_t bar = str.find('|', start);
                const std::string tok = str.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!tok.empty()) vals.emplace_back(tok, tok);
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
        }
        else
        {
            desc = str;
        }
        addOption(v->key, desc, "", std::move(vals), ""); // default = first value (addOption fills it in)
    }
}

void LibretroCore::registerOptionDefs(const retro_core_option_definition* d)
{
    for (; d && d->key; ++d)
    {
        std::vector<std::pair<std::string, std::string>> vals;
        for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i)
            vals.emplace_back(d->values[i].value, d->values[i].label ? d->values[i].label : d->values[i].value);
        addOption(d->key, d->desc ? d->desc : "", d->info ? d->info : "",
                  std::move(vals), d->default_value ? d->default_value : "");
    }
}

void LibretroCore::registerOptionDefsV2(const retro_core_option_v2_definition* d)
{
    for (; d && d->key; ++d)
    {
        std::vector<std::pair<std::string, std::string>> vals;
        for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i)
            vals.emplace_back(d->values[i].value, d->values[i].label ? d->values[i].label : d->values[i].value);
        addOption(d->key, d->desc ? d->desc : "", d->info ? d->info : "",
                  std::move(vals), d->default_value ? d->default_value : "");
    }
}

bool LibretroCore::environmentCb(unsigned cmd, void* data)
{
    auto* self = current_;
    if (std::getenv("MYMEDIAVAULT_ENVTRACE")) { fprintf(stderr, "[env] cmd=%u data=%p\n", cmd, data); fflush(stderr); }
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        self->pixelFormat_ = *(const retro_pixel_format*)data;
        return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    {
        // The core describes its address space (used by RetroAchievements to map achievement addresses to
        // the right RAM regions). Copy the descriptor array; the core keeps the pointed-to strings alive.
        const auto* mm = (const retro_memory_map*)data;
        self->memDescriptors_.assign(mm->descriptors, mm->descriptors + mm->num_descriptors);
        self->memoryMap_.descriptors = self->memDescriptors_.data();
        self->memoryMap_.num_descriptors = (unsigned)self->memDescriptors_.size();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char**)data = self->systemDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char**)data = self->saveDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool*)data = true; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((retro_log_callback*)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        ((retro_rumble_interface*)data)->set_rumble_state = rumbleSetStateCb; return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned*)data = RETRO_LANGUAGE_ENGLISH; return true; // report a language rather than leaving it unset
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned*)data = 2; return true; // we understand the v2 options API
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        auto* var = (retro_variable*)data;
        if (!var || !var->key) return false;
        auto it = self->optionValues_.find(var->key);
        var->value = (it != self->optionValues_.end()) ? it->second.c_str() : nullptr;
        return var->value != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool*)data = self->optionsDirty_;
        self->optionsDirty_ = false;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        self->registerVariablesLegacy((const retro_variable*)data);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        self->registerOptionDefs((const retro_core_option_definition*)data);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        if (auto* intl = (const retro_core_options_intl*)data)
            self->registerOptionDefs(intl->us);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        if (auto* v2 = (const retro_core_options_v2*)data)
            self->registerOptionDefsV2(v2->definitions);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        if (auto* v2i = (const retro_core_options_v2_intl*)data)
            if (v2i->us) self->registerOptionDefsV2(v2i->us->definitions);
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    {
        // Base interface: copy the 7 shared function pointers into our ext struct (label/path stay null).
        auto* cb = (const retro_disk_control_callback*)data;
        if (!cb) return false;
        self->disk_ = {};
        self->disk_.set_eject_state    = cb->set_eject_state;
        self->disk_.get_eject_state    = cb->get_eject_state;
        self->disk_.get_image_index    = cb->get_image_index;
        self->disk_.set_image_index    = cb->set_image_index;
        self->disk_.get_num_images     = cb->get_num_images;
        self->disk_.replace_image_index = cb->replace_image_index;
        self->disk_.add_image_index    = cb->add_image_index;
        self->hasDisk_ = true;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
    {
        auto* cb = (const retro_disk_control_ext_callback*)data;
        if (!cb) return false;
        self->disk_ = *cb;   // includes get_image_label for nicer disk names
        self->hasDisk_ = true;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    {
        // A core wants to render with a GPU API. We can host OpenGL / OpenGL ES via an offscreen context +
        // FBO (set up by the Qt layer); Vulkan/D3D aren't supported, so reject those and the core falls back to
        // software (or declines to load) exactly as before. Store the callback and give the core our framebuffer
        // + proc-address resolvers; RetroView creates the context and calls context_reset before the first frame.
        auto* cb = (retro_hw_render_callback*)data;
        if (!cb) return false;
        if (cb->context_type != RETRO_HW_CONTEXT_OPENGL && cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE
            && cb->context_type != RETRO_HW_CONTEXT_OPENGLES2 && cb->context_type != RETRO_HW_CONTEXT_OPENGLES3
            && cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION)
            return false;
        self->hwRender_ = *cb;
        self->hwRenderRequested_ = true;
        cb->get_current_framebuffer = hwGetCurrentFramebufferCb;
        cb->get_proc_address = hwGetProcAddressCb;
        return true;
    }
    default:
        return false;
    }
}

void LibretroCore::videoRefreshCb(const void* data, unsigned width, unsigned height, size_t pitch)
{
    auto* self = current_;
    self->frameW_ = width; self->frameH_ = height;
    if (data == RETRO_HW_FRAME_BUFFER_VALID) // the core rendered this frame into our GL FBO; nothing to convert
    {
        self->hwFramePending_ = true; // the frontend reads the FBO back after runFrame() returns
        return;
    }
    if (!data) return; // duped frame: keep the previous picture
    if (self->frame_.size() < (size_t)width * height * 4)
        self->frame_.assign((size_t)width * height * 4, 0);
    uint8_t* out = self->frame_.data();

    if (self->pixelFormat_ == RETRO_PIXEL_FORMAT_XRGB8888)
    {
        for (unsigned y = 0; y < height; ++y) {
            const uint32_t* src = (const uint32_t*)((const uint8_t*)data + y * pitch);
            uint8_t* dst = out + (size_t)y * width * 4;
            for (unsigned x = 0; x < width; ++x) {
                uint32_t p = src[x];
                *dst++ = (uint8_t)(p);        // B
                *dst++ = (uint8_t)(p >> 8);   // G
                *dst++ = (uint8_t)(p >> 16);  // R
                *dst++ = 0xFF;                // A
            }
        }
    }
    else if (self->pixelFormat_ == RETRO_PIXEL_FORMAT_RGB565)
    {
        for (unsigned y = 0; y < height; ++y) {
            const uint16_t* src = (const uint16_t*)((const uint8_t*)data + y * pitch);
            uint8_t* dst = out + (size_t)y * width * 4;
            for (unsigned x = 0; x < width; ++x) {
                uint16_t p = src[x];
                *dst++ = (uint8_t)(( p        & 0x1F) << 3); // B
                *dst++ = (uint8_t)(((p >> 5)  & 0x3F) << 2); // G
                *dst++ = (uint8_t)(((p >> 11) & 0x1F) << 3); // R
                *dst++ = 0xFF;
            }
        }
    }
    else // 0RGB1555
    {
        for (unsigned y = 0; y < height; ++y) {
            const uint16_t* src = (const uint16_t*)((const uint8_t*)data + y * pitch);
            uint8_t* dst = out + (size_t)y * width * 4;
            for (unsigned x = 0; x < width; ++x) {
                uint16_t p = src[x];
                *dst++ = (uint8_t)(( p        & 0x1F) << 3); // B
                *dst++ = (uint8_t)(((p >> 5)  & 0x1F) << 3); // G
                *dst++ = (uint8_t)(((p >> 10) & 0x1F) << 3); // R
                *dst++ = 0xFF;
            }
        }
    }
}

void LibretroCore::audioSampleCb(int16_t left, int16_t right)
{
    int16_t s[2] = { left, right };
    if (current_->onAudio) current_->onAudio(s, 1);
}
size_t LibretroCore::audioBatchCb(const int16_t* data, size_t frames)
{
    if (current_->onAudio) current_->onAudio(data, frames);
    return frames;
}
void LibretroCore::inputPollCb() {}
int16_t LibretroCore::inputStateCb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return current_->onInput ? current_->onInput(port, device, index, id) : 0;
}
bool LibretroCore::rumbleSetStateCb(unsigned port, retro_rumble_effect effect, uint16_t strength)
{
    if (!current_ || !current_->onRumble) return false;
    current_->onRumble(port, static_cast<unsigned>(effect), strength);
    return true;
}

// The core calls these (installed via SET_HW_RENDER) each frame to find the framebuffer to draw into and to
// resolve GL entry points. Both route to the Qt/GL layer's hooks; 0/null before RetroView has set them up.
uintptr_t LibretroCore::hwGetCurrentFramebufferCb()
{
    return (current_ && current_->hwGetFramebuffer) ? current_->hwGetFramebuffer() : 0;
}
retro_proc_address_t LibretroCore::hwGetProcAddressCb(const char* sym)
{
    if (!current_ || !current_->hwGetProcAddress) return nullptr;
    return reinterpret_cast<retro_proc_address_t>(current_->hwGetProcAddress(sym));
}
