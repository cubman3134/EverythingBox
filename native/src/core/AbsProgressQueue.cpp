#include "AbsProgressQueue.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

namespace {

// The shared ini, the same file AbsServerStore writes the servers into — so the queue sits beside the
// tokens under the one device-local prefix, and is removed from the same place.
QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString groupPrefix()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("audiobookshelf/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/offlineprogress/");
}

void save(const QString& serverId, const QVector<AbsProgressQueue::Entry>& rows)
{
    if (serverId.isEmpty()) return;
    if (rows.isEmpty()) { store().remove(AbsProgressQueue::queueKey(serverId)); store().sync(); return; }
    QJsonArray arr;
    for (const AbsProgressQueue::Entry& e : rows)
        arr.append(QJsonObject{
            { QStringLiteral("id"),   e.qualifiedId },
            { QStringLiteral("pos"),  e.position },
            { QStringLiteral("dur"),  e.duration },
            // As a double: a JSON number is one anyway, and a millisecond timestamp is exact in it.
            { QStringLiteral("t"),    double(e.whenMs) },
            { QStringLiteral("sent"), e.sent } });
    store().setValue(AbsProgressQueue::queueKey(serverId),
                     QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

} // namespace

// ---- The pure rules ----------------------------------------------------------------------------------

QVector<AbsProgressQueue::Entry> AbsProgressQueue::latestPerBook(const QVector<Entry>& existing, const Entry& in)
{
    QVector<Entry> out = existing;
    if (in.qualifiedId.isEmpty()) return out;
    for (Entry& have : out)
    {
        if (have.qualifiedId != in.qualifiedId) continue;
        // THE LATEST, and only the latest. An older report arriving after a newer one (a slow callback, a
        // clock step) must not rewind the one position this queue keeps for the book.
        if (in.whenMs >= have.whenMs) have = in;
        return out;
    }
    out.push_back(in);
    return out;
}

bool AbsProgressQueue::localIsNewer(const Entry& local, const Abs::Progress& server)
{
    // The server has never heard of this book for this user: there is no later truth to destroy.
    if (!server.found) return true;
    // THE COMPARISON. Strictly later: a report stamped at the same millisecond as the server's own update is
    // the server's update, not something the server has not heard.
    return local.whenMs > server.lastUpdateMs;
}

AbsProgressQueue::OpenPick AbsProgressQueue::pickOnOpen(bool answered, const Abs::Progress& server,
                                                        const Entry* local)
{
    OpenPick p;
    if (!answered)
    {
        // Nobody to ask: this device's last known position, sent or not, else the top of the book.
        if (local) { p.from = From::Local; p.position = local->position; }
        return p;
    }
    // THE SERVER WINS — unless this device holds an UNSENT report that is newer than the server's own.
    if (local && !local->sent && localIsNewer(*local, server))
    {
        p.from = From::Local;
        p.position = local->position;
        p.flushFirst = true;
        return p;
    }
    if (server.found) { p.from = From::Server; p.position = server.currentTime; return p; }
    // The server answered that it has never heard of the book, and nothing here is owed to it.
    if (local) { p.from = From::Local; p.position = local->position; }
    return p;
}

// ---- The store ---------------------------------------------------------------------------------------

QString AbsProgressQueue::queueKey(const QString& serverId) { return groupPrefix() + serverId; }

QVector<AbsProgressQueue::Entry> AbsProgressQueue::pending(const QString& serverId)
{
    QVector<Entry> out;
    if (serverId.isEmpty()) return out;
    const QByteArray json = store().value(queueKey(serverId)).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        const QJsonObject o = v.toObject();
        Entry e;
        e.qualifiedId = o.value(QStringLiteral("id")).toString();
        e.position    = o.value(QStringLiteral("pos")).toDouble();
        e.duration    = o.value(QStringLiteral("dur")).toDouble();
        e.whenMs      = qint64(o.value(QStringLiteral("t")).toDouble());
        e.sent        = o.value(QStringLiteral("sent")).toBool();
        if (!e.qualifiedId.isEmpty()) out.push_back(e);
    }
    return out;
}

QVector<AbsProgressQueue::Entry> AbsProgressQueue::unsent(const QString& serverId)
{
    QVector<Entry> out;
    for (const Entry& e : pending(serverId))
        if (!e.sent) out.push_back(e);
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.whenMs < b.whenMs; });
    return out;
}

void AbsProgressQueue::put(const Entry& e)
{
    const QString serverId = Abs::serverOf(e.qualifiedId);
    if (serverId.isEmpty()) return;     // not a qualified id: there is no server this could be owed to
    save(serverId, latestPerBook(pending(serverId), e));
}

bool AbsProgressQueue::entryFor(const QString& qualifiedId, Entry* out)
{
    for (const Entry& e : pending(Abs::serverOf(qualifiedId)))
        if (e.qualifiedId == qualifiedId) { if (out) *out = e; return true; }
    return false;
}

void AbsProgressQueue::markSent(const QString& qualifiedId, qint64 whenMs)
{
    const QString serverId = Abs::serverOf(qualifiedId);
    QVector<Entry> rows = pending(serverId);
    for (Entry& e : rows)
        if (e.qualifiedId == qualifiedId && e.whenMs == whenMs && !e.sent) { e.sent = true; save(serverId, rows); return; }
}

void AbsProgressQueue::adoptServer(const QString& qualifiedId, const Abs::Progress& server)
{
    if (!server.found) return;
    Entry e;
    e.qualifiedId = qualifiedId;
    e.position    = server.currentTime;
    e.duration    = server.duration;
    e.whenMs      = server.lastUpdateMs;
    e.sent        = true;
    const QString serverId = Abs::serverOf(qualifiedId);
    QVector<Entry> rows = pending(serverId);
    for (Entry& have : rows)
        if (have.qualifiedId == qualifiedId)
        {
            // Never over an unsent report that is NEWER than the server's answer: that one is still owed.
            if (!have.sent && have.whenMs > e.whenMs) return;
            have = e; save(serverId, rows); return;
        }
    rows.push_back(e);
    save(serverId, rows);
}

void AbsProgressQueue::remove(const QString& qualifiedId)
{
    const QString serverId = Abs::serverOf(qualifiedId);
    QVector<Entry> rows = pending(serverId);
    for (int i = rows.size() - 1; i >= 0; --i)
        if (rows.at(i).qualifiedId == qualifiedId) rows.remove(i);
    save(serverId, rows);
}

QStringList AbsProgressQueue::serversWithUnsent()
{
    QStringList out;
    const QString prefix = groupPrefix();
    for (const QString& k : store().allKeys())
    {
        if (!k.startsWith(prefix)) continue;
        const QString id = k.mid(prefix.size());
        if (id.isEmpty() || out.contains(id)) continue;
        if (!unsent(id).isEmpty()) out << id;
    }
    return out;
}
