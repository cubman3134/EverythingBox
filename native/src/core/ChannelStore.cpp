#include "ChannelStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Tombstones.h"      // a delete leaves a dated tombstone so a peer cannot resurrect it (see the header)

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUuid>

using channels::Channel;

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// The active profile leaf, matching FavoritesStore::favKey()'s per-profile shape. Factored out so the data key
// and the tombstone-store name below cannot drift on the profile spelling (both fall back to "default").
static QString channelProfile()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

static QString channelsKey() { return QStringLiteral("channels/") + channelProfile() + QStringLiteral("/items"); }

// Tombstone store for THIS profile's channels, keyed by channel id — the namespace CloudMerge's serializer
// reads (see CloudMerge.cpp channelTombStore()). Its per-profile shape mirrors channelsKey()'s.
static QString channelTombStore() { return QStringLiteral("channels/") + channelProfile(); }

static std::function<void()> g_changeHook;
void ChannelStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QVector<Channel> ChannelStore::list()
{
    QVector<Channel> out;
    const QByteArray json = store().value(channelsKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Channel c;
        c.id         = o.value(QStringLiteral("id")).toString();
        c.name       = o.value(QStringLiteral("name")).toString();
        c.sourceKind = channels::sourceKindFromInt(o.value(QStringLiteral("src")).toInt());
        c.sourceId   = o.value(QStringLiteral("srcid")).toString();
        c.ordering   = channels::orderingFromInt(o.value(QStringLiteral("ord")).toInt());
        c.startEpoch = static_cast<qint64>(o.value(QStringLiteral("start")).toDouble());
        c.startFromBeginning = o.value(QStringLiteral("beg")).toBool();
        c.ts         = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        // A row with no id has no merge identity and no way to be edited or deleted — it can only have come
        // from a hand-edited ini. Dropped rather than given one: minting an id here would mint a DIFFERENT id
        // on every device that read the same row, which is precisely the duplicate the id-stable form exists
        // to prevent.
        if (!c.id.isEmpty()) out.push_back(c);
    }
    return out;
}

static void saveAll(const QVector<Channel>& all)
{
    QJsonArray arr;
    for (const Channel& c : all)
    {
        if (c.id.isEmpty()) continue;
        QJsonObject o;
        o.insert(QStringLiteral("id"), c.id);
        o.insert(QStringLiteral("name"), c.name);
        o.insert(QStringLiteral("src"), channels::toInt(c.sourceKind));
        o.insert(QStringLiteral("srcid"), c.sourceId);
        o.insert(QStringLiteral("ord"), channels::toInt(c.ordering));
        o.insert(QStringLiteral("start"), static_cast<double>(c.startEpoch));
        o.insert(QStringLiteral("beg"), c.startFromBeginning);
        o.insert(QStringLiteral("ts"), static_cast<double>(c.ts));
        arr.append(o);
    }
    store().setValue(channelsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

bool ChannelStore::get(const QString& id, Channel& out)
{
    if (id.isEmpty()) return false;
    for (const Channel& c : list())
        if (c.id == id) { out = c; return true; }
    return false;
}

QString ChannelStore::add(Channel ch)
{
    if (ch.name.trimmed().isEmpty()) return QString();
    // RANDOM, not name-derived: a later rename must keep the id (see the header). A caller-supplied id is
    // honoured (the restore path), which is why this is not unconditional.
    if (ch.id.isEmpty()) ch.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    ch.ts = now;
    // A channel with no explicit start epoch goes on air NOW, not at the epoch: a channel created this
    // afternoon that claimed to have been broadcasting since 1970 would compute a lineup for every past day
    // it was asked about, and would put the viewer at a random point of a schedule that never aired.
    if (ch.startEpoch <= 0) ch.startEpoch = now;
    QVector<Channel> all = list();
    for (int i = all.size() - 1; i >= 0; --i)
        if (all[i].id == ch.id) all.remove(i);     // an explicit-id add is an upsert
    all.prepend(ch);                                // newest first
    saveAll(all);
    fireChanged();
    return ch.id;
}

bool ChannelStore::update(const Channel& ch)
{
    if (ch.id.isEmpty()) return false;
    QVector<Channel> all = list();
    int idx = -1;
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == ch.id) { idx = i; break; }
    if (idx < 0) return false;
    Channel stamped = ch;
    stamped.ts = QDateTime::currentSecsSinceEpoch();
    // The start epoch is a property of the CHANNEL, not of this edit: renaming a channel or switching it to
    // shuffle must not re-date when it went on air, or every edit would silently blank the day's history.
    if (stamped.startEpoch <= 0) stamped.startEpoch = all[idx].startEpoch;
    all[idx] = stamped;
    saveAll(all);
    fireChanged();
    return true;
}

void ChannelStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QVector<Channel> all = list();
    bool found = false;
    for (int i = all.size() - 1; i >= 0; --i)
        if (all[i].id == id) { all.remove(i); found = true; }
    if (!found) return;   // nothing removed: don't churn the store, tombstone nothing, or fire the hook
    // The dated tombstone goes down BEFORE the row does, exactly as FilterPresetStore::remove does: without
    // it the bare removal is indistinguishable from "never known" and a peer's copy walks back in on the next
    // merge. A re-created channel gets a fresh random id, never this one, so the tombstone is not something a
    // legitimate re-add has to clear.
    Tombstones::record(channelTombStore(), id);
    saveAll(all);
    fireChanged();
}
