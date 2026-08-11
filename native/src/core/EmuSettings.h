// Cross-emulator graphics settings (issue #103) — the PURE heart. RetroBat's promise is "users should not
// have to open an emulator to configure it": EmulatorManager already writes emulator config for PLUMBING
// (first-run skip, controller P1, BIOS, Cemu keys) but never for SETTINGS. This is the small cross-emulator
// vocabulary — the quartet the issue says carries most of the value — plus the per-emulator map from that
// vocabulary onto each emulator's own config format. #51's LaunchOptionsStore is the per-game override for the
// libretro tier (core / emulator id / extra args); this is the standalone-tier twin for graphics.
//
// THE QUARTET. internal-resolution multiplier, aspect ratio, vsync, renderer backend (with MSAA where it
// fits). Every field has an explicit UNSET spelling (0 for the two integer levers, an `Unset` enumerator for
// the rest) and there is exactly ONE spelling for unset. Unset means DON'T TOUCH — configEdits emits no edit
// for it, so a game with no override leaves the emulator's own value exactly as the user left it. That is the
// restraint the issue insists on: a user who hand-tuned Dolphin keeps their tuning unless they opt into ours.
//
// PURE, header-only, QtCore-only, NO DISK. configEdits(id, settings) returns the exact list of (file, section,
// key, value) INI edits to apply to that emulator — nothing else. The write-on-launch side (EmulatorManager)
// applies each edit with a MERGE-preserving upsert (setIniKey), never a whole-file rewrite. The store
// (EmuGfxStore) persists a per-game override and reads it back. Keeping this pure is what lets probe_emusettings
// assert the exact edits per emulator per setting, against hand-authored fixtures, without touching a disk or a
// live emulator — the probe pins the MAPPING; whether an emulator then honours the written key is not headlessly
// verifiable and is out of the probe's reach by construction.
//
// PER-EMULATOR CONFIDENCE / VERSION DRIFT. Every key here is a DOCUMENTED config key for the emulator named,
// but this repo cannot run the emulator to confirm the running build still reads it. An unsupported setting for
// an emulator yields NO edit (skipped, never an error) — so the failure mode of a wrong/renamed key is "this
// one setting does nothing", never a corrupted config. The resolution lever is the one that formats
// DIFFERENTLY per emulator and the probe pins each: Dolphin/PCSX2/DuckStation take a MULTIPLIER integer;
// Flycast takes a PIXEL HEIGHT (multiplier x the platform's native 480 lines), so 2x -> 960, not 2.
//
// EXTENSIBLE BY SHAPE. The built-in table below is data-driven per emulator id; a user emulator declaring its
// own feature map via the #52 JSON schema is the intended growth path (build a mapping from its JSON and route
// configEdits through it). That JSON wiring is a follow-up; the quartet on the shipped emulators is the value.
#pragma once
#include <QString>
#include <QVector>
#include <QJsonObject>

namespace EmuGfx
{
    // ---- the quartet value model (+ MSAA). Every lever has one, and only one, "unset" spelling. ------------
    enum class Aspect   { Unset, Auto, R4_3, R16_9, Stretch };
    enum class Vsync    { Unset, On, Off };
    enum class Renderer { Unset, Auto, Vulkan, D3D11, D3D12, OpenGL, Metal, Software };

    struct Settings
    {
        int      resMultiplier = 0;             // 0 = unset; 1..N internal-resolution multiplier over native
        Aspect   aspect        = Aspect::Unset;
        Vsync    vsync         = Vsync::Unset;
        Renderer renderer      = Renderer::Unset;
        int      msaa          = 0;             // 0 = unset; 1 = none/off, 2/4/8 = sample count

        bool isEmpty() const
        {
            return resMultiplier == 0 && aspect == Aspect::Unset && vsync == Vsync::Unset
                && renderer == Renderer::Unset && msaa == 0;
        }
        bool operator==(const Settings& o) const
        {
            return resMultiplier == o.resMultiplier && aspect == o.aspect && vsync == o.vsync
                && renderer == o.renderer && msaa == o.msaa;
        }
        bool operator!=(const Settings& o) const { return !(*this == o); }
    };

    // One INI edit: an upsert of `key` = `value` inside [`section`] of the file at `file` (relative to the
    // emulator's install dir). The write side preserves every other key in that file.
    struct ConfigEdit
    {
        QString file;      // path relative to the emulator's binDir (e.g. "User/Config/GFX.ini", "inis/PCSX2.ini")
        QString section;   // INI section name, no brackets (e.g. "EmuCore/GS")
        QString key;
        QString value;

        bool operator==(const ConfigEdit& o) const
        {
            return file == o.file && section == o.section && key == o.key && value == o.value;
        }
        bool operator!=(const ConfigEdit& o) const { return !(*this == o); }
    };

    // ---- pure: enum <-> stable JSON token (the store's on-disk spelling; independent of the INI spelling) --
    inline QString aspectToken(Aspect a)
    {
        switch (a) { case Aspect::Auto: return QStringLiteral("auto"); case Aspect::R4_3: return QStringLiteral("4:3");
                     case Aspect::R16_9: return QStringLiteral("16:9"); case Aspect::Stretch: return QStringLiteral("stretch");
                     default: return QString(); }
    }
    inline Aspect aspectFromToken(const QString& s)
    {
        if (s == QLatin1String("auto"))    return Aspect::Auto;
        if (s == QLatin1String("4:3"))     return Aspect::R4_3;
        if (s == QLatin1String("16:9"))    return Aspect::R16_9;
        if (s == QLatin1String("stretch")) return Aspect::Stretch;
        return Aspect::Unset;
    }
    inline QString vsyncToken(Vsync v)
    {
        switch (v) { case Vsync::On: return QStringLiteral("on"); case Vsync::Off: return QStringLiteral("off");
                     default: return QString(); }
    }
    inline Vsync vsyncFromToken(const QString& s)
    {
        if (s == QLatin1String("on"))  return Vsync::On;
        if (s == QLatin1String("off")) return Vsync::Off;
        return Vsync::Unset;
    }
    inline QString rendererToken(Renderer r)
    {
        switch (r) { case Renderer::Auto: return QStringLiteral("auto"); case Renderer::Vulkan: return QStringLiteral("vulkan");
                     case Renderer::D3D11: return QStringLiteral("d3d11"); case Renderer::D3D12: return QStringLiteral("d3d12");
                     case Renderer::OpenGL: return QStringLiteral("opengl"); case Renderer::Metal: return QStringLiteral("metal");
                     case Renderer::Software: return QStringLiteral("software"); default: return QString(); }
    }
    inline Renderer rendererFromToken(const QString& s)
    {
        if (s == QLatin1String("auto"))     return Renderer::Auto;
        if (s == QLatin1String("vulkan"))   return Renderer::Vulkan;
        if (s == QLatin1String("d3d11"))    return Renderer::D3D11;
        if (s == QLatin1String("d3d12"))    return Renderer::D3D12;
        if (s == QLatin1String("opengl"))   return Renderer::OpenGL;
        if (s == QLatin1String("metal"))    return Renderer::Metal;
        if (s == QLatin1String("software")) return Renderer::Software;
        return Renderer::Unset;
    }

    // ---- pure: Settings <-> canonical JSON (what EmuGfxStore persists) ------------------------------------
    // Every field written ONLY when set, so there is one spelling per record and fromJson(toJson(s)) == s.
    inline QJsonObject toJson(const Settings& s)
    {
        QJsonObject o;
        if (s.resMultiplier != 0)          o.insert(QStringLiteral("res"), s.resMultiplier);
        if (s.aspect != Aspect::Unset)     o.insert(QStringLiteral("aspect"), aspectToken(s.aspect));
        if (s.vsync != Vsync::Unset)       o.insert(QStringLiteral("vsync"), vsyncToken(s.vsync));
        if (s.renderer != Renderer::Unset) o.insert(QStringLiteral("renderer"), rendererToken(s.renderer));
        if (s.msaa != 0)                   o.insert(QStringLiteral("msaa"), s.msaa);
        return o;
    }
    inline Settings fromJson(const QJsonObject& o)
    {
        Settings s;
        s.resMultiplier = o.value(QStringLiteral("res")).toInt(0);
        if (s.resMultiplier < 0) s.resMultiplier = 0;                       // a multiplier is never negative
        s.aspect   = aspectFromToken(o.value(QStringLiteral("aspect")).toString());
        s.vsync    = vsyncFromToken(o.value(QStringLiteral("vsync")).toString());
        s.renderer = rendererFromToken(o.value(QStringLiteral("renderer")).toString());
        s.msaa     = o.value(QStringLiteral("msaa")).toInt(0);
        if (s.msaa < 0) s.msaa = 0;
        return s;
    }

    // ---- pure: resolve a per-game override over a per-system default -------------------------------------
    // Field-by-field: a per-game field that is SET wins; an unset per-game field falls through to the system
    // default; a field unset in both stays unset (-> no edit -> the emulator's own value is untouched). This is
    // the "per-game override else per-system default else don't touch" ladder the write side applies.
    inline Settings resolve(const Settings& perGame, const Settings& perSystem)
    {
        Settings r = perSystem;
        if (perGame.resMultiplier != 0)          r.resMultiplier = perGame.resMultiplier;
        if (perGame.aspect != Aspect::Unset)     r.aspect        = perGame.aspect;
        if (perGame.vsync != Vsync::Unset)       r.vsync         = perGame.vsync;
        if (perGame.renderer != Renderer::Unset) r.renderer      = perGame.renderer;
        if (perGame.msaa != 0)                   r.msaa          = perGame.msaa;
        return r;
    }

    // ---- pure: the per-emulator mapping — the mutation-tested core -----------------------------------------
    // Given an emulator id and a resolved settings set, the exact INI edits to apply. An unset lever emits
    // nothing; a lever an emulator does not support emits nothing (skipped, never an error). The value FORMAT is
    // per emulator per lever — this is where the real documented keys live.
    inline QVector<ConfigEdit> configEdits(const QString& emulatorId, const Settings& s)
    {
        QVector<ConfigEdit> out;
        auto emit_ = [&](const QString& file, const QString& section, const QString& key, const QString& value) {
            out.push_back(ConfigEdit{ file, section, key, value });
        };

        if (emulatorId == QLatin1String("dolphin"))
        {
            // Dolphin keeps graphics in User/Config/GFX.ini and the backend in User/Config/Dolphin.ini.
            const QString gfx = QStringLiteral("User/Config/GFX.ini");
            const QString ini = QStringLiteral("User/Config/Dolphin.ini");
            if (s.resMultiplier != 0)                                       // 1 = native, 2 = 2x, ... (a multiplier int)
                emit_(gfx, QStringLiteral("Settings"), QStringLiteral("InternalResolution"), QString::number(s.resMultiplier));
            switch (s.aspect) {                                            // AspectRatio: 0 Auto, 1 Force16:9, 2 Force4:3, 3 Stretch
                case Aspect::Auto:    emit_(gfx, QStringLiteral("Settings"), QStringLiteral("AspectRatio"), QStringLiteral("0")); break;
                case Aspect::R16_9:   emit_(gfx, QStringLiteral("Settings"), QStringLiteral("AspectRatio"), QStringLiteral("1")); break;
                case Aspect::R4_3:    emit_(gfx, QStringLiteral("Settings"), QStringLiteral("AspectRatio"), QStringLiteral("2")); break;
                case Aspect::Stretch: emit_(gfx, QStringLiteral("Settings"), QStringLiteral("AspectRatio"), QStringLiteral("3")); break;
                default: break;
            }
            if (s.vsync != Vsync::Unset)
                emit_(gfx, QStringLiteral("Hardware"), QStringLiteral("VSync"), s.vsync == Vsync::On ? QStringLiteral("True") : QStringLiteral("False"));
            switch (s.renderer) {                                          // Dolphin.ini [Core] GFXBackend (no "auto"/"metal" backend)
                case Renderer::Vulkan:   emit_(ini, QStringLiteral("Core"), QStringLiteral("GFXBackend"), QStringLiteral("Vulkan")); break;
                case Renderer::D3D11:    emit_(ini, QStringLiteral("Core"), QStringLiteral("GFXBackend"), QStringLiteral("D3D")); break;
                case Renderer::D3D12:    emit_(ini, QStringLiteral("Core"), QStringLiteral("GFXBackend"), QStringLiteral("D3D12")); break;
                case Renderer::OpenGL:   emit_(ini, QStringLiteral("Core"), QStringLiteral("GFXBackend"), QStringLiteral("OGL")); break;
                case Renderer::Software: emit_(ini, QStringLiteral("Core"), QStringLiteral("GFXBackend"), QStringLiteral("Software Renderer")); break;
                default: break;                                            // Auto / Metal: unsupported here -> no edit
            }
            if (s.msaa != 0)
                emit_(gfx, QStringLiteral("Settings"), QStringLiteral("MSAA"), QString::number(s.msaa));
        }
        else if (emulatorId == QLatin1String("pcsx2"))
        {
            // PCSX2 (pcsx2-qt) keeps everything in inis/PCSX2.ini; graphics live under [EmuCore/GS].
            const QString ini = QStringLiteral("inis/PCSX2.ini");
            const QString gs  = QStringLiteral("EmuCore/GS");
            if (s.resMultiplier != 0)                                       // upscale_multiplier: a multiplier (1 = native)
                emit_(ini, gs, QStringLiteral("upscale_multiplier"), QString::number(s.resMultiplier));
            switch (s.aspect) {                                            // AspectRatio: string enum
                case Aspect::Auto:    emit_(ini, gs, QStringLiteral("AspectRatio"), QStringLiteral("Auto 4:3/3:2")); break;
                case Aspect::R4_3:    emit_(ini, gs, QStringLiteral("AspectRatio"), QStringLiteral("4:3")); break;
                case Aspect::R16_9:   emit_(ini, gs, QStringLiteral("AspectRatio"), QStringLiteral("16:9")); break;
                case Aspect::Stretch: emit_(ini, gs, QStringLiteral("AspectRatio"), QStringLiteral("Stretch")); break;
                default: break;
            }
            if (s.vsync != Vsync::Unset)                                   // VsyncEnable: 0 off, 1 on
                emit_(ini, gs, QStringLiteral("VsyncEnable"), s.vsync == Vsync::On ? QStringLiteral("1") : QStringLiteral("0"));
            switch (s.renderer) {                                          // Renderer: GSRendererType int
                case Renderer::Auto:     emit_(ini, gs, QStringLiteral("Renderer"), QStringLiteral("-1")); break;
                case Renderer::D3D11:    emit_(ini, gs, QStringLiteral("Renderer"), QStringLiteral("3")); break;
                case Renderer::Software: emit_(ini, gs, QStringLiteral("Renderer"), QStringLiteral("13")); break;
                case Renderer::OpenGL:   emit_(ini, gs, QStringLiteral("Renderer"), QStringLiteral("12")); break;
                case Renderer::Vulkan:   emit_(ini, gs, QStringLiteral("Renderer"), QStringLiteral("14")); break;
                case Renderer::D3D12:    emit_(ini, gs, QStringLiteral("Renderer"), QStringLiteral("15")); break;
                case Renderer::Metal:    emit_(ini, gs, QStringLiteral("Renderer"), QStringLiteral("17")); break;
                default: break;
            }
            // PCSX2's modern GS has no MSAA lever (upscaling replaces it) -> msaa unsupported, no edit.
        }
        else if (emulatorId == QLatin1String("duckstation"))
        {
            // DuckStation keeps config in settings.ini; GPU scaling under [GPU], display levers under [Display].
            const QString ini = QStringLiteral("settings.ini");
            if (s.resMultiplier != 0)                                       // ResolutionScale: a multiplier int (1 = native)
                emit_(ini, QStringLiteral("GPU"), QStringLiteral("ResolutionScale"), QString::number(s.resMultiplier));
            switch (s.aspect) {                                            // Display AspectRatio: string enum
                case Aspect::Auto:    emit_(ini, QStringLiteral("Display"), QStringLiteral("AspectRatio"), QStringLiteral("Auto (Game Native)")); break;
                case Aspect::R4_3:    emit_(ini, QStringLiteral("Display"), QStringLiteral("AspectRatio"), QStringLiteral("4:3")); break;
                case Aspect::R16_9:   emit_(ini, QStringLiteral("Display"), QStringLiteral("AspectRatio"), QStringLiteral("16:9")); break;
                case Aspect::Stretch: emit_(ini, QStringLiteral("Display"), QStringLiteral("AspectRatio"), QStringLiteral("Stretch")); break;
                default: break;
            }
            if (s.vsync != Vsync::Unset)
                emit_(ini, QStringLiteral("Display"), QStringLiteral("VSync"), s.vsync == Vsync::On ? QStringLiteral("true") : QStringLiteral("false"));
            switch (s.renderer) {                                          // GPU Renderer: string enum
                case Renderer::Auto:     emit_(ini, QStringLiteral("GPU"), QStringLiteral("Renderer"), QStringLiteral("Automatic")); break;
                case Renderer::Vulkan:   emit_(ini, QStringLiteral("GPU"), QStringLiteral("Renderer"), QStringLiteral("Vulkan")); break;
                case Renderer::D3D11:    emit_(ini, QStringLiteral("GPU"), QStringLiteral("Renderer"), QStringLiteral("D3D11")); break;
                case Renderer::D3D12:    emit_(ini, QStringLiteral("GPU"), QStringLiteral("Renderer"), QStringLiteral("D3D12")); break;
                case Renderer::OpenGL:   emit_(ini, QStringLiteral("GPU"), QStringLiteral("Renderer"), QStringLiteral("OpenGL")); break;
                case Renderer::Metal:    emit_(ini, QStringLiteral("GPU"), QStringLiteral("Renderer"), QStringLiteral("Metal")); break;
                case Renderer::Software: emit_(ini, QStringLiteral("GPU"), QStringLiteral("Renderer"), QStringLiteral("Software")); break;
                default: break;
            }
            if (s.msaa != 0)                                               // Multisamples: MSAA sample count
                emit_(ini, QStringLiteral("GPU"), QStringLiteral("Multisamples"), QString::number(s.msaa));
        }
        else if (emulatorId == QLatin1String("flycast"))
        {
            // Flycast keeps config in emu.cfg under [config]. Its resolution lever is a PIXEL HEIGHT, not a
            // multiplier: rend.Resolution is the vertical render resolution in lines, native Dreamcast/NAOMI being
            // 480 — so an Nx multiplier is N*480 lines (this is the "some want a pixel height" case the probe
            // pins against the multiplier emulators). Flycast has no clean 4-value aspect enum and its other
            // levers are left to its own config for this landing, so ONLY resolution maps -> every other lever is
            // unsupported and emits nothing (the unsupported-yields-no-edit path, demonstrated).
            if (s.resMultiplier != 0)
                emit_(QStringLiteral("emu.cfg"), QStringLiteral("config"), QStringLiteral("rend.Resolution"),
                      QString::number(s.resMultiplier * 480));
        }
        // An unknown emulator id (or one with no mapping) yields no edits — degrade to "open the emulator for
        // this", never guess a key.
        return out;
    }
}
