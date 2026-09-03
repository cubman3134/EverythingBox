#include "JellyfinServerStore.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "Jellyfin.h"
#include "ProfileStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

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

// Per-profile, under the "jellyfin/" prefix that CloudSync::isDeviceLocalKey carves out of the synced
// bundle — so an access token never leaves this machine.
QString srvKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("jellyfin/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/servers");
}

std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

} // namespace

void JellyfinServerStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }

QList<JellyfinServer> JellyfinServerStore::list()
{
    QList<JellyfinServer> out;
    const QByteArray json = store().value(srvKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        JellyfinServer s;
        s.id             = o.value(QStringLiteral("id")).toString();
        s.name           = o.value(QStringLiteral("name")).toString();
        s.url            = o.value(QStringLiteral("url")).toString();
        s.userId         = o.value(QStringLiteral("userId")).toString();
        s.userName       = o.value(QStringLiteral("userName")).toString();
        s.token          = o.value(QStringLiteral("token")).toString();
        s.allowPlainHttp = o.value(QStringLiteral("allowPlainHttp")).toBool();
        // ABSENT MEANS ENABLED. A row written before this leaf existed must not read back as switched off —
        // that would hide a working server's whole library with nothing on screen to say why.
        s.enabled        = o.value(QStringLiteral("enabled")).toBool(true);
        // An id that is not a server id could never have qualified a row (Jellyfin::qualify refuses it), so
        // a row carrying one is unusable rather than merely odd — drop it here rather than let it mint keys
        // that nothing can resolve. The Subsonic store applies the same rule to its uuids.
        if (!Jellyfin::isServerId(s.id)) continue;
        out.push_back(s);
    }
    return out;
}

QList<JellyfinServer> JellyfinServerStore::enabled()
{
    QList<JellyfinServer> out;
    for (const JellyfinServer& s : list())
        if (s.enabled) out.push_back(s);
    return out;
}

QStringList JellyfinServerStore::ids()
{
    QStringList out;
    for (const JellyfinServer& s : list()) out << s.id;
    return out;
}

bool JellyfinServerStore::hasServers() { return !list().isEmpty(); }

static void saveAll(const QList<JellyfinServer>& all)
{
    QJsonArray arr;
    for (const JellyfinServer& s : all)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("url"), s.url);
        o.insert(QStringLiteral("userId"), s.userId);
        o.insert(QStringLiteral("userName"), s.userName);
        o.insert(QStringLiteral("token"), s.token);
        o.insert(QStringLiteral("enabled"), s.enabled);
        o.insert(QStringLiteral("allowPlainHttp"), s.allowPlainHttp);
        arr.append(o);
    }
    store().setValue(srvKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged();
}

bool JellyfinServerStore::add(const JellyfinServer& srv)
{
    // No identity, no server. See the header: a row we cannot qualify is worse than no row at all, because
    // it looks like it worked and then files everything it touches under keys nothing resolves.
    if (!Jellyfin::isServerId(srv.id)) return false;
    QList<JellyfinServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == srv.id) { all[i] = srv; saveAll(all); return true; }
    all.push_back(srv);
    saveAll(all);
    return true;
}

void JellyfinServerStore::update(const JellyfinServer& srv)
{
    if (srv.id.isEmpty()) return;
    QList<JellyfinServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == srv.id) { all[i] = srv; saveAll(all); return; }
}

void JellyfinServerStore::setEnabled(const QString& id, bool on)
{
    QList<JellyfinServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == id) { all[i].enabled = on; saveAll(all); return; }
}

void JellyfinServerStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QList<JellyfinServer> all = list();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id == id)
        {
            // The token goes with the row, in the same write. Nothing else is touched: the stored rows under
            // this server's qualified ids stay exactly where they are (the header says why).
            all.removeAt(i);
            saveAll(all);
            return;
        }
}

bool JellyfinServerStore::get(const QString& id, JellyfinServer& out)
{
    for (const JellyfinServer& s : list())
        if (s.id == id) { out = s; return true; }
    return false;
}

#ifdef EB_JELLYFIN_TEST_SEAM
void JellyfinServerStore::setIniPathForTesting(const QString& path)
{
    g_testIniPath = path;
    delete g_testStore;
    g_testStore = nullptr;
}
#endif
