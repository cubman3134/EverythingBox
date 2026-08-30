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

// The PLATFORM gate for the standalone engine: can this build spawn an external emulator process at all?
// False on Android and iOS, whose sandboxes cannot launch a downloaded desktop executable — GameLauncher::open
// refuses every standalone system there. Callers pass this as the `standaloneAvailable` argument below; the
// pure functions take a plain bool (NOT this macro) precisely so probe_emutargets can enumerate BOTH values.
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
inline constexpr bool kStandaloneBuildAvailable = false;
#else
inline constexpr bool kStandaloneBuildAvailable = true;
#endif

// THE standalone-gate rule, in ONE place — enumeration, current-value resolution and launch resolution all ask
// this, so the picker can never display an engine the launcher would not run (the divergence this header exists
// to prevent). The standalone engine survives when this build can spawn an external emulator process, OR when
// the system has NO libretro cores: the gate is a DEGRADE, and a system that declares no cores (gc / 3ds / nds)
// has nothing to degrade TO. Firing it there would strand the system on an empty core name — a blank
// " (libretro)" target with an empty ref — so it keeps its standalone target on every platform and the launcher
// keeps surfacing its existing "isn't supported on Android" refusal, which is the honest outcome.
// A null system is treated as not surviving (callers guard on `sys` first; this only keeps the helper total).
inline bool standaloneEngineSurvives(const GameSystem* sys, bool standaloneAvailable)
{
    return sys != nullptr && (standaloneAvailable || sys->cores.isEmpty());
}

// The registry emulators BOUND to a system: every ExternalEmulator whose `systems` list names this system id,
// in registry order. This binding is LOAD-BEARING, not the informational metadata EmulatorRegistry's header
// once called it: it is the second, weaker way an emulator reaches a system, alongside the system's own
// GameSystem::externalEmulator field.
//   * externalEmulator says "this emulator is this system's DEFAULT engine" (gc -> Dolphin);
//   * `systems` says "this emulator can RUN this system", making it selectable without moving the default.
// ares/n64 is the case that needs the second: N64 keeps mupen64plus_next as its default (it is the only N64
// engine RetroAchievements works on) while ares is offered in the picker. Both emulationTargetsFor and
// resolveEmulationTarget ask this ONE function, so the offered list and the resolved value cannot diverge.
// A user's own <data>/emulators/*.json entry declaring `systems` binds the same way — that is now a real
// wiring lever, not just round-tripped schema.
inline QStringList boundEmulatorsFor(const QString& systemId)
{
    QStringList ids;
    if (systemId.isEmpty()) return ids;
    for (const ExternalEmulator& e : EmulatorRegistry::all())
        if (e.systems.contains(systemId)) ids << e.id;
    return ids;
}

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
//   * LIBRETRO system (externalEmulator empty): one target per candidate core (cores[i]), THEN one standalone
//     target per registry emulator BOUND to the system (boundEmulatorsFor — e.systems contains sys->id), in
//     registry order and subject to the standalone platform gate, THEN — if RetroPark supports the system AND
//     this build can run RetroPark — the RetroPark target (displayed off cores[0]). The bound-emulator entries
//     are how an emulator reaches a system whose DEFAULT stays libretro: n64 offers
//     [libretro:mupen64plus_next, libretro:parallel_n64, standalone:ares, retropark], with the default still
//     leading because the cores come first. (Before this, ExternalEmulator::systems was read only on the
//     standalone branch below, so a libretro system could never offer a standalone target at all.)
//   * STANDALONE system (externalEmulator non-empty): the system's default emulator FIRST, then any
//     EmulatorRegistry emulator bound to the system (e.systems contains sys->id), de-duped; THEN one target per
//     candidate core (cores[i]) — a standalone system's cores are reachable too, so a user can move one game (or
//     the whole system) back onto the in-process tier; THEN — if RetroPark supports the system AND this build can
//     run RetroPark — the RetroPark target (displayed off the display name).
// `retroParkAvailable` is the BUILD/platform gate: a build without the RetroPark runtime (Android TV, iOS) passes
// false and NO RetroPark target is ever offered — the picker must not surface a target prepareCore would degrade
// away. `standaloneAvailable` is the matching gate for the STANDALONE engine, applied through
// standaloneEngineSurvives (the ONE spelling of that rule, shared with resolveEmulationTarget/resolveLaunch), and
// it gates ONLY the standalone entries — the system's own AND its bound ones — the core targets above are
// offered either way:
//   * gate OFF, system HAS cores (psx): the standalone entries are dropped and the list is exactly its cores,
//     which is precisely what resolveLaunch degrades the system to and what resolveEmulationTarget displays.
//   * gate OFF, system has NO cores (gc / 3ds / nds): the gate does not fire — there is nothing to degrade to —
//     so the list is exactly its standalone entries, matching the Standalone engine resolveLaunch still returns
//     (the launcher then surfaces its own "isn't supported" refusal).
// So the list is never empty for a system that declares an external emulator or any cores, on any platform: an
// empty picker beside a resolved current value is the exact divergence this header exists to prevent.
// Both are plain bools (NOT macros inside this pure model) so probe_emutargets can enumerate BOTH values.
// Deterministic and pure (SystemCatalog + EmulatorRegistry data only). A null system yields an empty list.
inline QList<EmulationTarget> emulationTargetsFor(const GameSystem* sys, bool retroParkAvailable,
                                                 bool standaloneAvailable)
{
    QList<EmulationTarget> out;
    if (!sys) return out;

    if (sys->externalEmulator.isEmpty())
    {
        // The system's cores lead, so its DEFAULT (cores[0]) is still the first row of the picker...
        for (const QString& core : sys->cores)
            out.push_back(EmulationTargets::libretro(core));
        // ...then any registry emulator BOUND to it (n64 -> ares), in registry order, behind the same platform
        // gate the standalone branch below uses. The gate is standaloneEngineSurvives and NOT a bare
        // `standaloneAvailable`: for a libretro system that declares cores the two are identical (which is every
        // libretro row in the catalog), but where they differ — a hypothetical CORELESS libretro system — using
        // the shared helper is what keeps this list and resolveEmulationTarget's `standaloneHolds` term the SAME
        // predicate, and a picker that offers a target the resolver would not return (or hides one it would) is
        // the exact divergence this header exists to prevent. No de-dup pass is needed here the way the
        // standalone branch needs one: this branch runs only when externalEmulator is EMPTY, so a bound id can
        // never repeat the system's own default.
        if (standaloneEngineSurvives(sys, standaloneAvailable))
            for (const QString& id : boundEmulatorsFor(sys->id))
                out.push_back(EmulationTargets::standalone(id));
    }
    else if (standaloneEngineSurvives(sys, standaloneAvailable))
    {
        // The system's own default emulator leads; bound registry emulators follow in registry order, de-duped
        // (a system that is standalone AND bound to the same emulator lists it once).
        QStringList ids;
        ids << sys->externalEmulator;
        for (const QString& id : boundEmulatorsFor(sys->id))
            if (!ids.contains(id))
                ids << id;
        for (const QString& id : ids)
            out.push_back(EmulationTargets::standalone(id));
    }

    // A STANDALONE system also offers its libretro cores, so a user can move one game (or the whole system)
    // back onto the in-process tier from the picker — and so the target list names the same cores the
    // platform gate above degrades to. A libretro system already listed its cores in the first branch.
    if (!sys->externalEmulator.isEmpty())
        for (const QString& core : sys->cores)
            out.push_back(EmulationTargets::libretro(core));

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

// Resolve the EFFECTIVE target for a launch. THE precedence ladder for engine selection lives here and nowhere
// else (resolveLaunch derives its engine from this function), in this exact order:
//
//   a. RETROPARK, WHERE IT IS AS SPECIFIC AS THE RUNGS IT JUMPS. backend := resolveBackend(perSystemBackend, ov)
//      — the per-game override backend beats the per-system default; an empty/unknown override inherits the
//      default (which may itself be RetroPark). A RetroPark backend yields the RetroPark target only when BOTH
//      of the following hold:
//        * the RetroPark selection is at least as specific as the per-GAME levers below — i.e. either the
//          override ITSELF names the retropark backend (a per-game RetroPark pick, which always wins here), or
//          the override selects no engine at all (ov.core and ov.emulatorId both empty), in which case a
//          per-SYSTEM RetroPark default wins over rungs (c)/(d) exactly as it always has. A per-system RetroPark
//          default must NOT beat a per-game core or emulator selection: applyTargetToOverride CLEARS ov.backend
//          for a Standalone target (and sets "libretro" for a Libretro one), so a per-game standalone pick on a
//          system defaulted to RetroPark still has resolveBackend returning RetroPark from the INHERITED default
//          — firing this rung on it would snap the per-game tick back to the retropark row and launch RetroPark,
//          inverting the very precedence this ladder exists to express. The per-game/per-system split is read
//          off the Override, since resolveBackend cannot report which source its answer came from;
//        * retroParkSupportsSystem(sys->id) AND this build can run RetroPark (retroParkAvailable).
//      Otherwise it falls through to the ladder below (mirrors clampBackendToSystem on the libretro arm and the
//      standalone divert's support gate — Slice 3b, no brick).
//      The retroParkAvailable term makes the CURRENT-VALUE DISPLAY match what prepareCore actually runs on a
//      build WITHOUT RetroPark: a stored/synced backend=retropark then resolves to (and displays) the underlying
//      engine, not a "(retropark)" label the launch would never honour.
//   b. PER-GAME override. ov.core non-empty -> the LIBRETRO arm; else ov.emulatorId non-empty -> the STANDALONE
//      arm. An EXPLICIT per-game core selection therefore beats the system's standalone built-in: this is what
//      makes the libretro cores a standalone system offers (emulationTargetsFor's second core loop) actually
//      reachable — picking "swanstation (libretro)" on a psx game must resolve BACK to libretro:swanstation, not
//      revert to standalone:duckstation. Before this the standalone arm was taken before the override was ever
//      consulted, so the picker listed targets the resolver could never return.
//   c. PER-SYSTEM default, consulted only when the per-game override selected no engine above. perSystemCore
//      non-empty -> the LIBRETRO arm; else perSystemEmulator non-empty -> the STANDALONE arm. (The per-system
//      writers — MainWindow::setSystemEmulationDefault / SettingsDialog::applySystemEmulationTarget — set one
//      lever and CLEAR the other, so exactly one of these is ever set.)
//   d. SYSTEM BUILT-IN: the STANDALONE arm ONLY where the system DECLARES an externalEmulator, else the
//      LIBRETRO arm. A merely BOUND emulator (rung (e)) does not move this rung: n64 is bound to ares and still
//      defaults to libretro:mupen64plus_next, which is the whole point of the binding.
//   e. The PLATFORM GATE applies to any standalone outcome above: standaloneEngineSurvives(sys,
//      standaloneAvailable) — where it does not survive, that outcome falls to the LIBRETRO arm. Symmetrically,
//      an engine the system does not HAVE cannot be selected. The standalone engine is one the system HAS when
//      it either declares an externalEmulator OR some registered emulator declares this system in its `systems`
//      list (boundEmulatorsFor) — the same binding emulationTargetsFor offers, so an explicit ares pick on n64
//      resolves while a STALE/synced ov.emulatorId on an unbound libretro system (nes) still cannot put it on
//      the standalone engine and try to spawn a child process for it. An explicit core selection takes the
//      libretro arm only where the system declares cores (a coreless system — gc / 3ds / nds — would otherwise
//      be stranded on a blank " (libretro)" target with an empty ref, the same stranding
//      standaloneEngineSurvives exists to prevent). Neither guard can hide an OFFERED target: a system with no
//      cores offers no libretro target, and a system with no declared and no bound emulator offers no
//      standalone one.
//      One more empty-ref guard sits in the standalone arm itself: a system with no DECLARED emulator has an
//      empty base id, so a stale ov.emulatorId naming an emulator the registry no longer offers resolves to ""
//      (resolveEmulatorId's documented fall-back) — a bare " (standalone)" target the launcher could never run.
//      That case falls to the libretro arm instead. It cannot arise for a declared system, whose base id is
//      never empty, so every existing standalone resolution is byte-identical.
//
// Within the chosen arm the ref comes from the mutation-tested LaunchOpts resolvers, unchanged — the ladder picks
// WHICH ARM, not how a ref resolves inside it:
//   * Standalone underlying := resolveEmulatorId(perSystemEmulator|externalEmulator, ov, registered ids).
//   * Libretro  underlying := resolveCore(perSystemCore|cores[0], ov, sys->cores).
// So a STALE lever still degrades exactly as those resolvers document: an ov.core naming a core the system no
// longer offers keeps the libretro ARM (the user did choose the in-process tier) but falls back to the base core,
// never erroring the launch out.
// perSystemCore / perSystemEmulator empty means "inherit the system built-in" (cores[0] / externalEmulator),
// matching Settings::coreFor's empty-is-default posture. `retroParkAvailable` and `standaloneAvailable` are the
// BUILD/platform gates (plain bools, not macros — the probe tests both values of each); the local Dolphin
// VEHICLE is a further LAUNCH-time device gate that resolveLaunch/prepareCore apply on top (Task 3), NOT
// modelled here.
inline EmulationTarget resolveEmulationTarget(const GameSystem* sys, const LaunchOpts::Override& ov,
                                              const QString& perSystemCore, const QString& perSystemEmulator,
                                              EmuBackend perSystemBackend, bool retroParkAvailable,
                                              bool standaloneAvailable)
{
    if (!sys) return EmulationTarget{};

    // (a) RETROPARK, but only where the RetroPark selection is at least as SPECIFIC as the per-game levers this
    // rung jumps ahead of. resolveBackend cannot tell the two sources apart (it returns RetroPark both for a
    // per-game ov.backend="retropark" and for an empty override inheriting a RetroPark per-system default), so
    // ask the Override directly: tryBackendFromString is the same recognised-spelling test resolveBackend uses,
    // so `perGameRetroPark` is exactly "the override itself named retropark".
    EmuBackend ovBackend;
    const bool perGameRetroPark   = tryBackendFromString(ov.backend, ovBackend)
                                    && ovBackend == EmuBackend::RetroPark;
    const bool perGameSelectsArm  = !ov.core.isEmpty() || !ov.emulatorId.isEmpty();
    const EmuBackend backend = LaunchOpts::resolveBackend(perSystemBackend, ov);
    if (backend == EmuBackend::RetroPark && (perGameRetroPark || !perGameSelectsArm)
        && retroParkAvailable && retroParkSupportsSystem(sys->id))
        return EmulationTargets::retropark(sys);

    // Not RetroPark (or clamped away because the system does not support it): rungs (b)..(e) of the ladder.
    // First, which engines this system HAS at all. The standalone engine needs the platform gate —
    // standaloneEngineSurvives is the ONE spelling of that rule, shared with emulationTargetsFor and
    // resolveLaunch — AND an emulator that reaches this system, which is EITHER of:
    //   * the system DECLARES one (externalEmulator): standalone is then also its BUILT-IN default, rung (d);
    //   * a registered emulator BINDS it (its `systems` list names sys->id): standalone is then SELECTABLE but
    //     not the default — exactly the ares/n64 case, and exactly what emulationTargetsFor offers.
    // Keeping the two terms apart is what preserves the anti-stale protection this guard exists for: a stale or
    // synced ov.emulatorId on an unbound libretro system (nes) still resolves to libretro, never to a child
    // process. Where the gate degrades (build cannot spawn a process AND the system has cores), the system
    // displays the libretro core it will actually launch on; where it cannot degrade (no cores), it displays the
    // standalone target resolveLaunch still resolves to. Either way the current value matches what prepareCore
    // launches. The libretro engine needs at least one candidate core to name.
    const bool declaresEmulator  = !sys->externalEmulator.isEmpty();
    const bool standaloneBuiltIn = declaresEmulator && standaloneEngineSurvives(sys, standaloneAvailable);
    const bool standaloneHolds   = (declaresEmulator || !boundEmulatorsFor(sys->id).isEmpty())
                                   && standaloneEngineSurvives(sys, standaloneAvailable);
    const bool libretroHolds     = !sys->cores.isEmpty();

    // (d) the system built-in — which is standalone only where the system DECLARES an emulator, so a bound-only
    // system (n64) keeps its libretro default — then overridden by (c) the per-system default and (b) the
    // per-game override, each of which selects an engine only through the lever it owns and each of which may
    // select the standalone engine wherever it `standaloneHolds` (declared OR bound). Every standalone outcome
    // carries rung (e)'s platform gate, applied once in the two terms above, for all four rungs.
    bool standaloneArm = standaloneBuiltIn;                                                  // (d) built-in
    if      (!ov.core.isEmpty())           standaloneArm = standaloneHolds && !libretroHolds; // (b) per-game core
    else if (!ov.emulatorId.isEmpty())     standaloneArm = standaloneHolds;                   // (b) per-game emu
    else if (!perSystemCore.isEmpty())     standaloneArm = standaloneHolds && !libretroHolds; // (c) per-system core
    else if (!perSystemEmulator.isEmpty()) standaloneArm = standaloneHolds;                   // (c) per-system emu

    if (standaloneArm)
    {
        QStringList validEmuIds;
        for (const ExternalEmulator& e : EmulatorRegistry::all()) validEmuIds << e.id;
        const QString baseId = perSystemEmulator.isEmpty() ? sys->externalEmulator : perSystemEmulator;
        // Rung (e)'s empty-ref guard: on a system with no DECLARED emulator the base id is empty, so a stale
        // ov.emulatorId the registry no longer offers resolves to "" — never return that as " (standalone)".
        const QString emuId = LaunchOpts::resolveEmulatorId(baseId, ov, validEmuIds);
        if (!emuId.isEmpty())
            return EmulationTargets::standalone(emuId);
    }

    const QString baseCore = perSystemCore.isEmpty() ? sys->cores.value(0) : perSystemCore;
    return EmulationTargets::libretro(LaunchOpts::resolveCore(baseCore, ov, sys->cores));
}

// The CorePlan-relevant outcome of a launch (Unified Emulation Picker Task 3): the FINAL engine + resolved
// levers GameLauncher::prepareCore maps onto its CorePlan. The ENGINE is whatever resolveEmulationTarget's
// precedence ladder above returns — this function does NOT re-decide it, so an explicit libretro selection on a
// standalone system (rung (b)/(c)) launches in-process here too, and there is exactly one home for that rule.
// On top of the ladder it applies the launch-time gates the pure resolver deliberately does NOT model:
//   * `retroParkAvailable` — false on a build WITHOUT RetroPark (no RetroParkView to launch on, no on-device
//     picker to change the setting). A resolved RetroPark target then degrades to the underlying engine so a
//     synced backend=retropark can never route open() to an inert surface (the cross-platform clamp).
//   * `dolphinVehiclePresent` — the local-only Dolphin vehicle (dolphin_present.dll) staged on THIS machine.
//     A PRESENTING RetroPark system (gc) needs it; absent, it degrades to the system's external emulator (the
//     Slice-3b clamp, no brick). A DRIVEN RetroPark system (nes shim, built into EB) needs neither gate.
//   * `standaloneAvailable` — false on a build that cannot spawn an external emulator process at all (Android,
//     iOS, where GameLauncher::open refuses every standalone system). A resolved Standalone engine then
//     degrades to the system's libretro cores WHERE IT HAS ANY; a system that declares none (gc / 3ds / nds)
//     stays Standalone so the launcher keeps surfacing its existing "isn't supported" message. That is the
//     standaloneEngineSurvives rule, and emulationTargetsFor / resolveEmulationTarget apply the SAME one, so
//     the offered list and the displayed current value never diverge from what this function resolves.
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
                                    EmuBackend perSystemBackend, bool retroParkAvailable, bool dolphinVehiclePresent,
                                    bool standaloneAvailable)
{
    ResolvedLaunch r;
    if (!sys) return r;

    // Ask the pure resolver for the SUPPORT-gated target (retroParkAvailable=true here): resolveLaunch stays the
    // launch-time authority for the cross-platform + vehicle clamps below, so its output is byte-identical to
    // before — passing the real retroParkAvailable in would degrade here instead, to the same final engine.
    // standaloneAvailable=true here is BEHAVIOURALLY INERT and pinned for clarity: `t` is consulted for its
    // ENGINE only (core / externalEmulatorId are re-resolved in the switch below), and the only engine the
    // standalone gate can change is Standalone -> Libretro, which the shared standaloneEngineSurvives clamp
    // below re-applies verbatim — so passing the real flag in would produce the same ResolvedLaunch for every
    // system. Pinning it keeps ONE site responsible for the platform clamp: this function, at the point where
    // the engine being degraded FROM is still visible. (It is NOT true that a gate-off gc would come back
    // Libretro — gc declares no cores, so standaloneEngineSurvives keeps it Standalone in the resolver too.)
    // The precedence ladder does not disturb that: every one of its standalone outcomes is guarded by the same
    // standaloneEngineSurvives term, so gate-off can still only turn Standalone into Libretro.
    const EmulationTarget t = resolveEmulationTarget(sys, ov, perSystemCore, perSystemEmulator, perSystemBackend,
                                                     /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);

    // Apply the launch-time RetroPark gates the pure resolver leaves to prepareCore. A RetroPark target that
    // cannot be honoured degrades to the system's UNDERLYING engine — which is exactly "the ladder with rung (a)
    // switched off", so ask the SAME resolver for it (retroParkAvailable=false can never return RetroPark) rather
    // than re-deciding the engine here. That keeps the precedence ladder in one place: a user who explicitly
    // selected a libretro core still degrades onto THAT core's engine, not onto the standalone built-in.
    // Rung (a)'s per-game/per-system specificity test needs NO counterpart here for the same reason: this
    // function never asks whether the backend is RetroPark, only whether the ENGINE the ladder returned is —
    // so a per-game standalone/core pick over a per-system RetroPark default now arrives here as Standalone /
    // Libretro and takes the matching switch arm (backend Libretro, presenting false), with no change needed.
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
            engine = resolveEmulationTarget(sys, ov, perSystemCore, perSystemEmulator, perSystemBackend,
                                            /*retroParkAvailable=*/false, /*standaloneAvailable=*/true).engine;
    }
    // The PLATFORM gate, through the shared standaloneEngineSurvives rule: a build that cannot spawn an external
    // emulator degrades a standalone system to its libretro cores, but only where the system HAS cores —
    // gc / 3ds / nds declare none, so they stay Standalone and the launcher surfaces its existing "isn't
    // supported on Android" message unchanged. Same rule the picker enumerates and displays.
    if (engine == EmuEngine::Standalone && !standaloneEngineSurvives(sys, standaloneAvailable))
        engine = EmuEngine::Libretro;
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
