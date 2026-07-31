// Headless coverage for the one-shot-but-resumable migration off the previous brand (BrandMigration).
//
// The step under the most scrutiny here is LocalIni, because it is the one that can lose every setting the
// user ever made. It is asserted directly (the legacy ini must still be on disk afterwards) and the assertion
// is mutation-tested: flipping QFile::copy to QFile::rename in BrandMigration.cpp must make this probe FAIL.
//
// Isolation: the local steps take an explicit data directory, so the ini/addon assertions run against a
// QTemporaryDir and never touch a real install. The FLAGS are device state and live in AppPaths::dataDir(),
// which for a probe build is this process's own scratch directory (issue #42): no flag is set when the probe
// starts, no cloud credential is configured, and so the Drive half of run() is dormant by construction rather
// than because the probe cleared up after whoever ran last.
//
// Prints BRAND-OK on success; any failure prints BRAND-FAIL <what> and exits non-zero.
#include "BrandMigration.h"
#include "Settings.h"
#include "AddonContext.h"   // sections 7/8 assert through the REAL per-addon config lookup, not a copy of it
#include "FavoritesStore.h" // sections 12/13: the stores that persist an add-on id INSIDE a value (#58)
#include "PlaylistStore.h"
#include "ProfileStore.h"   // ...and they are per-profile, which is half of what those sections assert

#include "AppBrand.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>
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

// ---- the REAL favourite/playlist lookup, and the one line of it that has to be re-spelled here -----------
//
// Neither of the two call sites can be linked into a headless probe: HomeView is Qt Widgets and most of the
// app, and AddonManager drags in QuickJS, the addon runtime and the network stack. So the resolution's final
// comparison is written out below. That is deliberately ALL that is written out — the half that actually
// broke, the id itself, comes out of FavoritesStore::list() / PlaylistStore::get() and is carried through
// exactly the transformations the real path applies to it.
//
// This is the distinction sections 12/13 live or die on. The bug is a RENAME: the favourite is still stored
// afterwards, its blob still parses, its title and thumbnail are all still there. Every "the favourite
// survived" check passes just as happily on the broken build. The only question that separates the two builds
// is whether the id it carries names an add-on that is actually loaded — which is precisely what the user hits
// when the toast says the source addon isn't available.
//
// Returns the id of the resolved source add-on, or empty for "isn't available".
static QString favoriteSourceAddon(const FavoriteItem& f, const QStringList& installedIds)
{
    const QString mime = QStringLiteral("fav:") + f.addonId; // HomeView.cpp: the tile carries it in `mime`
    const QString addonId = mime.mid(4);                     // HomeView::openFavorite strips the marker back off
    for (const QString& id : installedIds)                   // ...and matches it against manifest.id
        if (id == addonId) return id;
    return QString();                                        // -> "That favourite's source addon isn't available."
}

// The playlist half of the same path: playlistItemsCatalog stamps each row's sourceAddonId from the entry's
// stored addonId, and activateItem resolves it through AddonManager::sourceById (manifest.id ==).
static QString entrySourceAddon(const PlaylistEntry& e, const QStringList& installedIds)
{
    const QString sourceAddonId = e.addonId;
    for (const QString& id : installedIds)
        if (id == sourceAddonId) return id;
    return QString();
}

// Look one favourite up by itemId through the REAL store reader (never by re-parsing the blob here).
static bool favById(const QString& itemId, FavoriteItem& out)
{
    for (const FavoriteItem& f : FavoritesStore::list())
        if (f.itemId == itemId) { out = f; return true; }
    return false;
}
static bool entryById(const QString& playlistId, const QString& itemId, PlaylistEntry& out)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p)) return false;
    for (const PlaylistEntry& e : p.items)
        if (e.itemId == itemId) { out = e; return true; }
    return false;
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

// ---- fixtures for sections 12/13 --------------------------------------------------------------------------
//
// Written straight into the ini rather than through FavoritesStore::add() / PlaylistStore::addItem(), because
// what has to be described is a blob that ALREADY carries the WRONG add-on id — a state the stores' own
// writers cannot produce (they stamp whatever the loaded add-on reports). The blobs are byte-shaped exactly
// as those writers shape them, and everything is READ back through the real store readers.
static void seedFavorites(const QString& ini, const QString& profile,
                          const QVector<QPair<QString, QString>>& itemIdToAddonId)
{
    QJsonArray arr;
    for (const auto& pair : itemIdToAddonId)
    {
        QJsonObject o;
        o.insert(QStringLiteral("addonId"), pair.second);
        o.insert(QStringLiteral("itemId"), pair.first);
        o.insert(QStringLiteral("title"), QStringLiteral("Fixture ") + pair.first);
        o.insert(QStringLiteral("type"), QStringLiteral("movie"));
        o.insert(QStringLiteral("expandable"), false);
        o.insert(QStringLiteral("ts"), 1000.0);
        arr.append(o);
    }
    QSettings s(ini, QSettings::IniFormat);
    s.setValue(QStringLiteral("favorites/") + profile + QStringLiteral("/items"),
               QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    s.sync();
}

// One playlist per profile, in v2 shape with the schema already stamped, so PlaylistStore's v1->v2 migration
// is a proven no-op here and cannot be what rewrote the blob.
static void seedPlaylist(const QString& ini, const QString& profile, const QString& playlistId,
                         qint64 updatedAt, const QVector<QPair<QString, QString>>& itemIdToAddonId)
{
    QJsonArray items;
    for (const auto& pair : itemIdToAddonId)
    {
        QJsonObject e;
        e.insert(QStringLiteral("addonId"), pair.second);
        e.insert(QStringLiteral("itemId"), pair.first);
        e.insert(QStringLiteral("title"), QStringLiteral("Fixture ") + pair.first);
        e.insert(QStringLiteral("type"), QStringLiteral("movie"));
        e.insert(QStringLiteral("expandable"), false);
        items.append(e);
    }
    QJsonObject pl;
    pl.insert(QStringLiteral("id"), playlistId);
    pl.insert(QStringLiteral("categoryKey"), QStringLiteral("video"));
    pl.insert(QStringLiteral("name"), QStringLiteral("Fixture playlist"));
    pl.insert(QStringLiteral("updatedAt"), static_cast<double>(updatedAt));
    pl.insert(QStringLiteral("items"), items);
    QJsonArray arr; arr.append(pl);
    QSettings s(ini, QSettings::IniFormat);
    s.setValue(QStringLiteral("playlists/") + profile + QStringLiteral("/items"),
               QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    s.setValue(QStringLiteral("playlists/") + profile + QStringLiteral("/schema"), 2);
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
    // ...and an addon id inside a VALUE is NOT. This assertion used to say the opposite (#58 inverted it).
    // The prefix only ever appears inside an ini value as part of an add-on's SELF-REPORTED manifest id — a
    // foreign key this migration cannot observe — so rewriting it is the same guess isAddonIdKeyed refuses to
    // make for keys, and it is worse: it destroys the original, while the config case leaves it recoverable
    // under the other spelling. The stored id is reconciled against the ids that actually loaded instead
    // (BrandMigration::reconcileAddonRefs, sections 12/13).
    CHECK(readKey(newIni, QStringLiteral("favorites/one"))
                  .contains(QLatin1String(AppBrand::Legacy::kAddonPrefix))
              && !readKey(newIni, QStringLiteral("favorites/one"))
                      .contains(QLatin1String(AppBrand::kAddonPrefix)),
          "an addon id inside a VALUE is left alone (it is a foreign key, not a brand string)");
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

    // ---- 12. FAVOURITES: the add-on id stored INSIDE the blob, re-pointed at the id that loaded (#58) ------
    //
    // Sections 7-11 are about a key. This is the same root cause on the other surface it has: a favourite
    // persists the id of the add-on it was starred from INSIDE its JSON, and the ini rewrite used to rewrite
    // string values as well as key names. For the add-on pinned to the previous spelling that is a rename of
    // a foreign key nothing else follows, so HomeView::openFavorite resolves no source at all and the user is
    // told the source addon isn't available.
    //
    // Asserted through favoriteSourceAddon (see its comment) rather than by inspecting the blob. That is not
    // fussiness: the bug is a RENAME, the favourite is still there afterwards, and "the favourite is still
    // stored" is TRUE ON THE BROKEN BUILD. Only "the id it carries names an add-on that actually loaded"
    // separates them.
    //
    // EVERY PROFILE. These stores are namespaced per profile and only one is ever current, so the fixture
    // spreads the cases across four of them and the reconcile is called ONCE.
    {
        const QString ddir = AppPaths::dataDir();
        const QString ini  = newIniIn(ddir);

        const QString workerId    = legacyId(QStringLiteral("aiocatalog-worker"));  // PINNED to the previous id
        const QString workerWrong = currentId(QStringLiteral("aiocatalog-worker")); // where the rewrite put it
        const QString igdbNow     = currentId(QStringLiteral("igdb"));              // an id that genuinely MOVED
        const QString igdbWas     = legacyId(QStringLiteral("igdb"));
        const QString bothNow     = currentId(QStringLiteral("dualspell"));         // BOTH spellings loaded
        const QString bothWas     = legacyId(QStringLiteral("dualspell"));
        const QString thirdParty  = QStringLiteral("org.someone.catalog");
        // Loaded under NEITHER spelling: uninstalled, or remote with no cached manifest yet (which, on a
        // first launch offline, is every remote add-on there is). NOT "switched off", which this line used
        // to claim: AddonManager::loadFolder/loadRemoteSources apply no isEnabled test — enabled-gating is
        // serve-time — so a disabled INSTALLED add-on's id is in installedIds and its favourites resolve.
        const QString absentId    = currentId(QStringLiteral("notloaded"));

        // thirdParty is deliberately NOT in this list. Were it loaded, the "the stored id already resolves"
        // guard would return first and the third-party favourite below would prove nothing about the guard
        // that actually protects it — the one that refuses to act on an id belonging to NEITHER namespace.
        // A third-party add-on that is merely not loaded on this launch is the case where that guard is the
        // only thing standing between the user's favourite and an id invented for it.
        const QStringList installed{ workerId, igdbNow, bothNow, bothWas };

        seedFavorites(ini, QStringLiteral("alpha"),
                      { { QStringLiteral("fa-broken"), workerWrong },
                        { QStringLiteral("fa-third"),  thirdParty } });
        seedFavorites(ini, QStringLiteral("bravo"),
                      { { QStringLiteral("fb-moved"),  igdbWas },
                        { QStringLiteral("fb-absent"), absentId } });
        seedFavorites(ini, QStringLiteral("charlie"),
                      { { QStringLiteral("fc-was"), bothWas },
                        { QStringLiteral("fc-now"), bothNow } });
        seedFavorites(ini, QStringLiteral("delta"),
                      { { QStringLiteral("fd-ok"), workerId } });   // already correct

        // Captured to prove the never-clobber rule at the BYTE level: a profile with nothing wrong must not be
        // reserialized at all. Rewriting it would be invisible in any per-field check, and would churn a blob
        // the multi-device merge watches.
        const QString deltaBefore = readKey(ini, QStringLiteral("favorites/delta/items"));

        const int repointed = BrandMigration::reconcileAddonRefs(ddir, installed);
        CHECK(repointed == 2,
              "exactly the two stranded favourite references are reported as re-pointed");

        FavoriteItem f;

        // (a) the add-on PINNED to the previous spelling — the already-broken user, and the whole issue.
        ProfileStore::setCurrent(QStringLiteral("alpha"));
        // The premise, stated so the assertion below cannot pass vacuously if the blob ever stopped parsing.
        CHECK(favById(QStringLiteral("fa-broken"), f), "the repaired favourite is readable through the store");
        CHECK(favoriteSourceAddon(f, installed) == workerId,
              "a favourite of an add-on pinned to the previous id resolves its source addon again");
        CHECK(favById(QStringLiteral("fa-third"), f) && f.addonId == thirdParty,
              "a favourite whose source is under NEITHER namespace keeps its id verbatim");

        // (b) THE OTHER DIRECTION, and in a DIFFERENT PROFILE — one call had to reach both. An id that
        //     genuinely moved leaves its favourites naming the previous spelling (permanently now that the
        //     value rewrite is gone), so without this half the fix for (a) would strand every local add-on's
        //     favourites instead of the pinned one's.
        ProfileStore::setCurrent(QStringLiteral("bravo"));
        CHECK(favById(QStringLiteral("fb-moved"), f) && favoriteSourceAddon(f, installed) == igdbNow,
              "a favourite naming an id that DID move is carried forward, in a second profile");
        // (c) neither spelling loaded -> LEAVE IT. Moving it to a name that also does not resolve would trade
        //     a state that is still repairable on a later launch for one that never is.
        CHECK(favById(QStringLiteral("fb-absent"), f) && f.addonId == absentId,
              "an id loaded under NEITHER spelling is left exactly as stored, not guessed at");
        // (There is deliberately NO companion "...and it resolves to nothing" line here. It would restate
        //  the fixture — absentId is not in `installed`, so the resolution is empty by construction — and no
        //  mutation of the implementation can make it fail. An assertion that cannot fail is worse than none:
        //  it reads like coverage.)

        // (d) BOTH SPELLINGS LIVE — nothing is stranded, so nothing may move. Driving from the STORED id is
        //     what makes this fall out for free: whichever spelling the blob names is itself loaded.
        ProfileStore::setCurrent(QStringLiteral("charlie"));
        CHECK(favById(QStringLiteral("fc-was"), f) && favoriteSourceAddon(f, installed) == bothWas,
              "with both spellings loaded, a favourite of the previous one still resolves to THAT one");
        CHECK(favById(QStringLiteral("fc-now"), f) && favoriteSourceAddon(f, installed) == bothNow,
              "...and a favourite of the current one still resolves to THAT one");

        // (e) never clobber what is already right.
        CHECK(readKey(ini, QStringLiteral("favorites/delta/items")) == deltaBefore,
              "a profile whose favourites are already correct is not so much as reserialized");

        // (f) idempotent, across every profile at once, and no value ping-pongs between spellings.
        CHECK(BrandMigration::reconcileAddonRefs(ddir, installed) == 0,
              "a second pass re-points nothing");
        ProfileStore::setCurrent(QStringLiteral("alpha"));
        CHECK(favById(QStringLiteral("fa-broken"), f) && favoriteSourceAddon(f, installed) == workerId,
              "a second pass leaves the repaired favourite resolving");
        // ...and the two live spellings do not ping-pong. Checked after an ODD number of passes, on purpose:
        // with the already-resolves guard gone the pair swaps on EVERY run, so after an even number of them
        // it is back where it started and a check made at that moment passes on the broken build. This is the
        // same trap in miniature as "the favourite is still stored" — a state that happens to look right.
        BrandMigration::reconcileAddonRefs(ddir, installed);   // a third
        ProfileStore::setCurrent(QStringLiteral("charlie"));
        CHECK(favById(QStringLiteral("fc-was"), f) && favoriteSourceAddon(f, installed) == bothWas,
              "an odd number of passes still does not swap the two live spellings' favourites");
    }

    // ---- 13. PLAYLISTS: the same id, one level deeper (per ENTRY, since a playlist is mixed-source) --------
    {
        const QString ddir = AppPaths::dataDir();
        const QString ini  = newIniIn(ddir);

        const QString workerId    = legacyId(QStringLiteral("aiocatalog-worker"));
        const QString workerWrong = currentId(QStringLiteral("aiocatalog-worker"));
        const QString igdbNow     = currentId(QStringLiteral("igdb"));
        const QString igdbWas     = legacyId(QStringLiteral("igdb"));
        const QString absentId    = currentId(QStringLiteral("notloaded"));
        const QStringList installed{ workerId, igdbNow };

        const qint64 seededAt = 111;   // an updatedAt from long ago; the repair must not re-date it
        seedPlaylist(ini, QStringLiteral("echo"), QStringLiteral("pl-echo"), seededAt,
                     { { QStringLiteral("pe-broken"), workerWrong },
                       { QStringLiteral("pe-absent"), absentId } });
        seedPlaylist(ini, QStringLiteral("foxtrot"), QStringLiteral("pl-fox"), seededAt,
                     { { QStringLiteral("pf-moved"), igdbWas } });
        seedPlaylist(ini, QStringLiteral("golf"), QStringLiteral("pl-golf"), seededAt,
                     { { QStringLiteral("pg-ok"), workerId } });   // already correct

        const QString golfBefore = readKey(ini, QStringLiteral("playlists/golf/items"));

        const int repointed = BrandMigration::reconcileAddonRefs(ddir, installed);
        CHECK(repointed == 2, "exactly the two stranded playlist entries are reported as re-pointed");

        PlaylistEntry e;
        ProfileStore::setCurrent(QStringLiteral("echo"));
        CHECK(entryById(QStringLiteral("pl-echo"), QStringLiteral("pe-broken"), e),
              "the repaired playlist entry is readable through the store");
        CHECK(entrySourceAddon(e, installed) == workerId,
              "a playlist entry from an add-on pinned to the previous id resolves its source again");
        CHECK(entryById(QStringLiteral("pl-echo"), QStringLiteral("pe-absent"), e) && e.addonId == absentId,
              "a playlist entry whose add-on loaded under neither spelling is left exactly as stored");
        // updatedAt is the merge clock (mdsync T2, whole-object newest-wins). Re-dating a playlist because a
        // local repair touched it would let that repair beat a genuinely newer edit made on another device.
        //
        // A NOTE ON WHAT THIS ONE IS. a7040de's message claimed "every remaining assertion in both sections
        // is now killed by at least one mutation of the implementation". That is not true of this line and
        // cannot be: no statement in repointPlaylists or reconcileAddonRefs writes updatedAt, so there is
        // nothing here to mutate — it asserts the ABSENCE of a behaviour. It is kept deliberately, and it is
        // not the inert kind: it goes red the moment an edit ADDS a re-date (verified by inserting exactly
        // that line, which turns this and four of probe_cloudmerge's section-19 checks red together). The
        // distinction worth holding on to is between an assertion no CHANGE can kill — a restatement of the
        // fixture, which is what got deleted in a7040de — and one that only an INSERTION can kill, which is
        // what a tripwire is for.
        {
            Playlist p;
            CHECK(PlaylistStore::get(QStringLiteral("pl-echo"), p) && p.updatedAt == seededAt,
                  "the repair does NOT bump updatedAt (it would beat a newer edit from another device)");
        }

        ProfileStore::setCurrent(QStringLiteral("foxtrot"));
        CHECK(entryById(QStringLiteral("pl-fox"), QStringLiteral("pf-moved"), e)
                  && entrySourceAddon(e, installed) == igdbNow,
              "a playlist entry naming an id that DID move is carried forward, in a second profile");

        CHECK(readKey(ini, QStringLiteral("playlists/golf/items")) == golfBefore,
              "a profile whose playlists are already correct is not so much as reserialized");

        CHECK(BrandMigration::reconcileAddonRefs(ddir, installed) == 0,
              "a second pass over the playlists re-points nothing");
        ProfileStore::setCurrent(QStringLiteral("echo"));
        CHECK(entryById(QStringLiteral("pl-echo"), QStringLiteral("pe-broken"), e)
                  && entrySourceAddon(e, installed) == workerId,
              "a second pass leaves the repaired playlist entry resolving");
    }

    // ---- 14. A repair that could not be WRITTEN is never REPORTED (#58 review) ----------------------------
    // Both reconcile passes return a count their caller turns into something the user reads — "restored N
    // stranded setting(s)", "re-pointed N stored reference(s)". sync() was called and its status thrown away,
    // so an ini that cannot be written (read-only, full disk, locked by another process) produced that line
    // for a repair that landed nowhere — and produced it again on the next launch, and the one after.
    //
    // ONE DIRECTORY PER READ-ONLY CALL, and this is load-bearing rather than tidiness. QSettings shares a
    // QConfFile per PATH, and a sync that fails leaves the pending writes sitting in it. The next QSettings
    // opened on that same path re-attempts them during its own construction, fails again, and comes up with
    // status() != NoError — so the SECOND reconcile on a shared path returns 0 from its opening guard, having
    // never looked at anything. Written that way, "the second one also reports nothing" passed on a build
    // with the sync check removed: it was measuring the shared cache, not the fix. Each read-only call now
    // gets a path nothing else has touched.
    {
        const QString okDir  = AppPaths::dataDir() + QStringLiteral("/probe_sync_ok");
        const QString roRefs = AppPaths::dataDir() + QStringLiteral("/probe_sync_ro_refs");
        const QString roCfg  = AppPaths::dataDir() + QStringLiteral("/probe_sync_ro_cfg");
        for (const QString& d : { okDir, roRefs, roCfg }) { QDir(d).removeRecursively(); QDir().mkpath(d); }
        const QString okIni    = newIniIn(okDir);
        const QString roRefIni = newIniIn(roRefs);
        const QString roCfgIni = newIniIn(roCfg);

        const QString workerId    = legacyId(QStringLiteral("aiocatalog-worker"));  // PINNED to the previous id
        const QString workerWrong = currentId(QStringLiteral("aiocatalog-worker")); // where the rewrite put it
        const QStringList installed{ workerId };
        const QString strandedCfg = QStringLiteral("addoncfg/") + workerWrong + QStringLiteral("/apiKey");

        // The writable control. Without it the read-only asserts below would pass just as happily on a build
        // where the repair never fired at all — 0 for the wrong reason reads exactly like 0 for the right one.
        seedFavorites(okIni, QStringLiteral("hotel"), { { QStringLiteral("fh"), workerWrong } });
        writeKey(okIni, strandedCfg, QStringLiteral("KEY"));
        CHECK(BrandMigration::reconcileAddonRefs(okDir, installed) == 1,
              "the writable control: a stranded reference is repaired AND reported");
        CHECK(BrandMigration::reconcileAddonConfig(okDir, installed) == 1,
              "...and so is a stranded config value");

        // The same fixture in each read-only directory, then the write permission taken away.
        seedFavorites(roRefIni, QStringLiteral("hotel"), { { QStringLiteral("fh"), workerWrong } });
        writeKey(roCfgIni, strandedCfg, QStringLiteral("KEY"));
        QByteArray refsBefore, cfgBefore;
        { QFile f(roRefIni); if (f.open(QIODevice::ReadOnly)) refsBefore = f.readAll(); }
        { QFile f(roCfgIni); if (f.open(QIODevice::ReadOnly)) cfgBefore = f.readAll(); }
        CHECK(!refsBefore.isEmpty() && !cfgBefore.isEmpty(),
              "both read-only fixtures were written before their permissions changed");
        CHECK(QFile::setPermissions(roRefIni, QFile::ReadOwner | QFile::ReadUser)
                  && QFile::setPermissions(roCfgIni, QFile::ReadOwner | QFile::ReadUser),
              "both fixtures could be made read-only");

        CHECK(BrandMigration::reconcileAddonRefs(roRefs, installed) == 0,
              "a re-point that could not be flushed reports NOTHING repaired");
        CHECK(BrandMigration::reconcileAddonConfig(roCfg, installed) == 0,
              "...and neither does a config adoption that could not be flushed");
        // Read the FILE, not the settings: QSettings still holds the unflushed change in memory, so a
        // readback through it would show the repair that never reached the disk.
        QByteArray refsAfter, cfgAfter;
        { QFile f(roRefIni); if (f.open(QIODevice::ReadOnly)) refsAfter = f.readAll(); }
        { QFile f(roCfgIni); if (f.open(QIODevice::ReadOnly)) cfgAfter = f.readAll(); }
        CHECK(refsAfter == refsBefore && cfgAfter == cfgBefore,
              "...and the reported nothing is the truth: both files are byte-for-byte unchanged");

        for (const QString& f : { roRefIni, roCfgIni })
            QFile::setPermissions(f, QFile::ReadOwner | QFile::ReadUser
                                         | QFile::WriteOwner | QFile::WriteUser);
        bool residue = false;
        for (const QString& d : { okDir, roRefs, roCfg })
        { QDir(d).removeRecursively(); if (QFileInfo::exists(d)) residue = true; }
        CHECK(!residue, "every fixture directory is removed (this probe leaves no residue)");
    }

    clearAllFlags();
    if (failures) { std::fprintf(stderr, "BRAND: %d failure(s)\n", failures); return 1; }
    std::printf("BRAND-OK\n");
    return 0;
}
