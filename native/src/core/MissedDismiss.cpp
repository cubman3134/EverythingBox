#include "MissedDismiss.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "TraktMissed.h"   // missedDismissExpired — the expiry rule lives with the rest of the pure #25 layer

#include <QCryptographicHash>
#include <QHash>
#include <QSettings>
#include <QStringList>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture).
// Coherence with any other QSettings on the same file comes from every writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

const QLatin1String kRoot("missed/");

// "missed/<profileId>/shows" for a given id — one spelling, used by the live path and by the cross-profile
// sweep, so the two cannot look in different places.
QString showsGroupFor(const QString& profileId)
{
    return kRoot + (profileId.isEmpty() ? QStringLiteral("default") : profileId) + QStringLiteral("/shows");
}

// ItemMarks' hash, over ItemMarks' key space. Today's show keys are bare IMDB ids and would survive as ini
// leaves unhashed; hashing anyway is what makes that a property of the CALLER rather than a load-bearing
// assumption of the store.
QString hashKey(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

// ---- lazy cache, per ACTIVE profile ------------------------------------------------------------------
// planMissed asks once per show per rebuild, and the browse root rebuilds on every navigation, so the group
// resolution happens once per build rather than per call. Self-heals on a profile switch by the ItemMarks
// idiom: one currentId() read and a string compare.
bool                  mCacheBuilt = false;
QString               mCacheProfileId;
QHash<QString, qint64> mCache;      // showHash -> stamp

void ensureCache()
{
    const QString id = ProfileStore::currentId();
    if (mCacheBuilt && mCacheProfileId == id) return;

    mCache.clear();
    mCacheProfileId = id;
    QSettings& s = store();
    s.beginGroup(showsGroupFor(id));
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
    {
        // A row that is not a positive number is not a record. Reading it as 0 rather than dropping the
        // whole cache is the totality rule every other store here follows: one corrupt line must not cost
        // the user every dismissal they have made.
        const qint64 v = s.value(h).toString().toLongLong();
        if (v > 0) mCache.insert(h, v);
    }
    s.endGroup();
    mCacheBuilt = true;
}

} // namespace

qint64 MissedDismiss::through(const QString& showKey)
{
    if (showKey.isEmpty()) return 0;
    ensureCache();
    return mCache.value(hashKey(showKey), 0);
}

void MissedDismiss::dismissThrough(const QString& showKey, qint64 throughUnix)
{
    if (showKey.isEmpty()) return;
    ensureCache();
    const QString h = hashKey(showKey);
    // MONOTONE, and the early return is the mechanism rather than an optimisation: without it a stale
    // dismissal replayed by a peer would LOWER the stamp, un-dismissing episodes the user had already dealt
    // with, and the write would arm a push that carried the regression back out.
    //
    // It is also the whole of the "a non-positive stamp is not a record" rule, which is why there is no
    // second guard saying so: the cache holds nothing but positive values and defaults to 0, so any
    // throughUnix <= 0 fails this test against the floor and no row is ever created for it.
    if (mCache.value(h, 0) >= throughUnix) return;

    mCache.insert(h, throughUnix);
    store().setValue(showsGroupFor(mCacheProfileId) + QLatin1Char('/') + h, QString::number(throughUnix));
    store().sync();
    fireChanged();
}

int MissedDismiss::prune(qint64 nowUnix)
{
    QSettings& s = store();
    int removed = 0;
    // Across EVERY profile. The keys are read once, up front, because removing while iterating a live
    // allKeys() view is the kind of thing that works until the store is big enough to reallocate.
    const QStringList all = s.allKeys();
    for (const QString& k : all)
    {
        if (!k.startsWith(kRoot)) continue;
        // "missed/<profile>/shows/<hash>" and nothing else. A key of another shape under this root is
        // something a later version wrote and this one does not understand, so it is left alone rather than
        // swept up by a prefix match.
        if (k.section(QLatin1Char('/'), 2, 2) != QStringLiteral("shows")) continue;
        if (k.section(QLatin1Char('/'), 4).size() != 0) continue;
        if (!trakt::missedDismissExpired(s.value(k).toString().toLongLong(), nowUnix)) continue;
        s.remove(k);
        ++removed;
    }
    if (removed > 0) { s.sync(); invalidate(); }
    // Deliberately fires NO change hook. A collection is not an edit — every device reaches the same verdict
    // from its own clock, so pushing it would upload a document identical in meaning to the one already
    // there, and a peer that has not collected yet would only hand the row straight back.
    return removed;
}

void MissedDismiss::invalidate()
{
    mCacheBuilt = false;
    mCacheProfileId.clear();
    mCache.clear();
}

void MissedDismiss::setChangeHook(std::function<void()> hook)
{
    g_changeHook = std::move(hook);
}
