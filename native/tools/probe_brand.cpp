// Headless coverage for the one-shot-but-resumable migration off the previous brand (BrandMigration).
//
// The step under the most scrutiny here is LocalIni, because it is the one that can lose every setting the
// user ever made. It is asserted directly (the legacy ini must still be on disk afterwards) and the assertion
// is mutation-tested: flipping QFile::copy to QFile::rename in BrandMigration.cpp must make this probe FAIL.
//
// Isolation: the local steps take an explicit data directory, so the ini/addon assertions run against a
// QTemporaryDir and never touch a real install. The FLAGS are device state and live in the probe exe's own
// AppPaths::dataDir() (its build-tree folder, like every other core probe) — so the probe clears them at
// start, and clears the "cloud" group too so the Drive half of run() stays provably dormant.
//
// Prints BRAND-OK on success; any failure prints BRAND-FAIL <what> and exits non-zero.
#include "BrandMigration.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                                        \
    do { if (!(cond)) { std::fprintf(stderr, "BRAND-FAIL %s\n", (what)); ++failures; } } while (0)

static QString legacyIniIn(const QString& dir)
{
    return dir + QStringLiteral("/") + QLatin1String(AppBrand::Legacy::kIniFile);
}
static QString newIniIn(const QString& dir)
{
    return dir + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);
}

static void writeKey(const QString& ini, const QString& key, const QString& value)
{
    QSettings s(ini, QSettings::IniFormat);
    s.setValue(key, value);
    s.sync();
}
static QString readKey(const QString& ini, const QString& key)
{
    QSettings s(ini, QSettings::IniFormat);
    s.sync();
    return s.value(key).toString();
}

// The two namespaces, built from AppBrand rather than spelled out: this probe is the ONE place in the tree
// that legitimately handles both brands, and hard-coding the old strings here would just be a literal the
// "no mentions of the old name" gate has to be taught to ignore. Composing them still asserts the real
// property — an id that arrives under the previous prefix leaves under the current one.
static QString legacyId(const QString& leaf)
{
    return QLatin1String(AppBrand::Legacy::kAddonPrefix) + leaf;
}
static QString currentId(const QString& leaf)
{
    return QLatin1String(AppBrand::kAddonPrefix) + leaf;
}

static void writeManifest(const QString& dir, const QString& id)
{
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/manifest.json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(QJsonObject{ { QStringLiteral("id"), id },
                                       { QStringLiteral("name"), QStringLiteral("Probe addon") } })
                .toJson(QJsonDocument::Compact));
}
static QString manifestId(const QString& dir)
{
    QFile f(dir + QStringLiteral("/manifest.json"));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("id")).toString();
}

// A legacy install: the previous brand's ini with real settings in it, including an addon-namespaced key AND
// an addon id buried in a VALUE (a favourite's stored addonId is the shape that actually broke before).
static void seedLegacyInstall(const QString& dir)
{
    QSettings s(legacyIniIn(dir), QSettings::IniFormat);
    s.setValue(QStringLiteral("roms/folder"), QStringLiteral("D:/roms"));
    s.setValue(QStringLiteral("steam/apikey"), QStringLiteral("secret"));
    s.setValue(QStringLiteral("addons/") + legacyId(QStringLiteral("igdb")) + QStringLiteral("/enabled"),
               QStringLiteral("true"));
    s.setValue(QStringLiteral("favorites/one"),
               QStringLiteral("{\"addonId\":\"%1\"}").arg(legacyId(QStringLiteral("aiocatalog"))));
    s.sync();
}

static void clearAllFlags()
{
    for (BrandMigration::Step st : { BrandMigration::Step::LocalIni, BrandMigration::Step::AddonIds,
                                     BrandMigration::Step::DriveFolder, BrandMigration::Step::DriveFiles })
        BrandMigration::setDone(st, false);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The probe's own device state lives in the build-tree ini shared with the other core probes. Start from a
    // known-clean slate: no migration flags, and no cloud credentials (so run()'s Drive half never engages).
    {
        QSettings s(newIniIn(AppPaths::dataDir()), QSettings::IniFormat);
        s.remove(QStringLiteral("cloud"));
        s.sync();
    }
    // Section 5 drives the real run() against AppPaths::dataDir(). Older builds of the sibling probes wrote a
    // previous-brand ini into that same build-tree folder, and leaving one there would make run()'s outcome
    // depend on build history. Drop it — it is probe scratch, never a user's install.
    QFile::remove(legacyIniIn(AppPaths::dataDir()));
    clearAllFlags();

    // ---- 1. the ini step COPIES, and never destroys the legacy file -------------------------------------
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "the scratch directory was created");
    const QString dir = tmp.path();
    const QString legacyIni = legacyIniIn(dir);
    const QString newIni = newIniIn(dir);
    seedLegacyInstall(dir);

    CHECK(BrandMigration::migrateLocalIni(dir), "the ini step reports completion");
    CHECK(QFileInfo::exists(legacyIni), "the legacy ini still exists after migration");
    CHECK(QFileInfo::exists(newIni), "the new ini was created");
    CHECK(readKey(newIni, QStringLiteral("roms/folder")) == QStringLiteral("D:/roms"),
          "settings survived the copy");
    CHECK(readKey(newIni, QStringLiteral("steam/apikey")) == QStringLiteral("secret"),
          "credentials survived the copy");
    // The Goliath->previous-brand hop in main.cpp writes the LEGACY addon prefix, so its output is
    // indistinguishable from a native previous-brand ini. Both users therefore need this rewrite.
    CHECK(readKey(newIni, QStringLiteral("addons/") + currentId(QStringLiteral("igdb"))
                          + QStringLiteral("/enabled")) == QStringLiteral("true"),
          "an addon-namespaced KEY was rewritten to the current prefix");
    CHECK(!readKey(newIni, QStringLiteral("favorites/one")).contains(QLatin1String(AppBrand::Legacy::kAddonPrefix))
              && readKey(newIni, QStringLiteral("favorites/one"))
                     .contains(QLatin1String(AppBrand::kAddonPrefix)),
          "an addon id inside a VALUE was rewritten to the current prefix");
    CHECK(readKey(legacyIni, QStringLiteral("roms/folder")) == QStringLiteral("D:/roms"),
          "the retained legacy ini is intact, not emptied");

    // ---- 2. idempotence — a second run changes nothing and never re-copies over a newer file -------------
    writeKey(newIni, QStringLiteral("roms/folder"), QStringLiteral("D:/changed"));
    CHECK(BrandMigration::migrateLocalIni(dir), "a second run reports completion");
    CHECK(readKey(newIni, QStringLiteral("roms/folder")) == QStringLiteral("D:/changed"),
          "a second run does NOT clobber the migrated ini with the legacy one");

    // ---- 3. resumability — a step whose flag is unset runs again; one whose flag is set does not ---------
    CHECK(BrandMigration::done(BrandMigration::Step::LocalIni), "the completed step is flagged");
    BrandMigration::setDone(BrandMigration::Step::LocalIni, false);
    CHECK(BrandMigration::migrateLocalIni(dir), "the re-run reports completion");
    CHECK(BrandMigration::done(BrandMigration::Step::LocalIni), "an unset flag causes the step to re-run");
    // The re-run is where resumability could bite: with the flag gone, the ONLY thing standing between the
    // user and a restored two-year-old ini is the guard on the destination's content.
    CHECK(readKey(newIni, QStringLiteral("roms/folder")) == QStringLiteral("D:/changed"),
          "a re-run after a cleared flag still does NOT clobber the migrated ini");

    // ---- 4. addon ids -----------------------------------------------------------------------------------
    {
        QTemporaryDir atmp;
        const QString adir = atmp.path();
        const QString legacyDir = adir + QStringLiteral("/addons/") + legacyId(QStringLiteral("aiocatalog"));
        const QString namedDir = adir + QStringLiteral("/addons/igdb");
        const QString thirdParty = adir + QStringLiteral("/addons/org.someone.catalog");
        writeManifest(legacyDir, legacyId(QStringLiteral("aiocatalog")));
        writeManifest(namedDir, legacyId(QStringLiteral("igdb")));
        writeManifest(thirdParty, QStringLiteral("org.someone.catalog"));

        BrandMigration::setDone(BrandMigration::Step::AddonIds, false);
        CHECK(BrandMigration::migrateAddonIds(adir), "the addon-id step reports completion");
        CHECK(BrandMigration::done(BrandMigration::Step::AddonIds), "the addon-id step is flagged");
        CHECK(manifestId(namedDir) == currentId(QStringLiteral("igdb")),
              "a legacy manifest id was rewritten in place");
        CHECK(manifestId(thirdParty) == QStringLiteral("org.someone.catalog"),
              "a third-party addon id was left untouched");
        // A directory NAMED after its id (how installAddon lays third-party packages out) moves with the id,
        // or every stored reference to it resolves to a folder that no longer matches its manifest.
        const QString movedDir = adir + QStringLiteral("/addons/") + currentId(QStringLiteral("aiocatalog"));
        CHECK(QFileInfo::exists(movedDir), "an id-named directory was renamed alongside its id");
        CHECK(manifestId(movedDir) == currentId(QStringLiteral("aiocatalog")),
              "the renamed directory's manifest carries the current prefix");
        CHECK(!QFileInfo::exists(legacyDir), "the legacy id-named directory is gone (it MOVED, not copied)");
        // Idempotent.
        BrandMigration::setDone(BrandMigration::Step::AddonIds, false);
        CHECK(BrandMigration::migrateAddonIds(adir), "a second addon-id run reports completion");
        CHECK(manifestId(movedDir) == currentId(QStringLiteral("aiocatalog")),
              "a second addon-id run is a no-op");
    }

    // ---- 5. nothing to migrate is SUCCESS, not failure ---------------------------------------------------
    {
        QTemporaryDir fresh;
        clearAllFlags();
        bool called = false, allDone = false;
        // No legacy ini, no addons dir, and no cloud credentials -> every step is vacuously complete and run()
        // must resolve synchronously rather than waiting on a Drive round-trip that will never happen.
        CHECK(BrandMigration::migrateLocalIni(fresh.path()), "a fresh install's ini step completes immediately");
        CHECK(BrandMigration::migrateAddonIds(fresh.path()), "a fresh install's addon step completes immediately");
        CHECK(!QFileInfo::exists(legacyIniIn(fresh.path())),
              "a fresh install did not invent a legacy ini");
        clearAllFlags();
        BrandMigration::run([&](bool ok) { called = true; allDone = ok; });
        CHECK(called, "run() invoked its callback");
        CHECK(allDone, "a fresh install with no legacy data completes immediately");
        CHECK(BrandMigration::done(BrandMigration::Step::LocalIni)
                  && BrandMigration::done(BrandMigration::Step::AddonIds),
              "run() flagged both local steps");
        // Drive is not configured here, so its flags must stay UNSET — signing in later has to run them.
        CHECK(!BrandMigration::done(BrandMigration::Step::DriveFolder)
                  && !BrandMigration::done(BrandMigration::Step::DriveFiles),
              "the Drive steps are NOT flagged when Drive was never reached");
    }

    clearAllFlags();
    if (failures) { std::fprintf(stderr, "BRAND: %d failure(s)\n", failures); return 1; }
    std::printf("BRAND-OK\n");
    return 0;
}
