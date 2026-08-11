#include "IptvSourceStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, so each user has their own Live TV sources — the same keying FavoritesStore/PlaylistStore use.
static QString srcKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("iptv/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/sources");
}

// Change-callback (mdsync T2 parity): fired after a mutation to (re)arm the debounced Drive push; null in probes.
static std::function<void()> g_changeHook;
void IptvSourceStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QList<IptvSource> IptvSourceStore::list()
{
    QList<IptvSource> out;
    const QByteArray json = store().value(srcKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        IptvSource s;
        s.id     = o.value(QStringLiteral("id")).toString();
        s.name   = o.value(QStringLiteral("name")).toString();
        s.url    = o.value(QStringLiteral("url")).toString();
        s.epgUrl = o.value(QStringLiteral("epgUrl")).toString();
        if (!s.id.isEmpty()) out.push_back(s);
    }
    return out;
}

static void saveAll(const QList<IptvSource>& all)
{
    QJsonArray arr;
    for (const IptvSource& s : all)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("url"), s.url);
        // Written even when empty (the increment-2 case): its PRESENCE is the reserved slot increment 3 fills,
        // and round-tripping it now is what lets that increment skip a store migration.
        o.insert(QStringLiteral("epgUrl"), s.epgUrl);
        arr.append(o);
    }
    store().setValue(srcKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged();
}

QString IptvSourceStore::add(const IptvSource& src)
{
    IptvSource s = src;
    if (s.id.isEmpty()) s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<IptvSource> all = list();
    // De-dup by id: a re-add carrying an existing id updates that source in place rather than duplicating it.
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == s.id) { all[i] = s; saveAll(all); return s.id; }
    all.push_back(s);
    saveAll(all);
    return s.id;
}

void IptvSourceStore::update(const IptvSource& src)
{
    if (src.id.isEmpty()) return;
    QList<IptvSource> all = list();
    bool changed = false;
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == src.id) { all[i] = src; changed = true; break; }
    if (changed) saveAll(all);
}

void IptvSourceStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QList<IptvSource> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == id) { all.removeAt(i); saveAll(all); return; }
}

bool IptvSourceStore::get(const QString& id, IptvSource& out)
{
    for (const IptvSource& s : list())
        if (s.id == id) { out = s; return true; }
    return false;
}
