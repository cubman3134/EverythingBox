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
#include "Settings.h"
#include "AddonContext.h"   // sections 7/8 assert through the REAL per-addon config lookup, not a copy of it

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

    // ---- 6. the parental PIN salt is NOT a brand string ---------------------------------------------------
    // The PIN is stored as SHA-256(salt + pin), so the salt is an INPUT to a hash already written to every
    // existing user's ini — "renaming" it redefines the function and no PIN a user set ever matches again.
    // The rebrand's prose sweep DID rename it (caught in review), so this asserts the hash against a value
    // computed before the rename. It is deliberately a literal digest, not a re-derivation from the salt
    // constant: a check that recomputes from whatever the salt currently is would pass after the very
    // regression it exists to catch.
    {
        Settings::setParentalPin(QStringLiteral("1234"));
        QSettings ini(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                      QSettings::IniFormat);
        CHECK(ini.value(QStringLiteral("parental/pinHash")).toString()
                  == QStringLiteral("dd4e3ef8689981a15d885a493b987d6b72c04cd0077c569fe515d99dbedcff30"),
              "the parental PIN hash still matches the pre-rename salt");
        CHECK(Settings::checkParentalPin(QStringLiteral("1234")),
              "the PIN round-trips through checkParentalPin");
        Settings::setParentalPin(QString());   // leave the probe's own ini clean
    }

    // ---- 7. per-add-on config survives the ini step for an add-on that KEPT the previous id ---------------
    //
    // The gap section 4 leaves. Its fixtures are id-named LOCAL FOLDERS, so the only add-on it can describe is
    // one the migration can see and legitimately rename. The add-on this section is about is REMOTE: it has no
    // folder, its manifest lives behind a URL that migration time never fetches, and it deliberately keeps the
    // previous namespace forever, because for a remote add-on the id and the URL are identity rather than
    // branding. Nothing about it is observable from disk — so a rewrite of the keys it owns is a guess, and the
    // thing it silently destroys is the user's stored API keys.
    //
    // Asserted through AddonContext::readConfig — the SAME call the Configure screen and the outbound
    // per-user config header both use — and not through a key string spelled out here. That is the point: the
    // bug is a RENAME, so the keys do still exist afterwards and any check of the form "some addoncfg key is
    // present" passes just as happily on the broken build. The only question that separates the two is whether
    // the value comes back when looked up under the id the add-on ACTUALLY reports.
    //
    // Runs against AppPaths::dataDir() because that is where readConfig looks. Under EB_ISOLATED_DATA_DIR
    // (every probe target, #42) that is this process's own scratch directory, created at startup and removed
    // at exit — a fixture. No installed ini is opened here, and none can be.
    {
        const QString ddir = AppPaths::dataDir();
        // A destination holding user content would make migrateLocalIni short-circuit as "already migrated"
        // (correctly — that guard is what stops a stale snapshot landing on live settings). Earlier sections
        // have written here, so clear the slate: this directory is probe scratch, never an install.
        QFile::remove(newIniIn(ddir));
        QFile::remove(legacyIniIn(ddir));
        clearAllFlags();

        const QString workerId = legacyId(QStringLiteral("aiocatalog-worker"));   // the pinned, still-legacy id
        {
            QSettings s(legacyIniIn(ddir), QSettings::IniFormat);
            s.setValue(QStringLiteral("addoncfg/") + workerId + QStringLiteral("/apikey"),
                       QStringLiteral("fixture-token-alpha"));
            s.setValue(QStringLiteral("addon.enabled.") + workerId, false);   // the user turned it OFF
            // A key that IS ours to rename, alongside it: the exclusion must be surgical, not a blanket
            // "stop rewriting", or section 1's property quietly dies here.
            s.setValue(QStringLiteral("addons/") + legacyId(QStringLiteral("igdb")) + QStringLiteral("/enabled"),
                       QStringLiteral("true"));
            s.sync();
        }

        CHECK(BrandMigration::migrateLocalIni(ddir), "the ini step completes with per-addon config present");

        // THE assertion. On the pre-fix build the key was renamed to the current namespace while the add-on
        // went on reporting the previous one, so this lookup returned empty and the Configure field came up
        // blank with no error. Nothing else in this file would have noticed.
        CHECK(AddonContext::readConfig(workerId, QStringLiteral("apikey")) == QStringLiteral("fixture-token-alpha"),
              "an add-on that kept the previous id can still READ its stored config after migration");
        // ...and it was not copied to the other spelling either. A rewrite that duplicated rather than moved
        // would satisfy the line above while leaving a second copy of a user credential in the ini forever.
        CHECK(AddonContext::readConfig(currentId(QStringLiteral("aiocatalog-worker")),
                                       QStringLiteral("apikey")).isEmpty(),
              "the config was not also left under the id the add-on does NOT report");
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.sync();
            CHECK(s.value(QStringLiteral("addon.enabled.") + workerId).toString() == QStringLiteral("false"),
                  "the per-addon enabled flag also stayed under the id the add-on reports");
            CHECK(s.value(QStringLiteral("addons/") + currentId(QStringLiteral("igdb"))
                          + QStringLiteral("/enabled")).toString() == QStringLiteral("true"),
                  "a key that IS ours to rename was still rewritten (the exclusion is scoped, not a blanket)");
        }
    }

    // ---- 8. recovery: config already stranded by an earlier build is found again --------------------------
    //
    // Section 7 only helps someone who has not upgraded yet. Everyone who already has was renamed on the way
    // in, and their keys are sitting in the ini right now under a name nothing reads. That data is still
    // there, so it is recoverable — and if it is not recovered here it never is, because the ini step is
    // flagged and will not run again.
    //
    // Mutates the ini through QSettings only, never by replacing the file: readConfig's store is a long-lived
    // static (see the flagStorePath comment in BrandMigration.cpp for why that matters), and swapping the file
    // underneath it would leave it reading a snapshot instead of the fixture.
    {
        const QString ddir = AppPaths::dataDir();
        const QString workerId  = legacyId(QStringLiteral("aiocatalog-worker"));   // reports the PREVIOUS id
        const QString strandedTo = currentId(QStringLiteral("aiocatalog-worker")); // where the rewrite put it
        const QString igdbNow   = currentId(QStringLiteral("igdb"));               // reports the CURRENT id
        const QString igdbWas   = legacyId(QStringLiteral("igdb"));

        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            // (a) the already-broken user: renamed by a previous run, nothing under the id in use.
            s.setValue(QStringLiteral("addoncfg/") + strandedTo + QStringLiteral("/token"),
                       QStringLiteral("fixture-token-bravo"));
            // (b) the user who gave up and typed their key in again. BOTH spellings hold a value, and the one
            //     they re-entered is the one that must survive.
            s.setValue(QStringLiteral("addoncfg/") + strandedTo + QStringLiteral("/token2"),
                       QStringLiteral("fixture-token-stale"));
            s.setValue(QStringLiteral("addoncfg/") + workerId   + QStringLiteral("/token2"),
                       QStringLiteral("fixture-token-reentered"));
            // (c) the opposite direction: an add-on whose id genuinely DID move, whose config the ini step no
            //     longer follows. Without this half, fix (1) would strand every local add-on's config instead.
            s.setValue(QStringLiteral("addoncfg/") + igdbWas + QStringLiteral("/token"),
                       QStringLiteral("fixture-token-charlie"));
            s.sync();
        }

        const int restored = BrandMigration::reconcileAddonConfig(ddir, { workerId, igdbNow });
        CHECK(restored == 2, "reconcile reports exactly the values it carried across (not the ones it kept)");

        CHECK(AddonContext::readConfig(workerId, QStringLiteral("token")) == QStringLiteral("fixture-token-bravo"),
              "config stranded under the rewritten id is READABLE again under the id in use");
        CHECK(AddonContext::readConfig(igdbNow, QStringLiteral("token")) == QStringLiteral("fixture-token-charlie"),
              "config left behind by an id that DID move is carried forward to it");
        CHECK(AddonContext::readConfig(workerId, QStringLiteral("token2")) == QStringLiteral("fixture-token-reentered"),
              "a value already stored under the id in use WINS over the stale one");

        // The stale copies are gone — that is what makes a second run a no-op, and it is also what stops a
        // user credential living on in the ini under a name nothing reads.
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.sync();
            CHECK(!s.contains(QStringLiteral("addoncfg/") + strandedTo + QStringLiteral("/token"))
                      && !s.contains(QStringLiteral("addoncfg/") + strandedTo + QStringLiteral("/token2")),
                  "the stale copies were dropped, including the one that lost to a re-entered value");
        }

        // Idempotent: nothing left to carry, and nothing disturbed by asking again.
        CHECK(BrandMigration::reconcileAddonConfig(ddir, { workerId, igdbNow }) == 0,
              "a second reconcile carries nothing across");
        CHECK(AddonContext::readConfig(workerId, QStringLiteral("token2")) == QStringLiteral("fixture-token-reentered"),
              "a second reconcile leaves the winning value alone");

        // A third party's id is under neither namespace and must be left completely alone.
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.setValue(QStringLiteral("addoncfg/org.someone.catalog/token"), QStringLiteral("fixture-token-delta"));
            s.sync();
        }
        CHECK(BrandMigration::reconcileAddonConfig(ddir, { QStringLiteral("org.someone.catalog") }) == 0,
              "a third-party addon id is not reconciled against anything");
        CHECK(AddonContext::readConfig(QStringLiteral("org.someone.catalog"), QStringLiteral("token"))
                  == QStringLiteral("fixture-token-delta"),
              "a third-party addon's config is untouched");
    }

    clearAllFlags();
    if (failures) { std::fprintf(stderr, "BRAND: %d failure(s)\n", failures); return 1; }
    std::printf("BRAND-OK\n");
    return 0;
}
