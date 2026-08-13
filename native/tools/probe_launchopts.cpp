// Headless check of the per-game launch-override store (src/core/LaunchOptionsStore, issue #51) — the lever
// that lets ONE game pick a different core / standalone emulator / extra CLI args, consulted before the system
// default so an empty override is byte-for-byte today's launch. QtCore-only (a QSettings wrapper over the
// shared everythingbox.ini, global like MetaOverrides), so it runs under the offscreen QPA in CI and pins:
//
//   * PURE RESOLUTION — the mutation-tested heart the launch pipeline calls:
//       - resolveCore applies an override core ONLY when it is one of the system's candidate cores; an override
//         naming a non-candidate (stale/invalid) is ignored and the default stands; an empty override is the
//         default;
//       - resolveEmulatorId applies an override id ONLY when it is one of the currently-registered emulator
//         ids; an override naming a retired/removed emulator (not in the valid set) falls back to the default
//         rather than erroring the launch; an empty override is the default;
//       - appendExtraArgs joins the extra with exactly one space, and a blank extra is a byte-identical no-op.
//   * STORE — a record round-trips by key (core/emulatorId/extraArgs); an absent key reads back empty; a clear
//     leaves a HUSK (the row survives for the merge) that still reads as "no override"; clearing a game that
//     never carried an override writes nothing at all.
//
// Prints LAUNCHOPTS-OK on success; any failure prints LAUNCHOPTS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// starts empty, is never shared with a sibling probe or a previous run, and is removed at exit.
//
// FIXTURES ARE COMPUTED INDEPENDENTLY of the store: expected resolutions are hand-written literals, and the raw
// ini leaf is addressed by an MD5 taken with QCryptographicHash directly (not via LaunchOpts::hashKey), so an
// assertion cannot pass merely because it re-ran the function under test.
#include "LaunchOptionsStore.h"
#include "EmuBackend.h"       // RetroPark Slice 2a: the backend vocabulary resolveBackend returns
#include "Settings.h"         // backendFor/setBackendFor/setDefaultBackend — the per-system/global default source
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "LAUNCHOPTS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using LaunchOpts::Override;

// Independent oracle for the store's key hashing (md5-hex over UTF-8), so the probe can address a game's raw
// ini blob without calling LaunchOpts::hashKey — the same discipline probe_marks uses.
static QString md5hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

    // A real system's candidate cores (SNES): the default is cores[0] = "snes9x". Written as a literal here, NOT
    // read from SystemCatalog, so the assertions do not track a catalog change silently.
    const QStringList snesCores{ QStringLiteral("snes9x"), QStringLiteral("bsnes_mercury_balanced") };

    // ---- 1. resolveCore: override IN the candidate list is applied -----------------------------------------
    {
        Override ov; ov.core = QStringLiteral("bsnes_mercury_balanced");
        CHECK(LaunchOpts::resolveCore(QStringLiteral("snes9x"), ov, snesCores)
              == QStringLiteral("bsnes_mercury_balanced"));
    }

    // ---- 2. resolveCore: override NOT in the candidate list is ignored (falls back to the default) ---------
    {
        Override ov; ov.core = QStringLiteral("zsnes"); // not a candidate -> stale/invalid, ignored
        CHECK(LaunchOpts::resolveCore(QStringLiteral("snes9x"), ov, snesCores) == QStringLiteral("snes9x"));
    }

    // ---- 3. resolveCore: empty override leaves the default untouched ---------------------------------------
    {
        Override ov; // empty
        CHECK(LaunchOpts::resolveCore(QStringLiteral("snes9x"), ov, snesCores) == QStringLiteral("snes9x"));
    }

    // ---- 4. resolveEmulatorId: an override id IN the valid set is applied; one NOT in it (retired/removed)
    //         falls back to the default; an empty override leaves the default. Symmetric with resolveCore.
    {
        // A hand-written set of currently-registered emulator ids — NOT read from EmulatorRegistry, so the
        // assertions don't track a registry change silently.
        const QStringList validEmus{ QStringLiteral("duckstation"), QStringLiteral("pcsx2"),
                                     QStringLiteral("retroarch-standalone") };

        // In the valid set -> applied.
        Override ov; ov.emulatorId = QStringLiteral("retroarch-standalone");
        CHECK(LaunchOpts::resolveEmulatorId(QStringLiteral("duckstation"), ov, validEmus)
              == QStringLiteral("retroarch-standalone"));

        // NOT in the valid set (a retired/removed emulator) -> falls back to the default, does NOT error.
        Override retired; retired.emulatorId = QStringLiteral("nulldc"); // not registered any more
        CHECK(LaunchOpts::resolveEmulatorId(QStringLiteral("duckstation"), retired, validEmus)
              == QStringLiteral("duckstation"));

        // Empty override -> the default, byte-for-byte today's launch.
        Override empty;
        CHECK(LaunchOpts::resolveEmulatorId(QStringLiteral("duckstation"), empty, validEmus)
              == QStringLiteral("duckstation"));

        // An empty valid set can never apply an override — always the default (defends the AND condition).
        CHECK(LaunchOpts::resolveEmulatorId(QStringLiteral("duckstation"), ov, QStringList{})
              == QStringLiteral("duckstation"));
    }

    // ---- 5. appendExtraArgs: appended with exactly one space; blank extra is a byte-identical no-op --------
    {
        // Hand-computed expectations — NOT produced by re-running appendExtraArgs.
        CHECK(LaunchOpts::appendExtraArgs(QStringLiteral("-batch {fs} -- {rom}"), QStringLiteral("-slowboot"))
              == QStringLiteral("-batch {fs} -- {rom} -slowboot"));
        // A blank extra must not change the template at all (empty override == today's launch).
        CHECK(LaunchOpts::appendExtraArgs(QStringLiteral("-batch {rom}"), QString())
              == QStringLiteral("-batch {rom}"));
        // Whitespace-only extra is also a no-op (it is trimmed away).
        CHECK(LaunchOpts::appendExtraArgs(QStringLiteral("-batch {rom}"), QStringLiteral("   "))
              == QStringLiteral("-batch {rom}"));
        // The extra is trimmed before joining, and a trailing space on the base does not double up.
        CHECK(LaunchOpts::appendExtraArgs(QStringLiteral("{rom} "), QStringLiteral("  --foo bar  "))
              == QStringLiteral("{rom} --foo bar"));
        // An empty base yields just the trimmed extra (no leading space).
        CHECK(LaunchOpts::appendExtraArgs(QString(), QStringLiteral("--foo"))
              == QStringLiteral("--foo"));
    }

    // ---- 6. Store: a record round-trips by key; an absent key reads back empty -----------------------------
    {
        const QString key = QStringLiteral("romlib:C:/roms/snes/Chrono Trigger.sfc");
        Override ov; ov.core = QStringLiteral("bsnes_mercury_balanced");
        ov.emulatorId = QStringLiteral("myemu"); ov.extraArgs = QStringLiteral("-config foo=bar");
        LaunchOpts::set(key, ov);

        const Override got = LaunchOpts::get(key);
        CHECK(got.core == QStringLiteral("bsnes_mercury_balanced"));
        CHECK(got.emulatorId == QStringLiteral("myemu"));
        CHECK(got.extraArgs == QStringLiteral("-config foo=bar"));
        CHECK(!got.isEmpty());
        CHECK(LaunchOpts::has(key));

        // An unrelated key carries nothing.
        const Override none = LaunchOpts::get(QStringLiteral("romlib:C:/roms/snes/Nothing.sfc"));
        CHECK(none.isEmpty());
        CHECK(!LaunchOpts::has(QStringLiteral("romlib:C:/roms/snes/Nothing.sfc")));

        // The raw ini leaf lives under launchopts/items/<md5(key)> and carries the trimmed levers + a stamp —
        // addressed by the independent md5 oracle so the store's own hashing isn't the thing under test.
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("launchopts/items/") + md5hex(key);
        const QJsonObject blob = QJsonDocument::fromJson(s.value(leaf).toString().toUtf8()).object();
        CHECK(blob.value(QStringLiteral("core")).toString() == QStringLiteral("bsnes_mercury_balanced"));
        CHECK(blob.value(QStringLiteral("emulatorId")).toString() == QStringLiteral("myemu"));
        CHECK(blob.value(QStringLiteral("extraArgs")).toString() == QStringLiteral("-config foo=bar"));
        CHECK(static_cast<qint64>(blob.value(QStringLiteral("updatedAt")).toDouble()) > 0);
    }

    // ---- 7. Clear leaves a HUSK (the row survives for the merge) that still reads as "no override" ---------
    {
        const QString key = QStringLiteral("romlib:C:/roms/psx/Wipeout.cue");
        Override ov; ov.emulatorId = QStringLiteral("duckstation-alt");
        LaunchOpts::set(key, ov);
        CHECK(LaunchOpts::has(key));

        LaunchOpts::reset(key);                    // clear every lever
        CHECK(LaunchOpts::get(key).isEmpty());     // reads as "no override"
        CHECK(!LaunchOpts::has(key));

        // The ROW is not gone: a husk (all-empty blob with a fresh stamp) stays so a peer's stale copy can't
        // resurrect the override the user just cleared (issue #132). Addressed via the independent oracle.
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("launchopts/items/") + md5hex(key);
        CHECK(s.contains(leaf));                    // the husk row exists
        const QJsonObject husk = QJsonDocument::fromJson(s.value(leaf).toString().toUtf8()).object();
        CHECK(husk.value(QStringLiteral("core")).toString().isEmpty());
        CHECK(husk.value(QStringLiteral("emulatorId")).toString().isEmpty());
        CHECK(static_cast<qint64>(husk.value(QStringLiteral("updatedAt")).toDouble()) > 0);
    }

    // ---- 8. Clearing a game that never carried an override writes NOTHING (no phantom husk) ----------------
    {
        const QString key = QStringLiteral("romlib:C:/roms/nes/NeverTouched.nes");
        LaunchOpts::reset(key);
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("launchopts/items/") + md5hex(key);
        CHECK(!s.contains(leaf));                    // no row: "never known" must not be spelled as a clear
    }

    // ---- 9. resolveBackend (RetroPark Slice 2a): a recognised override wins; empty OR unrecognised falls
    //         back to the PASSED default WITHOUT erroring — symmetric with resolveCore's non-candidate check.
    {
        // Hand-computed expectations (independent oracle), NOT produced by re-running resolveBackend:
        //   "retropark" -> RetroPark, "libretro" -> Libretro, "" -> the passed default, anything else -> default.
        Override rp; rp.backend = QStringLiteral("retropark");
        CHECK(LaunchOpts::resolveBackend(EmuBackend::Libretro, rp) == EmuBackend::RetroPark);

        // Empty override inherits WHATEVER default is passed, not a hard-coded Libretro (defends the fallback).
        Override empty;
        CHECK(LaunchOpts::resolveBackend(EmuBackend::Libretro, empty) == EmuBackend::Libretro);
        CHECK(LaunchOpts::resolveBackend(EmuBackend::RetroPark, empty) == EmuBackend::RetroPark);

        // An unrecognised / retired token also falls back to the passed default — it must NOT error or flip.
        Override junk; junk.backend = QStringLiteral("nonsense");
        CHECK(LaunchOpts::resolveBackend(EmuBackend::Libretro, junk) == EmuBackend::Libretro);
        CHECK(LaunchOpts::resolveBackend(EmuBackend::RetroPark, junk) == EmuBackend::RetroPark);

        // An explicit "libretro" override pins libretro even when the passed default is RetroPark.
        Override lr; lr.backend = QStringLiteral("libretro");
        CHECK(LaunchOpts::resolveBackend(EmuBackend::RetroPark, lr) == EmuBackend::Libretro);
    }

    // ---- 10. Store: the backend lever round-trips through set()/get() and the raw ini blob -----------------
    //         A backend-only override is a REAL record (not a husk): if isEmpty() ignored backend, ensureCache
    //         would drop it and get() would read back empty — so this also pins backend into isEmpty/toJson.
    {
        const QString key = QStringLiteral("romlib:C:/roms/gc/Billy Hatcher.iso");
        Override ov; ov.backend = QStringLiteral("retropark");
        LaunchOpts::set(key, ov);

        const Override got = LaunchOpts::get(key);
        CHECK(got.backend == QStringLiteral("retropark"));
        CHECK(!got.isEmpty());                       // a backend-only override survives as a real record
        CHECK(LaunchOpts::has(key));

        // Raw ini leaf carries "backend" — addressed by the independent md5 oracle (not LaunchOpts::hashKey).
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("launchopts/items/") + md5hex(key);
        const QJsonObject blob = QJsonDocument::fromJson(s.value(leaf).toString().toUtf8()).object();
        CHECK(blob.value(QStringLiteral("backend")).toString() == QStringLiteral("retropark"));
    }

    // ---- 11. Task 3 wiring: the EXACT composition GameLauncher::prepareCore threads into CorePlan::backend for a
    //          libretro system — `LaunchOpts::resolveBackend(Settings::backendFor(sysId), ov)`. prepareCore itself
    //          isn't headless-reachable (it constructs no GameLauncher without a RetroView + full app state), so this
    //          pins the decision it composes: the per-system default (which is itself the global default when unset)
    //          feeds resolveBackend as the fallback, and the per-game override wins over it. Expected values are
    //          hand-computed from the rules (independent oracle), NOT read back from backendFor/resolveBackend.
    {
        Override empty;                                       // no per-game override -> inherit the default
        Override ovRp;  ovRp.backend = QStringLiteral("retropark");
        Override ovLr;  ovLr.backend = QStringLiteral("libretro");
        const QString sysA      = QStringLiteral("rp:sysA");
        const QString sysGlobal = QStringLiteral("rp:sysGlobal");

        // (a) Nothing set: per-system unset AND global unset -> backendFor == Libretro (today's default).
        CHECK(Settings::backendFor(sysA) == EmuBackend::Libretro);
        CHECK(LaunchOpts::resolveBackend(Settings::backendFor(sysA), empty) == EmuBackend::Libretro);
        // A retropark override wins over a libretro default (the per-game opt-in).
        CHECK(LaunchOpts::resolveBackend(Settings::backendFor(sysA), ovRp) == EmuBackend::RetroPark);

        // (b) Per-system default of RetroPark: the composition returns RetroPark for an EMPTY override — the case a
        //     mutant that hardcodes the default arg to Libretro (or drops backendFor) fails. And a libretro override
        //     still wins over the RetroPark system default.
        Settings::setBackendFor(sysA, EmuBackend::RetroPark);
        CHECK(Settings::backendFor(sysA) == EmuBackend::RetroPark);
        CHECK(LaunchOpts::resolveBackend(Settings::backendFor(sysA), empty) == EmuBackend::RetroPark);
        CHECK(LaunchOpts::resolveBackend(Settings::backendFor(sysA), ovLr)  == EmuBackend::Libretro);

        // (c) Global default of RetroPark flows to a system with no per-system choice.
        Settings::setDefaultBackend(EmuBackend::RetroPark);
        CHECK(Settings::backendFor(sysGlobal) == EmuBackend::RetroPark);
        CHECK(LaunchOpts::resolveBackend(Settings::backendFor(sysGlobal), empty) == EmuBackend::RetroPark);
    }

    // ---- 12. RetroPark FCEUmm-shim gating (Slice 2b): the shipped shim is NES-only, so the RetroPark backend
    //          is OFFERED (per-game picker) and HONOURED (launch) only for NES. retroParkSupportsSystem is the
    //          single predicate; clampBackendToSystem is the safety net GameLauncher::prepareCore applies AFTER
    //          resolveBackend so a synced per-game override / global RetroPark default can never brick a non-NES
    //          launch. Expected values are hand-computed literals (independent oracle), NOT read back from the
    //          functions under test. Mutating the predicate (always-true) or dropping the clamp fails these.
    {
        // The predicate: true ONLY for the canonical SystemCatalog NES id, false for every other system id and
        // for an empty id. "snes"/"genesis" are real non-NES ids written as literals here (not read from the
        // catalog) so the assertion does not track a catalog change silently.
        CHECK(retroParkSupportsSystem(QStringLiteral("nes"))     == true);
        CHECK(retroParkSupportsSystem(QStringLiteral("snes"))    == false);
        CHECK(retroParkSupportsSystem(QStringLiteral("genesis")) == false);
        CHECK(retroParkSupportsSystem(QString())                 == false);

        // The clamp: a resolved RetroPark backend STAYS RetroPark on NES but falls back to Libretro on any
        // unsupported (non-NES) system. Libretro is never altered on any system (defends the RetroPark-only arm).
        CHECK(clampBackendToSystem(EmuBackend::RetroPark, QStringLiteral("nes"))     == EmuBackend::RetroPark);
        CHECK(clampBackendToSystem(EmuBackend::RetroPark, QStringLiteral("snes"))    == EmuBackend::Libretro);
        CHECK(clampBackendToSystem(EmuBackend::RetroPark, QStringLiteral("genesis")) == EmuBackend::Libretro);
        CHECK(clampBackendToSystem(EmuBackend::Libretro,  QStringLiteral("snes"))    == EmuBackend::Libretro);
        CHECK(clampBackendToSystem(EmuBackend::Libretro,  QStringLiteral("nes"))     == EmuBackend::Libretro);

        // The EXACT composition prepareCore threads into CorePlan::backend for a libretro system:
        //   clampBackendToSystem(resolveBackend(backendFor(sysId), ov), sysId).
        // (a) A per-game retropark override on a non-NES system -> clamped to Libretro (does NOT brick the launch).
        Override ovRp; ovRp.backend = QStringLiteral("retropark");
        CHECK(clampBackendToSystem(LaunchOpts::resolveBackend(EmuBackend::Libretro, ovRp), QStringLiteral("snes"))
              == EmuBackend::Libretro);
        // (b) The same override on NES -> honoured (RetroPark).
        CHECK(clampBackendToSystem(LaunchOpts::resolveBackend(EmuBackend::Libretro, ovRp), QStringLiteral("nes"))
              == EmuBackend::RetroPark);
        // (c) A global/per-system RetroPark default (empty override) on a non-NES system -> clamped to Libretro.
        Override empty;
        CHECK(clampBackendToSystem(LaunchOpts::resolveBackend(EmuBackend::RetroPark, empty), QStringLiteral("genesis"))
              == EmuBackend::Libretro);
        // (d) That same default on NES -> stays RetroPark.
        CHECK(clampBackendToSystem(LaunchOpts::resolveBackend(EmuBackend::RetroPark, empty), QStringLiteral("nes"))
              == EmuBackend::RetroPark);
    }

    if (failures == 0) std::printf("LAUNCHOPTS-OK\n");
    else               std::fprintf(stderr, "LAUNCHOPTS: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
