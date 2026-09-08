#include "JellyseerrStore.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QCoreApplication>
#include <QSettings>

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

// Per-profile, under the "jellyseerr/" prefix that CloudSync::isDeviceLocalKey carves out of the synced
// bundle — so the API key never leaves this machine. Spelled ONCE, here.
QString base()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("jellyseerr/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QLatin1Char('/');
}

std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

} // namespace

JellyseerrConfig JellyseerrStore::get()
{
    JellyseerrConfig c;
    const QString b = base();
    c.url            = store().value(b + QStringLiteral("url")).toString().trimmed();
    c.apiKey         = store().value(b + QStringLiteral("apiKey")).toString();
    c.allowPlainHttp = store().value(b + QStringLiteral("allowPlainHttp"), false).toBool();
    return c;
}

bool JellyseerrStore::isConfigured() { return get().configured(); }

void JellyseerrStore::save(const JellyseerrConfig& c)
{
    const QString b = base();
    // A HALF-SAVE IS A CLEAR. Storing a url with no key would leave the surface offering a Request action
    // that can only ever fail on press, and storing a key with no url leaves a credential behind for a
    // service nobody can reach — which is a credential on disk for no benefit at all.
    if (!c.configured()) { clear(); return; }
    store().setValue(b + QStringLiteral("url"), c.url.trimmed());
    store().setValue(b + QStringLiteral("apiKey"), c.apiKey);
    store().setValue(b + QStringLiteral("allowPlainHttp"), c.allowPlainHttp);
    store().sync();
    fireChanged();
}

void JellyseerrStore::clear()
{
    const QString b = base();
    store().remove(b + QStringLiteral("url"));
    store().remove(b + QStringLiteral("apiKey"));
    store().remove(b + QStringLiteral("allowPlainHttp"));
    store().sync();
    fireChanged();
}

QString JellyseerrStore::statusLine()
{
    // NAMES NOTHING. Not the address (a private LAN name is still information about somebody's network),
    // not the account, and obviously not the key. "Set up" or "not set up" is the whole of it.
    return isConfigured()
        ? QCoreApplication::translate("JellyseerrStore",
              "A request service is set up. Requests you make are kept on this device only.")
        : QCoreApplication::translate("JellyseerrStore",
              "No request service yet. Set one up to ask for films and series you do not have.");
}

void JellyseerrStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }

#ifdef EB_REQUESTS_TEST_SEAM
void JellyseerrStore::setIniPathForTesting(const QString& path)
{
    g_testIniPath = path;
    delete g_testStore;
    g_testStore = nullptr;
}
#endif
