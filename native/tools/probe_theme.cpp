// Headless check of the per-profile theme choice (roadmap #57). ThemeChoice owns the theme setting: the
// per-profile key, whether a profile still owes us a pick, what to actually render, and the one-time
// migration that carries both the global->per-profile move and the XMB->Triple folder rename. Sections 1-6
// pin the PURE decisions (no ini, no filesystem) verbatim, so the UI layers can never drift from them —
// including legacyEffectiveGlobal (4b), the "an upgrade's appearance never changes on update" guarantee.
//
// Section 6b covers the OTHER pure theme decision this binary owns: ThemeFormFactors::fit — a theme
// manifest's `formFactors` declaration judged against the device deciding right now (issue #32). It lives
// here rather than in a probe of its own because it is the same shape of thing (a pure table about which
// theme a profile gets) and because ThemeFormFactors.cpp, like ThemeChoice.cpp, is QtCore-only.
//
// Section 7 covers the INI-BACKED half — forProfile/setForProfile/runMigrationForIds. That half had no
// coverage at all, which is exactly why a migration that could destroy the legacy value survived review and
// six mutations: every pure-table assertion passed while the ini path threw the value away. It runs against a
// scratch ini in the temp dir (ThemeChoice::setIniPathForTesting), never the app's real everythingbox.ini,
// and deletes every file it made before exiting.
//
// Prints THEME-OK on success; any failure prints THEME-FAIL <cond> and exits non-zero.
#include "ThemeChoice.h"
#include "ThemeFormFactors.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "THEME-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    const QStringList kShipped = { QStringLiteral("Channels"), QStringLiteral("Triple") };

    // ---- 1. keyFor: the exact key format, including the empty-profile-id case ------------------------
    CHECK(ThemeChoice::keyFor(QStringLiteral("abc123")) == QStringLiteral("themedHome/theme/abc123"));
    CHECK(ThemeChoice::keyFor(QString()) == QStringLiteral("themedHome/theme/default"));

    // ---- 2. needsPick: stored-or-not, and NOTHING else ----------------------------------------------
    // A profile with nothing stored owes a pick.
    CHECK(ThemeChoice::needsPick(QString()) == true);
    // A profile with a stored choice does not.
    CHECK(ThemeChoice::needsPick(QStringLiteral("Triple")) == false);
    CHECK(ThemeChoice::needsPick(QStringLiteral("Channels")) == false);
    // THE CONTRACT: a stored theme that is NOT installed on this device still does not force a pick.
    // "themedHome/theme/<id>" SYNCS (it is not in CloudSync::isDeviceLocalKey), so re-asking on a device
    // that merely lacks the folder would write an answer that syncs back and overwrites the choice made on
    // the other device. The missing folder is a rendering fact, and resolve() is what covers it:
    CHECK(ThemeChoice::needsPick(QStringLiteral("Grid")) == false);
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), kShipped) == QStringLiteral("Triple"));
    // ...and the user's stored choice is still there, untouched, for the device that does have the folder.
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"),
                               { QStringLiteral("Channels"), QStringLiteral("Grid"),
                                 QStringLiteral("Triple") }) == QStringLiteral("Grid"));

    // ---- 3. resolve: all four ordering steps --------------------------------------------------------
    // (a) stored, when installed. Both entries of kShipped, because a `installed.value(0)` mutation makes
    //     the index-0 name ("Channels") pass by coincidence — "Triple" at index 1 is what actually pins it.
    CHECK(ThemeChoice::resolve(QStringLiteral("Channels"), kShipped) == QStringLiteral("Channels"));
    CHECK(ThemeChoice::resolve(QStringLiteral("Triple"), kShipped) == QStringLiteral("Triple"));
    // (b) the fallback, when the stored theme is gone.
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), kShipped) == QStringLiteral("Triple"));
    CHECK(ThemeChoice::resolve(QString(), kShipped) == QStringLiteral("Triple"));
    // (c) the FIRST installed theme, when the fallback itself isn't installed. A user who deleted
    //     Triple and kept only a community theme must land on that theme, not on a name that is
    //     nowhere on disk.
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), { QStringLiteral("Aurora") })
          == QStringLiteral("Aurora"));
    CHECK(ThemeChoice::resolve(QString(), { QStringLiteral("Aurora"), QStringLiteral("Zed") })
          == QStringLiteral("Aurora"));
    // (d) nothing installed at all -> empty. Callers already handle this (MainWindow.cpp:3951).
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), {}).isEmpty());
    CHECK(ThemeChoice::resolve(QString(), {}).isEmpty());
    // resolve NEVER returns a folder that isn't installed — the invariant the whole function exists for.
    CHECK(kShipped.contains(ThemeChoice::resolve(QStringLiteral("Nonexistent"), kShipped)));

    // ---- 4. renameLegacyFolder: the XMB -> Triple move ---------------------------------------------
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("XMB")) == QStringLiteral("Triple"));
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("Channels")) == QStringLiteral("Channels"));
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("Grid")) == QStringLiteral("Grid"));
    CHECK(ThemeChoice::renameLegacyFolder(QString()).isEmpty());

    // ---- 4b. legacyEffectiveGlobal: what an UPGRADE was actually rendering before #57 ----------------
    // The pre-#57 read was store().value("themedHome/theme", "Default"), so an upgrade user who never touched
    // the setting was rendering "Default". planMigration seeds nothing for an empty global and resolve("")
    // now prefers "Triple", so without this the spec's "nobody's appearance changes on update" would be false.
    const QStringList kUpgraded = { QStringLiteral("Channels"), QStringLiteral("Default"),
                                    QStringLiteral("Triple") };
    // (a) An explicit global always wins, upgrade or not, installed or not.
    CHECK(ThemeChoice::legacyEffectiveGlobal(QStringLiteral("Grid"), true,  kUpgraded) == QStringLiteral("Grid"));
    CHECK(ThemeChoice::legacyEffectiveGlobal(QStringLiteral("Grid"), false, kUpgraded) == QStringLiteral("Grid"));
    // (b) Upgrade, no global, "Default" still on disk -> preserve "Default".
    CHECK(ThemeChoice::legacyEffectiveGlobal(QString(), true, kUpgraded) == QStringLiteral("Default"));
    // (c) Upgrade, no global, "Default" NOT installed -> empty, so they get the pick rather than a broken theme.
    CHECK(ThemeChoice::legacyEffectiveGlobal(QString(), true, kShipped).isEmpty());
    CHECK(ThemeChoice::legacyEffectiveGlobal(QString(), true, {}).isEmpty());
    // (d) FRESH install, no global -> empty even when "Default" happens to be installed. A fresh install has
    //     nothing to preserve; it owes a pick.
    CHECK(ThemeChoice::legacyEffectiveGlobal(QString(), false, kUpgraded).isEmpty());

    // ---- 5. planMigration: the table --------------------------------------------------------------
    const QStringList twoProfiles = { QStringLiteral("p1"), QStringLiteral("p2") };

    // (a) A legacy global value fans out to every profile that has none.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), twoProfiles, {});
        CHECK(out.size() == 2);
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Grid"));
        CHECK(out.value(QStringLiteral("p2")) == QStringLiteral("Grid"));
    }

    // (b) The rename rides along: a legacy global of XMB lands as Triple.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("XMB"), { QStringLiteral("p1") }, {});
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Triple"));
    }

    // (c) An EXISTING per-profile value is never overwritten by the global.
    {
        QHash<QString, QString> existing;
        existing.insert(QStringLiteral("p1"), QStringLiteral("Channels"));
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), { QStringLiteral("p1") }, existing);
        CHECK(out.isEmpty());
    }

    // (d) ...but an existing value still gets the folder rename applied.
    {
        QHash<QString, QString> existing;
        existing.insert(QStringLiteral("p1"), QStringLiteral("XMB"));
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), { QStringLiteral("p1") }, existing);
        CHECK(out.size() == 1);
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Triple"));
    }

    // (e) No legacy global and no stored value -> nothing written, so needsPick stays true and the
    //     profile gets the forced pick. This is the genuinely-fresh-install case.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QString(), { QStringLiteral("p1") }, {});
        CHECK(out.isEmpty());
        CHECK(ThemeChoice::needsPick(QString()) == true);
    }

    // (f) No profiles at all -> nothing to write, no crash.
    CHECK(ThemeChoice::planMigration(QStringLiteral("Grid"), {}, {}).isEmpty());

    // ---- 6. IDEMPOTENCE: applying the plan and re-running produces nothing -------------------------
    // The migration is flag-guarded in practice, but it must ALSO be naturally idempotent — a flag that
    // fails to persist (a crash between write and sync) must not corrupt anything on the second run.
    {
        QHash<QString, QString> existing;
        const QHash<QString, QString> first =
            ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing);
        for (auto it = first.constBegin(); it != first.constEnd(); ++it)
            existing.insert(it.key(), it.value());
        const QHash<QString, QString> second =
            ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing);
        CHECK(second.isEmpty());
        // ...and a third run over the SAME state is still empty.
        CHECK(ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing).isEmpty());
    }

    // ---- 6b. ThemeFormFactors::fit — the manifest declaration vs. THIS device (issue #32) ------------
    // The whole of the form-factor feature's logic. It is ADVISORY: no caller may gate on it, so every
    // assertion here is about what the picker LABELS, never about what it will render.
    {
        using ThemeFormFactors::Fit;
        auto arr = [](std::initializer_list<const char*> labels) {
            QJsonArray a;
            for (const char* l : labels) a.append(QString::fromUtf8(l));
            return QJsonValue(a);
        };
        const QJsonValue kShippedDecl = arr({ "desktop", "tv", "mobile", "handheld" });

        // (a) ABSENT is Undeclared — the decision every community theme in the registry depends on. Neither
        //     "supports everything" (Supported, which would invent a claim) nor "supports nothing"
        //     (Unsupported, which would flag every existing theme).
        CHECK(ThemeFormFactors::fit(QJsonValue(QJsonValue::Undefined), QStringLiteral("desktop"))
              == Fit::Undeclared);

        // (b) A value that is not an ARRAY is also Undeclared — strict on purpose (see the header). The bare
        //     string is the typo an author will actually make, and it must read as "you declared nothing",
        //     not as a declaration we guessed at: accepting it would mean inferring a claim, and inferring
        //     wrong is the silent lie this whole feature exists to prevent.
        CHECK(ThemeFormFactors::fit(QJsonValue(QStringLiteral("desktop")), QStringLiteral("desktop"))
              == Fit::Undeclared);
        CHECK(ThemeFormFactors::fit(QJsonValue(QJsonValue::Null), QStringLiteral("desktop")) == Fit::Undeclared);
        CHECK(ThemeFormFactors::fit(QJsonValue(QJsonObject()), QStringLiteral("desktop")) == Fit::Undeclared);

        // (c) An EMPTY array is a real declaration ("fits nothing"), NOT a missing one. This is the one shape
        //     where declaring the key is worse than omitting it, and it must not collapse into (a).
        CHECK(ThemeFormFactors::fit(arr({}), QStringLiteral("desktop")) == Fit::Unsupported);

        // (d) The shipped declaration matches all three modes FormFactor actually resolves. "mobile" and
        //     "tv" sit at indices 1 and 2, so an implementation that only ever tests the first entry — the
        //     natural off-by-one here — fails these and passes a single-element test.
        CHECK(ThemeFormFactors::fit(kShippedDecl, QStringLiteral("desktop")) == Fit::Supported);
        CHECK(ThemeFormFactors::fit(kShippedDecl, QStringLiteral("tv")) == Fit::Supported);
        CHECK(ThemeFormFactors::fit(kShippedDecl, QStringLiteral("mobile")) == Fit::Supported);

        // (e) A declaration that omits this device is Unsupported — the issue's motivating scenario, and the
        //     shape the bundled Night theme actually ships (["desktop"], seen from a TV). The Supported line
        //     is a deliberate CONTROL, not a duplicate of (d): without it, a fixture that failed to parse at
        //     all would make the Unsupported line below pass for entirely the wrong reason.
        CHECK(ThemeFormFactors::fit(arr({ "desktop" }), QStringLiteral("desktop")) == Fit::Supported);
        CHECK(ThemeFormFactors::fit(arr({ "desktop" }), QStringLiteral("tv")) == Fit::Unsupported);

        // (f) THE HANDHELD ANSWER (the issue's one genuine design question), pinned rather than implied.
        //     FormFactor resolves no handheld mode, so "handheld" is a label the app cannot check — a theme
        //     declaring ONLY it has named no device that exists, and reads Unsupported. That is the honest
        //     verdict: the author did not claim this device.
        CHECK(ThemeFormFactors::fit(arr({ "handheld" }), QStringLiteral("desktop")) == Fit::Unsupported);
        //     ...and the OTHER half of that answer: the match is a plain string compare with NO table of
        //     "modes we know", so the day a real FormFactor::Mode::Handheld lands and modeName() returns
        //     "handheld", every theme already declaring it starts matching with no change to this unit.
        //     An implementation that filtered labels against a hardcoded {desktop,tv,mobile} fails here.
        CHECK(ThemeFormFactors::fit(kShippedDecl, QStringLiteral("handheld")) == Fit::Supported);

        // (g) Entries are compared case- and whitespace-insensitively (authors hand-write this JSON)...
        CHECK(ThemeFormFactors::fit(arr({ "  DeSkToP " }), QStringLiteral("desktop")) == Fit::Supported);
        //     ...and so is the mode the caller passes in. FormFactor::modeName() is already lower-case, so
        //     this one is defence, not a live path — see the kill-matrix note.
        CHECK(ThemeFormFactors::fit(arr({ "desktop" }), QStringLiteral("  DESKTOP ")) == Fit::Supported);

        // (h) A non-string entry is ignored rather than aborting the scan: the real "desktop" after it still
        //     matches. QJsonValue::toString() yields an empty string for a number, which is why (i) matters.
        {
            QJsonArray mixed;
            mixed.append(3);
            mixed.append(QJsonValue(QJsonValue::Null));
            mixed.append(QStringLiteral("desktop"));
            CHECK(ThemeFormFactors::fit(QJsonValue(mixed), QStringLiteral("desktop")) == Fit::Supported);
            CHECK(ThemeFormFactors::fit(QJsonValue(mixed), QStringLiteral("tv")) == Fit::Unsupported);
        }

        // (i) THE TRIPWIRE: a caller that never resolved a mode passes an empty string. Blank entries are
        //     skipped, so that reads Unsupported (loud — every theme is flagged and someone notices) rather
        //     than Supported (silent — the feature quietly does nothing). Deliberate absence-of-behaviour.
        CHECK(ThemeFormFactors::fit(arr({ "" }), QString()) == Fit::Unsupported);
        CHECK(ThemeFormFactors::fit(kShippedDecl, QString()) == Fit::Unsupported);

        // (j) shortNote: a FITTING theme is decorated with nothing at all (a badge on every row is a badge on
        //     none), and the two non-fitting verdicts must never collapse into one sentence — "the author
        //     said no" and "nobody said" are different facts, which is the entire reason Fit has three states.
        CHECK(ThemeFormFactors::shortNote(Fit::Supported).isEmpty());
        CHECK(!ThemeFormFactors::shortNote(Fit::Unsupported).isEmpty());
        CHECK(!ThemeFormFactors::shortNote(Fit::Undeclared).isEmpty());
        CHECK(ThemeFormFactors::shortNote(Fit::Unsupported) != ThemeFormFactors::shortNote(Fit::Undeclared));
    }

    // ---- 7. THE INI-BACKED HALF: forProfile / setForProfile / runMigrationForIds -------------------
    // Hermetic: every case gets its own scratch ini under QDir::tempPath(), driven through
    // ThemeChoice::setIniPathForTesting, and all of them are deleted below. The app's real everythingbox.ini
    // is never opened — nothing here calls AppPaths::dataDir(), and the probe has no QCoreApplication.
    {
        const QString stem = QDir::tempPath() + QStringLiteral("/eb-probe-theme-%1-")
                                                    .arg(QCoreApplication::applicationPid());
        QStringList scratch;                     // every file made, for the cleanup assert at the end
        auto ini = [&](const char* tag) {
            const QString p = stem + QLatin1String(tag) + QStringLiteral(".ini");
            QFile::remove(p);                    // a previous run must never leak into this one
            scratch << p;
            ThemeChoice::setIniPathForTesting(p);
            return p;
        };
        auto seed = [](const QString& path, const QString& key, const QString& value) {
            QSettings s(path, QSettings::IniFormat); s.setValue(key, value); s.sync();
        };
        auto readBack = [](const QString& path, const QString& key) {
            QSettings s(path, QSettings::IniFormat); s.sync(); return s.value(key).toString();
        };
        auto has = [](const QString& path, const QString& key) {
            QSettings s(path, QSettings::IniFormat); s.sync(); return s.contains(key);
        };
        const QString kLegacy  = QStringLiteral("themedHome/theme");
        const QString kFlag    = QStringLiteral("device/themeChoiceMigrated");

        // (a) A legacy global with TWO profiles: both buckets written (with the rename applied), the global
        //     gone, the flag set.
        {
            const QString p = ini("a");
            seed(p, kLegacy, QStringLiteral("XMB"));
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1"), QStringLiteral("p2") }, kShipped);
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p1")) == QStringLiteral("Triple"));
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p2")) == QStringLiteral("Triple"));
            CHECK(has(p, kLegacy) == false);
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
            // ...and the accessor agrees with the file.
            CHECK(ThemeChoice::forProfile(QStringLiteral("p1")) == QStringLiteral("Triple"));
        }

        // (b) A legacy global with ZERO profiles — the install that never created a named profile, i.e. the
        //     COMMON case, since ProfileStore::list() returns empty until one is made. The value must land in
        //     the implicit ".../default" bucket. Before the fix the plan was empty here, so the global was
        //     fanned out to nothing and then DELETED, with the migrated flag guaranteeing no retry: the
        //     user's theme choice was gone irrecoverably.
        {
            const QString p = ini("b");
            seed(p, kLegacy, QStringLiteral("Channels"));
            ThemeChoice::runMigrationForIds({}, kShipped);
            CHECK(readBack(p, QStringLiteral("themedHome/theme/default")) == QStringLiteral("Channels"));
            CHECK(has(p, kLegacy) == false);
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
            CHECK(ThemeChoice::forProfile(QString()) == QStringLiteral("Channels"));
        }

        // (c) Running twice changes nothing — the flag short-circuits the second run, and even with the flag
        //     cleared the migration is naturally idempotent (the global is already gone, the bucket already
        //     holds the user's value, and nothing overwrites it).
        {
            const QString p = ini("c");
            seed(p, kLegacy, QStringLiteral("Channels"));
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1") }, kShipped);
            const QString after = readBack(p, QStringLiteral("themedHome/theme/p1"));
            CHECK(after == QStringLiteral("Channels"));

            ThemeChoice::runMigrationForIds({ QStringLiteral("p1") }, kShipped);         // flag-guarded
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p1")) == after);
            CHECK(has(p, kLegacy) == false);

            seed(p, kFlag, QStringLiteral("false"));                          // simulate a lost flag
            ThemeChoice::setIniPathForTesting(p);                             // re-read the file we just poked
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1") }, kShipped);
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p1")) == after);
            CHECK(has(p, kLegacy) == false);
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
        }

        // (d) setForProfile / forProfile round-trip, for a named id and for the empty (default) id.
        {
            const QString p = ini("d");
            CHECK(ThemeChoice::forProfile(QStringLiteral("p9")).isEmpty());     // unset reads empty
            ThemeChoice::setForProfile(QStringLiteral("p9"), QStringLiteral("Channels"));
            CHECK(ThemeChoice::forProfile(QStringLiteral("p9")) == QStringLiteral("Channels"));
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p9")) == QStringLiteral("Channels"));
            ThemeChoice::setForProfile(QString(), QStringLiteral("Triple"));
            CHECK(ThemeChoice::forProfile(QString()) == QStringLiteral("Triple"));
            CHECK(readBack(p, QStringLiteral("themedHome/theme/default")) == QStringLiteral("Triple"));
            // A stored value is a stored value: no pick owed, whatever this device has installed.
            CHECK(ThemeChoice::needsPick(ThemeChoice::forProfile(QStringLiteral("p9"))) == false);
        }

        // (e) An existing per-profile value SURVIVES the migration that removes the legacy global. The two
        //     live in the same ini group ("themedHome/theme" is both a scalar and the buckets' prefix), so
        //     this is the assertion that keeps the removal from taking the buckets with it.
        {
            const QString p = ini("e");
            seed(p, kLegacy, QStringLiteral("Channels"));
            seed(p, QStringLiteral("themedHome/theme/p1"), QStringLiteral("Grid"));
            ThemeChoice::setIniPathForTesting(p);
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1"), QStringLiteral("p2") }, kShipped);
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p1")) == QStringLiteral("Grid"));   // kept
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p2")) == QStringLiteral("Channels"));
            CHECK(has(p, kLegacy) == false);
        }

        // (f) THE CLOUD-RESTORE CASE: profiles EXIST and the default bucket still has to be migrated.
        //     `profiles/current` is device-local (CloudSync.cpp:501) while `profiles/list` SYNCS, so a device
        //     restored from the cloud legitimately has profiles with an EMPTY currentId() — every read then
        //     goes to ".../default". If the default bucket is only seeded when the profile list is empty, that
        //     bucket stays unwritten here, forProfile("") returns empty, and the user who already picked a
        //     theme is forced to pick again. The default bucket must be covered ALONGSIDE the real profiles.
        {
            const QString p = ini("f");
            seed(p, kLegacy, QStringLiteral("Channels"));
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1"), QStringLiteral("p2") }, kShipped);
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p1")) == QStringLiteral("Channels"));
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p2")) == QStringLiteral("Channels"));
            CHECK(readBack(p, QStringLiteral("themedHome/theme/default")) == QStringLiteral("Channels"));
            CHECK(has(p, kLegacy) == false);
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
            CHECK(ThemeChoice::forProfile(QString()) == QStringLiteral("Channels"));
        }

        // (g) THE UPGRADE WHO NEVER SET A THEME, with "Default" still on disk. No legacy global, so
        //     planMigration alone would write nothing — and resolve("") prefers "Triple", so their home would
        //     silently change appearance on update. AssetBootstrap is additive, so "Default" IS still installed
        //     for them: the migration must seed it. Profiles EXIST here, which is the upgrade discriminator.
        {
            const QString p = ini("g");
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1") }, kUpgraded);
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p1")) == QStringLiteral("Default"));
            CHECK(readBack(p, QStringLiteral("themedHome/theme/default")) == QStringLiteral("Default"));
            CHECK(ThemeChoice::needsPick(ThemeChoice::forProfile(QStringLiteral("p1"))) == false);
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
        }

        // (h) The SAME upgrade, but "Default" is NOT installed (they deleted it, or a mobile install whose
        //     assets never carried it). Seeding a folder that is nowhere on disk would store a choice the user
        //     never made and rob them of the pick, so nothing is written and needsPick stays true.
        {
            const QString p = ini("h");
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1") }, kShipped);
            CHECK(has(p, QStringLiteral("themedHome/theme/p1")) == false);
            CHECK(has(p, QStringLiteral("themedHome/theme/default")) == false);
            CHECK(ThemeChoice::needsPick(ThemeChoice::forProfile(QStringLiteral("p1"))) == true);
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
        }

        // (i) A genuinely FRESH install — NO profiles yet (the ctor migration runs before main.cpp's startup
        //     picker can create one) — must NOT inherit "Default" even though it is sitting there installed.
        //     Nothing has been chosen on this device, so the forced pick is the correct outcome.
        {
            const QString p = ini("i");
            ThemeChoice::runMigrationForIds({}, kUpgraded);
            CHECK(has(p, QStringLiteral("themedHome/theme/default")) == false);
            CHECK(ThemeChoice::needsPick(ThemeChoice::forProfile(QString())) == true);
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
        }

        // (j) An explicit legacy global still beats the "Default" preservation — the user's own choice wins
        //     over the pre-#57 hardcoded fallback, whatever is installed.
        {
            const QString p = ini("j");
            seed(p, kLegacy, QStringLiteral("Channels"));
            ThemeChoice::setIniPathForTesting(p);
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1") }, kUpgraded);
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p1")) == QStringLiteral("Channels"));
            CHECK(has(p, kLegacy) == false);
        }

        // (k) THE CLOUD-RESTORE RE-RUN (rerunMigrationAfterRestore). The device migrated at startup against an
        //     empty ini and set the flag; the flag is device-local, so the bundle that lands afterwards — here
        //     an OLD-version device's synced legacy scalar plus its profile, and no per-profile keys — can
        //     never clear it and would never be migrated. The re-run must carry the value, drop the global, and
        //     leave a per-profile key the bundle DID supply alone.
        {
            const QString p = ini("k");
            ThemeChoice::runMigrationForIds({}, kShipped);                 // startup, empty ini -> flag set
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
            seed(p, kLegacy, QStringLiteral("Grid"));                      // ...then the bundle lands
            seed(p, QStringLiteral("themedHome/theme/p2"), QStringLiteral("Channels")); // bundle's own per-profile key
            ThemeChoice::setIniPathForTesting(p);
            // rerunMigrationAfterRestore() reads ProfileStore, which this hermetic probe cannot seed — drive
            // the same two steps it performs (clear the flag, migrate over the restored ids) directly.
            { QSettings s(p, QSettings::IniFormat); s.remove(kFlag); s.sync(); }
            ThemeChoice::setIniPathForTesting(p);
            ThemeChoice::runMigrationForIds({ QStringLiteral("p1"), QStringLiteral("p2") }, kShipped);
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p1")) == QStringLiteral("Grid"));    // carried
            CHECK(readBack(p, QStringLiteral("themedHome/theme/p2")) == QStringLiteral("Channels"));// NOT clobbered
            CHECK(has(p, kLegacy) == false);                                // the dead global stops syncing onward
            CHECK(readBack(p, kFlag) == QStringLiteral("true"));
        }

        // NOTE: the old duplicate-id case is gone with the "covered" guard it existed for. Duplicates could only
        // ever wedge that guard (a deduped `existing` reading as UNcovered); with the guard removed a repeated
        // id just writes the same bucket twice, so `ids.removeDuplicates()` guarded nothing real either and both
        // are gone. Keeping the test would have been keeping a test to justify dead code.

        // Back to production, then wipe every scratch file: this probe leaves NOTHING on disk.
        ThemeChoice::setIniPathForTesting(QString());
        for (const QString& p : scratch)
        {
            QFile::remove(p);
            CHECK(QFile::exists(p) == false);
        }
    }

    if (failures == 0) { std::puts("THEME-OK"); return 0; }
    std::fprintf(stderr, "THEME: %d check(s) failed\n", failures);
    return 1;
}
