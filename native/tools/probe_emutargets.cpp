// Headless check of the unified emulation-target model (src/core/EmulationTarget.h, Unified Emulation Picker
// Task 1) — the pure model that turns a system into a list of engine-tagged run-targets, maps a chosen target
// onto the existing LaunchOpts::Override levers, and resolves the effective target (per-game override ->
// per-system default -> system built-in). Qt6::Core only (it reads SystemCatalog + EmulatorRegistry, both of
// which merge over this process's OWN scratch data dir — empty here — so the built-in tables are exactly what
// is enumerated), so it runs under the offscreen QPA in CI. It pins:
//
//   * ENUMERATION — emulationTargetsFor(nes) = [libretro:fceumm, libretro:nestopia, retropark] and
//     emulationTargetsFor(gc) = [standalone:dolphin, retropark], each with the exact tagged display string.
//   * SELECTION  — applyTargetToOverride sets the one lever its engine owns and CLEARS the other two.
//   * RESOLUTION — resolveEmulationTarget precedence: a per-game retropark override on gc -> retropark; empty
//     override + per-system core=nestopia on nes -> libretro:nestopia; nothing set -> the built-in default
//     (nes->libretro:fceumm, gc->standalone:dolphin); a retropark selection on a NON-supported system falls back
//     to the built-in (the Slice-3b support clamp).
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
    //         RetroPark supports nes, so the picker order is: each core (libretro), then RetroPark tagged off
    //         cores[0]. Every id and display is a hand-written literal (NOT read from the catalog).
    {
        const QList<EmulationTarget> t = emulationTargetsFor(nes);
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
    }

    // ---- 2. Enumeration: a STANDALONE system (gc). externalEmulator=dolphin, no other registry emulator is
    //         bound to gc (built-in `systems` fields are empty), RetroPark supports gc, so the order is:
    //         standalone Dolphin, then RetroPark tagged off Dolphin's display. Literals, not read back.
    {
        const QList<EmulationTarget> t = emulationTargetsFor(gc);
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
            const EmulationTarget t = resolveEmulationTarget(gc, ovRp, QString(), QString(), EmuBackend::Libretro);
            CHECK(t.engine == EmuEngine::RetroPark);
            CHECK(t.id == QStringLiteral("retropark"));
            CHECK(t.displayName == QStringLiteral("Dolphin (retropark)"));
        }

        // (b) Empty override + per-system core default "nestopia" on nes -> libretro:nestopia (the per-system
        //     default is honoured when there is no per-game override). nestopia IS a nes candidate.
        {
            const EmulationTarget t = resolveEmulationTarget(nes, empty, QStringLiteral("nestopia"), QString(),
                                                             EmuBackend::Libretro);
            CHECK(t.engine == EmuEngine::Libretro);
            CHECK(t.id == QStringLiteral("libretro:nestopia"));
            CHECK(t.ref == QStringLiteral("nestopia"));
        }

        // (c) Nothing set -> the system BUILT-IN default: nes -> libretro:fceumm (cores[0]); gc -> standalone:dolphin.
        {
            const EmulationTarget tn = resolveEmulationTarget(nes, empty, QString(), QString(), EmuBackend::Libretro);
            CHECK(tn.engine == EmuEngine::Libretro);
            CHECK(tn.id == QStringLiteral("libretro:fceumm"));

            const EmulationTarget tg = resolveEmulationTarget(gc, empty, QString(), QString(), EmuBackend::Libretro);
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
                                                                   EmuBackend::Libretro);
                CHECK(tov.engine == EmuEngine::Libretro);
                CHECK(tov.id == QStringLiteral("libretro:snes9x")); // snes cores[0]
                CHECK(tov.engine != EmuEngine::RetroPark);

                const EmulationTarget tdef = resolveEmulationTarget(snes, empty, QString(), QString(),
                                                                    EmuBackend::RetroPark);
                CHECK(tdef.engine == EmuEngine::Libretro);
                CHECK(tdef.id == QStringLiteral("libretro:snes9x"));
            }
        }

        // (e) A per-GAME core override wins over the per-system core default (both must be nes candidates). This
        //     defends the override>per-system ordering for the libretro arm.
        {
            Override ovCore; ovCore.core = QStringLiteral("fceumm");
            const EmulationTarget t = resolveEmulationTarget(nes, ovCore, QStringLiteral("nestopia"), QString(),
                                                             EmuBackend::Libretro);
            CHECK(t.id == QStringLiteral("libretro:fceumm")); // override fceumm beats per-system nestopia
        }

        // (f) A per-GAME emulator override on a standalone system wins, when it names a registered emulator id
        //     (cemu is a real EmulatorRegistry id). Defends the override>default ordering for the standalone arm.
        {
            Override ovEmu; ovEmu.emulatorId = QStringLiteral("cemu");
            const EmulationTarget t = resolveEmulationTarget(gc, ovEmu, QString(), QString(), EmuBackend::Libretro);
            CHECK(t.engine == EmuEngine::Standalone);
            CHECK(t.id == QStringLiteral("standalone:cemu"));
        }
    }

    if (failures == 0) std::printf("EMUTARGETS-OK\n");
    else               std::fprintf(stderr, "EMUTARGETS: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
