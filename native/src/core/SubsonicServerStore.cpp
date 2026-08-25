#include "SubsonicServerStore.h"
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

// Per-profile, under the "subsonic/" prefix that CloudSync::isDeviceLocalKey carves out of the synced
// bundle — so a password never leaves this machine.
static QString srvKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("subsonic/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/servers");
}

static std::function<void()> g_changeHook;
void SubsonicServerStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QList<SubsonicServer> SubsonicServerStore::list()
{
    QList<SubsonicServer> out;
    const QByteArray json = store().value(srvKey()).toString().toUtf8();
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        SubsonicServer s;
        s.id             = o.value(QStringLiteral("id")).toString();
        s.name           = o.value(QStringLiteral("name")).toString();
        s.url            = o.value(QStringLiteral("url")).toString();
        s.username       = o.value(QStringLiteral("username")).toString();
        s.password       = o.value(QStringLiteral("password")).toString();
        s.allowPlainHttp = o.value(QStringLiteral("allowPlainHttp")).toBool();
        s.legacyAuth     = o.value(QStringLiteral("legacyAuth")).toBool();
        // An id that is not a uuid could never have qualified an id (Subsonic::parse refuses it), so a row
        // carrying one is unusable rather than merely odd — drop it here rather than let it mint keys that
        // nothing can resolve.
        if (s.id.isEmpty() || QUuid::fromString(s.id).isNull()) continue;
        out.push_back(s);
    }
    return out;
}

bool SubsonicServerStore::hasServers()
{
    // The Music tab gate. One string read plus a parse of a handful of objects — see the header for why
    // that bound is not an accident.
    return !list().isEmpty();
}

static void saveAll(const QList<SubsonicServer>& all)
{
    QJsonArray arr;
    for (const SubsonicServer& s : all)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("url"), s.url);
        o.insert(QStringLiteral("username"), s.username);
        o.insert(QStringLiteral("password"), s.password);
        o.insert(QStringLiteral("allowPlainHttp"), s.allowPlainHttp);
        o.insert(QStringLiteral("legacyAuth"), s.legacyAuth);
        arr.append(o);
    }
    store().setValue(srvKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged();
}

QString SubsonicServerStore::add(const SubsonicServer& srv)
{
    SubsonicServer s = srv;
    if (s.id.isEmpty()) s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<SubsonicServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == s.id) { all[i] = s; saveAll(all); return s.id; }
    all.push_back(s);
    saveAll(all);
    return s.id;
}

void SubsonicServerStore::update(const SubsonicServer& srv)
{
    if (srv.id.isEmpty()) return;
    QList<SubsonicServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == srv.id) { all[i] = srv; saveAll(all); return; }
}

void SubsonicServerStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QList<SubsonicServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == id) { all.removeAt(i); saveAll(all); return; }
}

bool SubsonicServerStore::get(const QString& id, SubsonicServer& out)
{
    for (const SubsonicServer& s : list())
        if (s.id == id) { out = s; return true; }
    return false;
}
