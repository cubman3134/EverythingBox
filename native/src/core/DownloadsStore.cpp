#include "DownloadsStore.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QSettings>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile), QSettings::IniFormat);
    return s;
}

// Global (not per-profile): a downloaded file on disk is usable by any profile.
static QString downloadsKey() { return QStringLiteral("downloads/items"); }

QVector<DownloadedItem> DownloadsStore::list()
{
    QVector<DownloadedItem> out;
    const QByteArray json = store().value(downloadsKey()).toString().toUtf8();
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        DownloadedItem it;
        it.path   = o.value(QStringLiteral("path")).toString();
        it.title  = o.value(QStringLiteral("title")).toString();
        it.kind   = o.value(QStringLiteral("kind")).toString();
        it.thumb  = o.value(QStringLiteral("thumb")).toString();
        it.key    = o.value(QStringLiteral("key")).toString();
        it.system = o.value(QStringLiteral("system")).toString();
        it.form   = o.value(QStringLiteral("form")).toString();
        if (!it.path.isEmpty()) out.push_back(it);
    }
    return out;
}

static void save(const QVector<DownloadedItem>& items)
{
    QJsonArray arr;
    for (const DownloadedItem& it : items)
    {
        QJsonObject o;
        o.insert(QStringLiteral("path"), it.path);
        o.insert(QStringLiteral("title"), it.title);
        o.insert(QStringLiteral("kind"), it.kind);
        if (!it.thumb.isEmpty())  o.insert(QStringLiteral("thumb"), it.thumb);
        if (!it.key.isEmpty())    o.insert(QStringLiteral("key"), it.key);
        if (!it.system.isEmpty()) o.insert(QStringLiteral("system"), it.system);
        if (!it.form.isEmpty())   o.insert(QStringLiteral("form"), it.form);
        arr.append(o);
    }
    store().setValue(downloadsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

void DownloadsStore::add(const DownloadedItem& item)
{
    if (item.path.isEmpty()) return;
    QVector<DownloadedItem> items = list();
    const QString ident = item.key.isEmpty() ? item.path : item.key;
    for (int i = items.size() - 1; i >= 0; --i)
    {
        const QString other = items[i].key.isEmpty() ? items[i].path : items[i].key;
        if (other == ident) items.remove(i);
    }
    items.prepend(item); // newest first (uncapped: a downloads library is meant to be complete)
    save(items);
}

void DownloadsStore::remove(const QString& pathOrKey)
{
    if (pathOrKey.isEmpty()) return;
    QVector<DownloadedItem> items = list();
    bool changed = false;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].path == pathOrKey || (!items[i].key.isEmpty() && items[i].key == pathOrKey))
        { items.remove(i); changed = true; }
    if (changed) save(items);
}
