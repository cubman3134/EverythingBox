// Headless check of the per-game launch HOOKS feature (issue #64): the pure argv tokenizer + {rom} substitution
// (src/core/LaunchHooks.h) and the device-local, UNSYNCED per-game store (src/core/LaunchHooksStore). QtCore-
// only (no QProcess is exercised — a live hook running before/after a real launch is not headlessly drivable),
// so it runs under the offscreen QPA in CI and pins:
//
//   * PURE TOKENIZER — parseCommandLine splits on whitespace, collapses runs, groups a double-quoted span
//     (spaces and all) into ONE token with the quotes dropped, handles a quote mid-token, and maps an empty
//     line to an empty list.
//   * {rom} SUBSTITUTION — substituteRom replaces a whole-token {rom} AND a substring {rom} with the rom path,
//     keeping the token COUNT invariant so a spaced ROM path stays a single argument (the property a naive
//     join/replace/re-split would break — asserted explicitly on the list size).
//   * STORE — a record round-trips pre/post by key; an absent key reads back empty; clearing both hooks deletes
//     the row (device-local + unsynced, so a plain delete, no husk).
//
// Prints LAUNCHHOOKS-OK on success; any failure prints LAUNCHHOOKS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// starts empty and is removed at exit.
//
// FIXTURES ARE COMPUTED INDEPENDENTLY of the code under test: expected token lists are hand-written literals,
// and the raw ini leaf is addressed by an MD5 taken with QCryptographicHash directly (not via
// LaunchHooksStore::hashKey), so an assertion cannot pass merely because it re-ran the function under test.
#include "LaunchHooks.h"
#include "LaunchHooksStore.h"
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
    if (!(cond)) { std::fprintf(stderr, "LAUNCHHOOKS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Independent oracle for the store's key hashing (md5-hex over UTF-8), so the probe can address a game's raw
// ini blob without calling LaunchHooksStore::hashKey — the same discipline probe_launchopts/probe_marks use.
static QString md5hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

    // ---- 1. parseCommandLine: plain tokens split on whitespace, runs collapse ------------------------------
    {
        CHECK(LaunchHooks::parseCommandLine(QStringLiteral("foo bar baz"))
              == (QStringList{ QStringLiteral("foo"), QStringLiteral("bar"), QStringLiteral("baz") }));
        // Leading/trailing and repeated (mixed space/tab) whitespace collapses — no empty tokens leak in.
        CHECK(LaunchHooks::parseCommandLine(QStringLiteral("   a\t \t b  "))
              == (QStringList{ QStringLiteral("a"), QStringLiteral("b") }));
        // An empty (and a whitespace-only) line is an empty argv.
        CHECK(LaunchHooks::parseCommandLine(QString()).isEmpty());
        CHECK(LaunchHooks::parseCommandLine(QStringLiteral("     ")).isEmpty());
    }

    // ---- 2. parseCommandLine: double quotes group a span with spaces into ONE token, quotes dropped --------
    {
        // The quoted path is a single token WITH its spaces; the surrounding tokens are separate. Hand-written
        // expectation, NOT produced by re-running the tokenizer.
        const QStringList got = LaunchHooks::parseCommandLine(
            QStringLiteral("tool \"C:/My Games/x.iso\" --flag"));
        CHECK(got.size() == 3);
        CHECK(got.value(0) == QStringLiteral("tool"));
        CHECK(got.value(1) == QStringLiteral("C:/My Games/x.iso"));   // spaces preserved, quotes gone
        CHECK(got.value(2) == QStringLiteral("--flag"));
        // A quote mid-token joins the quoted run to the adjacent literal characters (still one token).
        CHECK(LaunchHooks::parseCommandLine(QStringLiteral("pre\"a b\"post"))
              == (QStringList{ QStringLiteral("prea bpost") }));
    }

    // ---- 3. substituteRom: whole-token {rom} becomes the spaced path as ONE token (count invariant) --------
    {
        const QString romPath = QStringLiteral("C:/My Games/Chrono Trigger.sfc");
        // A whole-token {rom}.
        QStringList argv = LaunchHooks::parseCommandLine(QStringLiteral("mount {rom} --readonly"));
        CHECK(argv.size() == 3);                                       // map, {rom}, --readonly
        const QStringList sub = LaunchHooks::substituteRom(argv, romPath);
        CHECK(sub.size() == 3);                                        // COUNT unchanged: the path was NOT re-split
        CHECK(sub.value(0) == QStringLiteral("mount"));
        CHECK(sub.value(1) == romPath);                               // the whole spaced path, one argument
        CHECK(sub.value(2) == QStringLiteral("--readonly"));
    }

    // ---- 4. substituteRom: a substring {rom} inside a larger token is replaced in place, still one token ---
    {
        const QString romPath = QStringLiteral("D:/games/a b.iso");
        const QStringList sub = LaunchHooks::substituteRom(
            QStringList{ QStringLiteral("--disc={rom}") }, romPath);
        CHECK(sub.size() == 1);
        CHECK(sub.value(0) == QStringLiteral("--disc=D:/games/a b.iso"));   // hand-computed
        // No {rom} anywhere -> the argv is returned unchanged.
        const QStringList none = LaunchHooks::substituteRom(
            QStringList{ QStringLiteral("a"), QStringLiteral("b") }, romPath);
        CHECK(none == (QStringList{ QStringLiteral("a"), QStringLiteral("b") }));
    }

    // ---- 5. Store: a record round-trips pre/post by key; an absent key reads back empty --------------------
    {
        const QString key = QStringLiteral("romlib:C:/roms/snes/Chrono Trigger.sfc");
        LaunchHooksStore::Hooks h;
        h.preLaunch = QStringLiteral("joyprofile.exe start snes");
        h.postExit  = QStringLiteral("joyprofile.exe stop");
        LaunchHooksStore::set(key, h);

        const LaunchHooksStore::Hooks got = LaunchHooksStore::get(key);
        CHECK(got.preLaunch == QStringLiteral("joyprofile.exe start snes"));
        CHECK(got.postExit  == QStringLiteral("joyprofile.exe stop"));
        CHECK(!got.isEmpty());
        CHECK(LaunchHooksStore::has(key));

        // An unrelated key carries nothing.
        const LaunchHooksStore::Hooks n = LaunchHooksStore::get(QStringLiteral("romlib:C:/roms/snes/Other.sfc"));
        CHECK(n.isEmpty());
        CHECK(!LaunchHooksStore::has(QStringLiteral("romlib:C:/roms/snes/Other.sfc")));

        // The raw ini leaf lives under launchhooks/items/<md5(key)> — addressed by the independent md5 oracle so
        // the store's own hashing isn't the thing under test.
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("launchhooks/items/") + md5hex(key);
        CHECK(s.contains(leaf));
        const QJsonObject blob = QJsonDocument::fromJson(s.value(leaf).toString().toUtf8()).object();
        CHECK(blob.value(QStringLiteral("preLaunch")).toString() == QStringLiteral("joyprofile.exe start snes"));
        CHECK(blob.value(QStringLiteral("postExit")).toString()  == QStringLiteral("joyprofile.exe stop"));
    }

    // ---- 6. Store: values are trimmed at write time -------------------------------------------------------
    {
        const QString key = QStringLiteral("romlib:C:/roms/psx/Wipeout.cue");
        LaunchHooksStore::Hooks h;
        h.preLaunch = QStringLiteral("   pre.bat   ");   // incidental whitespace
        LaunchHooksStore::set(key, h);
        CHECK(LaunchHooksStore::get(key).preLaunch == QStringLiteral("pre.bat"));   // trimmed
        CHECK(LaunchHooksStore::get(key).postExit.isEmpty());
    }

    // ---- 7. Clearing both hooks DELETES the row (device-local + unsynced: a plain remove, no husk) ---------
    {
        const QString key = QStringLiteral("romlib:C:/roms/gc/Zelda.iso");
        LaunchHooksStore::Hooks h; h.postExit = QStringLiteral("cleanup.sh");
        LaunchHooksStore::set(key, h);
        CHECK(LaunchHooksStore::has(key));

        LaunchHooksStore::reset(key);
        CHECK(LaunchHooksStore::get(key).isEmpty());
        CHECK(!LaunchHooksStore::has(key));

        // The ROW is gone — a hook is device-local and never merges, so there is nothing to out-race and no
        // husk is left behind (contrast LaunchOptionsStore, which keeps a husk because it syncs).
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("launchhooks/items/") + md5hex(key);
        CHECK(!s.contains(leaf));
    }

    if (failures == 0) std::printf("LAUNCHHOOKS-OK\n");
    else               std::fprintf(stderr, "LAUNCHHOOKS: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
