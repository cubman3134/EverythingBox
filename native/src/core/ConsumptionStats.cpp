#include "ConsumptionStats.h"
#include "AppPaths.h"
#include "ProfileStore.h"

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

// Per-profile group root: "stats/<profileId>" (or "stats/default" when no profile is selected). Same
// per-profile mechanic as PlayStats/ItemMarks/FavoritesStore.
QString profileGroup()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("stats/") + (id.isEmpty() ? QStringLiteral("default") : id);
}

// MD5-hex token of the opaque caller key — flattens paths/URLs (which may carry '/', '//', or trailing
// separators QSettings would normalize into colliding group paths) to a collision-safe leaf. Same hash pattern
// as ItemMarks::hashKey. Only ever built for a non-empty key (callers guard empty first).
QString hashKey(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString itemsGroup()               { return profileGroup() + QStringLiteral("/items"); }
QString itemKey(const QString& h)  { return itemsGroup() + QLatin1Char('/') + h; }
QString catSecondsKey(const QString& cat) { return profileGroup() + QStringLiteral("/cat/") + cat + QStringLiteral("/seconds"); }
QString catPagesKey()              { return profileGroup() + QStringLiteral("/cat/reading/pages"); }

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

    mCache.clear();
    mCatVideoSecs = mCatAudioSecs = mCatReadPages = 0;
    mCacheProfileId = id;

    const QString grp = QStringLiteral("stats/") + (id.isEmpty() ? QStringLiteral("default") : id);
    QSettings& s = store();
    s.beginGroup(grp + QStringLiteral("/items"));
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
    {
        const Entry e = entryFromJson(s.value(h).toString().toUtf8());
        mCache.insert(h, e);
    }
    s.endGroup();

    mCatVideoSecs = s.value(grp + QStringLiteral("/cat/video/seconds"), 0).toLongLong();
    mCatAudioSecs = s.value(grp + QStringLiteral("/cat/audio/seconds"), 0).toLongLong();
    mCatReadPages = s.value(grp + QStringLiteral("/cat/reading/pages"), 0).toLongLong();

    mCacheBuilt = true;
}

// Load one item's record directly from the store (writers read-modify-write independent of the cache).
Entry loadItem(const QString& h) { return entryFromJson(store().value(itemKey(h)).toString().toUtf8()); }

void saveItem(const QString& h, const Entry& e)
{
    store().setValue(itemKey(h), QString::fromUtf8(entryToJson(e)));
}

} // namespace

void ConsumptionStats::addMediaSeconds(const QString& key, const QString& category, qint64 secs,
                                       const QString& title)
{
    if (key.isEmpty() || secs <= 0 || !isMediaCategory(category)) return; // junk-free: no negative/zero/stray cat

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
    store().sync();
    invalidate();
}

void ConsumptionStats::addPagesRead(const QString& key, int page, const QString& title)
{
    if (key.isEmpty()) return;

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
