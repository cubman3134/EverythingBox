#include "ShaderPresetStore.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QHash>
#include <QSettings>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture as
// EmuGfxStore / LaunchOptionsStore). Coherence with any other QSettings on the same file comes from every writer
// calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

const QLatin1String kItemsGroup("shaderpreset/items");

QString itemKey(const QString& hash) { return kItemsGroup + QLatin1Char('/') + hash; }

// ---- lazy cache -------------------------------------------------------------------------------------------
// get() runs on the launch path (once per open), not per browse tile, so the cache is a modest win; it mirrors
// EmuGfxStore so the stores read and invalidate identically.
bool                     mCacheBuilt = false;
QHash<QString, QString>  mCache;      // itemHash -> preset id (empty records are never stored, so a hit is real)

void ensureCache()
{
    if (mCacheBuilt) return;
    mCache.clear();
    QSettings& s = store();
    s.beginGroup(kItemsGroup);
    const QStringList hashes = s.childKeys();
    for (const QString& hkey : hashes)
    {
        const QString id = s.value(hkey).toString();
        if (id.isEmpty()) continue;   // a stale empty value reads as "no override"
        mCache.insert(hkey, id);
    }
    s.endGroup();
    mCacheBuilt = true;
}

} // namespace

QString ShaderPresetStore::hashKey(const QString& key)
{
    // MD5-hex over UTF-8 — the scheme EmuGfxStore / LaunchOptionsStore / ItemMarks use over the SAME key space,
    // so one game hashes the same way in every per-item store. Flattens '/' in urls/paths that QSettings would
    // otherwise read as group nesting.
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString ShaderPresetStore::systemKey(const QString& systemId)
{
    // A reserved key for a system-wide default. The leading U+0001 (a control byte) can never appear in a real
    // game identity (a catalog id or a file path), so a system default and a game override never collide in the
    // shared hash space, and the per-system record hashes deterministically like any other.
    return QStringLiteral("\x01shaderpreset-system:") + systemId;
}

QString ShaderPresetStore::get(const QString& key)
{
    if (key.isEmpty()) return QString();
    ensureCache();
    return mCache.value(hashKey(key));
}

bool ShaderPresetStore::has(const QString& key)
{
    return !get(key).isEmpty();
}

void ShaderPresetStore::set(const QString& key, const QString& presetId)
{
    if (key.isEmpty()) return;
    const QString k = itemKey(hashKey(key));

    if (presetId.isEmpty())
    {
        // Empty id: remove the row. Device-local + unsynced, so a plain delete is correct — there is no peer copy
        // to out-race, so no husk is needed (contrast LaunchOptionsStore, which DOES sync).
        if (store().contains(k)) { store().remove(k); store().sync(); invalidate(); }
        return;
    }

    store().setValue(k, presetId);
    store().sync();
    invalidate();
}

void ShaderPresetStore::reset(const QString& key)
{
    set(key, QString());   // empty id -> row delete
}

QString ShaderPresetStore::systemDefault(const QString& systemId)
{
    if (systemId.isEmpty()) return QString();
    return get(systemKey(systemId));
}

void ShaderPresetStore::invalidate()
{
    mCacheBuilt = false;
    mCache.clear();
}
