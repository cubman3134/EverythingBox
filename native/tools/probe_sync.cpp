// Headless check of the SyncOffsets store (src/core/SyncOffsets) — the persistence foundation the player's
// audio/subtitle sync-controls (Task 2) wire onto. SyncOffsets is a plain QtCore Settings-store wrapper (no
// Quick/Widgets), so this runs under the offscreen QPA in CI and pins the contract the sync UI leans on:
//
//   * globals default to 0.0 (a fresh store, or a corrupt value, resolves to no offset);
//   * setGlobalDefault applies to every file that has no per-file override;
//   * a per-file override beats the global on its own axis, and only for its own key;
//   * clearPerFile drops the override back to the global;
//   * an empty key means "the globals" — resolve returns them and savePerFile is a no-op (never writes a
//     `sync/files//` junk key);
//   * both writers clamp to ±10.0 seconds.
//
// Prints SYNC-OK on success; any failure prints SYNC-FAIL <cond> and exits non-zero.
//
// Isolation: like the other core probes (see probe_formfactor), AppPaths::dataDir() is the probe exe's own
// folder in the build tree (portable app), so the mymediavault.ini it reads/writes is next to the probe and
// never touches a deployed install. We wipe the "sync" group at start so a leftover ini can't skew the
// defaults asserts.
#include "SyncOffsets.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QSettings>
#include <QtGlobal>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SYNC-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool eq(double a, double b) { return qAbs(a - b) < 1e-9; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/mymediavault.ini");

    // Reset: wipe any leftover sync/* keys so the defaults asserts start clean. Shares QSettings' per-file
    // cache with SyncOffsets' own store(), so this remove()+sync() is visible to every later read.
    {
        QSettings reset(iniPath, QSettings::IniFormat);
        reset.remove(QStringLiteral("sync"));
        reset.sync();
    }

    using SyncOffsets::Which;

    // 1. Globals default to 0.0 — a fresh store resolves to no offset on either axis.
    CHECK(eq(SyncOffsets::globalDefault(Which::Audio), 0.0));
    CHECK(eq(SyncOffsets::globalDefault(Which::Sub), 0.0));
    {
        auto p = SyncOffsets::resolve(QStringLiteral("k1"));
        CHECK(eq(p.audio, 0.0));
        CHECK(eq(p.sub, 0.0));
    }

    // 2. A global default applies to every file with no per-file override.
    SyncOffsets::setGlobalDefault(Which::Audio, 0.08);
    {
        auto p = SyncOffsets::resolve(QStringLiteral("k1"));
        CHECK(eq(p.audio, 0.08));   // global applies
        CHECK(eq(p.sub, 0.0));      // sub axis untouched
    }

    // 3. A per-file override beats the global — on its own key only.
    SyncOffsets::savePerFile(QStringLiteral("k1"), Which::Audio, -0.2);
    CHECK(eq(SyncOffsets::resolve(QStringLiteral("k1")).audio, -0.2));  // per-file wins for k1
    CHECK(eq(SyncOffsets::resolve(QStringLiteral("k2")).audio, 0.08));  // k2 still rides the global

    // 4. clearPerFile drops the override back to the global.
    SyncOffsets::clearPerFile(QStringLiteral("k1"), Which::Audio);
    CHECK(eq(SyncOffsets::resolve(QStringLiteral("k1")).audio, 0.08));

    // 5. Empty key == "the globals": resolve returns them, and savePerFile writes nothing (no per-file junk).
    {
        auto p = SyncOffsets::resolve(QString());
        CHECK(eq(p.audio, 0.08));   // == global audio
        CHECK(eq(p.sub, 0.0));      // == global sub
    }
    SyncOffsets::savePerFile(QString(), Which::Audio, 1.0);          // no-op on empty key
    CHECK(eq(SyncOffsets::globalDefault(Which::Audio), 0.08));       // globals unchanged
    {
        // Assert the Settings child-group absence: after clearing k1 and a no-op empty-key save, the
        // `sync/files` group must hold NO subgroups and NO stray keys (no `sync/files//audio` junk).
        QSettings ini(iniPath, QSettings::IniFormat);
        ini.sync();
        ini.beginGroup(QStringLiteral("sync/files"));
        const QStringList groups = ini.childGroups();
        const QStringList keys   = ini.childKeys();
        ini.endGroup();
        CHECK(groups.isEmpty());                 // no per-file subgroups linger (k1 was cleared)
        CHECK(!groups.contains(QString()));       // and specifically no empty-key `sync/files//` subgroup
        CHECK(keys.isEmpty());                    // empty-key save wrote no direct key
    }

    // 6. Both writers clamp to ±10.0 seconds.
    SyncOffsets::savePerFile(QStringLiteral("k1"), Which::Audio, 99.0);
    CHECK(eq(SyncOffsets::resolve(QStringLiteral("k1")).audio, 10.0));   // +99 clamped to +10
    SyncOffsets::setGlobalDefault(Which::Sub, -99.0);
    CHECK(eq(SyncOffsets::globalDefault(Which::Sub), -10.0));            // -99 clamped to -10
    CHECK(eq(SyncOffsets::resolve(QStringLiteral("k2")).sub, -10.0));    // and it flows to files via the global

    // 7. Read-side safety for values written OUTSIDE the API (older build, hand-edited ini, corruption): an
    // out-of-range global is clamped on read, a non-numeric (corrupt) one reads back as 0.0.
    {
        QSettings raw(iniPath, QSettings::IniFormat);
        raw.setValue(QStringLiteral("sync/global/audio"), 500.0);                       // out of range
        raw.setValue(QStringLiteral("sync/global/sub"), QStringLiteral("not-a-number")); // corrupt
        raw.sync();
    }
    CHECK(eq(SyncOffsets::globalDefault(Which::Audio), 10.0));   // read-side clamp: 500 -> 10
    CHECK(eq(SyncOffsets::globalDefault(Which::Sub), 0.0));      // corrupt string -> 0.0
    SyncOffsets::setGlobalDefault(Which::Audio, 0.0);           // restore clean globals for the collision asserts
    SyncOffsets::setGlobalDefault(Which::Sub, 0.0);

    // 8. Opaque-key contract: real resume keys are paths/URLs — some with '/', '//', or trailing separators
    // that QSettings would normalize into COLLIDING group paths. SyncOffsets hashes the key to a flat token
    // first, so keys that differ only in empty/duplicate separators — and a URL-shaped key — resolve
    // independently. (Against verbatim group nesting "a/b" and "a//b" alias to the same entry; this pins the
    // hash.)
    SyncOffsets::savePerFile(QStringLiteral("a/b"),  Which::Audio, 0.10);
    SyncOffsets::savePerFile(QStringLiteral("a//b"), Which::Audio, 0.20);
    SyncOffsets::savePerFile(QStringLiteral("https://x/y.mkv"), Which::Audio, 0.30);
    CHECK(eq(SyncOffsets::resolve(QStringLiteral("a/b")).audio,  0.10));
    CHECK(eq(SyncOffsets::resolve(QStringLiteral("a//b")).audio, 0.20));   // distinct from "a/b"
    CHECK(eq(SyncOffsets::resolve(QStringLiteral("https://x/y.mkv")).audio, 0.30));

    if (failures == 0) { std::puts("SYNC-OK"); return 0; }
    std::fprintf(stderr, "SYNC: %d check(s) failed\n", failures);
    return 1;
}
