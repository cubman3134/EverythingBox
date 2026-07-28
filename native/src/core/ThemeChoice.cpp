#include "ThemeChoice.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>

// Test-only redirect (see ThemeChoice.h). Empty means "production", which is the ONLY state the app ever
// sees — the check below costs one isEmpty() and leaves the production static exactly as it was.
static QString    gTestIniPath;
static QSettings* gTestStore = nullptr;

void ThemeChoice::setIniPathForTesting(const QString& path)
{
    delete gTestStore;              // reopen on the next store() call, so a re-seeded scratch file is re-read
    gTestStore   = nullptr;
    gTestIniPath = path;
}

// The file-local store() idiom every other core unit uses (ProfileStore.cpp:14, CloudMerge.cpp:21) — the
// shared portable ini, opened once. Keeping it local is what lets this TU stay QtCore-only.
static QSettings& store()
{
    if (!gTestIniPath.isEmpty())
    {
        if (!gTestStore) gTestStore = new QSettings(gTestIniPath, QSettings::IniFormat);
        return *gTestStore;
    }
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

void ThemeChoice::runMigration()
{
    QStringList ids;
    for (const Profile& p : ProfileStore::list()) ids << p.id;
    runMigrationForIds(ids);
}

void ThemeChoice::runMigrationForIds(const QStringList& profileIds)
{
    if (store().value(QLatin1String(kMigratedFlag), false).toBool()) return;

    // The implicit DEFAULT bucket. ProfileStore::list() is EMPTY on any install that never created a named
    // profile — the common case, not an edge case — and that install's theme lives at ".../default", the
    // bucket keyFor("") produces and the one ThemeStore::currentName() uses (Theme.cpp:116). Without this,
    // an empty id list made the plan empty, so the legacy value was fanned out to NOTHING and then deleted.
    QStringList ids = profileIds;
    if (ids.isEmpty()) ids << QString();

    QHash<QString, QString> existing;
    for (const QString& id : ids)
    {
        const QString v = store().value(keyFor(id)).toString();
        if (!v.isEmpty()) existing.insert(id, v);
    }

    const QString legacyGlobal = store().value(QLatin1String(kLegacyGlobalKey)).toString();
    const QHash<QString, QString> plan = planMigration(legacyGlobal, ids, existing);
    for (auto it = plan.constBegin(); it != plan.constEnd(); ++it)
        store().setValue(keyFor(it.key()), it.value());

    // Did the legacy value actually land somewhere? Either the plan wrote at least one bucket, or every
    // bucket already held its own value and there was nothing to carry. Anything else means the fan-out
    // covered NO bucket — so leave the global in place and leave the flag UNSET, and the next launch retries.
    // Deleting it there would destroy the user's choice permanently, with the flag guaranteeing no retry.
    const bool covered = !plan.isEmpty() || (!ids.isEmpty() && existing.size() == ids.size());
    if (!legacyGlobal.isEmpty() && !covered) { store().sync(); return; }

    // The global is gone once its value has been fanned out. Removing it is what makes a later accidental
    // read of the legacy key fail loudly instead of silently serving a stale device-wide theme.
    //
    // But the removal cannot be a bare remove(): the legacy key is a SCALAR whose name is also the buckets'
    // GROUP ("themedHome/theme" vs "themedHome/theme/<id>"), and QSettings::remove(k) deletes k AND every
    // key beneath k. A bare remove() therefore deletes every per-profile value — including the ones this
    // migration just wrote, and any the user already had. Snapshot the group, remove, put it back. This is
    // load-bearing; probe_theme section 7 (a)/(b)/(e) fail without it.
    QHash<QString, QString> buckets;
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
