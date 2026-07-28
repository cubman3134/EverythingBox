#include "ThemeChoice.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>

// Test-only redirect (see ThemeChoice.h). The whole seam — the statics, the setter, and the branch in
// store() — is compiled ONLY for probe_theme, which is the only target that defines the macro. In the app
// build none of it exists, so store() is byte-for-byte the plain production static it always was and there is
// no way, accidental or otherwise, to point this unit at another file.
#ifdef EB_THEMECHOICE_TEST_SEAM
static QString    gTestIniPath;
static QSettings* gTestStore = nullptr;

void ThemeChoice::setIniPathForTesting(const QString& path)
{
    delete gTestStore;              // reopen on the next store() call, so a re-seeded scratch file is re-read
    gTestStore   = nullptr;
    gTestIniPath = path;
}
#endif

// The file-local store() idiom every other core unit uses (ProfileStore.cpp:14, CloudMerge.cpp:21) — the
// shared portable ini, opened once. Keeping it local is what lets this TU stay QtCore-only.
static QSettings& store()
{
#ifdef EB_THEMECHOICE_TEST_SEAM
    if (!gTestIniPath.isEmpty())
    {
        if (!gTestStore) gTestStore = new QSettings(gTestIniPath, QSettings::IniFormat);
        return *gTestStore;
    }
#endif
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString ThemeChoice::keyFor(const QString& profileId)
{
    return QLatin1String(kKeyBase) + QStringLiteral("/")
         + (profileId.isEmpty() ? QStringLiteral("default") : profileId);
}

QString ThemeChoice::renameLegacyFolder(const QString& stored)
{
    return stored == QLatin1String(kRenamedFrom) ? QString::fromLatin1(kFallbackTheme) : stored;
}

QString ThemeChoice::legacyEffectiveGlobal(const QString& legacyGlobal, bool isUpgrade,
                                           const QStringList& installed)
{
    if (!legacyGlobal.isEmpty()) return legacyGlobal;   // an explicit choice always wins
    if (!isUpgrade)              return QString();      // fresh install: nothing to preserve — take the pick
    // Upgrade with no stored global: pre-#57 they were rendering the hardcoded "Default". Preserve it, but
    // only if it is really installed — seeding a folder that isn't on disk would store a choice they never
    // made AND skip the pick, leaving them on resolve()'s fallback with no way to notice.
    const QString legacyDefault = QString::fromLatin1(kLegacyDefaultTheme);
    return installed.contains(legacyDefault) ? legacyDefault : QString();
}

bool ThemeChoice::needsPick(const QString& stored)
{
    // Stored-but-not-installed is NOT a pick — this key syncs, so re-asking here would overwrite the other
    // device's answer. resolve() covers the missing folder. See the header for the full reasoning.
    return stored.isEmpty();
}

QString ThemeChoice::resolve(const QString& stored, const QStringList& installed)
{
    if (!stored.isEmpty() && installed.contains(stored)) return stored;
    const QString fallback = QString::fromLatin1(kFallbackTheme);
    if (installed.contains(fallback)) return fallback;
    return installed.value(0);   // empty when nothing is installed
}

QHash<QString, QString> ThemeChoice::planMigration(const QString& legacyGlobal,
                                                   const QStringList& profileIds,
                                                   const QHash<QString, QString>& existing)
{
    QHash<QString, QString> out;
    const QString global = renameLegacyFolder(legacyGlobal);
    for (const QString& id : profileIds)
    {
        const QString have = existing.value(id);
        if (!have.isEmpty())
        {
            // An existing choice is authoritative — the global never overwrites it. It only needs writing
            // back if the folder rename changed it.
            const QString renamed = renameLegacyFolder(have);
            if (renamed != have) out.insert(id, renamed);
            continue;
        }
        // No per-profile value: inherit the legacy global, if there was one. If there wasn't, write nothing
        // — needsPick then stays true and this profile gets the forced pick, which is correct for a genuinely
        // fresh install.
        if (!global.isEmpty()) out.insert(id, global);
    }
    return out;
}

QString ThemeChoice::forProfile(const QString& profileId)
{
    return store().value(keyFor(profileId)).toString();
}

void ThemeChoice::setForProfile(const QString& profileId, const QString& folder)
{
    store().setValue(keyFor(profileId), folder);
    store().sync();
}

void ThemeChoice::runMigration(const QStringList& installedThemes)
{
    QStringList ids;
    for (const Profile& p : ProfileStore::list()) ids << p.id;
    runMigrationForIds(ids, installedThemes);
}

void ThemeChoice::rerunMigrationAfterRestore(const QStringList& installedThemes)
{
    // The bundle was written through CloudSync's own QSettings on the same file; re-read before deciding.
    store().sync();
    store().remove(QLatin1String(kMigratedFlag));
    runMigration(installedThemes);
}

void ThemeChoice::runMigrationForIds(const QStringList& profileIds, const QStringList& installedThemes)
{
    if (store().value(QLatin1String(kMigratedFlag), false).toBool()) return;

    // The implicit DEFAULT bucket, seeded UNCONDITIONALLY — alongside any real profiles, not only when there
    // are none. Two independent reasons, and only the first is about the empty list:
    //   * ProfileStore::list() is EMPTY on any install that never created a named profile — the common case,
    //     not an edge case — and that install's theme lives at ".../default", the bucket keyFor("") produces
    //     and the one ThemeStore::currentName() uses (Theme.cpp:116). Without this, an empty id list made the
    //     plan empty, so the legacy value was fanned out to NOTHING and then deleted.
    //   * A device with profiles can STILL read from ".../default": `profiles/current` is device-local
    //     (CloudSync.cpp:501) while `profiles/list` SYNCS, so a cloud-restored device legitimately has
    //     profiles and an empty ProfileStore::currentId(). Seeding the default bucket only when the list is
    //     empty leaves that device's every read unset, and Task 4 then forces a pick on a user who already
    //     made one. The default bucket is a real bucket on every install; migrate it like any other.
    QStringList ids = profileIds;
    ids << QString();

    QHash<QString, QString> existing;
    for (const QString& id : ids)
    {
        const QString v = store().value(keyFor(id)).toString();
        if (!v.isEmpty()) existing.insert(id, v);
    }

    // UPGRADE vs. FRESH INSTALL is decided by "does this install already have profiles?", because the ONLY
    // caller (MainWindow's ctor) runs this before any profile can exist on a fresh install: main.cpp's startup
    // picker is presented from showEvent, i.e. after the ctor, so a first-run ini reaches here with an empty
    // ProfileStore::list(). An upgrade, by construction, already made its profile on a previous launch. There
    // is no other signal — the ini is otherwise indistinguishable, and a "was any key ever written?" probe
    // would be fooled by the keys main.cpp's own startup writes before us.
    const QString legacyGlobal = legacyEffectiveGlobal(store().value(QLatin1String(kLegacyGlobalKey)).toString(),
                                                       /*isUpgrade*/ !profileIds.isEmpty(), installedThemes);
    const QHash<QString, QString> plan = planMigration(legacyGlobal, ids, existing);
    for (auto it = plan.constBegin(); it != plan.constEnd(); ++it)
        store().setValue(keyFor(it.key()), it.value());

    // The global is gone once its value has been fanned out. Removing it is what makes a later accidental
    // read of the legacy key fail loudly instead of silently serving a stale device-wide theme.
    //
    // But the removal cannot be a bare remove(): the legacy key is a SCALAR whose name is also the buckets'
    // GROUP ("themedHome/theme" vs "themedHome/theme/<id>"), and QSettings::remove(k) deletes k AND every
    // key beneath k. A bare remove() therefore deletes every per-profile value — including the ones this
    // migration just wrote, and any the user already had. Snapshot the group, remove, put it back. This is
    // load-bearing; probe_theme section 7 (a)/(b)/(e) fail without it.
    QHash<QString, QString> buckets;
    // childKeys() is DIRECT children only — safe solely because a profile id is a brace-less UUID
    // (ProfileStore.cpp:139) and so can never contain a '/'; an id that did would sit in a nested group, be
    // missed by this snapshot, and be destroyed by the remove() below.
    store().beginGroup(QLatin1String(kKeyBase));
    for (const QString& child : store().childKeys()) buckets.insert(child, store().value(child).toString());
    store().endGroup();

    store().remove(QLatin1String(kLegacyGlobalKey));

    store().beginGroup(QLatin1String(kKeyBase));
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it)
        store().setValue(it.key(), it.value());
    store().endGroup();

    store().setValue(QLatin1String(kMigratedFlag), true);
    store().sync();
}
