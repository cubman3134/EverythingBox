#include "EmuGfxStore.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture as
// LaunchOptionsStore / LaunchHooksStore). Coherence with any other QSettings on the same file comes from every
// writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

const QLatin1String kItemsGroup("emugfx/items");

QString itemKey(const QString& hash) { return kItemsGroup + QLatin1Char('/') + hash; }

// ---- lazy cache -------------------------------------------------------------------------------------------
// get() runs on the launch path (once per open), not per browse tile, so the cache is a modest win; it mirrors
// LaunchOptionsStore / LaunchHooksStore so the three stores read and invalidate identically.
bool                             mCacheBuilt = false;
QHash<QString, EmuGfx::Settings> mCache;      // itemHash -> Settings (empty records are never stored, so a hit is real)

void ensureCache()
{
    if (mCacheBuilt) return;
    mCache.clear();
    QSettings& s = store();
    s.beginGroup(kItemsGroup);
    const QStringList hashes = s.childKeys();
    for (const QString& hkey : hashes)
    {
        const EmuGfx::Settings st =
            EmuGfx::fromJson(QJsonDocument::fromJson(s.value(hkey).toString().toUtf8()).object());
        if (st.isEmpty()) continue;   // a stale empty blob reads as "no override"
        mCache.insert(hkey, st);
    }
    s.endGroup();
    mCacheBuilt = true;
}

} // namespace

QString EmuGfxStore::hashKey(const QString& key)
{
    // MD5-hex over UTF-8 — the scheme LaunchOptionsStore / LaunchHooksStore / ItemMarks / MetaOverrides use over
    // the SAME key space, so one game hashes the same way in every per-item store. Flattens '/' in urls/paths
    // that QSettings would otherwise read as group nesting.
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString EmuGfxStore::systemKey(const QString& systemId)
{
    // A reserved key for a system-wide default. The leading U+0001 (a control byte) can never appear in a real
    // game identity (a catalog id or a file path), so a system default and a game override never collide in the
    // shared hash space, and the per-system record hashes deterministically like any other.
    return QStringLiteral("\x01emugfx-system:") + systemId;
}

EmuGfx::Settings EmuGfxStore::get(const QString& key)
{
    if (key.isEmpty()) return EmuGfx::Settings{};
    ensureCache();
    return mCache.value(hashKey(key));
}

bool EmuGfxStore::has(const QString& key)
{
    return !get(key).isEmpty();
}

void EmuGfxStore::set(const QString& key, const EmuGfx::Settings& in)
{
    if (key.isEmpty()) return;
    const QString k = itemKey(hashKey(key));

    if (in.isEmpty())
    {
        // Every lever cleared: remove the row. Device-local + unsynced, so a plain delete is correct — there is
        // no peer copy to out-race, so no husk is needed (contrast LaunchOptionsStore, which DOES sync).
        if (store().contains(k)) { store().remove(k); store().sync(); invalidate(); }
        return;
    }

    store().setValue(k, QString::fromUtf8(QJsonDocument(EmuGfx::toJson(in)).toJson(QJsonDocument::Compact)));
    store().sync();
    invalidate();
}

void EmuGfxStore::reset(const QString& key)
{
    set(key, EmuGfx::Settings{});   // all-unset -> row delete
}

EmuGfx::Settings EmuGfxStore::systemDefault(const QString& systemId)
{
    if (systemId.isEmpty()) return EmuGfx::Settings{};
    return get(systemKey(systemId));
}

void EmuGfxStore::invalidate()
{
    mCacheBuilt = false;
    mCache.clear();
}
