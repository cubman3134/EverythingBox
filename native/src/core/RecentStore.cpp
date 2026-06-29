#include "RecentStore.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

static const int kMaxRecents = 40;

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

// Recents are per-profile so each user's home content is exclusive to them.
static QString recentsKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("recent/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/items");
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
        if (!it.path.isEmpty()) out.push_back(it);
    }
    return out;
}

void RecentStore::add(const RecentItem& item)
{
    if (item.path.isEmpty()) return;
    QVector<RecentItem> items = list();
    // De-dup by stable key when present (a streamed item's path/URL changes per session), else by path.
    const QString ident = item.key.isEmpty() ? item.path : item.key;
    for (int i = items.size() - 1; i >= 0; --i)
    {
        const QString other = items[i].key.isEmpty() ? items[i].path : items[i].key;
        if (other == ident) items.remove(i);
    }
    items.prepend(item);                               // newest first
    while (items.size() > kMaxRecents) items.removeLast();

    QJsonArray arr;
    for (const RecentItem& it : items)
    {
        QJsonObject o;
        o.insert(QStringLiteral("path"), it.path);
        o.insert(QStringLiteral("title"), it.title);
        o.insert(QStringLiteral("kind"), it.kind);
        if (!it.thumb.isEmpty()) o.insert(QStringLiteral("thumb"), it.thumb);
        if (!it.key.isEmpty())   o.insert(QStringLiteral("key"), it.key);
        arr.append(o);
    }
    store().setValue(recentsKey(),
                     QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

void RecentStore::remove(const QString& pathOrKey)
{
    if (pathOrKey.isEmpty()) return;
    QVector<RecentItem> items = list();
    bool changed = false;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].path == pathOrKey || (!items[i].key.isEmpty() && items[i].key == pathOrKey))
        { items.remove(i); changed = true; }
    if (!changed) return;

    QJsonArray arr;
    for (const RecentItem& it : items)
    {
        QJsonObject o;
        o.insert(QStringLiteral("path"), it.path);
        o.insert(QStringLiteral("title"), it.title);
        o.insert(QStringLiteral("kind"), it.kind);
        if (!it.thumb.isEmpty()) o.insert(QStringLiteral("thumb"), it.thumb);
        if (!it.key.isEmpty())   o.insert(QStringLiteral("key"), it.key);
        arr.append(o);
    }
    store().setValue(recentsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

void RecentStore::clear()
{
    store().remove(recentsKey());
    store().sync();
}
