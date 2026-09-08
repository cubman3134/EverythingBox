#include "CoreInspect.h"
#include "libretro.h"

#include <QCoreApplication>
#include <QFileInfo>

#ifdef _WIN32
  #include <windows.h>
  static void* ci_open(const char* p) { return (void*)LoadLibraryA(p); }
  static void* ci_sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
  static void  ci_close(void* h) { if (h) FreeLibrary((HMODULE)h); }
#else
  #include <dlfcn.h>
  static void* ci_open(const char* p) { return dlopen(p, RTLD_LAZY | RTLD_LOCAL); }
  static void* ci_sym(void* h, const char* n) { return dlsym(h, n); }
  static void  ci_close(void* h) { if (h) dlclose(h); }
#endif

// The same shape as LibretroCore's guardedCall: a hard fault inside a core becomes a recoverable failure on
// Windows. Kept free of C++ objects that need unwinding, which __try requires.
#ifdef _MSC_VER
template <class Fn>
static bool ci_guarded(Fn&& fn)
{
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#else
template <class Fn>
static bool ci_guarded(Fn&& fn) { fn(); return true; }
#endif

QStringList CoreInspect::splitExtensions(const QString& validExtensions)
{
    QStringList out;
    const QStringList parts = validExtensions.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    for (const QString& raw : parts)
    {
        QString e = raw.trimmed().toLower();
        while (e.startsWith(QLatin1Char('.'))) e.remove(0, 1);
        if (!e.isEmpty() && !out.contains(e)) out.push_back(e);
    }
    return out;
}

QString CoreInspect::unmetSentence(const CoreInspection& in)
{
    if (in.unmet.isEmpty()) return QString();
    const QString shown = in.libraryName.trimmed().isEmpty()
                              ? QCoreApplication::translate("CoreInspect", "This core")
                              : in.libraryName.trimmed();
    return QCoreApplication::translate(
               "CoreInspect", "%1 asked for %2, which EverythingBox doesn't provide. It may still run without it.")
        .arg(shown, in.unmet.join(QCoreApplication::translate("CoreInspect", " and ")));
}

namespace {

// The recorder. A libretro environment callback is a plain C function pointer with no user data, so the
// destination is a file-static — the inspection is a short, single-threaded, non-reentrant call sequence
// (open, ask, close), so there is exactly one live recorder at a time.
struct Recorder
{
    bool        supportsNoGame = false;
    QStringList unmet;
    void note(const QString& phrase) { if (!unmet.contains(phrase)) unmet.push_back(phrase); }
};
Recorder* g_rec = nullptr;

bool RETRO_CALLCONV recordEnv(unsigned cmd, void* data)
{
    if (!g_rec) return false;
    switch (cmd)
    {
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            // The one fact this whole inspection exists to learn: a core that runs with no content at all.
            if (data) g_rec->supportsNoGame = *static_cast<const bool*>(data);
            return true;

        case RETRO_ENVIRONMENT_SET_HW_RENDER:
        {
            // A GL/GLES request is served by the existing RetroView hardware path, so it is NOT unmet. Any
            // other context type (Vulkan, D3D, a negotiation-only context) is: the frontend has no runtime
            // for it on this tier, and a core that insists will show a black screen instead of an error.
            const auto* cb = static_cast<const retro_hw_render_callback*>(data);
            if (!cb) return false;
            switch (cb->context_type)
            {
                case RETRO_HW_CONTEXT_NONE:
                case RETRO_HW_CONTEXT_OPENGL:
                case RETRO_HW_CONTEXT_OPENGL_CORE:
                case RETRO_HW_CONTEXT_OPENGLES2:
                case RETRO_HW_CONTEXT_OPENGLES3:
                case RETRO_HW_CONTEXT_OPENGLES_VERSION:
                    return true;
                case RETRO_HW_CONTEXT_VULKAN:
                    g_rec->note(QCoreApplication::translate("CoreInspect", "a Vulkan renderer"));
                    return false;
                case RETRO_HW_CONTEXT_D3D11:
                case RETRO_HW_CONTEXT_D3D10:
                case RETRO_HW_CONTEXT_D3D12:
                case RETRO_HW_CONTEXT_D3D9:
                    g_rec->note(QCoreApplication::translate("CoreInspect", "a Direct3D renderer"));
                    return false;
                default:
                    g_rec->note(QCoreApplication::translate("CoreInspect", "a hardware renderer this build has no runtime for"));
                    return false;
            }
        }

        case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:
            g_rec->note(QCoreApplication::translate("CoreInspect", "camera access"));
            return false;
        case RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE:
            g_rec->note(QCoreApplication::translate("CoreInspect", "location access"));
            return false;
        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
            g_rec->note(QCoreApplication::translate("CoreInspect", "device sensors"));
            return false;
        case RETRO_ENVIRONMENT_GET_MIDI_INTERFACE:
            g_rec->note(QCoreApplication::translate("CoreInspect", "a MIDI device"));
            return false;
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
            g_rec->note(QCoreApplication::translate("CoreInspect", "multi-cartridge subsystems"));
            return false;

        default:
            // Everything else is answered exactly as LibretroCore answers an unhandled command: "no". A core
            // asking a question we do not record is not a defect — most of the environment API is optional and
            // cores are written to cope with a refusal.
            return false;
    }
}

} // namespace

bool CoreInspect::inspect(const QString& path, CoreInspection* out, QString* error)
{
    const QString file = QFileInfo(path).fileName();
    if (path.trimmed().isEmpty() || !QFileInfo::exists(path))
    {
        if (error) *error = QCoreApplication::translate("CoreInspect", "There is no file at %1.").arg(path);
        return false;
    }

    void* handle = ci_open(path.toLocal8Bit().constData());
    if (!handle)
    {
        if (error) *error = QCoreApplication::translate(
                       "CoreInspect", "%1 couldn't be loaded as a library — it isn't a core this machine can run "
                                      "(wrong platform or architecture?).").arg(file);
        return false;
    }

    auto api      = reinterpret_cast<unsigned (*)()>(ci_sym(handle, "retro_api_version"));
    auto getInfo  = reinterpret_cast<void (*)(retro_system_info*)>(ci_sym(handle, "retro_get_system_info"));
    auto setEnv   = reinterpret_cast<void (*)(retro_environment_t)>(ci_sym(handle, "retro_set_environment"));
    // The exports a core MUST have for the frontend to host it at all. Checked here so "this file is not a
    // libretro core" is a sentence rather than a null call later.
    const char* required[] = { "retro_init", "retro_deinit", "retro_run", "retro_load_game",
                               "retro_unload_game", "retro_get_system_av_info" };
    bool missingRequired = (api == nullptr || getInfo == nullptr || setEnv == nullptr);
    for (const char* sym : required)
        if (!ci_sym(handle, sym)) missingRequired = true;
    if (missingRequired)
    {
        ci_close(handle);
        if (error) *error = QCoreApplication::translate(
                       "CoreInspect", "%1 loaded, but it isn't a libretro core — it's missing the libretro entry "
                                      "points.").arg(file);
        return false;
    }

    CoreInspection info;
    bool ok = ci_guarded([&] { info.apiVersion = api(); });
    if (!ok)
    {
        ci_close(handle);
        if (error) *error = QCoreApplication::translate("CoreInspect", "%1 faulted the moment it was asked what it is.")
                                .arg(file);
        return false;
    }
    if (info.apiVersion != RETRO_API_VERSION)
    {
        ci_close(handle);
        if (error) *error = QCoreApplication::translate(
                       "CoreInspect", "%1 speaks libretro API version %2; EverythingBox speaks version %3.")
                       .arg(file).arg(info.apiVersion).arg(unsigned(RETRO_API_VERSION));
        return false;
    }

    retro_system_info si{};
    ok = ci_guarded([&] { getInfo(&si); });
    if (!ok)
    {
        ci_close(handle);
        if (error) *error = QCoreApplication::translate("CoreInspect", "%1 faulted while reporting its system info.")
                                .arg(file);
        return false;
    }
    info.libraryName    = si.library_name ? QString::fromUtf8(si.library_name).trimmed() : QString();
    info.libraryVersion = si.library_version ? QString::fromUtf8(si.library_version).trimmed() : QString();
    info.extensions     = splitExtensions(si.valid_extensions ? QString::fromUtf8(si.valid_extensions) : QString());
    info.needFullpath   = si.need_fullpath;
    info.blockExtract   = si.block_extract;

    // The handshake half: everything a core declares by calling US. Guarded, because this is the first code of
    // the core's own that runs.
    Recorder rec;
    g_rec = &rec;
    ok = ci_guarded([&] { setEnv(recordEnv); });
    g_rec = nullptr;
    ci_close(handle);
    if (!ok)
    {
        if (error) *error = QCoreApplication::translate(
                       "CoreInspect", "%1 faulted during setup — it isn't compatible with this frontend.").arg(file);
        return false;
    }
    info.supportsNoGame = rec.supportsNoGame;
    info.unmet          = rec.unmet;

    if (out) *out = info;
    return true;
}
