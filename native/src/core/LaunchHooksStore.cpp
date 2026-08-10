#include "LaunchHooksStore.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture as
// LaunchOptionsStore). Coherence with any other QSettings on the same file comes from every writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

using LaunchHooksStore::Hooks;

const QLatin1String kItemsGroup("launchhooks/items");

QString itemKey(const QString& hash) { return kItemsGroup + QLatin1Char('/') + hash; }

// ---- lazy cache -------------------------------------------------------------------------------------------
// get() runs on the launch path (once per open), not per browse tile, so the cache is a modest win; it mirrors
// LaunchOptionsStore so the two stores read and invalidate identically.
bool                  mCacheBuilt = false;
QHash<QString, Hooks> mCache;      // itemHash -> Hooks (empty records are never stored, so a hit is real)

Hooks fromJson(const QJsonObject& o)
{
    Hooks h;
    h.preLaunch = o.value(QStringLiteral("preLaunch")).toString();
    h.postExit  = o.value(QStringLiteral("postExit")).toString();
    return h;
}

Hooks normalized(const Hooks& h)
{
    Hooks n;
    n.preLaunch = h.preLaunch.trimmed();
    n.postExit  = h.postExit.trimmed();
    return n;
}

void ensureCache()
{
    if (mCacheBuilt) return;
    mCache.clear();
    QSettings& s = store();
    s.beginGroup(kItemsGroup);
    const QStringList hashes = s.childKeys();
    for (const QString& hkey : hashes)
    {
        const Hooks h = fromJson(QJsonDocument::fromJson(s.value(hkey).toString().toUtf8()).object());
        if (h.isEmpty()) continue;   // a stale empty blob reads as "no hooks"
        mCache.insert(hkey, h);
    }
    s.endGroup();
    mCacheBuilt = true;
}

} // namespace

QString LaunchHooksStore::hashKey(const QString& key)
{
    // MD5-hex over UTF-8 — the scheme LaunchOptionsStore/ItemMarks/MetaOverrides use over the SAME key space, so
    // one game hashes the same way in every per-item store. Flattens '/' in urls/paths that QSettings would
    // otherwise read as group nesting.
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

LaunchHooksStore::Hooks LaunchHooksStore::get(const QString& key)
{
    if (key.isEmpty()) return Hooks{};
    ensureCache();
    return mCache.value(hashKey(key));
}

bool LaunchHooksStore::has(const QString& key)
{
    return !get(key).isEmpty();
}

void LaunchHooksStore::set(const QString& key, const Hooks& in)
{
    if (key.isEmpty()) return;
    const Hooks h = normalized(in);
    const QString k = itemKey(hashKey(key));

    if (h.isEmpty())
    {
        // Both hooks cleared: remove the row. Device-local + unsynced, so a plain delete is correct — there is
        // no peer copy to out-race, so no husk is needed (contrast LaunchOptionsStore, which DOES sync).
        if (store().contains(k)) { store().remove(k); store().sync(); invalidate(); }
        return;
    }

    QJsonObject o;
    o.insert(QStringLiteral("preLaunch"), h.preLaunch);
    o.insert(QStringLiteral("postExit"),  h.postExit);
    store().setValue(k, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    store().sync();
    invalidate();
}

void LaunchHooksStore::reset(const QString& key)
{
    set(key, Hooks{});   // both-empty -> row delete
}

void LaunchHooksStore::invalidate()
{
    mCacheBuilt = false;
    mCache.clear();
}
