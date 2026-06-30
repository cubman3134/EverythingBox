#include "PlaylistStore.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
    return s;
}

// Per-profile, so each user has their own playlists. All of a profile's playlists (across every catalogue)
// live in one JSON array; forCatalog() filters by catalogKey.
static QString plKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("playlists/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/items");
}

static PlaylistEntry entryFromJson(const QJsonObject& o)
{
    PlaylistEntry e;
    e.addonId      = o.value(QStringLiteral("addonId")).toString();
    e.itemId       = o.value(QStringLiteral("itemId")).toString();
    e.title        = o.value(QStringLiteral("title")).toString();
    e.subtitle     = o.value(QStringLiteral("subtitle")).toString();
    e.type         = o.value(QStringLiteral("type")).toString();
    e.thumbnailUrl = o.value(QStringLiteral("thumbnailUrl")).toString();
    e.expandable   = o.value(QStringLiteral("expandable")).toBool();
    return e;
}

static QJsonObject entryToJson(const PlaylistEntry& e)
{
    QJsonObject o;
    o.insert(QStringLiteral("addonId"), e.addonId);
    o.insert(QStringLiteral("itemId"), e.itemId);
    o.insert(QStringLiteral("title"), e.title);
    o.insert(QStringLiteral("subtitle"), e.subtitle);
    o.insert(QStringLiteral("type"), e.type);
    o.insert(QStringLiteral("thumbnailUrl"), e.thumbnailUrl);
    o.insert(QStringLiteral("expandable"), e.expandable);
    return o;
}

static QVector<Playlist> loadAll()
{
    QVector<Playlist> out;
    const QByteArray json = store().value(plKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Playlist p;
        p.id         = o.value(QStringLiteral("id")).toString();
        p.catalogKey = o.value(QStringLiteral("catalogKey")).toString();
        p.name       = o.value(QStringLiteral("name")).toString();
        for (const QJsonValue& iv : o.value(QStringLiteral("items")).toArray())
            if (iv.isObject()) p.items.push_back(entryFromJson(iv.toObject()));
        if (!p.id.isEmpty()) out.push_back(p);
    }
    return out;
}

static void saveAll(const QVector<Playlist>& all)
{
    QJsonArray arr;
    for (const Playlist& p : all)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), p.id);
        o.insert(QStringLiteral("catalogKey"), p.catalogKey);
        o.insert(QStringLiteral("name"), p.name);
        QJsonArray items;
        for (const PlaylistEntry& e : p.items) items.append(entryToJson(e));
        o.insert(QStringLiteral("items"), items);
        arr.append(o);
    }
    store().setValue(plKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

QVector<Playlist> PlaylistStore::forCatalog(const QString& catalogKey)
{
    QVector<Playlist> out;
    for (const Playlist& p : loadAll())
        if (p.catalogKey == catalogKey) out.push_back(p);
    return out;
}

bool PlaylistStore::get(const QString& id, Playlist& out)
{
    for (const Playlist& p : loadAll())
        if (p.id == id) { out = p; return true; }
    return false;
}

QString PlaylistStore::create(const QString& catalogKey, const QString& name)
{
    QVector<Playlist> all = loadAll();
    Playlist p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.catalogKey = catalogKey;
    p.name = name;
    all.push_back(p);
    saveAll(all);
    return p.id;
}

void PlaylistStore::rename(const QString& id, const QString& name)
{
    QVector<Playlist> all = loadAll();
    for (Playlist& p : all) if (p.id == id) { p.name = name; break; }
    saveAll(all);
}

void PlaylistStore::remove(const QString& id)
{
    QVector<Playlist> all = loadAll();
    for (int i = 0; i < all.size(); ++i) if (all[i].id == id) { all.remove(i); break; }
    saveAll(all);
}

void PlaylistStore::addItem(const QString& id, const PlaylistEntry& item)
{
    QVector<Playlist> all = loadAll();
    for (Playlist& p : all)
        if (p.id == id)
        {
            for (const PlaylistEntry& e : p.items) if (e.itemId == item.itemId) return; // already in it
            p.items.push_back(item);
            break;
        }
    saveAll(all);
}

void PlaylistStore::removeItem(const QString& id, const QString& itemId)
{
    QVector<Playlist> all = loadAll();
    for (Playlist& p : all)
        if (p.id == id)
        {
            for (int i = 0; i < p.items.size(); ++i)
                if (p.items[i].itemId == itemId) { p.items.remove(i); break; }
            break;
        }
    saveAll(all);
}

bool PlaylistStore::contains(const QString& id, const QString& itemId)
{
    Playlist p;
    if (!get(id, p)) return false;
    for (const PlaylistEntry& e : p.items) if (e.itemId == itemId) return true;
    return false;
}
