#include "JellyfinMigrate.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "Jellyfin.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QVariant>
#include <algorithm>

// Like MusicRemap and PcGameRemap, this unit works at the QSettings layer rather than through the store
// classes, and for the same three reasons those files give at length: EXACTNESS (a store's writer is an
// accrual API — there is no "set the absolute total" entry point on PlayStats at all — so replaying a record
// through it would rewrite the very fields the move exists to preserve), COVERAGE (a store reads the ACTIVE
// profile and writes only THIS device's namespace, while a record belongs to whichever profile accrued it),
// and LEANNESS (staying on QtCore is what lets probe_jellyfin link against Qt6::Core alone).
//
// The Batch class and the digest helpers below are deliberate near-copies of MusicRemap.cpp's, which are
// themselves near-copies of PcGameRemap.cpp's. That is a decision rather than an oversight, and it is the
// same one MusicRemap.cpp records: extracting the internals of data migrations that have already shipped and
// already moved real installs is a refactor with its own risk and does not belong inside an unrelated
// increment. Each helper names the source line it mirrors, and probe_jellyfin recomputes every key shape
// independently rather than calling these helpers, so a drift between this file and the stores it mirrors
// shows up as a failing check instead of as a passing tautology. A shared `ItemRemap` seam is the obvious
// follow-up; it should move all three units at once, under all three probes.

namespace {

#ifdef EB_JELLYFIN_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

QSettings& store()
{
#ifdef EB_JELLYFIN_TEST_SEAM
    if (!g_testIniPath.isEmpty())
    {
        // Deleting the cached QSettings rather than only re-pointing the path is the load-bearing half: a
        // function-local static is constructed exactly once, so a path captured on first use would pin every
        // later case to the first one's file. MusicRemap's seam has the same shape for the same reason.
        if (!g_testStore) g_testStore = new QSettings(g_testIniPath, QSettings::IniFormat);
        return *g_testStore;
    }
#endif
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// ---- the three key digests, each mirroring one store exactly ---------------------------------------------
// ItemMarks::hashKey (ItemMarks.cpp:46): FULL md5 hex.
QString md5Hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}
// ResumeStore::hashFor (ResumeStore.cpp:13): md5 hex TRUNCATED to the first 10 characters. A second, easily
// missed shape over the same digest.
QString md5Hex10(const QString& key) { return md5Hex(key).left(10); }
// PlayStats::sha1Of (PlayStats.cpp:29): SHA-1 hex, a third shape again.
QString sha1Hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// ---- write-then-verify, ONE FLUSH PER PASS ----------------------------------------------------------------
// MusicRemap.cpp's Batch, and its reasoning applies unchanged: setValue only accumulates into QSettings'
// in-memory map, so it costs nothing per record; commit() flushes ONCE and only then retires the sources, so
// a durability failure drops every queued removal and leaves every source standing (rule 2 at pass
// granularity, which is strictly more conservative than per record).
//
// The per-record read-back is a SERIALISATION gate, not a durability one, and — exactly as MusicRemap.cpp
// says of its own — it is a tripwire no test can trip: value() answers out of the same in-memory map
// setValue just wrote, so within one process the comparison is tautological. It stays because it costs
// nothing and because the day a QVariant type appears whose ini spelling is not its own toString(), this is
// the line that refuses to retire the source. Do not delete it as dead code, and do not expect a mutant of
// it to be killed — the reachable half of rule 2 is mutated instead, in jellyfin-mutants.json.
class Batch
{
public:
    explicit Batch(QSettings& s) : s_(s) {}

    bool write(const QString& key, const QVariant& value)
    {
        s_.setValue(key, value);
        dirty_ = true;
        return s_.value(key).toString() == value.toString();
    }

    void clearNow(const QString& key) { s_.remove(key); dirty_ = true; }
    void removeLater(const QString& key) { pending_.push_back(key); }

    bool commit()
    {
        if (!dirty_ && pending_.isEmpty()) return true;   // nothing to do: do not rewrite the file
        s_.sync();
        dirty_ = false;
        if (s_.status() != QSettings::NoError) { pending_.clear(); return false; }
        if (pending_.isEmpty()) return true;
        for (const QString& k : pending_) s_.remove(k);   // a group key removes its subkeys too
        pending_.clear();
        s_.sync();
        return s_.status() == QSettings::NoError;
    }

private:
    QSettings&  s_;
    QStringList pending_;
    bool        dirty_ = false;
};

// ---- the three literal stores ------------------------------------------------------------------------------
// Each keeps its references inside one JSON blob per profile. The rewrite is field-by-field and TABLE-DRIVEN:
// a field is replaced only when its current value is a key of the table, so a path, an addon item id or a
// Subsonic key sitting in the same object is untouched by construction rather than by a filter that could
// drift (rule 1).

QStringList profilesUnder(QSettings& s, const QString& group)
{
    s.beginGroup(group);
    const QStringList out = s.childGroups();
    s.endGroup();
    return out;
}

// Replace one string member of a JSON object when the table names it. Returns true if it changed.
bool remapField(QJsonObject& o, const QString& field, const QHash<QString, QString>& table)
{
    const auto it = table.constFind(o.value(field).toString());
    if (it == table.constEnd()) return false;
    o.insert(field, *it);
    return true;
}

// FAVOURITES: favorites/<profile>/items — an array of items carrying `itemId` and (optionally) `path`.
// PLAYLISTS:  playlists/<profile>/items — an array of playlists, each with its own `items` array of entries
//             carrying `itemId` and `path`.
// RECENTS:    recent/<profile>/items — an array of rows carrying `key` (the stable identity), `path` (what
//             is replayed) and `sitem` (the source item id a re-mint recipe uses).
//
// Every one of those field names is the writer's own spelling, and the probe recomputes them independently.
bool rewriteFlatArray(QSettings& s, const QString& key, const QHash<QString, QString>& table,
                      const QStringList& fields, Batch& b)
{
    const QString raw = s.value(key).toString();
    if (raw.isEmpty()) return false;
    const QJsonArray arr = QJsonDocument::fromJson(raw.toUtf8()).array();
    if (arr.isEmpty()) return false;
    QJsonArray out;
    bool changed = false;
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) { out.append(v); continue; }
        QJsonObject o = v.toObject();
        for (const QString& f : fields) changed = remapField(o, f, table) || changed;
        out.append(o);
    }
    if (!changed) return false;
    return b.write(key, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
}

bool rewritePlaylists(QSettings& s, const QString& key, const QHash<QString, QString>& table, Batch& b)
{
    const QString raw = s.value(key).toString();
    if (raw.isEmpty()) return false;
    const QJsonArray arr = QJsonDocument::fromJson(raw.toUtf8()).array();
    if (arr.isEmpty()) return false;
    QJsonArray out;
    bool changed = false;
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) { out.append(v); continue; }
        QJsonObject pl = v.toObject();
        QJsonArray entries;
        for (const QJsonValue& ev : pl.value(QStringLiteral("items")).toArray())
        {
            if (!ev.isObject()) { entries.append(ev); continue; }
            QJsonObject e = ev.toObject();
            changed = remapField(e, QStringLiteral("itemId"), table) || changed;
            changed = remapField(e, QStringLiteral("path"), table) || changed;
            entries.append(e);
        }
        if (pl.contains(QStringLiteral("items"))) pl.insert(QStringLiteral("items"), entries);
        out.append(pl);
    }
    if (!changed) return false;
    return b.write(key, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
}

// ---- the three hashed stores --------------------------------------------------------------------------------

// resume/<md5-10(id)>/{pos,dur,ts,title} — global, not per-profile.
//
// NEWEST WINS rather than an accumulation, and that is not a shortcut (MusicRemap's remapResume says it
// first): a resume position is a single point in a single stream, so there is nothing additive about two of
// them and a sum or a max of `pos` would produce a position that was never reached in either copy.
void moveResume(QSettings& s, const QHash<QString, QString>& table, Batch& b)
{
    for (auto it = table.cbegin(); it != table.cend(); ++it)
    {
        const QString src = QStringLiteral("resume/") + md5Hex10(it.key());
        const QString dst = QStringLiteral("resume/") + md5Hex10(it.value());
        if (src == dst) continue;
        if (!s.contains(src + QStringLiteral("/pos")) && !s.contains(src + QStringLiteral("/dur"))) continue;

        const bool dstHas = s.contains(dst + QStringLiteral("/pos"))
                         || s.contains(dst + QStringLiteral("/dur"));
        const qint64 srcTs = s.value(src + QStringLiteral("/ts"), 0).toLongLong();
        const qint64 dstTs = s.value(dst + QStringLiteral("/ts"), 0).toLongLong();

        if (!dstHas || srcTs > dstTs)
        {
            // Wholesale: a resume record's leaves only mean anything together, so a leaf the winner does not
            // carry is CLEARED rather than left standing (a newer `pos` of 30 beside an inherited `dur` of
            // 100 renders a progress bar for a position that was never 30% of anything).
            bool ok = true;
            for (const char* leaf : { "/pos", "/dur", "/title" })
            {
                const QString lk = src + QLatin1String(leaf);
                if (s.contains(lk)) ok = b.write(dst + QLatin1String(leaf), s.value(lk)) && ok;
            }
            if (!ok) continue;                          // leave BOTH in place; the next run retries
            for (const char* leaf : { "/pos", "/dur", "/title" })
                if (!s.contains(src + QLatin1String(leaf))) b.clearNow(dst + QLatin1String(leaf));

            // `ts` LAST, because it is the field the comparison above reads: written first, a run that died
            // halfway would leave the destination LOOKING newer than the source and the retry would skip it
            // and retire the source on top of a half-copied record.
            const QString sts = src + QStringLiteral("/ts");
            if (s.contains(sts)) { if (!b.write(dst + QStringLiteral("/ts"), s.value(sts))) continue; }
            else                 b.clearNow(dst + QStringLiteral("/ts"));
        }
        b.removeLater(src);
    }
}

// marks/<profile>/items/<md5(id)> — ONE JSON blob per item (watched state, tags, rating), not a group.
//
// NEWEST `updatedAt` WINS OUTRIGHT. A mark is a statement about the item, not an accumulation, so there is
// nothing to add; two statements are arbitrated by which was made last, which is the same rule the multi-
// device merge already applies to this store.
void moveMarks(QSettings& s, const QHash<QString, QString>& table, Batch& b)
{
    for (const QString& p : profilesUnder(s, QStringLiteral("marks")))
    {
        const QString root = QStringLiteral("marks/") + p + QStringLiteral("/items/");
        for (auto it = table.cbegin(); it != table.cend(); ++it)
        {
            const QString src = root + md5Hex(it.key());
            const QString dst = root + md5Hex(it.value());
            if (src == dst) continue;
            const QString blob = s.value(src).toString();
            if (blob.isEmpty()) continue;
            const QString dstBlob = s.value(dst).toString();
            if (!dstBlob.isEmpty())
            {
                const qint64 a = qint64(QJsonDocument::fromJson(dstBlob.toUtf8()).object()
                                        .value(QStringLiteral("updatedAt")).toDouble());
                const qint64 c = qint64(QJsonDocument::fromJson(blob.toUtf8()).object()
                                        .value(QStringLiteral("updatedAt")).toDouble());
                if (a >= c) { b.removeLater(src); continue; }   // the destination is newer: keep it
            }
            if (!b.write(dst, blob)) continue;
            b.removeLater(src);
        }
    }
}

// playstats/<profile>/<device>/<sha1(id)>/{total,last,sessions}, plus the pre-namespacing shape
// playstats/<profile>/<sha1(id)>/… that PlayStats::migratePlayProfile folds away — BOTH are handled, because
// this may well run before that fold on a machine upgrading from an old build, and a record it failed to see
// would be orphaned by the fold a moment later. (MusicRemap's consumption pass makes the same argument.)
//
// AN ACCUMULATOR MERGES BY ARITHMETIC: seconds and sessions SUM, `last` takes the later of the two. Any other
// rule throws away time that was genuinely spent.
//
// THIS PASS NEVER WRITES `lastWrite`. That leaf is stamped only by a namespace's OWNER at a real accrual and
// it is the freshness gate the multi-device merge orders on. A repair that stamped it — including in the
// foreign namespaces this pass rewrites, which this device does not own — would outrank the owner's genuine
// data and hand every peer this device's rekeyed copy in place of the one it is still reading.
void movePlayStats(QSettings& s, const QHash<QString, QString>& table, Batch& b)
{
    for (const QString& p : profilesUnder(s, QStringLiteral("playstats")))
    {
        const QString base = QStringLiteral("playstats/") + p;
        QStringList roots;
        roots << base + QLatin1Char('/');                                    // legacy, un-namespaced
        for (const QString& d : profilesUnder(s, base))
            roots << base + QLatin1Char('/') + d + QLatin1Char('/');         // a device namespace

        for (const QString& root : roots)
            for (auto it = table.cbegin(); it != table.cend(); ++it)
            {
                const QString src = root + sha1Hex(it.key());
                const QString dst = root + sha1Hex(it.value());
                if (src == dst) continue;
                const bool has = s.contains(src + QStringLiteral("/total"))
                              || s.contains(src + QStringLiteral("/last"))
                              || s.contains(src + QStringLiteral("/sessions"));
                if (!has) continue;

                const qint64 srcTotal = s.value(src + QStringLiteral("/total"), 0).toLongLong();
                const qint64 srcSess  = s.value(src + QStringLiteral("/sessions"), 0).toLongLong();
                const qint64 srcLast  = s.value(src + QStringLiteral("/last"), 0).toLongLong();
                const qint64 dstTotal = s.value(dst + QStringLiteral("/total"), 0).toLongLong();
                const qint64 dstSess  = s.value(dst + QStringLiteral("/sessions"), 0).toLongLong();
                const qint64 dstLast  = s.value(dst + QStringLiteral("/last"), 0).toLongLong();

                bool ok = b.write(dst + QStringLiteral("/total"), srcTotal + dstTotal);
                ok = b.write(dst + QStringLiteral("/sessions"), srcSess + dstSess) && ok;
                ok = b.write(dst + QStringLiteral("/last"), std::max(srcLast, dstLast)) && ok;
                if (!ok) continue;
                b.removeLater(src);
            }
    }
}

} // namespace

// ---- the pure table ---------------------------------------------------------------------------------------

JellyfinMigrate::Table JellyfinMigrate::tableFor(const QStringList& storedIds, const QString& serverId)
{
    Table t;
    // A serverId that is not a server id can only mint half-formed destinations, so the whole table is
    // empty rather than partly wrong (rule 1, applied once instead of per row).
    if (!Jellyfin::isServerId(serverId)) return t;
    for (const QString& id : storedIds)
    {
        const QString item = Jellyfin::legacyItemId(id);
        if (item.isEmpty()) continue;                       // not a legacy reference: leave it alone
        const QString dst = Jellyfin::qualify(serverId, item);
        if (dst.isEmpty() || dst == id) continue;           // never a self-map, never an empty destination
        t.map.insert(id, dst);
    }
    return t;
}

// ---- the enumeration ----------------------------------------------------------------------------------------

QStringList JellyfinMigrate::storedIds()
{
    QSettings& s = store();
    QSet<QString> seen;

    auto takeArray = [&](const QString& key, const QStringList& fields) {
        const QString raw = s.value(key).toString();
        if (raw.isEmpty()) return;
        for (const QJsonValue& v : QJsonDocument::fromJson(raw.toUtf8()).array())
        {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            for (const QString& f : fields)
            {
                const QString val = o.value(f).toString();
                if (!val.isEmpty()) seen.insert(val);
            }
        }
    };

    for (const QString& p : profilesUnder(s, QStringLiteral("favorites")))
        takeArray(QStringLiteral("favorites/") + p + QStringLiteral("/items"),
                  { QStringLiteral("itemId"), QStringLiteral("path") });

    for (const QString& p : profilesUnder(s, QStringLiteral("recent")))
        takeArray(QStringLiteral("recent/") + p + QStringLiteral("/items"),
                  { QStringLiteral("key"), QStringLiteral("path"), QStringLiteral("sitem") });

    for (const QString& p : profilesUnder(s, QStringLiteral("playlists")))
    {
        const QString raw = s.value(QStringLiteral("playlists/") + p + QStringLiteral("/items")).toString();
        if (raw.isEmpty()) continue;
        for (const QJsonValue& v : QJsonDocument::fromJson(raw.toUtf8()).array())
        {
            if (!v.isObject()) continue;
            for (const QJsonValue& ev : v.toObject().value(QStringLiteral("items")).toArray())
            {
                if (!ev.isObject()) continue;
                const QJsonObject e = ev.toObject();
                for (const QString& f : { QStringLiteral("itemId"), QStringLiteral("path") })
                {
                    const QString val = e.value(f).toString();
                    if (!val.isEmpty()) seen.insert(val);
                }
            }
        }
    }

    return QStringList(seen.cbegin(), seen.cend());
}

// ---- the sweep ------------------------------------------------------------------------------------------------

void JellyfinMigrate::applyMigration(const Table& table)
{
    // The cheap path, and the idempotence guarantee in one line: with nothing to move we do not open the
    // store, do not read a key and do not write one. A second run reaches here with an empty table BY
    // CONSTRUCTION — the first run left no legacy reference in any of the three readable stores — which is
    // what makes rule 4 structural rather than a stamp that can get out of step with the data.
    if (table.isEmpty()) return;

    QSettings& s = store();
    Batch b(s);

    // The three hashed stores: hash both ends of every mapping and move the record.
    moveResume(s, table.map, b);
    moveMarks(s, table.map, b);
    movePlayStats(s, table.map, b);

    // The three literal stores. These are ALSO the enumeration, so they are rewritten LAST: a run that died
    // between the two halves would leave the legacy references still readable, and the next run would build
    // the same table and finish the job. Rewriting them first would erase the only record of what still had
    // to be moved.
    for (const QString& p : profilesUnder(s, QStringLiteral("favorites")))
        rewriteFlatArray(s, QStringLiteral("favorites/") + p + QStringLiteral("/items"), table.map,
                         { QStringLiteral("itemId"), QStringLiteral("path") }, b);
    for (const QString& p : profilesUnder(s, QStringLiteral("recent")))
        rewriteFlatArray(s, QStringLiteral("recent/") + p + QStringLiteral("/items"), table.map,
                         { QStringLiteral("key"), QStringLiteral("path"), QStringLiteral("sitem") }, b);
    for (const QString& p : profilesUnder(s, QStringLiteral("playlists")))
        rewritePlaylists(s, QStringLiteral("playlists/") + p + QStringLiteral("/items"), table.map, b);

    b.commit();
}

void JellyfinMigrate::migrateSingleServer(const QStringList& configuredServerIds)
{
    // NO SERVER: nothing to attribute a bare row to. SEVERAL: the row is ambiguous and this unit does not
    // guess — see JellyfinMigrate.h. Both answers are "do nothing", and both are reached before the ini is
    // enumerated, so the common case costs a size() test.
    if (configuredServerIds.size() != 1) return;
    applyMigration(tableFor(storedIds(), configuredServerIds.first()));
}

#ifdef EB_JELLYFIN_TEST_SEAM
void JellyfinMigrate::setIniPathForTesting(const QString& path)
{
    g_testIniPath = path;
    delete g_testStore;
    g_testStore = nullptr;
}
#endif
