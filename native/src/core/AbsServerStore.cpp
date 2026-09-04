#include "AbsServerStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUuid>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, under the "audiobookshelf/" prefix that CloudSync::isDeviceLocalKey carves out of the
// synced bundle — so the API token never leaves this machine. Spelled ONCE, here.
static QString srvKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("audiobookshelf/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/servers");
}

static std::function<void()> g_changeHook;
void AbsServerStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QList<AbsServer> AbsServerStore::list()
{
    QList<AbsServer> out;
    const QByteArray json = store().value(srvKey()).toString().toUtf8();
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        AbsServer s;
        s.id             = o.value(QStringLiteral("id")).toString();
        s.name           = o.value(QStringLiteral("name")).toString();
        s.url            = o.value(QStringLiteral("url")).toString();
        s.username       = o.value(QStringLiteral("username")).toString();
        s.token          = o.value(QStringLiteral("token")).toString();
        // Absent means ON: a row written before this field existed is a server the user set up and expects
        // to see, and defaulting it off would make an upgrade look like a data loss.
        s.enabled        = o.contains(QStringLiteral("enabled"))
                               ? o.value(QStringLiteral("enabled")).toBool() : true;
        s.allowPlainHttp = o.value(QStringLiteral("allowPlainHttp")).toBool();
        // An id carrying a scheme separator could never have qualified anything (Abs::qualify refuses it),
        // so a row holding one is unusable rather than merely odd — drop it here rather than let it mint
        // keys nothing can resolve.
        if (s.id.isEmpty() || s.id.contains(QLatin1Char(':')) || s.id.contains(QLatin1Char('#'))) continue;
        out.push_back(s);
    }
    return out;
}

QList<AbsServer> AbsServerStore::enabledList()
{
    QList<AbsServer> out;
    for (const AbsServer& s : list()) if (s.enabled) out.push_back(s);
    return out;
}

bool AbsServerStore::hasServers()
{
    return !list().isEmpty();
}

static void saveAll(const QList<AbsServer>& all)
{
    QJsonArray arr;
    for (const AbsServer& s : all)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("url"), s.url);
        o.insert(QStringLiteral("username"), s.username);
        o.insert(QStringLiteral("token"), s.token);
        o.insert(QStringLiteral("enabled"), s.enabled);
        o.insert(QStringLiteral("allowPlainHttp"), s.allowPlainHttp);
        arr.append(o);
    }
    store().setValue(srvKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged();
}

QString AbsServerStore::add(const AbsServer& srv)
{
    AbsServer s = srv;
    if (s.id.isEmpty()) s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<AbsServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == s.id) { all[i] = s; saveAll(all); return s.id; }
    all.push_back(s);
    saveAll(all);
    return s.id;
}

void AbsServerStore::update(const AbsServer& srv)
{
    if (srv.id.isEmpty()) return;
    QList<AbsServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == srv.id) { all[i] = srv; saveAll(all); return; }
}

void AbsServerStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QList<AbsServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == id) { all.removeAt(i); saveAll(all); return; }
}

bool AbsServerStore::get(const QString& id, AbsServer& out)
{
    for (const AbsServer& s : list())
        if (s.id == id) { out = s; return true; }
    return false;
}
