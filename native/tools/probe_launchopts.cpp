// Headless check of the per-game launch-override store (src/core/LaunchOptionsStore, issue #51) — the lever
// that lets ONE game pick a different core / standalone emulator / extra CLI args, consulted before the system
// default so an empty override is byte-for-byte today's launch. QtCore-only (a QSettings wrapper over the
// shared everythingbox.ini, global like MetaOverrides), so it runs under the offscreen QPA in CI and pins:
//
//   * PURE RESOLUTION — the mutation-tested heart the launch pipeline calls:
//       - resolveCore applies an override core ONLY when it is one of the system's candidate cores; an override
//         naming a non-candidate (stale/invalid) is ignored and the default stands; an empty override is the
//         default;
//       - resolveEmulatorId applies a non-empty override id, else the default;
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

    // ---- 4. resolveEmulatorId: a non-empty override id replaces the default; empty leaves it ---------------
    {
        Override ov; ov.emulatorId = QStringLiteral("retroarch-standalone");
        CHECK(LaunchOpts::resolveEmulatorId(QStringLiteral("duckstation"), ov)
              == QStringLiteral("retroarch-standalone"));
        Override empty;
        CHECK(LaunchOpts::resolveEmulatorId(QStringLiteral("duckstation"), empty)
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

    if (failures == 0) std::printf("LAUNCHOPTS-OK\n");
    else               std::fprintf(stderr, "LAUNCHOPTS: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
