#include "FavoritesStore.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

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
        if (!it.itemId.isEmpty()) out.push_back(it);
    }
    return out;
}

static void save(const QVector<FavoriteItem>& items)
{
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
    QVector<FavoriteItem> items = list();
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].itemId == itemId) items.remove(i);
    save(items);
}

bool FavoritesStore::isFavorite(const QString& itemId)
{
    for (const FavoriteItem& it : list())
        if (it.itemId == itemId) return true;
    return false;
}
