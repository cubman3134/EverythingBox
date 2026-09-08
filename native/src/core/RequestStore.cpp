#include "RequestStore.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <algorithm>

namespace {

#ifdef EB_REQUESTS_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

QSettings& store()
{
#ifdef EB_REQUESTS_TEST_SEAM
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

// Per-profile, under the "requests/" prefix CloudSync::isDeviceLocalKey carves out. Spelled ONCE, here.
QString listKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("requests/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/list");
}

std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

QJsonObject toJson(const requests::StoredRequest& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("key"), r.key);
    o.insert(QStringLiteral("backend"), r.backendId);
    o.insert(QStringLiteral("title"), r.title);
    if (!r.thumb.isEmpty()) o.insert(QStringLiteral("thumb"), r.thumb);
    o.insert(QStringLiteral("mediaType"), r.mediaType);
    if (!r.imdb.isEmpty()) o.insert(QStringLiteral("imdb"), r.imdb);
    if (!r.tmdb.isEmpty()) o.insert(QStringLiteral("tmdb"), r.tmdb);
    if (!r.seasons.isEmpty())
    {
        QJsonArray arr;
        for (int s : r.seasons) arr.append(s);
        o.insert(QStringLiteral("seasons"), arr);
    }
    o.insert(QStringLiteral("at"), double(r.requestedAt));
    o.insert(QStringLiteral("status"), r.status);
    return o;
}

requests::StoredRequest fromJson(const QJsonObject& o)
{
    requests::StoredRequest r;
    r.key         = o.value(QStringLiteral("key")).toString();
    r.backendId   = o.value(QStringLiteral("backend")).toString();
    r.title       = o.value(QStringLiteral("title")).toString();
    r.thumb       = o.value(QStringLiteral("thumb")).toString();
    r.mediaType   = o.value(QStringLiteral("mediaType")).toString();
    r.imdb        = o.value(QStringLiteral("imdb")).toString();
    r.tmdb        = o.value(QStringLiteral("tmdb")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("seasons")).toArray())
        if (v.toInt() > 0) r.seasons.push_back(v.toInt());
    r.requestedAt = qint64(o.value(QStringLiteral("at")).toDouble());
    // An absent or unrecognised token reads as "unknown" through requests::availabilityFromToken. Stored
    // verbatim so a row written by a newer build keeps its own word rather than being flattened here.
    r.status      = o.value(QStringLiteral("status")).toString();
    return r;
}

QVector<requests::StoredRequest> readAll()
{
    QVector<requests::StoredRequest> out;
    const QByteArray json = store().value(listKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const requests::StoredRequest r = fromJson(v.toObject());
        // A row with no key can never be refreshed, matched or removed — it would sit on the shelf for ever
        // showing an unknown status nothing could ever resolve. Dropped on read.
        if (r.key.isEmpty()) continue;
        out.push_back(r);
    }
    return out;
}

void writeAll(const QVector<requests::StoredRequest>& rows)
{
    QJsonArray arr;
    for (const requests::StoredRequest& r : rows) arr.append(toJson(r));
    store().setValue(listKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged();
}

} // namespace

QVector<requests::StoredRequest> RequestStore::list() { return readAll(); }
int RequestStore::count() { return int(readAll().size()); }

void RequestStore::add(const requests::StoredRequest& row)
{
    if (row.key.isEmpty()) return;
    QVector<requests::StoredRequest> rows = readAll();
    for (requests::StoredRequest& r : rows)
    {
        if (r.key != row.key) continue;
        // The same title, asked for again. Update in place — see the header on why a second row would be a
        // lie about what happened. The season sets UNION: asking for season 3 after season 1 means the
        // shelf's row is about both, not only the newer press.
        QVector<int> merged = r.seasons;
        for (int s : row.seasons) if (!merged.contains(s)) merged.push_back(s);
        std::sort(merged.begin(), merged.end());
        // ...unless either press was for the WHOLE thing (an empty season list), which subsumes any subset.
        r.seasons     = (r.seasons.isEmpty() || row.seasons.isEmpty()) ? QVector<int>{} : merged;
        r.backendId   = row.backendId;
        r.title       = row.title.isEmpty() ? r.title : row.title;
        r.thumb       = row.thumb.isEmpty() ? r.thumb : row.thumb;
        r.mediaType   = row.mediaType.isEmpty() ? r.mediaType : row.mediaType;
        if (!row.imdb.isEmpty()) r.imdb = row.imdb;
        if (!row.tmdb.isEmpty()) r.tmdb = row.tmdb;
        r.requestedAt = row.requestedAt > 0 ? row.requestedAt
                                            : QDateTime::currentSecsSinceEpoch();
        r.status      = row.status;
        writeAll(rows);
        return;
    }
    requests::StoredRequest fresh = row;
    if (fresh.requestedAt <= 0) fresh.requestedAt = QDateTime::currentSecsSinceEpoch();
    if (fresh.status.isEmpty()) fresh.status = requests::statusToken(requests::Availability::Unknown);
    rows.push_back(fresh);
    writeAll(rows);
}

void RequestStore::setStatus(const QString& key, const QString& statusToken)
{
    if (key.isEmpty()) return;
    QVector<requests::StoredRequest> rows = readAll();
    for (requests::StoredRequest& r : rows)
    {
        if (r.key != key) continue;
        if (r.status == statusToken) return;      // no write, no hook, no redraw for an unchanged status
        r.status = statusToken;
        writeAll(rows);
        return;
    }
    // No row: a refresh does NOT create one. See the header.
}

void RequestStore::remove(const QString& key)
{
    QVector<requests::StoredRequest> rows = readAll();
    const int before = int(rows.size());
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&key](const requests::StoredRequest& r) { return r.key == key; }),
               rows.end());
    if (int(rows.size()) != before) writeAll(rows);
}

bool RequestStore::get(const QString& key, requests::StoredRequest& out)
{
    for (const requests::StoredRequest& r : readAll())
        if (r.key == key) { out = r; return true; }
    return false;
}

void RequestStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }

#ifdef EB_REQUESTS_TEST_SEAM
void RequestStore::setIniPathForTesting(const QString& path)
{
    g_testIniPath = path;
    delete g_testStore;
    g_testStore = nullptr;
}
#endif
