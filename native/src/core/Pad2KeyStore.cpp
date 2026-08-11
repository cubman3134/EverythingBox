#include "Pad2KeyStore.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
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

using Pad2KeyStore::Entry;

std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

const QLatin1String kItemsGroup("pad2key/items");
QString itemKey(const QString& hash) { return kItemsGroup + QLatin1Char('/') + hash; }

// ---- lazy cache (mirrors LaunchOptionsStore exactly so the two read and invalidate identically) -------------
bool                  mCacheBuilt = false;
QHash<QString, Entry> mCache;   // itemHash -> Entry (husks are NOT cached; they read as absent)

Entry entryFromJson(const QJsonObject& o)
{
    Entry e;
    e.enabled   = o.value(QStringLiteral("enabled")).toBool(false);
    e.profile   = pad2key::fromJson(o.value(QStringLiteral("profile")).toObject());
    e.updatedAt = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
    return e;
}

// ONE canonical spelling: `enabled` written only when true, `profile` only when non-empty. Two devices that
// authored the same entry therefore produce byte-identical records, so CloudMerge's equal-timestamp tie-break
// sees no difference and neither device flips the other's copy.
QJsonObject entryToJson(const Entry& e)
{
    QJsonObject o;
    if (e.enabled)            o.insert(QStringLiteral("enabled"), true);
    if (!e.profile.isEmpty()) o.insert(QStringLiteral("profile"), pad2key::toJson(e.profile));
    o.insert(QStringLiteral("updatedAt"), static_cast<double>(e.updatedAt));
    return o;
}

void ensureCache()
{
    if (mCacheBuilt) return;
    mCache.clear();
    QSettings& s = store();
    s.beginGroup(kItemsGroup);
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
    {
        const Entry e = entryFromJson(QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
        if (e.isEmpty()) continue;   // a clear husk: present for the merge, but nothing to apply
        mCache.insert(h, e);
    }
    s.endGroup();
    mCacheBuilt = true;
}

// "Did this set() actually change the stored record?" — compares the user levers (enabled + the profile's
// canonical JSON) and DELIBERATELY ignores updatedAt. Comparing the serialised profile is the exact,
// order-independent content check (a QHash has no operator== that helps here). Mirrors LaunchOpts::contentEqual.
bool contentEqual(const Entry& a, const Entry& b)
{
    if (a.enabled != b.enabled) return false;
    return pad2key::toJson(a.profile) == pad2key::toJson(b.profile);
}

Entry normalized(const Entry& in)
{
    Entry n = in;
    n.profile.name = in.profile.name.trimmed();
    return n;
}

} // namespace

QString Pad2KeyStore::hashKey(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

Pad2KeyStore::Entry Pad2KeyStore::get(const QString& key)
{
    if (key.isEmpty()) return Entry{};
    ensureCache();
    return mCache.value(hashKey(key));
}

bool Pad2KeyStore::has(const QString& key)     { return !get(key).isEmpty(); }
bool Pad2KeyStore::enabled(const QString& key) { return get(key).enabled; }

// The stamp is gated on a REAL content change, and a husk is only ever left where a record existed to clear —
// LaunchOptionsStore::set carries the same two-halves rule and its header explains why. An all-empty write on an
// un-configured game writes nothing; a byte-equal write does not restamp (issue #167).
void Pad2KeyStore::set(const QString& key, const Entry& in)
{
    if (key.isEmpty()) return;
    Entry e = normalized(in);
    const QString k = itemKey(hashKey(key));

    const Entry stored = store().contains(k)
        ? entryFromJson(QJsonDocument::fromJson(store().value(k).toString().toUtf8()).object())
        : Entry{};
    if (contentEqual(e, stored)) return;                 // byte-equal write: no stamp, no husk, no push

    e.updatedAt = QDateTime::currentSecsSinceEpoch();
    store().setValue(k, QString::fromUtf8(QJsonDocument(entryToJson(e)).toJson(QJsonDocument::Compact)));
    store().sync();
    invalidate();
    fireChanged();
}

void Pad2KeyStore::setEnabled(const QString& key, bool on)
{
    Entry e = get(key);          // keep any custom profile the game already carries
    e.enabled = on;
    set(key, e);
}

void Pad2KeyStore::reset(const QString& key)
{
    // NOT store().remove(): a deleted row reads as "never seen", so a peer holding the old record would
    // resurrect it on merge. The husk is a newer, empty record that wins and carries the clear.
    set(key, Entry{});
}

pad2key::Profile Pad2KeyStore::effectiveProfile(const QString& key, const QString& systemId)
{
    const Entry e = get(key);
    if (!e.profile.isEmpty()) return e.profile;          // a custom profile wins
    return pad2key::defaultProfile(systemId);            // else the per-system default (empty for most systems)
}

void Pad2KeyStore::invalidate()
{
    mCacheBuilt = false;
    mCache.clear();
}

void Pad2KeyStore::setChangeHook(std::function<void()> hook)
{
    g_changeHook = std::move(hook);
}
