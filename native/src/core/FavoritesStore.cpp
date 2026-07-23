#include "FavoritesStore.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "RecentStore.h"
#include "DownloadsStore.h"
#include "Tombstones.h"

#include <QSettings>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, so each user has their own favourites.
static QString favKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("favorites/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/items");
}

// Tombstone store namespace for THIS profile's favourites (mirrors favKey()'s per-profile shape). Keyed by
// itemId — see Tombstones.h.
static QString favTombstoneStore()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("favorites/") + (id.isEmpty() ? QStringLiteral("default") : id);
}

static void save(const QVector<FavoriteItem>& items);

QVector<FavoriteItem> FavoritesStore::list()
{
    QVector<FavoriteItem> out;
    const QByteArray json = store().value(favKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        FavoriteItem it;
        it.addonId      = o.value(QStringLiteral("addonId")).toString();
        it.itemId       = o.value(QStringLiteral("itemId")).toString();
        it.title        = o.value(QStringLiteral("title")).toString();
        it.subtitle     = o.value(QStringLiteral("subtitle")).toString();
        it.type         = o.value(QStringLiteral("type")).toString();
        it.thumbnailUrl = o.value(QStringLiteral("thumbnailUrl")).toString();
        it.expandable   = o.value(QStringLiteral("expandable")).toBool();
        it.path         = o.value(QStringLiteral("path")).toString();
        it.kind         = o.value(QStringLiteral("kind")).toString();
        it.system       = o.value(QStringLiteral("system")).toString();
        it.ts           = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        if (!it.itemId.isEmpty()) out.push_back(it);
    }
    // One-time (per profile per run) migration: local-game favourites saved before `system` was stamped
    // never matched a console's ★ Favorites folder — recover it from the game's Recent/Downloads entry
    // (authoritative for ambiguous extensions), else the ROM extension, and persist.
    static QSet<QString> migrated;
    if (!migrated.contains(favKey()))
    {
        migrated.insert(favKey());
        auto storeSystem = [](const FavoriteItem& f) -> QString {
            for (const DownloadedItem& d : DownloadsStore::list())
                if (d.path == f.path || (!f.itemId.isEmpty() && d.key == f.itemId)) return d.system;
            for (const RecentItem& r : RecentStore::list())
                if (r.path == f.path || (!f.itemId.isEmpty() && r.key == f.itemId)) return r.system;
            return QString();
        };
        if (backfillSystems(out, storeSystem)) save(out);
    }
    return out;
}

static void save(const QVector<FavoriteItem>& items)
{
    // The write funnel stamps a timestamp per favourite: a fresh add (ts==0) is stamped now; an already-timed
    // favourite keeps its stamp across rewrites (add/remove of OTHER items must not bump it). This is the ts
    // the multi-device merge (T2) uses to pick the newest occurrence of an id across devices.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QJsonArray arr;
    for (const FavoriteItem& it : items)
    {
        QJsonObject o;
        o.insert(QStringLiteral("addonId"), it.addonId);
        o.insert(QStringLiteral("itemId"), it.itemId);
        o.insert(QStringLiteral("title"), it.title);
        o.insert(QStringLiteral("subtitle"), it.subtitle);
        o.insert(QStringLiteral("type"), it.type);
        o.insert(QStringLiteral("thumbnailUrl"), it.thumbnailUrl);
        o.insert(QStringLiteral("expandable"), it.expandable);
        if (!it.path.isEmpty()) o.insert(QStringLiteral("path"), it.path);
        if (!it.kind.isEmpty()) o.insert(QStringLiteral("kind"), it.kind);
        if (!it.system.isEmpty()) o.insert(QStringLiteral("system"), it.system);
        o.insert(QStringLiteral("ts"), static_cast<double>(it.ts > 0 ? it.ts : now));
        arr.append(o);
    }
    store().setValue(favKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

void FavoritesStore::add(const FavoriteItem& item)
{
    if (item.itemId.isEmpty()) return;
    QVector<FavoriteItem> items = list();
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].itemId == item.itemId) items.remove(i); // de-dup
    items.prepend(item);                                     // newest first
    save(items);
}

void FavoritesStore::remove(const QString& itemId)
{
    if (itemId.isEmpty()) return;
    QVector<FavoriteItem> items = list();
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].itemId == itemId) items.remove(i);
    save(items);
    // Tombstone the un-starred id so the merge (T2) doesn't resurrect it from a device that still has it.
    Tombstones::record(favTombstoneStore(), itemId);
}

QSet<QString> FavoritesStore::allKeys()
{
    QSet<QString> keys;
    QSettings& s = store();
    s.beginGroup(QStringLiteral("favorites"));
    const QStringList profiles = s.childGroups();
    for (const QString& profile : profiles)
    {
        const QByteArray json = s.value(profile + QStringLiteral("/items")).toString().toUtf8();
        for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
        {
            const QJsonObject o = v.toObject();
            const QString id   = o.value(QStringLiteral("itemId")).toString();
            const QString path = o.value(QStringLiteral("path")).toString();
            if (!id.isEmpty())   keys.insert(id);
            if (!path.isEmpty()) keys.insert(path);
        }
    }
    s.endGroup();
    return keys;
}

bool FavoritesStore::isFavorite(const QString& itemId)
{
    for (const FavoriteItem& it : list())
        if (it.itemId == itemId) return true;
    return false;
}
