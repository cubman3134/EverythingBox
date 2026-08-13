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
//     supports the system AND this build can run RetroPark — the RetroPark target (displayed off cores[0]).
//   * STANDALONE system (externalEmulator non-empty): the system's default emulator FIRST, then any
//     EmulatorRegistry emulator bound to the system (e.systems contains sys->id), de-duped; THEN — if RetroPark
//     supports the system AND this build can run RetroPark — the RetroPark target (displayed off the display name).
// `retroParkAvailable` is the BUILD/platform gate: a build without the RetroPark runtime (Android TV, iOS) passes
// false and NO RetroPark target is ever offered — the picker must not surface a target prepareCore would degrade
// away. It is a plain bool (NOT a macro inside this pure model) so probe_emutargets can enumerate BOTH values.
// Deterministic and pure (SystemCatalog + EmulatorRegistry data only). A null system yields an empty list.
inline QList<EmulationTarget> emulationTargetsFor(const GameSystem* sys, bool retroParkAvailable)
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

    if (retroParkAvailable && retroParkSupportsSystem(sys->id))
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
//   * A RetroPark backend yields the RetroPark target ONLY where retroParkSupportsSystem(sys->id) AND this build
//     can run RetroPark (retroParkAvailable); otherwise it degrades to the system's built-in libretro/standalone
//     default (mirrors clampBackendToSystem on the libretro arm and the standalone divert's support gate — Slice
//     3b, no brick). The retroParkAvailable term makes the CURRENT-VALUE DISPLAY match what prepareCore actually
//     runs on a build WITHOUT RetroPark: a stored/synced backend=retropark then resolves to (and displays) the
//     underlying engine, not a "(retropark)" label the launch would never honour.
//   * Standalone underlying := resolveEmulatorId(perSystemEmulator|externalEmulator, ov, registered ids).
//   * Libretro  underlying := resolveCore(perSystemCore|cores[0], ov, sys->cores).
// perSystemCore / perSystemEmulator empty means "inherit the system built-in" (cores[0] / externalEmulator),
// matching Settings::coreFor's empty-is-default posture. `retroParkAvailable` is the BUILD/platform gate (a plain
// bool, not a macro — the probe tests both); the local Dolphin VEHICLE is a further LAUNCH-time device gate that
// resolveLaunch/prepareCore apply on top (Task 3), NOT modelled here.
inline EmulationTarget resolveEmulationTarget(const GameSystem* sys, const LaunchOpts::Override& ov,
                                              const QString& perSystemCore, const QString& perSystemEmulator,
                                              EmuBackend perSystemBackend, bool retroParkAvailable)
{
    if (!sys) return EmulationTarget{};

    const EmuBackend backend = LaunchOpts::resolveBackend(perSystemBackend, ov);
    if (backend == EmuBackend::RetroPark && retroParkAvailable && retroParkSupportsSystem(sys->id))
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

// The CorePlan-relevant outcome of a launch (Unified Emulation Picker Task 3): the FINAL engine + resolved
// levers GameLauncher::prepareCore maps onto its CorePlan, AFTER applying the two launch-time RetroPark gates
// the pure resolveEmulationTarget above deliberately does NOT model:
//   * `retroParkAvailable` — false on a build WITHOUT RetroPark (no RetroParkView to launch on, no on-device
//     picker to change the setting). A resolved RetroPark target then degrades to the underlying engine so a
//     synced backend=retropark can never route open() to an inert surface (the cross-platform clamp).
//   * `dolphinVehiclePresent` — the local-only Dolphin vehicle (dolphin_present.dll) staged on THIS machine.
//     A PRESENTING RetroPark system (gc) needs it; absent, it degrades to the system's external emulator (the
//     Slice-3b clamp, no brick). A DRIVEN RetroPark system (nes shim, built into EB) needs neither gate.
// Factored pure so probe_emutargets mutation-tests the whole target->CorePlan mapping without constructing a
// GameLauncher (which needs a RetroView + full app state). prepareCore fills only corePath / error / archive
// handling on top of this; every CorePlan FIELD DECISION lives here.
struct ResolvedLaunch
{
    EmuEngine  engine = EmuEngine::Libretro;   // the FINAL engine after the vehicle / cross-platform gates
    QString    core;                           // libretro core (also the DRIVEN RetroPark shim core); else empty
    QString    externalEmulatorId;             // standalone emulator id; empty for libretro / presenting-retropark
    EmuBackend backend = EmuBackend::Libretro; // CorePlan::backend — RetroPark only for an honoured RetroPark target
    bool       retroparkPresenting = false;    // CorePlan::retroparkPresenting (gc presenting core → Vulkan runtime)
};

inline ResolvedLaunch resolveLaunch(const GameSystem* sys, const LaunchOpts::Override& ov,
                                    const QString& perSystemCore, const QString& perSystemEmulator,
                                    EmuBackend perSystemBackend, bool retroParkAvailable, bool dolphinVehiclePresent)
{
    ResolvedLaunch r;
    if (!sys) return r;

    // Ask the pure resolver for the SUPPORT-gated target (retroParkAvailable=true here): resolveLaunch stays the
    // launch-time authority for the cross-platform + vehicle clamps below, so its output is byte-identical to
    // before — passing the real retroParkAvailable in would degrade here instead, to the same final engine.
    const EmulationTarget t = resolveEmulationTarget(sys, ov, perSystemCore, perSystemEmulator, perSystemBackend,
                                                     /*retroParkAvailable=*/true);

    // Apply the launch-time RetroPark gates the pure resolver leaves to prepareCore. A RetroPark target that
    // cannot be honoured degrades to the system's UNDERLYING engine (libretro core / external emulator) — the
    // 3b clamp — so an un-honourable opt-in never bricks the launch.
    EmuEngine engine = t.engine;
    bool presenting = false;
    if (engine == EmuEngine::RetroPark)
    {
        presenting = retroParkSystemIsPresenting(sys->id);
        // Invariant: a RetroPark STANDALONE system needs the local vehicle only when PRESENTING. Today gc is the
        // only RetroPark standalone system and it IS presenting, so this equals the old "vehicle always required";
        // a future DRIVEN standalone RetroPark system would correctly NOT require it — do not collapse this term
        // to an unconditional vehicle check.
        const bool honour = retroParkAvailable && (!presenting || dolphinVehiclePresent);
        if (!honour)
            engine = sys->externalEmulator.isEmpty() ? EmuEngine::Libretro : EmuEngine::Standalone;
    }
    r.engine = engine;

    switch (engine)
    {
        case EmuEngine::Standalone:
        {
            // The standalone underlying — also the fallback when a presenting RetroPark target's vehicle was
            // absent. resolveEmulatorId(perSystemEmulator|externalEmulator, ov, registered ids), identical to
            // the standalone target's ref for a genuine standalone resolution.
            QStringList validEmuIds;
            for (const ExternalEmulator& e : EmulatorRegistry::all()) validEmuIds << e.id;
            const QString baseId = perSystemEmulator.isEmpty() ? sys->externalEmulator : perSystemEmulator;
            r.externalEmulatorId = LaunchOpts::resolveEmulatorId(baseId, ov, validEmuIds);
            break;
        }
        case EmuEngine::RetroPark:
            r.backend = EmuBackend::RetroPark;
            r.retroparkPresenting = presenting;
            // A DRIVEN RetroPark target (nes shim) still carries a resolved libretro core to RetroParkView (its
            // openGame takes the core name); a PRESENTING target (gc) renders GC/Wii itself and has no core.
            if (!presenting)
            {
                const QString baseCore = perSystemCore.isEmpty() ? sys->cores.value(0) : perSystemCore;
                r.core = LaunchOpts::resolveCore(baseCore, ov, sys->cores);
            }
            break;
        case EmuEngine::Libretro:
        {
            const QString baseCore = perSystemCore.isEmpty() ? sys->cores.value(0) : perSystemCore;
            r.core = LaunchOpts::resolveCore(baseCore, ov, sys->cores);
            break;
        }
    }
    return r;
}
