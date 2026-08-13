// The unified "emulation target" model (Unified Emulation Picker, Task 1). ONE responsibility: describe every
// concrete way a game can run on a system as a single engine-tagged run-target, so the per-game picker and the
// per-system default become ONE choice instead of the split Core / Emulator + Backend selection.
//
// An emulation TARGET is an (engine, ref) pair with a tagged display and a stable id:
//   * Libretro core   -> engine=Libretro,  ref=<core base name>,      id="libretro:<core>",     "<core> (libretro)"
//   * RetroPark       -> engine=RetroPark,  ref="" (system implies     id="retropark",           "<underlying> (retropark)"
//                        the underlying: NES->FCEUmm shim, gc->Dolphin)
//   * Standalone      -> engine=Standalone, ref=<ExternalEmulator id>, id="standalone:<emuId>",  "<display> (standalone)"
//
// This is PURE VOCABULARY (Qt6::Core only), a sibling of EmuBackend.h: it composes the EXISTING levers rather
// than adding a store — enumeration reads SystemCatalog + EmulatorRegistry, selection maps a target back onto the
// existing LaunchOpts::Override fields (core / emulatorId / backend), and resolution reuses the mutation-tested
// LaunchOpts::resolveCore / resolveEmulatorId / resolveBackend + the retroParkSupportsSystem support-gate so the
// semantics match Slice 3b exactly (a RetroPark target only survives where RetroPark supports the system;
// otherwise it degrades to the system's built-in libretro/standalone default — the 3b clamp, no store change).
//
// Header-only: the helpers are trivial and pure. Any target that includes this links the LaunchOpts resolvers
// (LaunchOptionsStore.cpp) it delegates to; nothing else new.
#pragma once
#include <QList>
#include <QString>
#include <QStringList>

#include "EmuBackend.h"          // backend vocabulary + retroParkSupportsSystem support-gate
#include "SystemCatalog.h"       // GameSystem (cores / externalEmulator) + SystemCatalog::byId
#include "EmulatorRegistry.h"    // ExternalEmulator (displayName / systems) + EmulatorRegistry::all/byId
#include "LaunchOptionsStore.h"  // LaunchOpts::Override + the resolveCore/resolveEmulatorId/resolveBackend resolvers

// The three engines a game can launch on. Libretro is the historical default; RetroPark and Standalone are the
// two alternatives the picker tags. (Distinct from EmuBackend, which names only the in-process pair
// Libretro/RetroPark — Standalone is a child-process launch that has no EmuBackend value.)
enum class EmuEngine { Libretro, RetroPark, Standalone };

// One concrete run-target. `ref` is the engine's payload (core base name / "" / emulator id); `displayName` is
// the engine-tagged label shown in the picker; `id` is the stable string form for storage/logging.
struct EmulationTarget
{
    EmuEngine engine = EmuEngine::Libretro;
    QString   ref;          // libretro: core base name; retropark: "" ; standalone: ExternalEmulator id
    QString   displayName;  // "<core> (libretro)" / "<underlying> (retropark)" / "<display> (standalone)"
    QString   id;           // "libretro:<core>" / "retropark" / "standalone:<emuId>"
};

namespace EmulationTargets
{
    // ---- pure builders: one spelling per target ----------------------------------------------------------
    inline EmulationTarget libretro(const QString& core)
    {
        EmulationTarget t;
        t.engine = EmuEngine::Libretro;
        t.ref = core;
        t.displayName = core + QStringLiteral(" (libretro)");
        t.id = QStringLiteral("libretro:") + core;
        return t;
    }

    // The RetroPark target for a system. ref is empty (the system implies the underlying core/emulator); the
    // display tags the system's UNDERLYING default — its externalEmulator's display for a standalone system
    // (gc -> "Dolphin"), else its default libretro core cores[0] (nes -> "fceumm").
    inline EmulationTarget retropark(const GameSystem* sys)
    {
        QString underlying;
        if (sys && !sys->externalEmulator.isEmpty())
        {
            const ExternalEmulator* e = EmulatorRegistry::byId(sys->externalEmulator);
            underlying = e ? e->displayName : sys->externalEmulator;
        }
        else if (sys)
        {
            underlying = sys->cores.value(0);
        }
        EmulationTarget t;
        t.engine = EmuEngine::RetroPark;
        t.ref.clear();
        t.displayName = underlying + QStringLiteral(" (retropark)");
        t.id = QStringLiteral("retropark");
        return t;
    }

    inline EmulationTarget standalone(const QString& emuId)
    {
        const ExternalEmulator* e = EmulatorRegistry::byId(emuId);
        const QString disp = e ? e->displayName : emuId;
        EmulationTarget t;
        t.engine = EmuEngine::Standalone;
        t.ref = emuId;
        t.displayName = disp + QStringLiteral(" (standalone)");
        t.id = QStringLiteral("standalone:") + emuId;
        return t;
    }
}

// Enumerate every run-target a system offers, in picker order:
//   * LIBRETRO system (externalEmulator empty): one target per candidate core (cores[i]), THEN — if RetroPark
//     supports the system — the RetroPark target (displayed off cores[0]).
//   * STANDALONE system (externalEmulator non-empty): the system's default emulator FIRST, then any
//     EmulatorRegistry emulator bound to the system (e.systems contains sys->id), de-duped; THEN — if RetroPark
//     supports the system — the RetroPark target (displayed off the externalEmulator's display name).
// Deterministic and pure (SystemCatalog + EmulatorRegistry data only). A null system yields an empty list.
inline QList<EmulationTarget> emulationTargetsFor(const GameSystem* sys)
{
    QList<EmulationTarget> out;
    if (!sys) return out;

    if (sys->externalEmulator.isEmpty())
    {
        for (const QString& core : sys->cores)
            out.push_back(EmulationTargets::libretro(core));
    }
    else
    {
        // The system's own default emulator leads; bound registry emulators follow in registry order, de-duped.
        QStringList ids;
        ids << sys->externalEmulator;
        for (const ExternalEmulator& e : EmulatorRegistry::all())
            if (e.systems.contains(sys->id) && !ids.contains(e.id))
                ids << e.id;
        for (const QString& id : ids)
            out.push_back(EmulationTargets::standalone(id));
    }

    if (retroParkSupportsSystem(sys->id))
        out.push_back(EmulationTargets::retropark(sys));

    return out;
}

// Map a chosen target back onto the EXISTING per-game Override levers (no store schema change). Each engine sets
// its own lever and CLEARS the others, so a target is a self-consistent unit:
//   * Libretro   -> core=<ref>, backend="libretro", emulatorId cleared.
//   * RetroPark  -> backend="retropark", core+emulatorId cleared (the system implies the underlying).
//   * Standalone -> emulatorId=<ref>, backend+core cleared.
// extraArgs / updatedAt are untouched (the picker owns engine selection, not the per-emulator extra args).
inline void applyTargetToOverride(const EmulationTarget& t, LaunchOpts::Override& ov)
{
    switch (t.engine)
    {
        case EmuEngine::Libretro:
            ov.core = t.ref;
            ov.backend = backendToString(EmuBackend::Libretro);   // "libretro"
            ov.emulatorId.clear();
            break;
        case EmuEngine::RetroPark:
            ov.backend = backendToString(EmuBackend::RetroPark);  // "retropark"
            ov.core.clear();
            ov.emulatorId.clear();
            break;
        case EmuEngine::Standalone:
            ov.emulatorId = t.ref;
            ov.backend.clear();
            ov.core.clear();
            break;
    }
}

// Resolve the EFFECTIVE target for a launch: per-GAME override wins where it selects a target, else the
// per-SYSTEM defaults, else the system BUILT-IN default. Reuses the mutation-tested LaunchOpts resolvers so the
// precedence matches GameLauncher::prepareCore:
//   * backend := resolveBackend(perSystemBackend, ov)  — the per-game override backend beats the per-system
//     default; an empty/unknown override inherits the default (which may itself be RetroPark).
//   * A RetroPark backend yields the RetroPark target ONLY where retroParkSupportsSystem(sys->id); otherwise it
//     degrades to the system's built-in libretro/standalone default (mirrors clampBackendToSystem on the
//     libretro arm and the standalone divert's support gate — Slice 3b, no brick).
//   * Standalone underlying := resolveEmulatorId(perSystemEmulator|externalEmulator, ov, registered ids).
//   * Libretro  underlying := resolveCore(perSystemCore|cores[0], ov, sys->cores).
// perSystemCore / perSystemEmulator empty means "inherit the system built-in" (cores[0] / externalEmulator),
// matching Settings::coreFor's empty-is-default posture. Vehicle presence is a LAUNCH-time device gate handled
// by prepareCore (Task 3), NOT modelled here — this pure model gates only on system SUPPORT.
inline EmulationTarget resolveEmulationTarget(const GameSystem* sys, const LaunchOpts::Override& ov,
                                              const QString& perSystemCore, const QString& perSystemEmulator,
                                              EmuBackend perSystemBackend)
{
    if (!sys) return EmulationTarget{};

    const EmuBackend backend = LaunchOpts::resolveBackend(perSystemBackend, ov);
    if (backend == EmuBackend::RetroPark && retroParkSupportsSystem(sys->id))
        return EmulationTargets::retropark(sys);

    // Not RetroPark (or clamped away because the system does not support it): the underlying engine's default.
    if (!sys->externalEmulator.isEmpty())
    {
        QStringList validEmuIds;
        for (const ExternalEmulator& e : EmulatorRegistry::all()) validEmuIds << e.id;
        const QString baseId = perSystemEmulator.isEmpty() ? sys->externalEmulator : perSystemEmulator;
        return EmulationTargets::standalone(LaunchOpts::resolveEmulatorId(baseId, ov, validEmuIds));
    }

    const QString baseCore = perSystemCore.isEmpty() ? sys->cores.value(0) : perSystemCore;
    return EmulationTargets::libretro(LaunchOpts::resolveCore(baseCore, ov, sys->cores));
}
