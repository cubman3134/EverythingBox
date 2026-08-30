// Headless check of the unified emulation-target model (src/core/EmulationTarget.h, Unified Emulation Picker
// Task 1) — the pure model that turns a system into a list of engine-tagged run-targets, maps a chosen target
// onto the existing LaunchOpts::Override levers, and resolves the effective target (per-game override ->
// per-system default -> system built-in). Qt6::Core only (it reads SystemCatalog + EmulatorRegistry, both of
// which merge over this process's OWN scratch data dir — empty here — so the built-in tables are exactly what
// is enumerated), so it runs under the offscreen QPA in CI. It pins:
//
//   * ENUMERATION — emulationTargetsFor(nes, true) = [libretro:fceumm, libretro:nestopia, retropark] and
//     emulationTargetsFor(gc, true) = [standalone:dolphin, retropark], each with the exact tagged display string;
//     with retroParkAvailable=false the RetroPark target is OMITTED (nes -> 2, gc -> 1) — the build/platform gate.
//   * SELECTION  — applyTargetToOverride sets the one lever its engine owns and CLEARS the other two.
//   * RESOLUTION — resolveEmulationTarget precedence: a per-game retropark override on gc -> retropark; empty
//     override + per-system core=nestopia on nes -> libretro:nestopia; nothing set -> the built-in default
//     (nes->libretro:fceumm, gc->standalone:dolphin); a retropark selection on a NON-supported system falls back
//     to the built-in (the Slice-3b support clamp); and a retropark selection with retroParkAvailable=false falls
//     back to the built-in too (the DISPLAY gate — the current value must match what prepareCore launches).
//   * ROUND-TRIP  — the invariant tying the two together: for nes / psx / gc, EVERY target emulationTargetsFor
//     offers resolves back to itself, both when selected per-game (applyTargetToOverride over an empty override)
//     and when selected as the per-system default. The picker can never list a target the resolver won't return.
//
// Prints EMUTARGETS-OK on success; any failure prints EMUTARGETS-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE HAND-COMPUTED ORACLES: expected ids / displays / override fields are written as literals derived
// from the SystemCatalog + EmulatorRegistry built-in tables and the model's documented rules — never produced by
// re-running the function under test — so an assertion cannot pass merely because it echoed the code.
#include "EmulationTarget.h"
#include "SystemCatalog.h"
#include "EmulatorRegistry.h"
#include "EmuBackend.h"
#include "LaunchOptionsStore.h"

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "EMUTARGETS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using LaunchOpts::Override;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const GameSystem* nes = SystemCatalog::byId(QStringLiteral("nes"));
    const GameSystem* gc  = SystemCatalog::byId(QStringLiteral("gc"));
    CHECK(nes != nullptr);
    CHECK(gc  != nullptr);
    if (!nes || !gc) { std::fprintf(stderr, "EMUTARGETS: catalog missing nes/gc\n"); return 1; }

    // ---- 1. Enumeration: a LIBRETRO system (nes). Built-in cores are {fceumm, nestopia} (cores[0]=fceumm) and
    //         RetroPark supports nes, so with retroParkAvailable=true the picker order is: each core (libretro),
    //         then RetroPark tagged off cores[0]. Every id and display is a hand-written literal (NOT read back).
    {
        const QList<EmulationTarget> t = emulationTargetsFor(nes, /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
        CHECK(t.size() == 3);
        if (t.size() == 3)
        {
            CHECK(t[0].engine == EmuEngine::Libretro);
            CHECK(t[0].id == QStringLiteral("libretro:fceumm"));
            CHECK(t[0].ref == QStringLiteral("fceumm"));
            CHECK(t[0].displayName == QStringLiteral("fceumm (libretro)"));

            CHECK(t[1].engine == EmuEngine::Libretro);
            CHECK(t[1].id == QStringLiteral("libretro:nestopia"));
            CHECK(t[1].ref == QStringLiteral("nestopia"));
            CHECK(t[1].displayName == QStringLiteral("nestopia (libretro)"));

            CHECK(t[2].engine == EmuEngine::RetroPark);
            CHECK(t[2].id == QStringLiteral("retropark"));
            CHECK(t[2].ref.isEmpty());
            CHECK(t[2].displayName == QStringLiteral("fceumm (retropark)")); // tagged off cores[0]
        }

        // A build WITHOUT RetroPark (retroParkAvailable=false — Android TV / iOS) OMITS the RetroPark target: the
        // two libretro cores remain, no retropark entry. This is the fix's headline: the picker must not offer a
        // target prepareCore would degrade away. Dropping the retroParkAvailable check in emulationTargetsFor
        // (always appending retropark) wrongly yields size 3 here -> mutation-kill.
        const QList<EmulationTarget> tNo = emulationTargetsFor(nes, /*retroParkAvailable=*/false, /*standaloneAvailable=*/true);
        CHECK(tNo.size() == 2);
        if (tNo.size() == 2)
        {
            CHECK(tNo[0].id == QStringLiteral("libretro:fceumm"));
            CHECK(tNo[1].id == QStringLiteral("libretro:nestopia"));
        }
        for (const EmulationTarget& x : tNo) CHECK(x.engine != EmuEngine::RetroPark);
    }

    // ---- 2. Enumeration: a STANDALONE system (gc). externalEmulator=dolphin, no other registry emulator is
    //         bound to gc (built-in `systems` fields are empty), RetroPark supports gc, so with
    //         retroParkAvailable=true the order is: standalone Dolphin, then RetroPark tagged off Dolphin's
    //         display. Literals, not read back.
    {
        const QList<EmulationTarget> t = emulationTargetsFor(gc, /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
        CHECK(t.size() == 2);
        if (t.size() == 2)
        {
            CHECK(t[0].engine == EmuEngine::Standalone);
            CHECK(t[0].id == QStringLiteral("standalone:dolphin"));
            CHECK(t[0].ref == QStringLiteral("dolphin"));
            CHECK(t[0].displayName == QStringLiteral("Dolphin (standalone)")); // EmulatorRegistry dolphin display

            CHECK(t[1].engine == EmuEngine::RetroPark);
            CHECK(t[1].id == QStringLiteral("retropark"));
            CHECK(t[1].ref.isEmpty());
            CHECK(t[1].displayName == QStringLiteral("Dolphin (retropark)")); // tagged off externalEmulator display
        }

        // retroParkAvailable=false OMITS the RetroPark target on the standalone tier too: only the Dolphin
        // standalone target survives (size 1). Same mutation-kill of the retroParkAvailable check.
        const QList<EmulationTarget> tNo = emulationTargetsFor(gc, /*retroParkAvailable=*/false, /*standaloneAvailable=*/true);
        CHECK(tNo.size() == 1);
        if (tNo.size() == 1)
        {
            CHECK(tNo[0].engine == EmuEngine::Standalone);
            CHECK(tNo[0].id == QStringLiteral("standalone:dolphin"));
        }
        for (const EmulationTarget& x : tNo) CHECK(x.engine != EmuEngine::RetroPark);
    }

    // ---- 3. applyTargetToOverride: each engine sets its OWN lever and CLEARS the other two. Start from an
    //         override that has all three levers dirty, so a mutant that forgets to clear a field is caught.
    {
        // Libretro -> core=<ref>, backend="libretro", emulatorId cleared.
        {
            Override ov; ov.core = QStringLiteral("stale"); ov.emulatorId = QStringLiteral("stale");
            ov.backend = QStringLiteral("stale");
            applyTargetToOverride(EmulationTargets::libretro(QStringLiteral("nestopia")), ov);
            CHECK(ov.core == QStringLiteral("nestopia"));
            CHECK(ov.backend == QStringLiteral("libretro"));
            CHECK(ov.emulatorId.isEmpty());
        }
        // RetroPark -> backend="retropark", core+emulatorId cleared.
        {
            Override ov; ov.core = QStringLiteral("stale"); ov.emulatorId = QStringLiteral("stale");
            ov.backend = QStringLiteral("stale");
            applyTargetToOverride(EmulationTargets::retropark(gc), ov);
            CHECK(ov.backend == QStringLiteral("retropark"));
            CHECK(ov.core.isEmpty());
            CHECK(ov.emulatorId.isEmpty());
        }
        // Standalone -> emulatorId=<ref>, backend+core cleared.
        {
            Override ov; ov.core = QStringLiteral("stale"); ov.emulatorId = QStringLiteral("stale");
            ov.backend = QStringLiteral("stale");
            applyTargetToOverride(EmulationTargets::standalone(QStringLiteral("dolphin")), ov);
            CHECK(ov.emulatorId == QStringLiteral("dolphin"));
            CHECK(ov.backend.isEmpty());
            CHECK(ov.core.isEmpty());
        }
    }

    // ---- 4. resolveEmulationTarget precedence. Hand-computed expected ids from the rules; per-system defaults
    //         are passed IN (the model is pure — Settings is not consulted).
    {
        Override empty;

        // (a) A per-GAME retropark override on gc -> the RetroPark target (gc is supported). The override beats
        //     the (Libretro) per-system default. This is the headline opt-in.
        {
            Override ovRp; ovRp.backend = QStringLiteral("retropark");
            const EmulationTarget t = resolveEmulationTarget(gc, ovRp, QString(), QString(), EmuBackend::Libretro,
                                                             /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
            CHECK(t.engine == EmuEngine::RetroPark);
            CHECK(t.id == QStringLiteral("retropark"));
            CHECK(t.displayName == QStringLiteral("Dolphin (retropark)"));
        }

        // (b) Empty override + per-system core default "nestopia" on nes -> libretro:nestopia (the per-system
        //     default is honoured when there is no per-game override). nestopia IS a nes candidate.
        {
            const EmulationTarget t = resolveEmulationTarget(nes, empty, QStringLiteral("nestopia"), QString(),
                                                             EmuBackend::Libretro, /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
            CHECK(t.engine == EmuEngine::Libretro);
            CHECK(t.id == QStringLiteral("libretro:nestopia"));
            CHECK(t.ref == QStringLiteral("nestopia"));
        }

        // (c) Nothing set -> the system BUILT-IN default: nes -> libretro:fceumm (cores[0]); gc -> standalone:dolphin.
        {
            const EmulationTarget tn = resolveEmulationTarget(nes, empty, QString(), QString(), EmuBackend::Libretro,
                                                              /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
            CHECK(tn.engine == EmuEngine::Libretro);
            CHECK(tn.id == QStringLiteral("libretro:fceumm"));

            const EmulationTarget tg = resolveEmulationTarget(gc, empty, QString(), QString(), EmuBackend::Libretro,
                                                              /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
            CHECK(tg.engine == EmuEngine::Standalone);
            CHECK(tg.id == QStringLiteral("standalone:dolphin"));
        }

        // (d) A retropark SELECTION on a NON-supported system falls back to that system's built-in default (the
        //     Slice-3b support clamp). snes does NOT support RetroPark, so a per-game retropark override on snes
        //     resolves to libretro:snes9x (cores[0]) — it must NOT surface a retropark target. A global RetroPark
        //     per-system default (empty override, perSystemBackend=RetroPark) clamps the same way. Dropping the
        //     retroParkSupportsSystem gate would wrongly yield a retropark target here (mutation-kill).
        {
            const GameSystem* snes = SystemCatalog::byId(QStringLiteral("snes"));
            CHECK(snes != nullptr);
            if (snes)
            {
                Override ovRp; ovRp.backend = QStringLiteral("retropark");
                const EmulationTarget tov = resolveEmulationTarget(snes, ovRp, QString(), QString(),
                                                                   EmuBackend::Libretro, /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
                CHECK(tov.engine == EmuEngine::Libretro);
                CHECK(tov.id == QStringLiteral("libretro:snes9x")); // snes cores[0]
                CHECK(tov.engine != EmuEngine::RetroPark);

                const EmulationTarget tdef = resolveEmulationTarget(snes, empty, QString(), QString(),
                                                                    EmuBackend::RetroPark, /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
                CHECK(tdef.engine == EmuEngine::Libretro);
                CHECK(tdef.id == QStringLiteral("libretro:snes9x"));
            }
        }

        // (e) A per-GAME core override wins over the per-system core default (both must be nes candidates). This
        //     defends the override>per-system ordering for the libretro arm.
        {
            Override ovCore; ovCore.core = QStringLiteral("fceumm");
            const EmulationTarget t = resolveEmulationTarget(nes, ovCore, QStringLiteral("nestopia"), QString(),
                                                             EmuBackend::Libretro, /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
            CHECK(t.id == QStringLiteral("libretro:fceumm")); // override fceumm beats per-system nestopia
        }

        // (f) A per-GAME emulator override on a standalone system wins, when it names a registered emulator id
        //     (cemu is a real EmulatorRegistry id). Defends the override>default ordering for the standalone arm.
        {
            Override ovEmu; ovEmu.emulatorId = QStringLiteral("cemu");
            const EmulationTarget t = resolveEmulationTarget(gc, ovEmu, QString(), QString(), EmuBackend::Libretro,
                                                             /*retroParkAvailable=*/true, /*standaloneAvailable=*/true);
            CHECK(t.engine == EmuEngine::Standalone);
            CHECK(t.id == QStringLiteral("standalone:cemu"));
        }

        // (g) THE DISPLAY GATE (this fix): a stored/synced backend=retropark override with retroParkAvailable=FALSE
        //     (Android TV / iOS build) must resolve to the system's BUILT-IN engine, NOT the retropark target — so
        //     the picker's current-value DISPLAY matches what prepareCore actually launches (libretro/standalone).
        //     gc -> standalone:dolphin; nes -> libretro:fceumm. With retroParkAvailable=TRUE (proven in (a)) the
        //     same override yields retropark, so this pins the new term specifically. Dropping the retroParkAvailable
        //     check in resolveEmulationTarget wrongly surfaces the retropark target here -> mutation-kill.
        {
            Override ovRp; ovRp.backend = QStringLiteral("retropark");

            const EmulationTarget tg = resolveEmulationTarget(gc, ovRp, QString(), QString(), EmuBackend::Libretro,
                                                              /*retroParkAvailable=*/false, /*standaloneAvailable=*/true);
            CHECK(tg.engine == EmuEngine::Standalone);
            CHECK(tg.id == QStringLiteral("standalone:dolphin"));
            CHECK(tg.engine != EmuEngine::RetroPark);

            const EmulationTarget tn = resolveEmulationTarget(nes, ovRp, QString(), QString(), EmuBackend::Libretro,
                                                              /*retroParkAvailable=*/false, /*standaloneAvailable=*/true);
            CHECK(tn.engine == EmuEngine::Libretro);
            CHECK(tn.id == QStringLiteral("libretro:fceumm"));
            CHECK(tn.engine != EmuEngine::RetroPark);

            // A per-SYSTEM RetroPark default (empty override, perSystemBackend=RetroPark) clamps the same way when
            // the build lacks RetroPark: gc displays the standalone default, not "(retropark)".
            const EmulationTarget tgDef = resolveEmulationTarget(gc, empty, QString(), QString(),
                                                                 EmuBackend::RetroPark, /*retroParkAvailable=*/false, /*standaloneAvailable=*/true);
            CHECK(tgDef.engine == EmuEngine::Standalone);
            CHECK(tgDef.id == QStringLiteral("standalone:dolphin"));
        }
    }

    // ---- 5. resolveLaunch: the target -> CorePlan-field mapping GameLauncher::prepareCore now uses (Unified
    //         Emulation Picker Task 3). Composes resolveEmulationTarget with the two launch-time RetroPark gates
    //         (retroParkAvailable = the cross-platform clamp; dolphinVehiclePresent = the 3b vehicle gate for the
    //         PRESENTING gc core), then reports the FINAL engine + resolved core / externalEmulatorId / backend /
    //         retroparkPresenting. prepareCore isn't headless-constructible, so this pins the whole mapping. All
    //         expected values are hand-computed literals from the documented rules, never read back.
    {
        Override empty;
        Override ovRp; ovRp.backend = QStringLiteral("retropark");
        const GameSystem* snes = SystemCatalog::byId(QStringLiteral("snes"));

        // (a) DEFAULT NES (nothing set, RetroPark built + no vehicle needed for a driven system) -> libretro:fceumm.
        //     backend Libretro, core fceumm, NO externalEmulatorId, not presenting. This is the byte-identical
        //     libretro default: mutating the mapping to set externalEmulatorId or flip backend fails here.
        {
            const ResolvedLaunch r = resolveLaunch(nes, empty, QString(), QString(), EmuBackend::Libretro,
                                                   /*retroParkAvailable*/true, /*vehicle*/false, /*standaloneAvailable*/true);
            CHECK(r.engine == EmuEngine::Libretro);
            CHECK(r.core == QStringLiteral("fceumm"));
            CHECK(r.externalEmulatorId.isEmpty());
            CHECK(r.backend == EmuBackend::Libretro);
            CHECK(r.retroparkPresenting == false);
        }

        // (b) DEFAULT gc (nothing set) -> standalone: externalEmulatorId=dolphin, backend Libretro, no core. The
        //     byte-identical standalone default. Vehicle staged but irrelevant (no RetroPark opt-in).
        {
            const ResolvedLaunch r = resolveLaunch(gc, empty, QString(), QString(), EmuBackend::Libretro,
                                                   /*retroParkAvailable*/true, /*vehicle*/true, /*standaloneAvailable*/true);
            CHECK(r.engine == EmuEngine::Standalone);
            CHECK(r.externalEmulatorId == QStringLiteral("dolphin"));
            CHECK(r.core.isEmpty());
            CHECK(r.backend == EmuBackend::Libretro);
            CHECK(r.retroparkPresenting == false);
        }

        // (c) gc + per-game retropark override + vehicle PRESENT -> PRESENTING RetroPark: backend RetroPark,
        //     presenting true, NO externalEmulatorId, NO core (the Vulkan runtime renders GC itself). The headline
        //     opt-in. Dropping the presenting=true assignment, or leaking externalEmulatorId, fails here.
        {
            const ResolvedLaunch r = resolveLaunch(gc, ovRp, QString(), QString(), EmuBackend::Libretro,
                                                   /*retroParkAvailable*/true, /*vehicle*/true, /*standaloneAvailable*/true);
            CHECK(r.engine == EmuEngine::RetroPark);
            CHECK(r.backend == EmuBackend::RetroPark);
            CHECK(r.retroparkPresenting == true);
            CHECK(r.externalEmulatorId.isEmpty());
            CHECK(r.core.isEmpty());
        }

        // (d) gc + retropark override + vehicle ABSENT -> the 3b clamp: falls back to the external emulator
        //     (engine Standalone, externalEmulatorId=dolphin, backend Libretro). Dropping the vehicle gate would
        //     wrongly keep it on RetroPark and fails here (mutation-kill of the vehicle term).
        {
            const ResolvedLaunch r = resolveLaunch(gc, ovRp, QString(), QString(), EmuBackend::Libretro,
                                                   /*retroParkAvailable*/true, /*vehicle*/false, /*standaloneAvailable*/true);
            CHECK(r.engine == EmuEngine::Standalone);
            CHECK(r.externalEmulatorId == QStringLiteral("dolphin"));
            CHECK(r.backend == EmuBackend::Libretro);
            CHECK(r.retroparkPresenting == false);
        }

        // (e) nes + retropark override + RetroPark available -> DRIVEN RetroPark: backend RetroPark, presenting
        //     FALSE, and it STILL carries the shim core (fceumm) for RetroParkView::openGame. The nes shim needs
        //     no vehicle (vehicle=false here). This is the 2b driven path: mutating presenting to true, or dropping
        //     the core resolution on the driven arm, fails here.
        {
            const ResolvedLaunch r = resolveLaunch(nes, ovRp, QString(), QString(), EmuBackend::Libretro,
                                                   /*retroParkAvailable*/true, /*vehicle*/false, /*standaloneAvailable*/true);
            CHECK(r.engine == EmuEngine::RetroPark);
            CHECK(r.backend == EmuBackend::RetroPark);
            CHECK(r.retroparkPresenting == false);
            CHECK(r.core == QStringLiteral("fceumm"));
            CHECK(r.externalEmulatorId.isEmpty());
        }

        // (f) The cross-platform clamp: !retroParkAvailable degrades a resolved RetroPark target to the underlying
        //     engine. gc -> Standalone/dolphin; nes -> Libretro/fceumm. Both keep backend Libretro. Mutating the
        //     retroParkAvailable gate (always honour) would wrongly surface RetroPark and fails here.
        {
            const ResolvedLaunch rg = resolveLaunch(gc, ovRp, QString(), QString(), EmuBackend::Libretro,
                                                    /*retroParkAvailable*/false, /*vehicle*/true, /*standaloneAvailable*/true);
            CHECK(rg.engine == EmuEngine::Standalone);
            CHECK(rg.externalEmulatorId == QStringLiteral("dolphin"));
            CHECK(rg.backend == EmuBackend::Libretro);

            const ResolvedLaunch rn = resolveLaunch(nes, ovRp, QString(), QString(), EmuBackend::Libretro,
                                                    /*retroParkAvailable*/false, /*vehicle*/false, /*standaloneAvailable*/true);
            CHECK(rn.engine == EmuEngine::Libretro);
            CHECK(rn.core == QStringLiteral("fceumm"));
            CHECK(rn.backend == EmuBackend::Libretro);
        }

        // (g) A per-SYSTEM emulatorFor default (emuDefault=cemu) on gc drives externalEmulatorId=cemu (cemu is a
        //     real registry id). Empty per-game override -> the per-system default is honoured. Pins that
        //     resolveLaunch threads perSystemEmulator into the standalone base (Task 2 <-> Task 3 wiring).
        {
            const ResolvedLaunch r = resolveLaunch(gc, empty, QString(), QStringLiteral("cemu"), EmuBackend::Libretro,
                                                   /*retroParkAvailable*/true, /*vehicle*/true, /*standaloneAvailable*/true);
            CHECK(r.engine == EmuEngine::Standalone);
            CHECK(r.externalEmulatorId == QStringLiteral("cemu"));
        }

        // (h) A per-SYSTEM backendFor default of RetroPark on gc (empty override) + vehicle present -> PRESENTING
        //     RetroPark. The opt-in can come from the per-system default, not only a per-game override. And the
        //     same default on the UNSUPPORTED snes clamps to libretro:snes9x (never surfaces RetroPark).
        {
            const ResolvedLaunch r = resolveLaunch(gc, empty, QString(), QString(), EmuBackend::RetroPark,
                                                   /*retroParkAvailable*/true, /*vehicle*/true, /*standaloneAvailable*/true);
            CHECK(r.engine == EmuEngine::RetroPark);
            CHECK(r.retroparkPresenting == true);

            if (snes)
            {
                const ResolvedLaunch rs = resolveLaunch(snes, empty, QString(), QString(), EmuBackend::RetroPark,
                                                        /*retroParkAvailable*/true, /*vehicle*/true, /*standaloneAvailable*/true);
                CHECK(rs.engine == EmuEngine::Libretro);
                CHECK(rs.core == QStringLiteral("snes9x"));
                CHECK(rs.backend == EmuBackend::Libretro);
            }
        }

        // (i) A per-SYSTEM core default (coreDefault=nestopia) on nes, no override -> libretro core nestopia (the
        //     Task 2/3 core wiring for the libretro arm). A per-GAME core override would win, but here we pin the
        //     per-system default flows through to r.core.
        {
            const ResolvedLaunch r = resolveLaunch(nes, empty, QStringLiteral("nestopia"), QString(),
                                                   EmuBackend::Libretro, /*retroParkAvailable*/true, /*vehicle*/false, /*standaloneAvailable*/true);
            CHECK(r.engine == EmuEngine::Libretro);
            CHECK(r.core == QStringLiteral("nestopia"));
        }
    }

    // ---- 6. standaloneAvailable: the platform gate for a build that cannot spawn an external emulator
    //         (Android / iOS). A standalone system WITH libretro cores degrades to Libretro; one WITHOUT
    //         cores (gc/3ds/nds declare none) stays Standalone and keeps the launcher's "not supported"
    //         message. Hand-computed from the SystemCatalog built-in table.
    {
        const GameSystem* psx = SystemCatalog::byId(QStringLiteral("psx"));
        const GameSystem* n64 = SystemCatalog::byId(QStringLiteral("n64"));
        CHECK(psx != nullptr);
        CHECK(n64 != nullptr);
        if (psx && n64)
        {
            const Override empty;

            // psx has cores { swanstation, mednafen_psx_hw, pcsx_rearmed } -> degrades to cores[0].
            const ResolvedLaunch rp = resolveLaunch(psx, empty, QString(), QString(), EmuBackend::Libretro,
                                                    /*retroParkAvailable*/false, /*vehicle*/false,
                                                    /*standaloneAvailable*/false);
            CHECK(rp.engine == EmuEngine::Libretro);
            CHECK(rp.core == QStringLiteral("swanstation"));
            CHECK(rp.externalEmulatorId.isEmpty());

            // gc declares NO cores -> nothing to degrade to, so it stays Standalone.
            const ResolvedLaunch rg = resolveLaunch(gc, empty, QString(), QString(), EmuBackend::Libretro,
                                                    /*retroParkAvailable*/false, /*vehicle*/false,
                                                    /*standaloneAvailable*/false);
            CHECK(rg.engine == EmuEngine::Standalone);
            CHECK(rg.externalEmulatorId == QStringLiteral("dolphin"));

            // standaloneAvailable=true is byte-for-byte today: psx still resolves to its emulator.
            const ResolvedLaunch rpOn = resolveLaunch(psx, empty, QString(), QString(), EmuBackend::Libretro,
                                                      /*retroParkAvailable*/false, /*vehicle*/false,
                                                      /*standaloneAvailable*/true);
            CHECK(rpOn.engine == EmuEngine::Standalone);
            CHECK(rpOn.externalEmulatorId == QStringLiteral("duckstation"));

            // The gate also removes the standalone target from the OFFERED list, so the picker never shows
            // a target prepareCore would degrade away. psx: [standalone:duckstation, libretro x3] with the
            // gate on; the standalone entry is gone with it off, leaving EXACTLY the three cores in catalog
            // order. Pin the contents, not just the absence of Standalone — an empty list also contains no
            // Standalone entry, and an empty list here is the divergence (resolveEmulationTarget below still
            // displays libretro:swanstation). psx is not a RetroPark system, so no retropark entry either way.
            const QList<EmulationTarget> off = emulationTargetsFor(psx, /*retroParkAvailable*/false,
                                                                   /*standaloneAvailable*/false);
            CHECK(off.size() == 3);
            CHECK(off.value(0).id == QStringLiteral("libretro:swanstation"));
            if (off.size() == 3)
            {
                CHECK(off[1].id == QStringLiteral("libretro:mednafen_psx_hw"));
                CHECK(off[2].id == QStringLiteral("libretro:pcsx_rearmed"));
            }
            for (const EmulationTarget& t : off) CHECK(t.engine != EmuEngine::Standalone);

            // And the CURRENT-VALUE display matches: with the gate off, psx displays its libretro core.
            const EmulationTarget cur = resolveEmulationTarget(psx, empty, QString(), QString(),
                                                               EmuBackend::Libretro, /*retroParkAvailable*/false,
                                                               /*standaloneAvailable*/false);
            CHECK(cur.engine == EmuEngine::Libretro);
            CHECK(cur.id == QStringLiteral("libretro:swanstation"));

            // The OTHER half of the rule: gc declares NO cores, so the gate cannot fire for it in ANY of the
            // three functions. resolveLaunch (above) keeps it Standalone; the current-value display must agree
            // — a gc game on Android shows "Dolphin (standalone)", NOT the blank " (libretro)" a fall-through
            // to cores.value(0) (an EMPTY string) would produce.
            const EmulationTarget curGc = resolveEmulationTarget(gc, empty, QString(), QString(),
                                                                 EmuBackend::Libretro, /*retroParkAvailable*/false,
                                                                 /*standaloneAvailable*/false);
            CHECK(curGc.engine == EmuEngine::Standalone);
            CHECK(curGc.id == QStringLiteral("standalone:dolphin"));
            CHECK(curGc.ref == QStringLiteral("dolphin"));
            CHECK(!curGc.ref.isEmpty());
            CHECK(curGc.displayName == QStringLiteral("Dolphin (standalone)"));

            // And the offered list keeps that target rather than going empty — an empty picker for a game the
            // launcher still resolves as Standalone is the same divergence, one surface over.
            const QList<EmulationTarget> gcOff = emulationTargetsFor(gc, /*retroParkAvailable*/false,
                                                                     /*standaloneAvailable*/false);
            CHECK(!gcOff.isEmpty());
            CHECK(gcOff.size() == 1);
            CHECK(gcOff.value(0).engine == EmuEngine::Standalone);
            CHECK(gcOff.value(0).id == QStringLiteral("standalone:dolphin"));
        }
    }

    // ---- 7. A STANDALONE system also offers its libretro cores, after its emulators and before RetroPark.
    //         psx: [standalone:duckstation, libretro:swanstation, libretro:mednafen_psx_hw,
    //               libretro:pcsx_rearmed]. RetroPark does not support psx, so no retropark entry.
    //         Hand-computed from the SystemCatalog built-in psx row.
    {
        const GameSystem* psx = SystemCatalog::byId(QStringLiteral("psx"));
        CHECK(psx != nullptr);
        if (psx)
        {
            const QList<EmulationTarget> t = emulationTargetsFor(psx, /*retroParkAvailable*/true,
                                                                 /*standaloneAvailable*/true);
            CHECK(t.size() == 4);
            if (t.size() == 4)
            {
                CHECK(t[0].id == QStringLiteral("standalone:duckstation"));
                CHECK(t[0].displayName == QStringLiteral("DuckStation (standalone)"));
                CHECK(t[1].id == QStringLiteral("libretro:swanstation"));
                CHECK(t[1].displayName == QStringLiteral("swanstation (libretro)"));
                CHECK(t[2].id == QStringLiteral("libretro:mednafen_psx_hw"));
                CHECK(t[3].id == QStringLiteral("libretro:pcsx_rearmed"));
            }
            // gc declares no cores, so it is unchanged: [standalone:dolphin, retropark].
            const QList<EmulationTarget> tg = emulationTargetsFor(gc, /*retroParkAvailable*/true,
                                                                  /*standaloneAvailable*/true);
            CHECK(tg.size() == 2);
            if (tg.size() == 2)
            {
                CHECK(tg[0].id == QStringLiteral("standalone:dolphin"));
                CHECK(tg[1].id == QStringLiteral("retropark"));
            }
            // With the platform gate off, psx offers ONLY its cores.
            const QList<EmulationTarget> tOff = emulationTargetsFor(psx, /*retroParkAvailable*/true,
                                                                    /*standaloneAvailable*/false);
            CHECK(tOff.size() == 3);
            if (tOff.size() == 3) CHECK(tOff[0].id == QStringLiteral("libretro:swanstation"));
        }
    }

    // ---- 8. THE ROUND-TRIP INVARIANT: every target the picker OFFERS is a target the resolver can RETURN.
    //         This is deliberately NOT a hand-computed fixture — it is a PROPERTY quantified over the whole
    //         offered list, which is the only shape that catches "the picker grew an option the resolver never
    //         returns". (The fixture rule still holds for every id/display literal above: those are written out
    //         from the documented rules, never read back. Here nothing is read back either — the oracle is
    //         t.id itself, the value the picker showed the user, compared against a SEPARATE function.)
    //
    //         The bug this pins: a standalone system offers its libretro cores, but resolveEmulationTarget took
    //         its standalone arm before ever consulting the override, so picking "swanstation (libretro)" on a
    //         psx game wrote ov.core=swanstation and then resolved straight back to standalone:duckstation —
    //         the tick reverted and DuckStation still launched.
    {
        const char* sysIds[] = { "nes", "psx", "gc" };
        for (const char* sysId : sysIds)
        {
            const GameSystem* s = SystemCatalog::byId(QString::fromLatin1(sysId));
            CHECK(s != nullptr);
            if (!s) continue;

            for (const EmulationTarget& t : emulationTargetsFor(s, /*retroParkAvailable*/true,
                                                                /*standaloneAvailable*/true))
            {
                // PER-GAME half: select the target through applyTargetToOverride (a fresh, empty override, as a
                // game with no prior selection has) and re-resolve it.
                Override ov;
                applyTargetToOverride(t, ov);
                const EmulationTarget back = resolveEmulationTarget(s, ov, QString(), QString(),
                                                                    EmuBackend::Libretro,
                                                                    /*retroParkAvailable*/true,
                                                                    /*standaloneAvailable*/true);
                if (back.id != t.id)
                {
                    std::fprintf(stderr,
                                 "EMUTARGETS-FAIL round-trip(per-game) system=%s offered=%s resolved=%s (line %d)\n",
                                 sysId, qPrintable(t.id), qPrintable(back.id), __LINE__);
                    ++failures;
                }

                // PER-SYSTEM half: the same target chosen as the SYSTEM default. The per-system writers
                // (MainWindow::setSystemEmulationDefault / SettingsDialog::applySystemEmulationTarget) map a
                // libretro target onto coreFor=<ref> with emulatorFor cleared, a standalone target onto
                // emulatorFor=<ref> with coreFor cleared, and RetroPark onto backendFor=RetroPark with both
                // cleared — so drive each engine's own lever here, over an EMPTY per-game override.
                const QString psCore = (t.engine == EmuEngine::Libretro)   ? t.ref : QString();
                const QString psEmu  = (t.engine == EmuEngine::Standalone) ? t.ref : QString();
                const EmuBackend psBackend = (t.engine == EmuEngine::RetroPark) ? EmuBackend::RetroPark
                                                                                : EmuBackend::Libretro;
                const EmulationTarget backSys = resolveEmulationTarget(s, Override{}, psCore, psEmu, psBackend,
                                                                       /*retroParkAvailable*/true,
                                                                       /*standaloneAvailable*/true);
                if (backSys.id != t.id)
                {
                    std::fprintf(stderr,
                                 "EMUTARGETS-FAIL round-trip(per-system) system=%s offered=%s resolved=%s (line %d)\n",
                                 sysId, qPrintable(t.id), qPrintable(backSys.id), __LINE__);
                    ++failures;
                }
            }
        }
    }

    if (failures == 0) std::printf("EMUTARGETS-OK\n");
    else               std::fprintf(stderr, "EMUTARGETS: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
