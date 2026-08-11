// Device performance profiles (issue #119) — the PURE heart. #103 shipped the WRITE mechanism (EmuGfx::Settings
// + configEdits): a cross-emulator graphics quartet written into an emulator's config on launch. But it has no
// notion of HARDWARE, so a fresh install on a 15W handheld and a desktop GPU tower get identical defaults, and
// the handheld user discovers PS2-at-3x doesn't hold 60fps by playing it. This adds the missing notion: identify
// the machine, then hand back a tuned EmuGfx::Settings the hardware can actually hold — a THIRD, WEAKEST layer
// under #103's per-system default and #51/#95's per-game override.
//
// PURE, header-only, QtCore-only, NO WMI / NO OS CALLS. identify() classifies from inputs the CALLER gathers (a
// DMI product string on x86, an ro.product string on Android, and a capability triple for the generic fallback);
// defaultsFor() is a data table keyed by (profile, emulator id). Keeping both pure is what lets
// probe_deviceprofile pin every known string -> the right Kind, every capability bucket -> the right tier, and
// the whole table exhaustively, against hand-authored fixtures, with no hardware. The thin platform glue that
// actually reads the registry / ro.product lives in DeviceProfileDetect (non-pure) and only feeds this.
//
// NEVER PRETEND TO BE SURE. An unrecognised machine does NOT get a wrong handheld guess — it falls to a
// capability TIER (Low/Mid/High), and when even that is unknowable, Unknown is a real value whose defaults are
// all-unset (a no-op: the emulator's / user's own values stand, so a machine with no opinion is regression-free).
//
// PRECEDENCE (applied in GameLauncher via EmuGfx::resolve): per-game > per-system > per-device > unset. The
// device layer is the weakest, so it only fills levers neither the game nor the system set, and an all-unset
// device profile changes nothing.
#pragma once
#include <QString>
#include "EmuSettings.h"

namespace DeviceProfile
{
    // The classification result. A KNOWN handheld (by DMI/ro.product) or a generic capability TIER or Unknown.
    // isHandheld is true only for the named handhelds — a tier says nothing about form factor, so GenericLow is
    // NOT a handheld even though many handhelds land there when their DMI string is unrecognised.
    enum class Kind
    {
        Unknown = 0,
        SteamDeckLCD,     // Valve Jupiter
        SteamDeckOLED,    // Valve Galileo
        RogAlly,          // Asus ROG Ally (RC71L) / Ally X (RC72LA) — same Z1-class APU
        LegionGo,         // Lenovo Legion Go (83E1)
        GenericLow,       // capability fallback — weak iGPU / low RAM / few cores
        GenericMid,       // capability fallback — midrange
        GenericHigh,      // capability fallback — strong desktop-class
    };

    struct Profile
    {
        Kind    kind        = Kind::Unknown;
        QString displayName;
        bool    isHandheld  = false;

        bool operator==(const Profile& o) const
        { return kind == o.kind && displayName == o.displayName && isHandheld == o.isHandheld; }
        bool operator!=(const Profile& o) const { return !(*this == o); }
    };

    // The capability probe inputs for the generic tier fallback. The caller gathers whatever it can; any field
    // may be absent (empty renderer, 0 cores, 0 RAM) and the tier logic degrades gracefully.
    struct Capability
    {
        QString gpuRenderer;      // GL_RENDERER-style string, or a driver description; "" if unknown
        int     logicalCores = 0; // QThread::idealThreadCount(); 0 if unknown
        double  ramGB        = 0.0;// total physical RAM in GiB; 0 if unknown
    };

    // ---- pure: is a renderer string a SOFTWARE rasteriser (no real GPU)? -----------------------------------
    // llvmpipe / swiftshader / "software" / Microsoft Basic Render Driver all mean "no usable GPU" — that pins a
    // machine to Low regardless of RAM/cores. Matched case-insensitively on well-known substrings.
    inline bool isSoftwareRenderer(const QString& renderer)
    {
        const QString r = renderer.toLower();
        return r.contains(QLatin1String("llvmpipe"))
            || r.contains(QLatin1String("swiftshader"))
            || r.contains(QLatin1String("softpipe"))
            || r.contains(QLatin1String("software"))
            || r.contains(QLatin1String("basic render"));   // "Microsoft Basic Render Driver"
    }

    // ---- pure: capability triple -> a generic tier ---------------------------------------------------------
    // DOCUMENTED THRESHOLDS (checked in order):
    //   1. a SOFTWARE renderer  -> GenericLow  (no GPU can hold any upscale, whatever the CPU/RAM)
    //   2. ramGB >= 16 AND cores >= 8            -> GenericHigh (desktop-class)
    //   3. ramGB <  8  OR  cores <= 4            -> GenericLow  (handheld / thin-client class)
    //   4. otherwise                             -> GenericMid
    // When nothing is known (empty renderer, 0 cores, 0 RAM) rule 3 fires (0 < 8) and the machine is treated as
    // Low — the safe direction: a conservative cap never asks a weak GPU for more than it can hold.
    inline Kind tierFromCapability(const Capability& cap)
    {
        if (isSoftwareRenderer(cap.gpuRenderer)) return Kind::GenericLow;
        if (cap.ramGB >= 16.0 && cap.logicalCores >= 8) return Kind::GenericHigh;
        if (cap.ramGB < 8.0 || cap.logicalCores <= 4)   return Kind::GenericLow;
        return Kind::GenericMid;
    }

    // ---- pure: a known x86 DMI product string -> a handheld Kind (or Unknown) -------------------------------
    // The handheld list is SHORT and well-known; these are the public board/product codenames the vendors ship
    // in SMBIOS. Matched case-insensitively and by substring so a "Jupiter" vs "Galileo" (exact Valve product
    // names) and a "ROG Ally RC71L_RC71L" / "83E1" (vendor product names that carry extra tokens) all resolve.
    // An unrecognised string returns Unknown — the caller then falls to a capability tier, never a wrong guess.
    inline Kind fromDmiProduct(const QString& product)
    {
        const QString p = product.trimmed().toLower();
        if (p.isEmpty()) return Kind::Unknown;
        if (p.contains(QLatin1String("jupiter"))) return Kind::SteamDeckLCD;   // Valve Steam Deck LCD
        if (p.contains(QLatin1String("galileo"))) return Kind::SteamDeckOLED;  // Valve Steam Deck OLED
        if (p.contains(QLatin1String("rc71l")) || p.contains(QLatin1String("rc72la"))
            || p.contains(QLatin1String("rog ally"))) return Kind::RogAlly;    // Asus ROG Ally / Ally X
        if (p.contains(QLatin1String("legion go")) || p.contains(QLatin1String("83e1")))
            return Kind::LegionGo;                                             // Lenovo Legion Go
        return Kind::Unknown;
    }

    // ---- pure: an Android ro.product.* string -> a handheld Kind (or Unknown) -------------------------------
    // Best-effort for the Android handhelds (Retroid/AYN and friends). The concrete device list is far less
    // stable than the x86 four, so v1 recognises none by name and always returns Unknown here — the caller then
    // uses the capability tier, which is the honest answer for an Android box we can't positively name. The hook
    // exists (and is pinned by the probe as "unknown -> Unknown, never a wrong guess") so a future list slots in
    // without touching identify()'s shape.
    inline Kind fromAndroidProduct(const QString& roProduct)
    {
        Q_UNUSED(roProduct);
        return Kind::Unknown;
    }

    // ---- pure: Kind -> display name / handheld flag --------------------------------------------------------
    inline QString displayName(Kind k)
    {
        switch (k)
        {
            case Kind::SteamDeckLCD:  return QStringLiteral("Steam Deck (LCD)");
            case Kind::SteamDeckOLED: return QStringLiteral("Steam Deck (OLED)");
            case Kind::RogAlly:       return QStringLiteral("ROG Ally");
            case Kind::LegionGo:      return QStringLiteral("Legion Go");
            case Kind::GenericLow:    return QStringLiteral("Low-power device");
            case Kind::GenericMid:    return QStringLiteral("Midrange device");
            case Kind::GenericHigh:   return QStringLiteral("High-performance device");
            case Kind::Unknown:       return QStringLiteral("Unknown device");
        }
        return QStringLiteral("Unknown device");
    }
    inline bool isHandheld(Kind k)
    {
        switch (k)
        {
            case Kind::SteamDeckLCD:
            case Kind::SteamDeckOLED:
            case Kind::RogAlly:
            case Kind::LegionGo:
                return true;
            default:
                return false;   // a generic tier / Unknown says nothing about form factor
        }
    }

    // ---- pure: the top-level classifier -------------------------------------------------------------------
    // A named x86 handheld wins (a DMI match is a positive identification); else a named Android handheld; else
    // the capability tier. Never returns a handheld Kind it did not positively match.
    inline Profile identify(const QString& dmiProduct, const QString& androidProduct, const Capability& cap)
    {
        Kind k = fromDmiProduct(dmiProduct);
        if (k == Kind::Unknown) k = fromAndroidProduct(androidProduct);
        if (k == Kind::Unknown) k = tierFromCapability(cap);
        Profile p;
        p.kind        = k;
        p.displayName = displayName(k);
        p.isHandheld  = isHandheld(k);
        return p;
    }

    // ---- pure: Kind <-> stable token (device-local persistence + the manual override) ----------------------
    inline QString kindToken(Kind k)
    {
        switch (k)
        {
            case Kind::SteamDeckLCD:  return QStringLiteral("steamdeck-lcd");
            case Kind::SteamDeckOLED: return QStringLiteral("steamdeck-oled");
            case Kind::RogAlly:       return QStringLiteral("rog-ally");
            case Kind::LegionGo:      return QStringLiteral("legion-go");
            case Kind::GenericLow:    return QStringLiteral("generic-low");
            case Kind::GenericMid:    return QStringLiteral("generic-mid");
            case Kind::GenericHigh:   return QStringLiteral("generic-high");
            case Kind::Unknown:       return QStringLiteral("unknown");
        }
        return QStringLiteral("unknown");
    }
    inline Kind kindFromToken(const QString& t)
    {
        const QString s = t.trimmed().toLower();
        if (s == QLatin1String("steamdeck-lcd"))  return Kind::SteamDeckLCD;
        if (s == QLatin1String("steamdeck-oled")) return Kind::SteamDeckOLED;
        if (s == QLatin1String("rog-ally"))        return Kind::RogAlly;
        if (s == QLatin1String("legion-go"))       return Kind::LegionGo;
        if (s == QLatin1String("generic-low"))     return Kind::GenericLow;
        if (s == QLatin1String("generic-mid"))     return Kind::GenericMid;
        if (s == QLatin1String("generic-high"))    return Kind::GenericHigh;
        return Kind::Unknown;
    }

    // A Profile from a persisted/override Kind token (the display name + handheld flag are DERIVED, never stored,
    // so they cannot drift from the Kind).
    inline Profile profileFromToken(const QString& t)
    {
        const Kind k = kindFromToken(t);
        Profile p;
        p.kind        = k;
        p.displayName = displayName(k);
        p.isHandheld  = isHandheld(k);
        return p;
    }

    // ---- pure: the tuned per-device, per-emulator defaults TABLE -------------------------------------------
    // Returns the EmuGfx::Settings this device should use for this emulator as the WEAKEST layer under
    // per-system and per-game. Design of the table:
    //   * A HANDHELD caps internal resolution to what its APU holds at 60fps and pins the renderer to Vulkan
    //     (the backend these handhelds ship) with vsync On (a battery/tearing default). The Deck (RDNA2 8CU,
    //     15W) is more conservative than the Z1-class Ally / Legion Go (RDNA3), so a Deck gets PS2 at 2x while
    //     an Ally gets 3x — the concrete "2x not 3x" opinion the issue asks for.
    //   * A GENERIC LOW / MID tier caps resolution too but leaves the RENDERER unset — forcing Vulkan on an
    //     arbitrary desktop GPU is not our call, and the emulator's own backend auto-pick is fine there.
    //   * A GENERIC HIGH tier and Unknown leave EVERYTHING unset — a no-op. A strong desktop defers entirely to
    //     the emulator's / user's own values (and a machine we can't identify must never be second-guessed).
    //   * Resolution is a MULTIPLIER for dolphin/pcsx2/duckstation and (via configEdits) becomes a pixel height
    //     for flycast; this table always speaks in the multiplier, and configEdits does the flycast conversion.
    //   * An emulator this table has no opinion on -> an all-unset Settings (no edit). The four #103 maps
    //     (pcsx2/dolphin/duckstation/flycast) are the ones with opinions; everything else is left alone.
    inline EmuGfx::Settings defaultsFor(const Profile& profile, const QString& emulatorId)
    {
        // NOTE: EmuGfx::Settings/Renderer/Vsync are FULLY QUALIFIED throughout — the app links a global `Settings`
        // namespace, so a `using namespace EmuGfx` here makes the bare name `Settings` ambiguous in the app TU.
        EmuGfx::Settings s;   // all-unset by default -> a no-op unless a branch below fills levers

        const QString id = emulatorId.trimmed().toLower();
        const bool dolphin = (id == QLatin1String("dolphin"));
        const bool pcsx2   = (id == QLatin1String("pcsx2"));
        const bool duck    = (id == QLatin1String("duckstation"));
        const bool flycast = (id == QLatin1String("flycast"));
        if (!(dolphin || pcsx2 || duck || flycast)) return s;   // no opinion for other emulators

        // A handheld shares a shape: cap the multiplier per emulator, force Vulkan, vsync On. The only thing
        // that varies across the handhelds is the multiplier, from a per-Kind budget.
        auto handheld = [&](int mDolphin, int mPcsx2, int mDuck, int mFlycast) {
            if (dolphin) s.resMultiplier = mDolphin;
            if (pcsx2)   s.resMultiplier = mPcsx2;
            if (duck)    s.resMultiplier = mDuck;
            if (flycast) s.resMultiplier = mFlycast;
            s.renderer = EmuGfx::Renderer::Vulkan;   // the backend these handhelds ship; flycast ignores it (no-op edit)
            s.vsync    = EmuGfx::Vsync::On;           // tearing/battery default on a fixed handheld panel
        };
        // A generic tier caps the multiplier only — renderer/vsync left to the emulator on an unknown desktop.
        auto genericRes = [&](int mDolphin, int mPcsx2, int mDuck, int mFlycast) {
            if (dolphin) s.resMultiplier = mDolphin;
            if (pcsx2)   s.resMultiplier = mPcsx2;
            if (duck)    s.resMultiplier = mDuck;
            if (flycast) s.resMultiplier = mFlycast;
        };

        switch (profile.kind)
        {
            // Deck LCD/OLED: RDNA2, 8 CU, ~15W. PS2 at 2x (not 3x), GC at 2x, PS1 cheap at 4x, DC at 2x.
            case Kind::SteamDeckLCD:
            case Kind::SteamDeckOLED:
                handheld(/*dolphin*/2, /*pcsx2*/2, /*duck*/4, /*flycast*/2);
                break;
            // Z1-class RDNA3 handhelds (ROG Ally / Ally X / Legion Go): a step up — GC/PS2 at 3x, PS1 at 5x, DC 3x.
            case Kind::RogAlly:
            case Kind::LegionGo:
                handheld(/*dolphin*/3, /*pcsx2*/3, /*duck*/5, /*flycast*/3);
                break;
            // Generic tiers: cap resolution, leave renderer/vsync alone.
            case Kind::GenericLow:
                genericRes(/*dolphin*/2, /*pcsx2*/2, /*duck*/3, /*flycast*/2);
                break;
            case Kind::GenericMid:
                genericRes(/*dolphin*/3, /*pcsx2*/3, /*duck*/5, /*flycast*/3);
                break;
            // High + Unknown: everything unset — the emulator/user value stands (no-op, no regression).
            case Kind::GenericHigh:
            case Kind::Unknown:
                break;
        }
        return s;
    }
}
