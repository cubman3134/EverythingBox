#include "RecentStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Tombstones.h"   // issue #150: an explicit removal is dated; a cap eviction is not

#include <QSettings>
#include <QDateTime>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

static const int kMaxRecents = 40;

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Recents are per-profile so each user's home content is exclusive to them.
static QString profileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

static QString recentsKey()
{
    return QStringLiteral("recent/") + profileId() + QStringLiteral("/items");
}

// The tombstone namespace for THIS profile's recents (issue #150). Per profile, mirroring the store's own
// namespacing exactly as favourites and playlists do, so one profile's removals are invisible under another's.
static QString recentsTombStore()
{
    return QStringLiteral("recent/") + profileId();
}

// The stable identity the merge de-duplicates a recents entry by: its key when it has one (a streamed item's
// path/URL changes per session), else its path. The tombstone key is the SAME identity, so a tombstone lands on
// the entry the union pass is holding.
static QString identOf(const RecentItem& it)
{
    return it.key.isEmpty() ? it.path : it.key;
}

// Serialize + persist the list. One spelling for add/remove/clear — they wrote three copies of this block.
static void saveList(const QVector<RecentItem>& items)
{
    QJsonArray arr;
    for (const RecentItem& it : items)
    {
        QJsonObject o;
        o.insert(QStringLiteral("path"), it.path);
        o.insert(QStringLiteral("title"), it.title);
        o.insert(QStringLiteral("kind"), it.kind);
        if (!it.thumb.isEmpty()) o.insert(QStringLiteral("thumb"), it.thumb);
        if (!it.key.isEmpty())   o.insert(QStringLiteral("key"), it.key);
        if (!it.system.isEmpty()) o.insert(QStringLiteral("system"), it.system);
        if (it.ts > 0) o.insert(QStringLiteral("ts"), (double)it.ts);
        arr.append(o);
    }
    store().setValue(recentsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

QVector<RecentItem> RecentStore::list()
{
    QVector<RecentItem> out;
    const QByteArray json = store().value(recentsKey()).toString().toUtf8();
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        RecentItem it;
        it.path  = o.value(QStringLiteral("path")).toString();
        it.title = o.value(QStringLiteral("title")).toString();
        it.kind  = o.value(QStringLiteral("kind")).toString();
        it.thumb = o.value(QStringLiteral("thumb")).toString();
        it.key   = o.value(QStringLiteral("key")).toString();
        it.system = o.value(QStringLiteral("system")).toString();
        it.ts    = (qint64)o.value(QStringLiteral("ts")).toDouble();
        if (!it.path.isEmpty()) out.push_back(it);
    }
    return out;
}

void RecentStore::add(const RecentItem& item)
{
    if (item.path.isEmpty()) return;
    RecentItem entry = item;
    if (entry.ts == 0) entry.ts = QDateTime::currentSecsSinceEpoch(); // stamp so cross-device sync can merge by recency
    QVector<RecentItem> items = list();
    // De-dup by stable key when present (a streamed item's path/URL changes per session), else by path. This
    // removal is a MOVE-TO-FRONT, not a deletion — the entry is re-inserted on the next line — so it records
    // nothing (issue #150).
    const QString ident = identOf(entry);
    for (int i = items.size() - 1; i >= 0; --i)
        if (identOf(items[i]) == ident) items.remove(i);
    items.prepend(entry);                              // newest first
    // CAP EVICTION, AND IT RECORDS NOTHING (issue #150). Dropping the 41st-oldest entry is the list running out
    // of room, not the user saying "forget this" — tombstoning it would make the cap PERMANENT, so an item that
    // scrolled off could never re-enter on a later re-watch and a peer with a shorter history could never hand
    // it back. It needs no record either way: the merge unions both devices' lists and re-applies the cap, so
    // an evicted entry re-appears only if it is still among the newest 40 overall, which is exactly right.
    while (items.size() > kMaxRecents) items.removeLast();
    saveList(items);

    // Opening something UNDOES an earlier explicit removal of it, so drop any tombstone. Without this the
    // removal would go on suppressing the entry on every peer whose clock reads the re-open at or before the
    // removal's second, and — worse — the tombstone would outlive the entry it describes in the document.
    Tombstones::remove(recentsTombStore(), identOf(entry));
}

void RecentStore::remove(const QString& pathOrKey)
{
    if (pathOrKey.isEmpty()) return;
    QVector<RecentItem> items = list();
    QStringList removed;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].path == pathOrKey || (!items[i].key.isEmpty() && items[i].key == pathOrKey))
        { removed.push_back(identOf(items[i])); items.remove(i); }
    if (removed.isEmpty()) return;

    saveList(items);
    // THE USER'S EXPLICIT REMOVE, so it is dated (issue #150). Without a tombstone this deletion looked exactly
    // like "this device has never opened that item", and the merge — which unions the two lists and cannot read
    // a reason out of an absence — handed the entry straight back from any peer that still had it.
    //
    // Tombstoned by the entry's OWN identity, not by the argument: callers pass whichever of path/key they have
    // (HomeView::uninstallGameItem passes both in turn), while the union pass keys on key-else-path. Naming the
    // argument would file the tombstone under something the merge never looks up.
    for (const QString& id : removed) Tombstones::record(recentsTombStore(), id);
}

void RecentStore::clear()
{
    // Clear is "remove everything", one explicit user action per entry, so every entry gets a tombstone —
    // otherwise emptying the list on one device just re-downloads it from the next peer to sync.
    const QVector<RecentItem> items = list();
    store().remove(recentsKey());
    store().sync();
    for (const RecentItem& it : items) Tombstones::record(recentsTombStore(), identOf(it));
}

RecentStore::Relaunch RecentStore::relaunchFor(const QString& kind)
{
    if (kind == QStringLiteral("steamgame")) return Relaunch::SteamGame;
    if (kind == QStringLiteral("epicgame"))  return Relaunch::EpicGame;
    if (kind == QStringLiteral("goggame"))   return Relaunch::GogGame;
    if (kind == QStringLiteral("battlenetgame")) return Relaunch::BattleNetGame;
    if (kind == QStringLiteral("pcgame"))    return Relaunch::PcGame;
    if (kind == QStringLiteral("video"))     return Relaunch::Video;
    if (kind == QStringLiteral("audio"))     return Relaunch::Audio;
    if (kind == QStringLiteral("document"))  return Relaunch::Document;
    if (kind == QStringLiteral("game"))      return Relaunch::Game;
    return Relaunch::Unknown;
}
