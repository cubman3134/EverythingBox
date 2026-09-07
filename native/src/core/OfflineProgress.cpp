#include "OfflineProgress.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

namespace {

#ifdef EB_JELLYFIN_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

QSettings& store()
{
#ifdef EB_JELLYFIN_TEST_SEAM
    if (!g_testIniPath.isEmpty())
    {
        if (!g_testStore) g_testStore = new QSettings(g_testIniPath, QSettings::IniFormat);
        return *g_testStore;
    }
#endif
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString groupPrefix()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("jellyfin/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/offlineprogress/");
}

} // namespace

#ifdef EB_JELLYFIN_TEST_SEAM
void OfflineProgress::setIniPathForTesting(const QString& path)
{
    g_testIniPath = path;
    delete g_testStore;
    g_testStore = nullptr;
}
#endif

QString OfflineProgress::queueKey(const QString& serverId) { return groupPrefix() + serverId; }

// ---- The pure rules ----------------------------------------------------------------------------------

QVector<OfflineProgress::Report> OfflineProgress::boundedAppend(const QVector<Report>& existing,
                                                                const Report& in, int cap)
{
    if (cap <= 0) return {};
    QVector<Report> out = existing;
    out.push_back(in);
    // Oldest first out. Taken from the FRONT rather than by sorting: the queue is appended to in time order
    // by construction, and a sort here would quietly repair a caller that had stopped being.
    while (out.size() > cap) out.removeFirst();
    return out;
}

QVector<OfflineProgress::Report> OfflineProgress::collapse(const QVector<Report>& in)
{
    QVector<Report> out;
    for (const Report& r : in)
    {
        if (r.qualifiedId.isEmpty()) continue;
        bool merged = false;
        for (Report& have : out)
        {
            if (have.qualifiedId != r.qualifiedId) continue;
            // The NEWEST wins outright — position, event and both session ids. Keeping the older event
            // beside the newer position would send a Stop at a Progress's position, or the reverse.
            if (r.whenMs >= have.whenMs) have = r;
            merged = true;
            break;
        }
        if (!merged) out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const Report& a, const Report& b) {
        if (a.whenMs != b.whenMs) return a.whenMs < b.whenMs;
        return a.qualifiedId < b.qualifiedId;      // stable: two flushes send the same order
    });
    return out;
}

bool OfflineProgress::shouldApply(const Report& r, const Jellyfin::UserState& server)
{
    if (r.qualifiedId.isEmpty()) return false;
    // The server has never heard of this item (or answered without a UserData block): there is no later
    // truth to destroy, so the queued report is the only thing anybody knows.
    if (!server.ok) return true;
    // The server says it is FINISHED. A position report un-marks played on Jellyfin, so applying a queued
    // mid-episode position here would not merely rewind the position — it would re-open a finished episode
    // on every device. Strictly worse than dropping it.
    if (server.played) return false;
    const double serverSeconds = Jellyfin::secondsFromTicks(server.positionTicks);
    // THE STALE-REPORT RULE. Strictly later, by more than the slack — see the header on why the slack is
    // half a report interval and not zero.
    if (serverSeconds > r.positionSeconds + kStaleSlackSeconds) return false;
    return true;
}

// ---- The store ---------------------------------------------------------------------------------------

QVector<OfflineProgress::Report> OfflineProgress::pending(const QString& serverId)
{
    QVector<Report> out;
    if (serverId.isEmpty()) return out;
    const QByteArray json = store().value(queueKey(serverId)).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Report r;
        r.qualifiedId     = o.value(QStringLiteral("id")).toString();
        r.positionSeconds = o.value(QStringLiteral("pos")).toDouble();
        r.ev              = o.value(QStringLiteral("ev")).toInt(int(Jellyfin::ProgressEvent::Progress));
        r.playSessionId   = o.value(QStringLiteral("ps")).toString();
        r.mediaSourceId   = o.value(QStringLiteral("ms")).toString();
        r.whenMs          = qint64(o.value(QStringLiteral("t")).toDouble());
        if (!r.qualifiedId.isEmpty()) out.push_back(r);
    }
    return out;
}

void OfflineProgress::replace(const QString& serverId, const QVector<Report>& rows)
{
    if (serverId.isEmpty()) return;
    if (rows.isEmpty()) { clearServer(serverId); return; }
    QJsonArray arr;
    for (const Report& r : rows)
    {
        arr.append(QJsonObject{
            { QStringLiteral("id"),  r.qualifiedId },
            { QStringLiteral("pos"), r.positionSeconds },
            { QStringLiteral("ev"),  r.ev },
            { QStringLiteral("ps"),  r.playSessionId },
            { QStringLiteral("ms"),  r.mediaSourceId },
            // As a double: a JSON number is a double anyway, and QJsonValue has no 64-bit integer type.
            // Millisecond timestamps are exact in a double until the year 287396.
            { QStringLiteral("t"),   double(r.whenMs) } });
    }
    store().setValue(queueKey(serverId),
                     QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

void OfflineProgress::clearServer(const QString& serverId)
{
    if (serverId.isEmpty()) return;
    store().remove(queueKey(serverId));
    store().sync();
}

void OfflineProgress::enqueue(const Report& r)
{
    const QString serverId = Jellyfin::serverOf(r.qualifiedId);
    if (serverId.isEmpty()) return;     // not a qualified id: there is no server to owe this to
    replace(serverId, boundedAppend(pending(serverId), r, kMaxPerServer));
}

QStringList OfflineProgress::serversWithPending()
{
    QStringList out;
    const QString prefix = groupPrefix();
    const QStringList keys = store().allKeys();
    for (const QString& k : keys)
    {
        if (!k.startsWith(prefix)) continue;
        const QString id = k.mid(prefix.size());
        if (!id.isEmpty() && !out.contains(id)) out << id;
    }
    return out;
}
