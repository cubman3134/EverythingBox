#include "ConsumptionStats.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Settings.h"           // deviceId() — the accumulator namespace (mdsync T3)

#include <QSettings>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QHash>
#include <algorithm>

// Shares the portable mymediavault.ini with the other stores (same AppPaths::dataDir() posture as
// PlayStats/ItemMarks). Coherence with other QSettings on the same file comes from every writer calling
// sync(); QSettings reloads on access when the on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

namespace {

using ConsumptionStats::Totals;

// One cached per-title record. Mirrors the public Totals plus the item's category (kept internal — the public
// API surfaces category through the rollups/topTitles filter, not on Totals).
struct Entry
{
    qint64  mediaSeconds = 0;
    qint64  pagesRead    = 0;
    qint64  lastActivity = 0;
    QString title;
    QString category; // "video" | "audio" | "reading"
};

// Post-upgrade accumulator schema: 1 = device-namespaced (stats/<profile>/<deviceId>/...). A profile whose
// stats/<profile>/schema stamp is < this is still in the legacy un-namespaced shape and gets folded once.
static constexpr int kStatsSchema = 1;

// The active profile id, resolved ("default" when none is selected). Same per-profile mechanic as
// PlayStats/ItemMarks/FavoritesStore.
QString resolvedProfileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

// Per-profile group root: "stats/<profileId>".
QString profileGroup() { return QStringLiteral("stats/") + resolvedProfileId(); }

// This device's WRITE namespace under the active profile: "stats/<profileId>/<deviceId>" (mdsync T3). Every
// writer targets this; the readers SUM across all sibling device namespaces.
QString deviceGroup()  { return profileGroup() + QLatin1Char('/') + Settings::deviceId(); }

QString statsSchemaKey(const QString& profile) { return QStringLiteral("stats/") + profile + QStringLiteral("/schema"); }

// Fold a single profile's legacy un-namespaced keys into THIS device's namespace, once (guarded by the
// per-profile schema stamp — the PlaylistStore precedent). At first migration the device namespace is empty
// (namespacing is new) AND every writer folds before it writes, so a plain move-then-remove is exact; a second
// call short-circuits on the stamp (and even un-stamped would find nothing left to fold). A failed write is
// left UN-stamped so the next run retries (never stamp a lost migration).
void migrateStatsProfile(const QString& profile)
{
    QSettings& s = store();
    if (s.value(statsSchemaKey(profile)).toInt() >= kStatsSchema) return;

    const QString dev  = Settings::deviceId();
    const QString base = QStringLiteral("stats/") + profile;
    const QString devB = base + QLatin1Char('/') + dev;

    s.beginGroup(base + QStringLiteral("/items"));
    const QStringList legacyItems = s.childKeys();
    s.endGroup();
    for (const QString& h : legacyItems)
        s.setValue(devB + QStringLiteral("/items/") + h, s.value(base + QStringLiteral("/items/") + h).toString());

    for (const char* leaf : {"video/seconds", "audio/seconds", "reading/pages"})
    {
        const QString ck = base + QStringLiteral("/cat/") + QLatin1String(leaf);
        if (s.contains(ck)) s.setValue(devB + QStringLiteral("/cat/") + QLatin1String(leaf), s.value(ck));
    }

    s.remove(base + QStringLiteral("/items")); // legacy roots gone (folded); device namespace is disjoint
    s.remove(base + QStringLiteral("/cat"));

    if (s.status() == QSettings::NoError) { s.setValue(statsSchemaKey(profile), kStatsSchema); s.sync(); }
}

// MD5-hex token of the opaque caller key — flattens paths/URLs (which may carry '/', '//', or trailing
// separators QSettings would normalize into colliding group paths) to a collision-safe leaf. Same hash pattern
// as ItemMarks::hashKey. Only ever built for a non-empty key (callers guard empty first).
QString hashKey(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString itemsGroup()               { return deviceGroup() + QStringLiteral("/items"); }
QString itemKey(const QString& h)  { return itemsGroup() + QLatin1Char('/') + h; }
QString catSecondsKey(const QString& cat) { return deviceGroup() + QStringLiteral("/cat/") + cat + QStringLiteral("/seconds"); }
QString catPagesKey()              { return deviceGroup() + QStringLiteral("/cat/reading/pages"); }

bool isMediaCategory(const QString& c) { return c == QStringLiteral("video") || c == QStringLiteral("audio"); }

Entry entryFromJson(const QByteArray& json)
{
    Entry e;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    e.mediaSeconds = qint64(o.value(QStringLiteral("mediaSeconds")).toDouble());
    e.pagesRead    = qint64(o.value(QStringLiteral("pagesRead")).toDouble());
    e.lastActivity = qint64(o.value(QStringLiteral("lastActivity")).toDouble());
    e.title        = o.value(QStringLiteral("title")).toString();
    e.category     = o.value(QStringLiteral("category")).toString();
    return e;
}

QByteArray entryToJson(const Entry& e)
{
    QJsonObject o;
    o.insert(QStringLiteral("mediaSeconds"), double(e.mediaSeconds));
    o.insert(QStringLiteral("pagesRead"),    double(e.pagesRead));
    o.insert(QStringLiteral("lastActivity"), double(e.lastActivity));
    o.insert(QStringLiteral("title"),        e.title);
    o.insert(QStringLiteral("category"),     e.category);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

Totals toTotals(const Entry& e)
{
    Totals t;
    t.mediaSeconds = e.mediaSeconds;
    t.pagesRead    = e.pagesRead;
    t.lastActivity = e.lastActivity;
    t.title        = e.title;
    return t;
}

// ---- Lazy per-profile cache ------------------------------------------------------------------------------
// get()/rollups/topTitles are the read paths (the Stats panel). Their per-call cost is a currentId() read + a
// QString compare (the self-healing profile-switch check); the group resolution + blob parse happen ONCE at
// (re)build. invalidate() forces a rebuild for external ini changes; this store's own writers invalidate.
QString mCacheProfileId;   // profile id the cache was built for; empty => not built
bool    mCacheBuilt = false;
QHash<QString, Entry> mCache; // itemHash -> Entry
qint64  mCatVideoSecs = 0;
qint64  mCatAudioSecs = 0;
qint64  mCatReadPages = 0;

void ensureCache()
{
    const QString id = ProfileStore::currentId();
    if (mCacheBuilt && mCacheProfileId == id) return;

    migrateStatsProfile(resolvedProfileId()); // fold legacy into this device's namespace before reading

    mCache.clear();
    mCatVideoSecs = mCatAudioSecs = mCatReadPages = 0;
    mCacheProfileId = id;

    const QString grp = profileGroup();
    QSettings& s = store();
    // Every child GROUP of the profile is a per-device namespace (the "schema" stamp is a child KEY, excluded).
    // Sum each device's items (per hash) and category rollups — the reader contract is the union total.
    s.beginGroup(grp);
    const QStringList devices = s.childGroups();
    s.endGroup();

    for (const QString& dev : devices)
    {
        const QString devGrp = grp + QLatin1Char('/') + dev;
        s.beginGroup(devGrp + QStringLiteral("/items"));
        const QStringList hashes = s.childKeys();
        for (const QString& h : hashes)
        {
            const Entry e = entryFromJson(s.value(h).toString().toUtf8());
            auto it = mCache.find(h);
            if (it == mCache.end()) { mCache.insert(h, e); continue; }
            Entry& c = it.value();
            c.mediaSeconds += e.mediaSeconds;   // union total across devices
            c.pagesRead    += e.pagesRead;
            if (e.lastActivity >= c.lastActivity) // newest device's activity owns title/category for display
            {
                c.lastActivity = e.lastActivity;
                if (!e.title.isEmpty()) c.title = e.title;
                c.category = e.category;
            }
        }
        s.endGroup();

        mCatVideoSecs += s.value(devGrp + QStringLiteral("/cat/video/seconds"), 0).toLongLong();
        mCatAudioSecs += s.value(devGrp + QStringLiteral("/cat/audio/seconds"), 0).toLongLong();
        mCatReadPages += s.value(devGrp + QStringLiteral("/cat/reading/pages"), 0).toLongLong();
    }

    mCacheBuilt = true;
}

// Load one item's record directly from the store (writers read-modify-write independent of the cache).
Entry loadItem(const QString& h) { return entryFromJson(store().value(itemKey(h)).toString().toUtf8()); }

void saveItem(const QString& h, const Entry& e)
{
    store().setValue(itemKey(h), QString::fromUtf8(entryToJson(e)));
}

// Per-namespace freshness stamp (mdsync T4). The OWNER device writes stats/<profile>/<deviceId>/lastWrite at
// every accrual; the stamp is a plain scalar leaf, so it rides the existing verbatim serialize/merge path with
// zero shape-specific coupling, is written ONLY by the owner and copied verbatim thereafter, and thus faithfully
// orders any two copies of a namespace (fresh vs stale) — the newest-wins-per-namespace signal CloudMerge reads.
// (It sits DIRECTLY under the device group, never under items/ or cat/, so no reader/rollup counts it.)
void stampDeviceWrite()
{
    store().setValue(deviceGroup() + QStringLiteral("/lastWrite"), QDateTime::currentSecsSinceEpoch());
}

} // namespace

void ConsumptionStats::addMediaSeconds(const QString& key, const QString& category, qint64 secs,
                                       const QString& title)
{
    if (key.isEmpty() || secs <= 0 || !isMediaCategory(category)) return; // junk-free: no negative/zero/stray cat

    migrateStatsProfile(resolvedProfileId()); // this device writes only its own namespace
    const QString h = hashKey(key);
    Entry e = loadItem(h);
    e.mediaSeconds += secs;
    e.category     = category;
    e.lastActivity = QDateTime::currentSecsSinceEpoch();
    if (!title.isEmpty()) e.title = title; // last non-empty title wins (no reverse lookup at display)
    saveItem(h, e);

    // Category rollup (maintained incrementally; the probe checks it stays coherent with the per-title sum).
    const QString rk = catSecondsKey(category);
    store().setValue(rk, store().value(rk, 0).toLongLong() + secs);
    stampDeviceWrite();   // freshness stamp for this device's namespace (mdsync T4)
    store().sync();
    invalidate();
}

void ConsumptionStats::addPagesRead(const QString& key, int page, const QString& title)
{
    if (key.isEmpty()) return;

    migrateStatsProfile(resolvedProfileId()); // this device writes only its own namespace
    const QString h = hashKey(key);
    Entry e = loadItem(h);
    // High-water: pagesRead IS the max page index ever reached (the deltas telescope), so accrue only the new
    // ground. A revisit or a backward turn (page <= pagesRead) is a no-op — never accrues, never decrements.
    const qint64 delta = qint64(page) - e.pagesRead;
    if (delta <= 0) return;

    e.pagesRead    += delta;
    e.category      = QStringLiteral("reading");
    e.lastActivity  = QDateTime::currentSecsSinceEpoch();
    if (!title.isEmpty()) e.title = title;
    saveItem(h, e);

    const QString rk = catPagesKey();
    store().setValue(rk, store().value(rk, 0).toLongLong() + delta);
    stampDeviceWrite();   // freshness stamp for this device's namespace (mdsync T4)
    store().sync();
    invalidate();
}

Totals ConsumptionStats::get(const QString& key)
{
    if (key.isEmpty()) return Totals{};
    ensureCache();
    return toTotals(mCache.value(hashKey(key))); // absent -> default Totals{}
}

qint64 ConsumptionStats::categorySeconds(const QString& category)
{
    ensureCache();
    if (category == QStringLiteral("video")) return mCatVideoSecs;
    if (category == QStringLiteral("audio")) return mCatAudioSecs;
    return 0;
}

qint64 ConsumptionStats::categoryPages()
{
    ensureCache();
    return mCatReadPages;
}

QVector<QPair<QString, Totals>> ConsumptionStats::topTitles(const QString& category, int n)
{
    ensureCache();
    const bool reading = (category == QStringLiteral("reading"));

    QVector<QPair<QString, Entry>> rows;
    for (auto it = mCache.constBegin(); it != mCache.constEnd(); ++it)
        if (it.value().category == category) rows.push_back({ it.key(), it.value() });

    std::sort(rows.begin(), rows.end(), [reading](const QPair<QString, Entry>& a, const QPair<QString, Entry>& b) {
        return reading ? (a.second.pagesRead > b.second.pagesRead)
                       : (a.second.mediaSeconds > b.second.mediaSeconds);
    });

    QVector<QPair<QString, Totals>> out;
    for (int i = 0; i < rows.size() && (n <= 0 || i < n); ++i)
        out.push_back({ rows[i].first, toTotals(rows[i].second) });
    return out;
}

void ConsumptionStats::invalidate()
{
    mCacheBuilt = false;
    mCacheProfileId.clear();
    mCache.clear();
    mCatVideoSecs = mCatAudioSecs = mCatReadPages = 0;
}

void ConsumptionStats::migrate()
{
    // Fold EVERY profile's legacy accumulators into this device's namespace (guarded per profile). The child
    // groups of "stats" are the profile ids. Run once at startup before any CloudMerge serialize.
    QSettings& s = store();
    s.beginGroup(QStringLiteral("stats"));
    const QStringList profiles = s.childGroups();
    s.endGroup();
    for (const QString& p : profiles) migrateStatsProfile(p);
    invalidate();
}
