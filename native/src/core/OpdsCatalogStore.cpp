#include "OpdsCatalogStore.h"
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

// Per-profile, under the "opds/" prefix that CloudSync::isDeviceLocalKey carves out of the synced bundle —
// so a credential-bearing catalog url never leaves this machine.
static QString catKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("opds/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/catalogs");
}

static std::function<void()> g_changeHook;
void OpdsCatalogStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QList<OpdsCatalog> OpdsCatalogStore::list()
{
    QList<OpdsCatalog> out;
    const QByteArray json = store().value(catKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        OpdsCatalog c;
        c.id       = o.value(QStringLiteral("id")).toString();
        c.name     = o.value(QStringLiteral("name")).toString();
        c.url      = o.value(QStringLiteral("url")).toString();
        c.username = o.value(QStringLiteral("username")).toString();
        c.password = o.value(QStringLiteral("password")).toString();
        if (!c.id.isEmpty()) out.push_back(c);
    }
    return out;
}

static void saveAll(const QList<OpdsCatalog>& all)
{
    QJsonArray arr;
    for (const OpdsCatalog& c : all)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), c.id);
        o.insert(QStringLiteral("name"), c.name);
        o.insert(QStringLiteral("url"), c.url);
        o.insert(QStringLiteral("username"), c.username);
        o.insert(QStringLiteral("password"), c.password);
        arr.append(o);
    }
    store().setValue(catKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged();
}

QString OpdsCatalogStore::add(const OpdsCatalog& cat)
{
    OpdsCatalog c = cat;
    if (c.id.isEmpty()) c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<OpdsCatalog> all = list();
    // De-dup by id: a re-add carrying an existing id updates that catalog in place rather than duplicating it.
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == c.id) { all[i] = c; saveAll(all); return c.id; }
    all.push_back(c);
    saveAll(all);
    return c.id;
}

void OpdsCatalogStore::update(const OpdsCatalog& cat)
{
    if (cat.id.isEmpty()) return;
    QList<OpdsCatalog> all = list();
    bool changed = false;
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == cat.id) { all[i] = cat; changed = true; break; }
    if (changed) saveAll(all);
}

void OpdsCatalogStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QList<OpdsCatalog> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == id) { all.removeAt(i); saveAll(all); return; }
}

bool OpdsCatalogStore::get(const QString& id, OpdsCatalog& out)
{
    for (const OpdsCatalog& c : list())
        if (c.id == id) { out = c; return true; }
    return false;
}
