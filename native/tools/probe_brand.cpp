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

// AddonManager::isEnabled's rule, re-spelled. AddonContext is linked so the addoncfg sections can assert
// through the real readConfig; AddonManager cannot be — it drags in QuickJS, the network stack and the whole
// addon runtime — so the on/off flag has no callable accessor here. The DEFAULT is the load-bearing half of
// the copy: a MISSING key reads as ENABLED. That is what makes these assertions discriminate at all. An
// add-on the user switched off, whose flag is still stranded under the other spelling, silently comes back
// ON — so "is it off?" is a real question about the reconcile and not a restatement of a key name.
static bool addonEnabledIn(const QString& ini, const QString& id)
{
    QSettings s(ini, QSettings::IniFormat);
    s.sync();
    return s.value(QStringLiteral("addon.enabled.") + id, true).toBool();
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

    // ---- 9. BOTH spellings of one id are loaded — neither is stranded, so neither may be touched -----------
    //
    // Sections 7 and 8 each describe an id that exists under exactly ONE spelling, which is what makes the
    // counterpart a dead name whose config is safe to consume. This section is the state where that premise is
    // false: a pre-rebrand sideloaded package (or a remote manifest reporting the previous spelling) loaded
    // alongside its bundled counterpart, so reload() hands reconcile BOTH ids and BOTH have live config.
    //
    // Reachable without doing anything exotic: the reserved-namespace install guard retires the previous
    // prefix as soon as Step::AddonIds is flagged, and addRemoteSource applies no namespace guard at all.
    //
    // Left unguarded the loop visits the pair twice and one credential does not survive. Pass one finds the
    // destination occupied, carries nothing, and removes the source key anyway. Pass two — same pair, roles
    // swapped — finds that side now vacant and moves the survivor onto it. The value that vanishes may well
    // have been typed in AFTER the migration, in which case the legacy ini beside the exe does not hold it
    // either and it is simply gone. Every later load then shuttles the survivor back and forth.
    {
        const QString ddir = AppPaths::dataDir();
        const QString bothNow = currentId(QStringLiteral("dualspell"));   // the bundled counterpart
        const QString bothWas = legacyId(QStringLiteral("dualspell"));    // the pre-rebrand package
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.setValue(QStringLiteral("addoncfg/") + bothNow + QStringLiteral("/apikey"),
                       QStringLiteral("fixture-token-echo"));    // typed AFTER migration: in no backup
            s.setValue(QStringLiteral("addoncfg/") + bothWas + QStringLiteral("/apikey"),
                       QStringLiteral("fixture-token-foxtrot"));
            s.setValue(QStringLiteral("addon.enabled.") + bothNow, false);  // the user switched THIS one off
            s.setValue(QStringLiteral("addon.enabled.") + bothWas, true);
            s.sync();
        }

        CHECK(BrandMigration::reconcileAddonConfig(ddir, { bothNow, bothWas }) == 0,
              "two live spellings of one id reconcile to nothing — neither side is stranded");
        // Asserted through readConfig, not through key presence: the bug class is a RENAME, so both keys are
        // still there afterwards on the broken build and only the VALUE each id reads apart tells them apart.
        CHECK(AddonContext::readConfig(bothNow, QStringLiteral("apikey")) == QStringLiteral("fixture-token-echo"),
              "a loaded add-on's config is not deleted because the other spelling is also loaded");
        CHECK(AddonContext::readConfig(bothWas, QStringLiteral("apikey")) == QStringLiteral("fixture-token-foxtrot"),
              "the other loaded add-on still reads its OWN config, not its counterpart's");
        CHECK(addonEnabledIn(newIniIn(ddir), bothNow) == false
                  && addonEnabledIn(newIniIn(ddir), bothWas) == true,
              "the on/off flags of two live spellings are left alone too");

        // The launch-after-launch half. Unguarded, the pair does not settle: each run swaps the survivor to
        // the other spelling and reports a restore, forever. Running it again must change nothing at all.
        CHECK(BrandMigration::reconcileAddonConfig(ddir, { bothNow, bothWas }) == 0,
              "a second run over two live spellings still reconciles nothing");
        CHECK(AddonContext::readConfig(bothNow, QStringLiteral("apikey")) == QStringLiteral("fixture-token-echo")
                  && AddonContext::readConfig(bothWas, QStringLiteral("apikey"))
                         == QStringLiteral("fixture-token-foxtrot"),
              "neither value ping-pongs between the spellings across runs");
    }

    // ---- 10. the on/off flag is reconciled too, in BOTH directions ----------------------------------------
    //
    // Section 7 asserts only that addon.enabled.<id> is EXCLUDED from the rewrite. That leaves the other half
    // untested: a flag that is already stranded — either because an earlier build's rewrite moved it, or
    // because the add-on's id legitimately moved and the exclusion left the flag behind — has to be carried
    // onto the id in use, exactly as the config keys are. Its default is "enabled", so the failure is quiet:
    // an add-on the user deliberately switched off just turns itself back on.
    {
        const QString ddir = AppPaths::dataDir();
        // (a) an add-on that KEPT the previous id, whose flag an earlier rewrite pushed to the current one.
        const QString keptId     = legacyId(QStringLiteral("enflag-kept"));
        const QString keptStrand = currentId(QStringLiteral("enflag-kept"));
        // (b) an add-on whose id genuinely MOVED, whose flag the exclusion now leaves under the previous one.
        const QString movedId     = currentId(QStringLiteral("enflag-moved"));
        const QString movedStrand = legacyId(QStringLiteral("enflag-moved"));
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.setValue(QStringLiteral("addon.enabled.") + keptStrand, false);
            s.setValue(QStringLiteral("addon.enabled.") + movedStrand, false);
            s.sync();
        }
        CHECK(BrandMigration::reconcileAddonConfig(ddir, { keptId, movedId }) == 2,
              "both stranded on/off flags are reported as carried across");
        CHECK(addonEnabledIn(newIniIn(ddir), keptId) == false,
              "an OFF flag stranded under the rewritten id is honoured again under the id in use");
        CHECK(addonEnabledIn(newIniIn(ddir), movedId) == false,
              "an OFF flag left behind by an id that DID move is carried forward to it");
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.sync();
            CHECK(!s.contains(QStringLiteral("addon.enabled.") + keptStrand)
                      && !s.contains(QStringLiteral("addon.enabled.") + movedStrand),
                  "the stranded flag keys are consumed, so a later run has nothing to do");
        }
        CHECK(BrandMigration::reconcileAddonConfig(ddir, { keptId, movedId }) == 0,
              "a second flag reconcile carries nothing across");
        CHECK(addonEnabledIn(newIniIn(ddir), keptId) == false && addonEnabledIn(newIniIn(ddir), movedId) == false,
              "a second flag reconcile leaves both settled flags off");
    }

    // ---- 11. an EMPTY incumbent is not an incumbent -------------------------------------------------------
    //
    // The never-clobber rule protects a value the user re-entered. A blank is not that. Both Configure
    // surfaces write blanks on Save — the classic dialog writes EVERY declared field verbatim — so the person
    // this recovery exists for, whose Configure screen necessarily came up blank, only has to have opened it
    // and pressed Save to have "" sitting under the id in use. Testing presence rather than content would let
    // that blank win AND then drop the real stranded credential, leaving one copy in a file beside the exe
    // that nobody will ever be told to open.
    {
        const QString ddir = AppPaths::dataDir();
        const QString blankId  = legacyId(QStringLiteral("blankinc"));   // reports the PREVIOUS id
        const QString blankWas = currentId(QStringLiteral("blankinc"));  // where the rewrite stranded it
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.setValue(QStringLiteral("addoncfg/") + blankWas + QStringLiteral("/apikey"),
                       QStringLiteral("fixture-token-golf"));            // the real one, stranded
            s.setValue(QStringLiteral("addoncfg/") + blankId + QStringLiteral("/apikey"), QString());
            // ...and a leaf where BOTH sides are blank, which must NOT be counted as a restore: the
            // "restored N stranded setting(s)" line has to mean something was actually given back.
            s.setValue(QStringLiteral("addoncfg/") + blankWas + QStringLiteral("/nothing"), QString());
            s.setValue(QStringLiteral("addoncfg/") + blankId + QStringLiteral("/nothing"), QString());
            s.sync();
        }
        // The premise, stated as an assertion rather than assumed: a blank really is STORED and present.
        // If QSettings ever stopped round-tripping an empty value the section below would pass vacuously.
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.sync();
            CHECK(s.contains(QStringLiteral("addoncfg/") + blankId + QStringLiteral("/apikey")),
                  "a blank written by Save is present in the ini (the incumbent this section is about)");
        }

        CHECK(BrandMigration::reconcileAddonConfig(ddir, { blankId }) == 1,
              "the blank-vs-blank leaf is not counted; only the real value is reported as restored");
        CHECK(AddonContext::readConfig(blankId, QStringLiteral("apikey")) == QStringLiteral("fixture-token-golf"),
              "a stranded credential beats an EMPTY incumbent rather than being dropped for it");
        {
            QSettings s(newIniIn(ddir), QSettings::IniFormat);
            s.sync();
            CHECK(!s.contains(QStringLiteral("addoncfg/") + blankWas + QStringLiteral("/apikey")),
                  "the stranded copy was consumed once its value had been carried across");
        }
        CHECK(BrandMigration::reconcileAddonConfig(ddir, { blankId }) == 0,
              "a second run over the recovered value carries nothing");
        CHECK(AddonContext::readConfig(blankId, QStringLiteral("apikey")) == QStringLiteral("fixture-token-golf"),
              "a second run leaves the recovered value alone");
    }

    clearAllFlags();
    if (failures) { std::fprintf(stderr, "BRAND: %d failure(s)\n", failures); return 1; }
    std::printf("BRAND-OK\n");
    return 0;
}
