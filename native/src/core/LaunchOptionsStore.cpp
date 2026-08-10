#include "LaunchOptionsStore.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QSettings>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture).
// Coherence with any other QSettings on the same file comes from every writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

using LaunchOpts::Override;

// Change-callback (mdsync T2 contract): fired after every mutation to (re)arm the debounced Drive push.
std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

const QLatin1String kItemsGroup("launchopts/items");

QString itemKey(const QString& hash) { return kItemsGroup + QLatin1Char('/') + hash; }

// ---- lazy cache -------------------------------------------------------------------------------------------
// get() runs on the launch path (once per open) rather than per browse tile, so the cache is a modest win, but
// it mirrors MetaOverrides exactly so the two stores read and invalidate identically. No profile dimension to
// self-heal: the store is global.
bool                     mCacheBuilt = false;
QHash<QString, Override> mCache;      // itemHash -> Override (husks are NOT cached; they read as absent)

void ensureCache()
{
    if (mCacheBuilt) return;
    mCache.clear();
    QSettings& s = store();
    s.beginGroup(kItemsGroup);
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
    {
        const Override ov = LaunchOpts::fromJson(
            QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
        if (ov.isEmpty()) continue;  // a clear husk: present for the merge, but nothing to apply
        mCache.insert(h, ov);
    }
    s.endGroup();
    mCacheBuilt = true;
}

// The "did this set() actually change the stored record?" decision. Compares the three user levers and
// DELIBERATELY ignores updatedAt — the question is whether the CONTENT changed, not whether a stamp differs.
// Both sides are canonical when this is called (normalized incoming, fromJson stored), so field equality is
// exact. Mirrors MetaOverrides::contentEqual (issue #167).
bool contentEqual(const Override& a, const Override& b)
{
    return a.core == b.core && a.emulatorId == b.emulatorId && a.extraArgs == b.extraArgs;
}

} // namespace

bool Override::isEmpty() const
{
    return core.isEmpty() && emulatorId.isEmpty() && extraArgs.isEmpty();
}

// ---- pure: canonical record <-> JSON ----------------------------------------------------------------------

Override LaunchOpts::fromJson(const QJsonObject& o)
{
    Override ov;
    ov.core       = o.value(QStringLiteral("core")).toString();
    ov.emulatorId = o.value(QStringLiteral("emulatorId")).toString();
    ov.extraArgs  = o.value(QStringLiteral("extraArgs")).toString();
    ov.updatedAt  = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
    return ov;
}

Override LaunchOpts::normalized(const Override& ov)
{
    Override n;
    n.core       = ov.core.trimmed();
    n.emulatorId = ov.emulatorId.trimmed();
    n.extraArgs  = ov.extraArgs.trimmed();
    n.updatedAt  = ov.updatedAt;
    return n;
}

QJsonObject LaunchOpts::toJson(const Override& in)
{
    // ONE canonical spelling: trimmed, and an unset lever is ABSENT (never ""). Two devices that made the same
    // override therefore produce byte-identical records, so CloudMerge's equal-timestamp tie-break sees no
    // difference and neither device flips the other's copy.
    const Override ov = normalized(in);
    QJsonObject o;
    if (!ov.core.isEmpty())       o.insert(QStringLiteral("core"), ov.core);
    if (!ov.emulatorId.isEmpty()) o.insert(QStringLiteral("emulatorId"), ov.emulatorId);
    if (!ov.extraArgs.isEmpty())  o.insert(QStringLiteral("extraArgs"), ov.extraArgs);
    o.insert(QStringLiteral("updatedAt"), static_cast<double>(ov.updatedAt));
    return o;
}

// ---- pure resolution — the mutation-tested heart ----------------------------------------------------------

QString LaunchOpts::resolveCore(const QString& baseCore, const Override& ov, const QStringList& candidateCores)
{
    // The override core wins ONLY when it is a current candidate. A blank override, or one naming a core the
    // system no longer offers (a data-file edit removed it, a stale sync), falls back to the default — silently,
    // because the alternative is refusing to launch a game over a setting the user can't see to fix.
    if (!ov.core.isEmpty() && candidateCores.contains(ov.core)) return ov.core;
    return baseCore;
}

QString LaunchOpts::resolveEmulatorId(const QString& baseId, const Override& ov)
{
    return ov.emulatorId.isEmpty() ? baseId : ov.emulatorId;
}

QString LaunchOpts::appendExtraArgs(const QString& resolvedArgs, const QString& extra)
{
    const QString e = extra.trimmed();
    if (e.isEmpty()) return resolvedArgs;           // no override -> byte-for-byte today's args
    if (resolvedArgs.isEmpty()) return e;
    QString out = resolvedArgs;
    if (!out.endsWith(QLatin1Char(' '))) out += QLatin1Char(' ');
    return out + e;                                 // exactly one space between the template and the extra
}

// ---- store ------------------------------------------------------------------------------------------------

QString LaunchOpts::hashKey(const QString& key)
{
    // MD5-hex over UTF-8 — ItemMarks'/MetaOverrides' scheme over the SAME key space, so the same game hashes the
    // same way in every per-item store. Flattens urls/paths whose '/' QSettings would read as group nesting.
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

Override LaunchOpts::get(const QString& key)
{
    if (key.isEmpty()) return Override{};
    ensureCache();
    return mCache.value(hashKey(key));
}

bool LaunchOpts::has(const QString& key)
{
    return !get(key).isEmpty();
}

// The stamp is gated on a REAL content change, and a husk is only ever left where a record existed to clear —
// the two halves of the rule stated next to CloudMerge::remoteReplaces, MetaOverrides::set carries them
// verbatim and the header explains why. An all-empty write on an un-overridden game writes nothing (no husk,
// no stamp, no push); a byte-equal write is a no-op that does not restamp (issue #167).
void LaunchOpts::set(const QString& key, const Override& in)
{
    if (key.isEmpty()) return;
    Override ov = normalized(in);
    const QString k = itemKey(hashKey(key));

    const Override stored = store().contains(k)
        ? fromJson(QJsonDocument::fromJson(store().value(k).toString().toUtf8()).object())
        : Override{};
    if (contentEqual(ov, stored)) return;              // byte-equal write: no stamp, no husk, no push

    ov.updatedAt = QDateTime::currentSecsSinceEpoch(); // a real change: the merge funnel bumps the stamp
    store().setValue(k, QString::fromUtf8(QJsonDocument(toJson(ov)).toJson(QJsonDocument::Compact)));
    store().sync();
    invalidate();
    fireChanged();
}

void LaunchOpts::reset(const QString& key)
{
    // NOT store().remove(): a deleted row reads as "never seen", so a peer holding the old override would
    // resurrect it on merge. The husk is a newer, empty record that wins and carries the clear. Resetting a
    // game that carries no override writes nothing at all — the guard in set().
    set(key, Override{});
}

void LaunchOpts::invalidate()
{
    mCacheBuilt = false;
    mCache.clear();
}

void LaunchOpts::setChangeHook(std::function<void()> hook)
{
    g_changeHook = std::move(hook);
}
