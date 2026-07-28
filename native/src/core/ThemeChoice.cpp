#include "ThemeChoice.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>

// The file-local store() idiom every other core unit uses (ProfileStore.cpp:14, CloudMerge.cpp:21) — the
// shared portable ini, opened once. Keeping it local is what lets this TU stay QtCore-only.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString ThemeChoice::keyFor(const QString& profileId)
{
    return QLatin1String(kLegacyGlobalKey) + QStringLiteral("/")
         + (profileId.isEmpty() ? QStringLiteral("default") : profileId);
}

QString ThemeChoice::renameLegacyFolder(const QString& stored)
{
    return stored == QLatin1String(kRenamedFrom) ? QString::fromLatin1(kFallbackTheme) : stored;
}

bool ThemeChoice::needsPick(const QString& stored, const QStringList& installed)
{
    return stored.isEmpty() || !installed.contains(stored);
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
    if (store().value(QLatin1String(kMigratedFlag), false).toBool()) return;

    QStringList ids;
    QHash<QString, QString> existing;
    for (const Profile& p : ProfileStore::list())
    {
        ids << p.id;
        const QString v = store().value(keyFor(p.id)).toString();
        if (!v.isEmpty()) existing.insert(p.id, v);
    }

    const QString legacyGlobal = store().value(QLatin1String(kLegacyGlobalKey)).toString();
    const QHash<QString, QString> plan = planMigration(legacyGlobal, ids, existing);
    for (auto it = plan.constBegin(); it != plan.constEnd(); ++it)
        store().setValue(keyFor(it.key()), it.value());

    // The global is gone once its value has been fanned out. Removing it is what makes a later accidental
    // read of the legacy key fail loudly instead of silently serving a stale device-wide theme.
    store().remove(QLatin1String(kLegacyGlobalKey));
    store().setValue(QLatin1String(kMigratedFlag), true);
    store().sync();
}
