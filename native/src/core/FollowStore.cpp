#include "FollowStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Tombstones.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, so each viewer follows their own shows. The spelling is favourites' spelling with a different
// group name; CloudMerge's serializer builds the same key from the profile id it is iterating.
static QString followKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("follow/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/items");
}

// Tombstone namespace for THIS profile's follows (mirrors followKey()'s per-profile shape), keyed by itemId.
static QString followTombstoneStore()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("follow/") + (id.isEmpty() ? QStringLiteral("default") : id);
}

static std::function<void()> g_changeHook;
void FollowStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QVector<FollowItem> FollowStore::list()
{
    QVector<FollowItem> out;
    const QByteArray json = store().value(followKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        FollowItem it;
        it.addonId      = o.value(QStringLiteral("addonId")).toString();
        it.itemId       = o.value(QStringLiteral("itemId")).toString();
        it.title        = o.value(QStringLiteral("title")).toString();
        it.subtitle     = o.value(QStringLiteral("subtitle")).toString();
        it.type         = o.value(QStringLiteral("type")).toString();
        it.thumbnailUrl = o.value(QStringLiteral("thumbnailUrl")).toString();
        it.ts           = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        it.muted        = o.value(QStringLiteral("muted")).toBool();
        if (!it.itemId.isEmpty()) out.push_back(it);
    }
    return out;
}

// A pure persister, favourites' rule verbatim: each row's ts is written VERBATIM, never backfilled, so
// following one series does not re-date the rest and let their rewrite beat a real deletion tombstone.
static void save(const QVector<FollowItem>& items)
{
    QJsonArray arr;
    for (const FollowItem& it : items)
    {
        QJsonObject o;
        o.insert(QStringLiteral("addonId"), it.addonId);
        o.insert(QStringLiteral("itemId"), it.itemId);
        o.insert(QStringLiteral("title"), it.title);
        o.insert(QStringLiteral("subtitle"), it.subtitle);
        o.insert(QStringLiteral("type"), it.type);
        o.insert(QStringLiteral("thumbnailUrl"), it.thumbnailUrl);
        o.insert(QStringLiteral("ts"), static_cast<double>(it.ts));
        // Only when set, so a row nobody muted is byte-for-byte the row increment 1 wrote (and a peer running
        // that build merges it unchanged; it passes the unknown key through, since rows travel whole).
        if (it.muted) o.insert(QStringLiteral("muted"), true);
        arr.append(o);
    }
    store().setValue(followKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

void FollowStore::add(const FollowItem& item)
{
    if (item.itemId.isEmpty()) return;
    QVector<FollowItem> items = list();
    bool wasMuted = false;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].itemId == item.itemId) { wasMuted = wasMuted || items[i].muted; items.remove(i); }   // de-dup
    FollowItem stamped = item;
    stamped.muted = stamped.muted || wasMuted;   // refreshing a row that is still followed keeps its mute
    stamped.ts = QDateTime::currentSecsSinceEpoch();
    items.prepend(stamped);                                    // newest first
    save(items);
    // A re-follow must BEAT any tombstone this device still holds for the same id, or the next merge would
    // suppress the row we just wrote. The tombstone rule is "a tombstone at-or-after the item's ts wins", and
    // a same-second re-follow is exactly that tie — so the undo is spelled the way Tombstones.h spells it:
    // erase the record, rather than hoping a fresher stamp outruns it.
    Tombstones::remove(followTombstoneStore(), item.itemId);
    fireChanged();
}

void FollowStore::remove(const QString& itemId)
{
    if (itemId.isEmpty()) return;
    QVector<FollowItem> items = list();
    bool had = false;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].itemId == itemId) { items.remove(i); had = true; }
    if (!had) return;   // nothing to unfollow: no rewrite, no tombstone, no sync churn
    save(items);
    Tombstones::record(followTombstoneStore(), itemId);
    fireChanged();
}

bool FollowStore::isFollowed(const QString& itemId)
{
    if (itemId.isEmpty()) return false;
    for (const FollowItem& it : list())
        if (it.itemId == itemId) return true;
    return false;
}

int FollowStore::count() { return int(list().size()); }

void FollowStore::setMuted(const QString& itemId, bool muted)
{
    if (itemId.isEmpty()) return;
    QVector<FollowItem> items = list();
    for (FollowItem& it : items)
    {
        if (it.itemId != itemId) continue;
        if (it.muted == muted) return;
        it.muted = muted;
        // Strictly newer than what we held, so the merge's newest-ts rule carries the edit to every peer even
        // when the follow and the mute land in the same second.
        it.ts = qMax(QDateTime::currentSecsSinceEpoch(), it.ts + 1);
        save(items);
        fireChanged();
        return;
    }
}

bool FollowStore::isMuted(const QString& itemId)
{
    if (itemId.isEmpty()) return false;
    for (const FollowItem& it : list())
        if (it.itemId == itemId) return it.muted;
    return false;
}
