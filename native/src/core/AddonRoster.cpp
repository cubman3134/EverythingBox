#include "AddonRoster.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "Tombstones.h"

#include <QSettings>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cstring>

// Shares the portable everythingbox.ini with AddonManager and every other store. Coherence comes from every
// writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

const QLatin1String kUrlsKey("addon.remote.urls");
const QLatin1String kEnabledPrefix("addon.enabled.");

std::function<void()> g_changeHook;
bool g_migrationsRun = false;

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

// The live URL list, normalised and de-duplicated, in its stored order.
QStringList liveUrls()
{
    QStringList out;
    const QJsonArray arr = QJsonDocument::fromJson(store().value(kUrlsKey).toByteArray()).array();
    for (const QJsonValue& v : arr)
    {
        const QString b = AddonRoster::normalizeBase(v.toString());
        if (!b.isEmpty() && !out.contains(b)) out << b;
    }
    return out;
}

void writeLiveUrls(const QStringList& urls)
{
    QJsonArray arr;
    for (const QString& u : urls) arr.append(u);
    // Byte-for-byte AddonManager's own spelling (a QByteArray of compact JSON), so the two writers agree.
    store().setValue(kUrlsKey, QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// Every live addon.enabled.<id>, id -> flag. allKeys rather than childKeys: an id carrying a '/' becomes a
// QSettings group path, and allKeys still names it in full.
QHash<QString, bool> liveFlags()
{
    QHash<QString, bool> out;
    for (const QString& k : store().allKeys())
        if (k.startsWith(kEnabledPrefix) && k.size() > kEnabledPrefix.size())
            out.insert(k.mid(kEnabledPrefix.size()), store().value(k).toBool());
    return out;
}

// The local tombstones, key -> newest ts.
QHash<QString, qint64> localTombs()
{
    QHash<QString, qint64> map;
    for (const Tombstones::Entry& e : Tombstones::all(AddonRoster::tombStore()))
        if (e.ts > map.value(e.key, 0)) map.insert(e.key, e.ts);
    return map;
}

bool suppressed(const AddonRoster::Record& r, const QHash<QString, qint64>& tombs)
{
    // The favourites/presets rule, verbatim: a REAL tombstone at-or-after the record's ts suppresses it. "No
    // tombstone" is an ABSENT key, never ts 0, so a ts-0 record is only ever swept by a real deletion.
    return tombs.contains(r.key) && tombs.value(r.key) >= r.ts;
}

int indexByUrl(const QVector<AddonRoster::Record>& recs, const QString& url)
{
    for (int i = 0; i < recs.size(); ++i)
        if (!recs[i].url.isEmpty() && recs[i].url == url) return i;
    return -1;
}

int indexByKey(const QVector<AddonRoster::Record>& recs, const QString& key)
{
    for (int i = 0; i < recs.size(); ++i)
        if (recs[i].key == key) return i;
    return -1;
}

// A key that IS a URL (the fallback for an add-on whose manifest was never cached) has no addon.enabled.<id>
// twin, because the live flag is keyed by manifest id.
bool keyIsUrl(const AddonRoster::Record& r) { return r.key.contains(QStringLiteral("://")); }

} // namespace

QString AddonRoster::itemsKey()
{
    return QStringLiteral("roster/") + QLatin1String(kScope) + QStringLiteral("/items");
}

QString AddonRoster::tombStore()
{
    return QStringLiteral("roster/") + QLatin1String(kScope);
}

QString AddonRoster::normalizeBase(const QString& raw)
{
    QString b = raw.trimmed();
    if (b.endsWith(QStringLiteral("/manifest.json"))) b.chop(int(strlen("/manifest.json")));
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b;
}

QString AddonRoster::manifestCacheKey(const QString& base)
{
    const QByteArray h = QCryptographicHash::hash(base.toUtf8(), QCryptographicHash::Md5).toHex();
    return QStringLiteral("addon.remote.manifest.") + QString::fromUtf8(h);
}

QString AddonRoster::keyForUrl(const QString& base)
{
    // Both manifest dialects AddonManager accepts (Stremio's and our own) carry the add-on's id as a top-level
    // "id", and that is exactly what AddonManager::planRemoteAdd matches on.
    const QByteArray cached = store().value(manifestCacheKey(base)).toByteArray();
    const QString id = cached.isEmpty()
        ? QString() : QJsonDocument::fromJson(cached).object().value(QStringLiteral("id")).toString();
    return id.isEmpty() ? base : id;
}

bool AddonRoster::isLiveKey(const QString& key)
{
    return key == kUrlsKey || key.startsWith(kEnabledPrefix);
}

QJsonObject AddonRoster::toJson(const Record& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("key"), r.key);
    o.insert(QStringLiteral("url"), r.url);
    o.insert(QStringLiteral("enabled"), r.enabled);
    o.insert(QStringLiteral("ts"), static_cast<double>(r.ts));
    return o;
}

AddonRoster::Record AddonRoster::fromJson(const QJsonObject& o)
{
    Record r;
    r.key = o.value(QStringLiteral("key")).toString();
    if (r.key.isEmpty()) return Record{};
    r.url = normalizeBase(o.value(QStringLiteral("url")).toString());
    r.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
    r.ts = ts > 0 ? ts : 0;
    return r;
}

QVector<AddonRoster::Record> AddonRoster::records()
{
    QVector<Record> out;
    const QJsonArray arr = QJsonDocument::fromJson(store().value(itemsKey()).toString().toUtf8()).array();
    for (const QJsonValue& v : arr)
    {
        const Record r = fromJson(v.toObject());
        if (!r.key.isEmpty() && indexByKey(out, r.key) < 0) out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) { return a.key < b.key; });
    return out;
}

void AddonRoster::saveRecords(QVector<Record> recs)
{
    std::sort(recs.begin(), recs.end(), [](const Record& a, const Record& b) { return a.key < b.key; });
    QJsonArray arr;
    for (const Record& r : recs) arr.append(toJson(r));
    store().setValue(itemsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

bool AddonRoster::reconcile(qint64 now)
{
    if (now <= 0) now = nowSecs();
    QSettings& s = store();
    // No shadow at all yet: this is the first run of a build that has one. What is live is BACKFILLED at ts 0
    // (its real time is unknown) and nothing is tombstoned — an absence here predates the shadow and says
    // nothing about a removal.
    const bool backfill = !s.contains(itemsKey());

    QVector<Record> recs = records();
    QHash<QString, qint64> tombs = localTombs();
    bool changed = backfill;

    // The stamp a LOCAL EDIT gets: now, but always strictly newer than the version it edits and than any
    // tombstone on the key — so the edit wins every merge it meets, clock skew and same-second re-adds
    // included (a re-add in the second of its own removal would otherwise tie the tombstone and lose to `>=`).
    auto editStamp = [&](const Record* r, const QString& key) {
        qint64 ts = now;
        if (r) ts = std::max(ts, r->ts + 1);
        if (tombs.contains(key)) ts = std::max(ts, tombs.value(key) + 1);
        return ts;
    };
    auto clearTomb = [&](const QString& key) {
        if (!tombs.contains(key)) return;
        Tombstones::remove(tombStore(), key);
        tombs.remove(key);
    };

    const QStringList live = liveUrls();
    const QSet<QString> liveSet(live.begin(), live.end());
    const QHash<QString, bool> flags = liveFlags();

    // 1. Every live URL has a record.
    for (const QString& u : live)
    {
        const int i = indexByUrl(recs, u);
        if (i >= 0)
        {
            // Live, yet suppressed by our own tombstone: a removal that was undone underneath us (a settings
            // Discard restores the live list; the tombstone is not in the transaction). Re-adding it now is what
            // the user's Discard meant, on every device.
            if (!backfill && suppressed(recs[i], tombs))
            {
                recs[i].ts = editStamp(&recs[i], recs[i].key);
                clearTomb(recs[i].key);
                changed = true;
            }
            continue;
        }
        const QString k = keyForUrl(u);
        const int j = indexByKey(recs, k);
        if (j >= 0)
        {
            // A second live URL for an add-on whose record's URL is also live: pre-#80 duplicates. The record
            // keeps its URL and the projection collapses the list onto it, exactly as #80's replace does.
            if (!recs[j].url.isEmpty() && liveSet.contains(recs[j].url)) continue;
            // Otherwise this key's URL changed (a re-configure), or an enabled-only record gained a URL.
            recs[j].url = u;
            if (!backfill) { recs[j].ts = editStamp(&recs[j], k); clearTomb(k); }
            changed = true;
            continue;
        }
        Record n;
        n.key = k;
        n.url = u;
        n.enabled = flags.value(k, true);
        n.ts = backfill ? 0 : editStamp(nullptr, k);
        if (!backfill) clearTomb(k);
        recs.push_back(n);
        changed = true;
    }

    // 2. A record whose URL left the live list was removed here — outside AddonManager's API when it gets this
    //    far (a Discard, an older build) — so it is a removal, dated now. Not on a backfill: see above.
    if (!backfill)
    {
        for (int i = recs.size() - 1; i >= 0; --i)
        {
            const Record& r = recs[i];
            if (r.url.isEmpty() || liveSet.contains(r.url)) continue;
            if (!suppressed(r, tombs))
            {
                const qint64 ts = editStamp(&r, r.key);
                Tombstones::record(tombStore(), r.key, ts);
                // ...and into the local view too, or pass 3 below would find the removed add-on's leftover
                // flag with no record and no tombstone and re-create it as a NEWER enabled-only record — one
                // that out-dates this very tombstone on every peer, so the removal never propagates.
                tombs.insert(r.key, ts);
            }
            recs.remove(i);
            changed = true;
        }
    }

    // 3. Every live flag has a record, and a flipped flag is an edit.
    for (auto it = flags.begin(); it != flags.end(); ++it)
    {
        const int j = indexByKey(recs, it.key());
        if (j < 0)
        {
            // A flag left behind by an add-on this device removed (removeRemoteSource keeps it, so a re-add
            // keeps the user's choice) is not a new statement: the tombstone already says what happened.
            if (tombs.contains(it.key())) continue;
            Record n;
            n.key = it.key();
            n.enabled = it.value();
            n.ts = backfill ? 0 : editStamp(nullptr, it.key());
            recs.push_back(n);
            changed = true;
        }
        else if (!backfill && recs[j].enabled != it.value())
        {
            recs[j].enabled = it.value();
            recs[j].ts = editStamp(&recs[j], it.key());
            changed = true;
        }
    }

    if (changed) saveRecords(recs);
    return changed && !(backfill && recs.isEmpty() && live.isEmpty() && flags.isEmpty());
}

void AddonRoster::touched()
{
    if (reconcile() && g_changeHook) g_changeHook();
}

bool AddonRoster::project(bool dryRun)
{
    QSettings& s = store();
    const QVector<Record> recs = records();
    const QHash<QString, qint64> tombs = localTombs();
    const QStringList live = liveUrls();

    QStringList out;
    QSet<QString> used;
    auto take = [&](const QString& u) { if (!used.contains(u)) { used.insert(u); out << u; } };

    for (const QString& u : live)
    {
        const int i = indexByUrl(recs, u);
        if (i >= 0)
        {
            if (!suppressed(recs[i], tombs)) take(u);    // suppressed: removed on another device -> dropped
            continue;
        }
        const QString k = keyForUrl(u);
        const int j = indexByKey(recs, k);
        if (j >= 0 && !suppressed(recs[j], tombs))
        {
            // Re-configured on another device: the newer URL takes this entry's place (#80's in-place rule).
            // An enabled-only record states nothing about a URL, so the live one stays.
            take(recs[j].url.isEmpty() ? u : recs[j].url);
            continue;
        }
        if (tombs.contains(k)) continue;                 // its record lost to a tombstone: removed elsewhere
        take(u);                                         // no statement either way: never drop on absence
    }
    for (const Record& r : recs)                         // key order: the same on every device
        if (!r.url.isEmpty() && !suppressed(r, tombs)) take(r.url);

    bool changed = (out != live);

    struct FlagWrite { QString key; bool value; };
    QVector<FlagWrite> flagWrites;
    for (const Record& r : recs)
    {
        if (keyIsUrl(r) || suppressed(r, tombs)) continue;
        const QString k = QString(kEnabledPrefix) + r.key;
        const bool cur = s.contains(k) ? s.value(k).toBool() : true;   // absent = enabled (AddonManager's default)
        if (cur != r.enabled) flagWrites.push_back({ k, r.enabled });
    }
    if (!flagWrites.isEmpty()) changed = true;

    if (dryRun || !changed) return changed;

    if (out != live)
    {
        const QSet<QString> kept(out.begin(), out.end());
        for (const QString& u : live)
            if (!kept.contains(u)) s.remove(manifestCacheKey(u));   // as removeRemoteSource does
        writeLiveUrls(out);
    }
    for (const FlagWrite& w : flagWrites) s.setValue(w.key, w.value);
    s.sync();
    return true;
}

void AddonRoster::adoptLegacySnapshot(const QString& urlsJson, const QHash<QString, bool>& enabled)
{
    reconcile();                                        // this device's own edits are stamped first
    QVector<Record> recs = records();
    const QHash<QString, qint64> tombs = localTombs();
    bool changed = false;

    const QJsonArray arr = QJsonDocument::fromJson(urlsJson.toUtf8()).array();
    for (const QJsonValue& v : arr)
    {
        const QString base = normalizeBase(v.toString());
        if (base.isEmpty() || indexByUrl(recs, base) >= 0) continue;   // already held, by URL
        const QString k = keyForUrl(base);
        if (tombs.contains(k)) continue;                // removed here: an undated snapshot cannot undo that
        const int j = indexByKey(recs, k);
        if (j >= 0)
        {
            // Already held under its id. A URL we hold wins over the snapshot's; an enabled-only record gains
            // the URL, keeping its own stamp.
            if (recs[j].url.isEmpty()) { recs[j].url = base; changed = true; }
            continue;
        }
        Record n;
        n.key = k;
        n.url = base;
        n.enabled = enabled.value(k, true);
        n.ts = 0;
        recs.push_back(n);
        changed = true;
    }
    for (auto it = enabled.begin(); it != enabled.end(); ++it)
    {
        if (it.key().isEmpty() || indexByKey(recs, it.key()) >= 0 || tombs.contains(it.key())) continue;
        Record n;
        n.key = it.key();
        n.enabled = it.value();
        n.ts = 0;
        recs.push_back(n);
        changed = true;
    }
    if (!changed) return;
    saveRecords(recs);
    project();
    if (g_changeHook) g_changeHook();
}

void AddonRoster::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }

void AddonRoster::markMigrationsRun() { g_migrationsRun = true; }
bool AddonRoster::migrationsRun() { return g_migrationsRun; }
