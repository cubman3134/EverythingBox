#include "PlaylistStore.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "MediaCategories.h"
#include "Tombstones.h"

#include <QSettings>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

// Schema versions of the per-profile playlist blob: 1 = legacy per-catalogue (catalogKey), 2 = per-category
// (categoryKey + preserved legacyKey). migrateToCategories() folds 1 -> 2 once, stamped per profile.
static constexpr int kSchemaVersion = 2;

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
    return s;
}

// Per-profile, so each user has their own playlists. All of a profile's playlists (across every category)
// live in one JSON array; forCategory() filters by categoryKey.
static QString plKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("playlists/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/items");
}

// Per-profile migration stamp: once it reaches kSchemaVersion this profile's playlists are in category shape.
static QString schemaKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("playlists/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/schema");
}

// Tombstone store namespace for THIS profile's playlists (mirrors plKey()'s per-profile shape). Keyed by
// playlist id — see Tombstones.h.
static QString plTombstoneStore()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("playlists/") + (id.isEmpty() ? QStringLiteral("default") : id);
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
    e.path         = o.value(QStringLiteral("path")).toString();
    e.kind         = o.value(QStringLiteral("kind")).toString();
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
    if (!e.path.isEmpty()) o.insert(QStringLiteral("path"), e.path);
    if (!e.kind.isEmpty()) o.insert(QStringLiteral("kind"), e.kind);
    return o;
}

// Reads the profile's playlists exactly as stored, tolerating both shapes: a v2 blob has categoryKey (+
// optional legacyKey); a v1 blob has only catalogKey, which we fold into legacyKey here so the migrator has
// the old key to derive from. categoryKey stays empty for an un-migrated v1 playlist (the migrator fills it).
static QVector<Playlist> loadAllRaw()
{
    QVector<Playlist> out;
    const QByteArray json = store().value(plKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Playlist p;
        p.id          = o.value(QStringLiteral("id")).toString();
        p.categoryKey = o.value(QStringLiteral("categoryKey")).toString();
        p.legacyKey   = o.value(QStringLiteral("legacyKey")).toString();
        if (p.legacyKey.isEmpty()) p.legacyKey = o.value(QStringLiteral("catalogKey")).toString(); // v1 field
        p.name        = o.value(QStringLiteral("name")).toString();
        p.updatedAt   = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
        for (const QJsonValue& iv : o.value(QStringLiteral("items")).toArray())
            if (iv.isObject()) p.items.push_back(entryFromJson(iv.toObject()));
        if (!p.id.isEmpty()) out.push_back(p);
    }
    return out;
}

// Change-callback (mdsync T2): fired after a mutation to (re)arm the debounced Drive push; null in probes.
static std::function<void()> g_changeHook;
void PlaylistStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

static void saveAll(const QVector<Playlist>& all)
{
    QJsonArray arr;
    for (const Playlist& p : all)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), p.id);
        o.insert(QStringLiteral("categoryKey"), p.categoryKey);
        if (!p.legacyKey.isEmpty()) o.insert(QStringLiteral("legacyKey"), p.legacyKey); // provenance, if any
        o.insert(QStringLiteral("name"), p.name);
        o.insert(QStringLiteral("updatedAt"), static_cast<double>(p.updatedAt));
        QJsonArray items;
        for (const PlaylistEntry& e : p.items) items.append(entryToJson(e));
        o.insert(QStringLiteral("items"), items);
        arr.append(o);
    }
    store().setValue(plKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged(); // every playlist mutation funnels through here; (re)arm the debounced Drive push
}

bool PlaylistStore::migrateToCategories()
{
    if (store().value(schemaKey()).toInt() >= kSchemaVersion) return false; // already migrated for this profile
    QVector<Playlist> all = loadAllRaw();
    bool changed = false;
    for (Playlist& p : all)
        if (p.categoryKey.isEmpty()) // an un-migrated v1 playlist: derive from the legacy catalogKey's type
        {
            const QString type = p.legacyKey.section(QLatin1Char('|'), 2, 2); // "addonId|catalogId|catalogType"
            p.categoryKey = core::mediaCategory(type); // unknown/empty type -> "video" (the oracle's fallback)
            changed = true;
        }
    if (changed) saveAll(all); // rewrite in v2 shape (categoryKey + preserved legacyKey; ids/items untouched)
    // A failed write must NOT stamp the schema version: stamping a lost migration would mark this profile
    // "done" while its playlists are still in v1 shape (and no later run would retry). Only stamp when the
    // store is healthy — an errored save leaves the profile un-stamped so the next access migrates again.
    if (store().status() != QSettings::NoError) return false;
    store().setValue(schemaKey(), kSchemaVersion);
    store().sync();
    return changed;
}

// Every public accessor migrates first (cheap once stamped), so callers always see category-shaped playlists.
static QVector<Playlist> loadAll()
{
    PlaylistStore::migrateToCategories();
    return loadAllRaw();
}

QVector<Playlist> PlaylistStore::forCategory(const QString& categoryKey)
{
    QVector<Playlist> out;
    for (const Playlist& p : loadAll())
        if (p.categoryKey == categoryKey) out.push_back(p);
    return out;
}

bool PlaylistStore::get(const QString& id, Playlist& out)
{
    for (const Playlist& p : loadAll())
        if (p.id == id) { out = p; return true; }
    return false;
}

// Every mutator stamps the affected playlist's updatedAt (the merge clock for whole-object newest-wins) and
// persists through the single setValue funnel (saveAll -> plKey). remove() has no surviving object to stamp,
// so it records a tombstone instead.
static qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

QString PlaylistStore::create(const QString& categoryKey, const QString& name)
{
    QVector<Playlist> all = loadAll();
    Playlist p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.categoryKey = categoryKey;
    p.name = name;
    p.updatedAt = nowSecs();
    all.push_back(p);
    saveAll(all);
    return p.id;
}

void PlaylistStore::rename(const QString& id, const QString& name)
{
    QVector<Playlist> all = loadAll();
    for (Playlist& p : all) if (p.id == id) { p.name = name; p.updatedAt = nowSecs(); break; }
    saveAll(all);
}

void PlaylistStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QVector<Playlist> all = loadAll();
    for (int i = 0; i < all.size(); ++i) if (all[i].id == id) { all.remove(i); break; }
    saveAll(all);
    // Tombstone the deleted playlist so the merge (T2) doesn't resurrect it from a device that still has it.
    Tombstones::record(plTombstoneStore(), id);
}

void PlaylistStore::addItem(const QString& id, const PlaylistEntry& item)
{
    QVector<Playlist> all = loadAll();
    for (Playlist& p : all)
        if (p.id == id)
        {
            for (const PlaylistEntry& e : p.items) if (e.itemId == item.itemId) return; // already in it
            p.items.push_back(item);
            p.updatedAt = nowSecs();
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
                if (p.items[i].itemId == itemId) { p.items.remove(i); p.updatedAt = nowSecs(); break; }
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
