#include "LocalResolveCache.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

void LocalResolveCache::load()
{
    byPath_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
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
}

void LocalResolveCache::save() const
{
    QJsonObject root;
    for (auto it = byPath_.constBegin(); it != byPath_.constEnd(); ++it)
    {
        const Entry& e = it.value();
        QJsonArray ids; for (const QString& s : e.ids) ids.append(s);
        root.insert(it.key(), QJsonObject{
            { QStringLiteral("size"), (double)e.size }, { QStringLiteral("mtime"), (double)e.mtime },
            { QStringLiteral("matched"), e.matched }, { QStringLiteral("ts"), (double)e.ts },
            { QStringLiteral("ids"), ids } });
    }
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
