#include "LocalResolveCache.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

void LocalResolveCache::load()
{
    byPath_.clear();
    byShow_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    // New nested layout: { "paths": { … }, "shows": { … } }.
    // Backward compat: a legacy movie-only cache is a FLAT { path: {…} } object with no
    // "paths"/"shows" keys — treat the whole root as the paths map.
    const bool nested = root.contains(QStringLiteral("paths")) || root.contains(QStringLiteral("shows"));
    const QJsonObject paths = nested ? root.value(QStringLiteral("paths")).toObject() : root;
    for (auto it = paths.constBegin(); it != paths.constEnd(); ++it)
    {
        const QJsonObject o = it.value().toObject();
        Entry e;
        e.size = (qint64)o.value(QStringLiteral("size")).toDouble();
        e.mtime = (qint64)o.value(QStringLiteral("mtime")).toDouble();
        e.matched = o.value(QStringLiteral("matched")).toBool();
        e.ts = (qint64)o.value(QStringLiteral("ts")).toDouble();
        for (const QJsonValue& v : o.value(QStringLiteral("ids")).toArray()) e.ids << v.toString();
        byPath_.insert(it.key(), e);
    }

    const QJsonObject shows = root.value(QStringLiteral("shows")).toObject();
    for (auto it = shows.constBegin(); it != shows.constEnd(); ++it)
    {
        const QJsonObject o = it.value().toObject();
        ShowEntry e;
        e.matched = o.value(QStringLiteral("matched")).toBool();
        e.ts = (qint64)o.value(QStringLiteral("ts")).toDouble();
        for (const QJsonValue& v : o.value(QStringLiteral("ids")).toArray()) e.ids << v.toString();
        byShow_.insert(it.key(), e);
    }
}

void LocalResolveCache::save() const
{
    QJsonObject paths;
    for (auto it = byPath_.constBegin(); it != byPath_.constEnd(); ++it)
    {
        const Entry& e = it.value();
        QJsonArray ids; for (const QString& s : e.ids) ids.append(s);
        paths.insert(it.key(), QJsonObject{
            { QStringLiteral("size"), (double)e.size }, { QStringLiteral("mtime"), (double)e.mtime },
            { QStringLiteral("matched"), e.matched }, { QStringLiteral("ts"), (double)e.ts },
            { QStringLiteral("ids"), ids } });
    }
    QJsonObject shows;
    for (auto it = byShow_.constBegin(); it != byShow_.constEnd(); ++it)
    {
        const ShowEntry& e = it.value();
        QJsonArray ids; for (const QString& s : e.ids) ids.append(s);
        shows.insert(it.key(), QJsonObject{
            { QStringLiteral("matched"), e.matched }, { QStringLiteral("ts"), (double)e.ts },
            { QStringLiteral("ids"), ids } });
    }
    QJsonObject root;
    root.insert(QStringLiteral("paths"), paths);
    root.insert(QStringLiteral("shows"), shows);
    QFile f(file_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool LocalResolveCache::isFresh(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs, qint64 retryDays) const
{
    const auto it = byPath_.constFind(path);
    if (it == byPath_.constEnd()) return false;
    const Entry& e = it.value();
    if (e.size != size || e.mtime != mtime) return false;   // file changed → re-resolve
    if (e.matched) return true;                             // a match never expires (until the file changes)
    return (nowSecs - e.ts) < retryDays * 86400;           // a nomatch is fresh only within the retry window
}

void LocalResolveCache::putMatched(const QString& path, qint64 size, qint64 mtime, const QStringList& ids, qint64 nowSecs)
{ byPath_.insert(path, Entry{ size, mtime, ids, true, nowSecs }); }

void LocalResolveCache::putNoMatch(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs)
{ byPath_.insert(path, Entry{ size, mtime, {}, false, nowSecs }); }

QHash<QString, QStringList> LocalResolveCache::matchedIdsByPath() const
{
    QHash<QString, QStringList> out;
    for (auto it = byPath_.constBegin(); it != byPath_.constEnd(); ++it)
        if (it.value().matched && !it.value().ids.isEmpty()) out.insert(it.key(), it.value().ids);
    return out;
}

bool LocalResolveCache::isShowFresh(const QString& showKey, qint64 nowSecs, qint64 retryDays) const
{
    const auto it = byShow_.constFind(showKey);
    if (it == byShow_.constEnd()) return false;
    if (it.value().matched) return true;                       // a resolved show never expires (until re-match)
    return (nowSecs - it.value().ts) < retryDays * 86400;      // a nomatch is fresh only within the retry window
}

void LocalResolveCache::putShowMatched(const QString& showKey, const QStringList& ids, qint64 nowSecs)
{ byShow_.insert(showKey, ShowEntry{ ids, true, nowSecs }); }

void LocalResolveCache::putShowNoMatch(const QString& showKey, qint64 nowSecs)
{ byShow_.insert(showKey, ShowEntry{ {}, false, nowSecs }); }

QHash<QString, QStringList> LocalResolveCache::seriesIdsByShow() const
{
    QHash<QString, QStringList> out;
    for (auto it = byShow_.constBegin(); it != byShow_.constEnd(); ++it)
        if (it.value().matched && !it.value().ids.isEmpty()) out.insert(it.key(), it.value().ids);
    return out;
}
