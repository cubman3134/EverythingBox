// Headless check of the Android asset bootstrap (src/core/AssetBootstrap) — the first-run extractor that
// seeds a fresh install's empty data dir with the stock themes2/ + first-party addons/ baked into the APK.
// AssetBootstrap::run is a pure function of (sourceRoot, dataDir, appVersion), so this exercises the exact
// production path with a temp source dir standing in for the APK's "assets:/mmv" and a temp dataDir standing
// in for AppPaths::dataDir() — no Android toolchain, no display, runs under the offscreen QPA in CI.
//
// Pins the semantics the app depends on (see AssetBootstrap.h):
//   1. fresh dataDir            -> themes2/ + addons/ both copied, stamp .assets-version == appVersion;
//   2. re-run, same version     -> full no-op (stock file NOT re-copied — content + mtime preserved);
//   3. bumped version           -> stock theme REFRESHED (overwritten back to source), an already-present
//                                  addon LEFT ALONE (copy-if-absent), a user-added theme dir UNTOUCHED;
//   4. missing sourceRoot        -> clean no-op (no dirs created, no stamp written).
//
// Prints BOOTSTRAP-OK on success; any failure prints BOOTSTRAP-FAIL <cond> and exits non-zero.
#include "AssetBootstrap.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "BOOTSTRAP-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool writeFile(const QString& path, const QString& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(text.toUtf8());
    return true;
}

static QString readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // --- Build a temp "APK assets" source: themes2/T/theme.json + addons/a/manifest.json. ---
    QTemporaryDir srcDir;
    QTemporaryDir dataDir;
    CHECK(srcDir.isValid());
    CHECK(dataDir.isValid());
    const QString src  = srcDir.path();
    const QString data = dataDir.path();

    const QString kThemeSrc  = QStringLiteral("{\"name\":\"T\",\"stock\":true}");
    const QString kAddonSrc  = QStringLiteral("{\"id\":\"a\",\"stock\":true}");
    CHECK(writeFile(src + "/themes2/T/theme.json", kThemeSrc));
    CHECK(writeFile(src + "/addons/a/manifest.json", kAddonSrc));

    const QString stamp     = data + "/.assets-version";
    const QString themeDst  = data + "/themes2/T/theme.json";
    const QString addonDst  = data + "/addons/a/manifest.json";

    // --- 1. Fresh dataDir: both trees extracted, stamp written. ---
    bool changed = AssetBootstrap::run(src, data, QStringLiteral("1.0.0"));
    CHECK(changed);                                   // it did work
    CHECK(QFileInfo::exists(themeDst));
    CHECK(QFileInfo::exists(addonDst));
    CHECK(readFile(themeDst) == kThemeSrc);
    CHECK(readFile(addonDst) == kAddonSrc);
    CHECK(readFile(stamp).trimmed() == QStringLiteral("1.0.0"));

    // --- 2. Same version: full no-op. Dirty the stock files + add a user theme; nothing must move. ---
    CHECK(writeFile(themeDst, QStringLiteral("USER-EDITED-THEME")));
    CHECK(writeFile(addonDst, QStringLiteral("USER-EDITED-ADDON")));
    CHECK(writeFile(data + "/themes2/UserTheme/theme.json", QStringLiteral("MY-OWN-THEME")));
    const qint64 themeMtimeBefore = QFileInfo(themeDst).lastModified().toMSecsSinceEpoch();
    QThread::msleep(1100); // make any accidental rewrite show up as a newer mtime on 1s-resolution filesystems

    changed = AssetBootstrap::run(src, data, QStringLiteral("1.0.0"));
    CHECK(!changed);                                  // stamp already current -> no work
    CHECK(readFile(themeDst) == QStringLiteral("USER-EDITED-THEME"));   // stock NOT re-copied
    CHECK(QFileInfo(themeDst).lastModified().toMSecsSinceEpoch() == themeMtimeBefore); // mtime unchanged
    CHECK(readFile(addonDst) == QStringLiteral("USER-EDITED-ADDON"));
    CHECK(readFile(stamp).trimmed() == QStringLiteral("1.0.0"));

    // --- 3. Bumped version: stock theme REFRESHED, present addon LEFT ALONE, user theme UNTOUCHED. ---
    changed = AssetBootstrap::run(src, data, QStringLiteral("2.0.0"));
    CHECK(changed);
    CHECK(readFile(themeDst) == kThemeSrc);                             // refreshed back to source
    CHECK(readFile(addonDst) == QStringLiteral("USER-EDITED-ADDON"));   // copy-if-absent: untouched
    CHECK(readFile(data + "/themes2/UserTheme/theme.json") == QStringLiteral("MY-OWN-THEME")); // user theme safe
    CHECK(readFile(stamp).trimmed() == QStringLiteral("2.0.0"));

    // --- 4. Missing sourceRoot: clean no-op. ---
    QTemporaryDir dataDir2;
    CHECK(dataDir2.isValid());
    const QString data2 = dataDir2.path();
    changed = AssetBootstrap::run(src + "/does-not-exist", data2, QStringLiteral("1.0.0"));
    CHECK(!changed);
    CHECK(!QFileInfo::exists(data2 + "/themes2"));
    CHECK(!QFileInfo::exists(data2 + "/addons"));
    CHECK(!QFileInfo::exists(data2 + "/.assets-version"));

    if (failures == 0) { std::puts("BOOTSTRAP-OK"); return 0; }
    std::fprintf(stderr, "BOOTSTRAP: %d check(s) failed\n", failures);
    return 1;
}
