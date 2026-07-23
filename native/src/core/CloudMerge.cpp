#include "CloudMerge.h"
#include "AppPaths.h"
#include "Tombstones.h"
#include "ItemMarks.h"
#include "ConsumptionStats.h"   // invalidate() after a namespaced-accumulator merge (mdsync T3)
#include "Settings.h"           // deviceId() — never clobber our own accumulator namespace on merge

#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <algorithm>

// Shares the portable mymediavault.ini with every other store (same AppPaths::dataDir() posture). Coherence
// with the store front-ends comes from every writer calling sync(); QSettings reloads on access when the
// on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

namespace {

// ---- small json helpers -----------------------------------------------------------------------------------

// Canonical serialized bytes of a JSON object (QJsonObject stores keys sorted, so Compact output is stable and
// device-independent). The order-independent equal-timestamp tie-break (mdsync T3, carried from the T2 review)
// compares these bytes lexically.
QByteArray canon(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

// Should the remote value replace the local one? Newest timestamp wins; on EQUAL timestamps a deterministic
// ORDER-INDEPENDENT decision — the lexically-greater canonical value bytes — so A-merges-B and B-merges-A pick
// the SAME winner (identical values compare equal -> no replace -> a no-op anyway). This uniform rule
// supersedes the divergent legacy ties (four stores kept-local, recents kept-remote via `>=`); that legacy
// recents byte behaviour is DELIBERATELY superseded here.
bool remoteReplaces(qint64 remoteTs, qint64 localTs, const QByteArray& remoteCanon, const QByteArray& localCanon)
{
    if (remoteTs != localTs) return remoteTs > localTs;
    return remoteCanon > localCanon; // equal ts -> order-independent value tie-break
}

QJsonArray stringsToArray(const QStringList& list)
{
    QJsonArray a;
    for (const QString& s : list) a.append(s);
    return a;
}

QStringList arrayToStrings(const QJsonArray& a)
{
    QStringList out;
    for (const QJsonValue& v : a)
    {
        const QString s = v.toString();
        if (!s.isEmpty() && !out.contains(s)) out.push_back(s);
    }
    return out;
}

// The profile ids present under a store: the union of its data groups and its tombstone groups (a profile that
// has ONLY deletions — every item removed — must still be serialized so its tombstones propagate).
QStringList profilesFor(const QString& dataRoot)
{
    QSet<QString> ids;
    QSettings& s = store();
    s.beginGroup(dataRoot);
    for (const QString& g : s.childGroups()) ids.insert(g);
    s.endGroup();
    s.beginGroup(QStringLiteral("deleted/") + dataRoot);
    for (const QString& g : s.childGroups()) ids.insert(g);
    s.endGroup();
    QStringList out(ids.begin(), ids.end());
    out.sort();
    return out;
}

// A store's tombstones as a JSON array [{key,ts}] for the document.
QJsonArray tombsToArray(const QString& tombStore)
{
    QJsonArray a;
    for (const Tombstones::Entry& e : Tombstones::all(tombStore))
    {
        QJsonObject o;
        o.insert(QStringLiteral("key"), e.key);
        o.insert(QStringLiteral("ts"), static_cast<double>(e.ts));
        a.append(o);
    }
    return a;
}

// Merge local + remote tombstones for a store into key->newest-ts, and IMPORT each into the local store at its
// faithful ts (record() never downgrades) so this device re-propagates the peer's deletion. Returns the map for
// the caller's suppression pass.
QHash<QString, qint64> mergeTombs(const QString& tombStore, const QJsonArray& remote)
{
    QHash<QString, qint64> map;
    for (const Tombstones::Entry& e : Tombstones::all(tombStore))
        if (e.ts > map.value(e.key, 0)) map.insert(e.key, e.ts);
    for (const QJsonValue& v : remote)
    {
        const QJsonObject o = v.toObject();
        const QString key = o.value(QStringLiteral("key")).toString();
        const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        if (key.isEmpty() || ts <= 0) continue;
        if (ts > map.value(key, 0)) map.insert(key, ts);
        Tombstones::record(tombStore, key, ts); // faithful ts; no-op if a newer local one already exists
    }
    return map;
}

// ---- resume / recent (moved VERBATIM from MainWindow, semantics unchanged) ---------------------------------

void serializeResumeRecent(QJsonObject& resume, QJsonObject& recent)
{
    for (const QString& key : store().allKeys())
    {
        if (key.startsWith(QStringLiteral("resume/")))
        {
            const QString rest = key.mid(7);       // "<hash>/<field>"
            const int slash = rest.indexOf(QLatin1Char('/'));
            if (slash <= 0) continue;
            const QString hash = rest.left(slash), field = rest.mid(slash + 1);
            QJsonObject e = resume.value(hash).toObject();
            if      (field == QStringLiteral("pos"))   e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("dur"))   e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("ts"))    e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("title")) e.insert(field, store().value(key).toString());
            resume.insert(hash, e);
        }
        else if (key.startsWith(QStringLiteral("recent/"))) // "recent/<profile>/items" -> the list JSON string
        {
            recent.insert(key.mid(7), store().value(key).toString());
        }
    }
}

void mergeResume(const QJsonObject& resume)
{
    // For each item, keep whichever position was saved more recently (ts). Never delete a local entry. On an
    // EQUAL ts, the order-independent value tie-break decides (below), replacing the old keep-local-on-tie.
    for (auto it = resume.begin(); it != resume.end(); ++it)
    {
        const QJsonObject re = it.value().toObject();
        const QString prefix = QStringLiteral("resume/") + it.key() + QLatin1Char('/');
        const bool haveLocal = store().contains(prefix + QStringLiteral("pos"));
        if (haveLocal)
        {
            const qint64 localTs  = static_cast<qint64>(store().value(prefix + QStringLiteral("ts"), 0.0).toDouble());
            const qint64 remoteTs = static_cast<qint64>(re.value(QStringLiteral("ts")).toDouble());
            // Rebuild the local entry in the SAME shape serializeResumeRecent emits, so its canonical bytes
            // match this device's serialized form (a prerequisite for order-independence).
            QJsonObject localObj;
            if (store().contains(prefix + QStringLiteral("pos")))   localObj.insert(QStringLiteral("pos"),   store().value(prefix + QStringLiteral("pos")).toDouble());
            if (store().contains(prefix + QStringLiteral("dur")))   localObj.insert(QStringLiteral("dur"),   store().value(prefix + QStringLiteral("dur")).toDouble());
            if (store().contains(prefix + QStringLiteral("ts")))    localObj.insert(QStringLiteral("ts"),    store().value(prefix + QStringLiteral("ts")).toDouble());
            if (store().contains(prefix + QStringLiteral("title"))) localObj.insert(QStringLiteral("title"), store().value(prefix + QStringLiteral("title")).toString());
            if (!remoteReplaces(remoteTs, localTs, canon(re), canon(localObj))) continue;
        }
        if (re.contains(QStringLiteral("pos")))   store().setValue(prefix + QStringLiteral("pos"),   re.value(QStringLiteral("pos")).toDouble());
        if (re.contains(QStringLiteral("dur")))   store().setValue(prefix + QStringLiteral("dur"),   re.value(QStringLiteral("dur")).toDouble());
        if (re.contains(QStringLiteral("ts")))    store().setValue(prefix + QStringLiteral("ts"),    re.value(QStringLiteral("ts")).toDouble());
        if (re.contains(QStringLiteral("title"))) store().setValue(prefix + QStringLiteral("title"), re.value(QStringLiteral("title")).toString());
    }
}

void mergeRecent(const QJsonObject& recent)
{
    // Union the local + remote lists per profile by stable identity (key, else path), keeping the newest ts for
    // each, sorted newest-first and capped.
    for (auto it = recent.begin(); it != recent.end(); ++it)
    {
        const QString localKey = QStringLiteral("recent/") + it.key();
        QHash<QString, QJsonObject> byId;
        // Dedup by id keeping the winner per remoteReplaces (newest ts; equal ts -> greater canonical bytes).
        // The tie-break is order-independent, so which list is ingested first no longer changes the winner
        // (the old `>=` made the second-ingested list win ties — an order-dependent divergence).
        auto ingest = [&byId](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("key")).toString().isEmpty()
                                       ? o.value(QStringLiteral("path")).toString()
                                       : o.value(QStringLiteral("key")).toString();
                if (id.isEmpty()) continue;
                auto cur = byId.constFind(id);
                if (cur == byId.constEnd()
                    || remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble()),
                                      static_cast<qint64>(cur.value().value(QStringLiteral("ts")).toDouble()),
                                      canon(o), canon(cur.value())))
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(localKey).toString().toUtf8()).array()); // local first
        ingest(QJsonDocument::fromJson(it.value().toString().toUtf8()).array());              // then remote
        QList<QJsonObject> merged = byId.values();
        // Newest-first; ties broken by canonical bytes so the cap-40 cut is deterministic (order-independent).
        std::sort(merged.begin(), merged.end(), [](const QJsonObject& a, const QJsonObject& b) {
            const double at = a.value(QStringLiteral("ts")).toDouble(), bt = b.value(QStringLiteral("ts")).toDouble();
            if (at != bt) return at > bt;
            return canon(a) > canon(b);
        });
        QJsonArray out;
        for (int i = 0; i < merged.size() && i < 40; ++i) out.append(merged[i]); // cap matches RecentStore's
        store().setValue(localKey, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
    }
}

// ---- marks (items newest-updatedAt; vocab/pinned union-minus-tombstoned) -----------------------------------

QString marksItemsGroup(const QString& p) { return QStringLiteral("marks/") + p + QStringLiteral("/items"); }
QString vocabKey(const QString& p)        { return QStringLiteral("marks/") + p + QStringLiteral("/tagVocab"); }
QString pinnedKey(const QString& p)       { return QStringLiteral("marks/") + p + QStringLiteral("/pinnedTags"); }
QString vocabTombStore(const QString& p)  { return QStringLiteral("marks/") + p + QStringLiteral("/tagVocab"); }
QString pinnedTombStore(const QString& p) { return QStringLiteral("marks/") + p + QStringLiteral("/pinnedTags"); }

void serializeMarks(QJsonObject& marks)
{
    for (const QString& p : profilesFor(QStringLiteral("marks")))
    {
        QJsonObject items;
        {
            QSettings& s = store();
            s.beginGroup(marksItemsGroup(p));
            const QStringList hashes = s.childKeys();
            for (const QString& h : hashes)
                items.insert(h, QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
            s.endGroup();
        }
        const QStringList vocab  = arrayToStrings(QJsonDocument::fromJson(store().value(vocabKey(p)).toString().toUtf8()).array());
        const QStringList pinned = arrayToStrings(QJsonDocument::fromJson(store().value(pinnedKey(p)).toString().toUtf8()).array());
        const QJsonArray vTombs = tombsToArray(vocabTombStore(p));
        const QJsonArray pTombs = tombsToArray(pinnedTombStore(p));
        if (items.isEmpty() && vocab.isEmpty() && pinned.isEmpty() && vTombs.isEmpty() && pTombs.isEmpty())
            continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tagVocab"), stringsToArray(vocab));
        po.insert(QStringLiteral("pinnedTags"), stringsToArray(pinned));
        po.insert(QStringLiteral("vocabTombs"), vTombs);
        po.insert(QStringLiteral("pinnedTombs"), pTombs);
        marks.insert(p, po);
    }
}

void mergeMarks(const QJsonObject& marks)
{
    for (auto it = marks.begin(); it != marks.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Items: newest updatedAt wins per hash; never delete.
        const QJsonObject remoteItems = po.value(QStringLiteral("items")).toObject();
        QSettings& s = store();
        for (auto ri = remoteItems.begin(); ri != remoteItems.end(); ++ri)
        {
            const QJsonObject rblob = ri.value().toObject();
            const qint64 rTs = static_cast<qint64>(rblob.value(QStringLiteral("updatedAt")).toDouble());
            const QString ikey = marksItemsGroup(p) + QLatin1Char('/') + ri.key();
            const QByteArray localRaw = s.value(ikey).toString().toUtf8();
            if (!localRaw.isEmpty())
            {
                const QJsonObject lblob = QJsonDocument::fromJson(localRaw).object();
                const qint64 lTs = static_cast<qint64>(lblob.value(QStringLiteral("updatedAt")).toDouble());
                if (!remoteReplaces(rTs, lTs, canon(rblob), canon(lblob))) continue; // equal ts -> value tie-break
            }
            s.setValue(ikey, QString::fromUtf8(canon(rblob)));
        }

        // Tombstones (merged + imported); then vocab/pinned = union MINUS tombstoned.
        const QHash<QString, qint64> vTombs = mergeTombs(vocabTombStore(p),  po.value(QStringLiteral("vocabTombs")).toArray());
        const QHash<QString, qint64> pTombs = mergeTombs(pinnedTombStore(p), po.value(QStringLiteral("pinnedTombs")).toArray());

        QStringList vocab = arrayToStrings(QJsonDocument::fromJson(s.value(vocabKey(p)).toString().toUtf8()).array());
        for (const QString& t : arrayToStrings(po.value(QStringLiteral("tagVocab")).toArray()))
            if (!vocab.contains(t)) vocab.push_back(t);
        QStringList mergedVocab;
        for (const QString& t : vocab)
            if (!vTombs.contains(t)) mergedVocab.push_back(t);        // a deleted tag stays gone
        if (mergedVocab.isEmpty()) s.remove(vocabKey(p));
        else s.setValue(vocabKey(p), QString::fromUtf8(QJsonDocument(stringsToArray(mergedVocab)).toJson(QJsonDocument::Compact)));

        QStringList pinned = arrayToStrings(QJsonDocument::fromJson(s.value(pinnedKey(p)).toString().toUtf8()).array());
        for (const QString& t : arrayToStrings(po.value(QStringLiteral("pinnedTags")).toArray()))
            if (!pinned.contains(t)) pinned.push_back(t);
        QStringList mergedPinned;
        for (const QString& t : pinned)
            if (!vTombs.contains(t) && !pTombs.contains(t)) mergedPinned.push_back(t); // deleted OR unpinned -> no shelf
        if (mergedPinned.isEmpty()) s.remove(pinnedKey(p));
        else s.setValue(pinnedKey(p), QString::fromUtf8(QJsonDocument(stringsToArray(mergedPinned)).toJson(QJsonDocument::Compact)));

        s.sync();
    }
}

// ---- favourites (union by itemId newest-ts + tombstones) ---------------------------------------------------

QString favKey(const QString& p)       { return QStringLiteral("favorites/") + p + QStringLiteral("/items"); }
QString favTombStore(const QString& p)  { return QStringLiteral("favorites/") + p; }

void serializeFavorites(QJsonObject& favorites)
{
    for (const QString& p : profilesFor(QStringLiteral("favorites")))
    {
        const QJsonArray items = QJsonDocument::fromJson(store().value(favKey(p)).toString().toUtf8()).array();
        const QJsonArray tombs = tombsToArray(favTombStore(p));
        if (items.isEmpty() && tombs.isEmpty()) continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tombs"), tombs);
        favorites.insert(p, po);
    }
}

void mergeFavorites(const QJsonObject& favorites)
{
    for (auto it = favorites.begin(); it != favorites.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Union local + remote by itemId, newest ts wins.
        QHash<QString, QJsonObject> byId;
        QStringList order; // preserve a stable newest-first order (local first, then remote extras)
        auto ingest = [&](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("itemId")).toString();
                if (id.isEmpty()) continue;
                if (!byId.contains(id)) { byId.insert(id, o); order.push_back(id); }
                else if (remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble()),
                                        static_cast<qint64>(byId[id].value(QStringLiteral("ts")).toDouble()),
                                        canon(o), canon(byId[id]))) // equal ts -> order-independent value tie-break
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(favKey(p)).toString().toUtf8()).array());
        ingest(po.value(QStringLiteral("items")).toArray());

        const QHash<QString, qint64> tombs = mergeTombs(favTombStore(p), po.value(QStringLiteral("tombs")).toArray());

        QJsonArray out;
        for (const QString& id : order)
        {
            const QJsonObject o = byId.value(id);
            const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
            if (tombs.value(id, 0) >= ts) continue; // tombstone beats older/equal; a strictly-newer re-add wins
            out.append(o);
        }
        store().setValue(favKey(p), QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        store().sync();
    }
}

// ---- playlists (whole-object newest-updatedAt + tombstones) ------------------------------------------------

QString plKey(const QString& p)        { return QStringLiteral("playlists/") + p + QStringLiteral("/items"); }
QString plTombStore(const QString& p)   { return QStringLiteral("playlists/") + p; }

void serializePlaylists(QJsonObject& playlists)
{
    for (const QString& p : profilesFor(QStringLiteral("playlists")))
    {
        const QJsonArray items = QJsonDocument::fromJson(store().value(plKey(p)).toString().toUtf8()).array();
        const QJsonArray tombs = tombsToArray(plTombStore(p));
        if (items.isEmpty() && tombs.isEmpty()) continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tombs"), tombs);
        playlists.insert(p, po);
    }
}

void mergePlaylists(const QJsonObject& playlists)
{
    for (auto it = playlists.begin(); it != playlists.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Whole-object union by playlist id, newest updatedAt wins.
        QHash<QString, QJsonObject> byId;
        QStringList order;
        auto ingest = [&](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("id")).toString();
                if (id.isEmpty()) continue;
                if (!byId.contains(id)) { byId.insert(id, o); order.push_back(id); }
                else if (remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble()),
                                        static_cast<qint64>(byId[id].value(QStringLiteral("updatedAt")).toDouble()),
                                        canon(o), canon(byId[id]))) // equal updatedAt -> order-independent tie-break
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(plKey(p)).toString().toUtf8()).array());
        ingest(po.value(QStringLiteral("items")).toArray());

        const QHash<QString, qint64> tombs = mergeTombs(plTombStore(p), po.value(QStringLiteral("tombs")).toArray());

        QJsonArray out;
        for (const QString& id : order)
        {
            const QJsonObject o = byId.value(id);
            const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
            if (tombs.value(id, 0) >= ts) continue; // deleted unless a strictly-newer edit resurrects it
            out.append(o);
        }
        store().setValue(plKey(p), QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        store().sync();
    }
}

// ---- device-namespaced accumulators (stats / playstats) — union VERBATIM, never arithmetic (mdsync T3) -----
// Shape: rootPrefix/<profile>/<device>/<sub...>  serialized as { "<profile>": { "<device>": { "<sub>": val }}}.
// A device's namespace is ONLY ever written by that device, so on merge each REMOTE namespace is copied
// wholesale (verbatim replace, never arithmetic) and the LOCAL device's own namespace is never touched.
// Repeated merges therefore never double-count (replace, not add).
//
// Per-namespace freshness (mdsync T4, from the T3 review): a peer can carry a STALE copy of a THIRD device's
// namespace, so a blind verbatim replace would let that stale copy downgrade a locally-fresher one. Each owner
// stamps a `lastWrite` scalar leaf in its namespace at accrual (ConsumptionStats/PlayStats), and that stamp
// travels verbatim; mergeNamespaced replaces a foreign namespace only when the incoming lastWrite is strictly
// NEWER than the local copy's (or the local copy is absent) — newest-wins per foreign namespace.

void serializeNamespaced(const QString& rootPrefix, QJsonObject& out)
{
    QSettings& s = store();
    const QString pfx = rootPrefix + QLatin1Char('/');
    for (const QString& key : s.allKeys())
    {
        if (!key.startsWith(pfx)) continue;
        const QString rest = key.mid(pfx.size());          // "<profile>/<device>/<sub...>"
        const int s1 = rest.indexOf(QLatin1Char('/'));
        if (s1 <= 0) continue;
        const int s2 = rest.indexOf(QLatin1Char('/'), s1 + 1);
        if (s2 <= s1) continue;                             // excludes the "<profile>/schema" stamp (one slash)
        const QString profile = rest.left(s1);
        const QString device  = rest.mid(s1 + 1, s2 - s1 - 1);
        const QString sub     = rest.mid(s2 + 1);
        // Defensive: a device id is a UUID, never these legacy-shape leaves — skip un-migrated stats so a
        // pre-migration key can't be mis-serialized under a fake device (startup migrate() normally precludes).
        if (device == QStringLiteral("items") || device == QStringLiteral("cat")) continue;
        QJsonObject prof = out.value(profile).toObject();
        QJsonObject dev  = prof.value(device).toObject();
        dev.insert(sub, s.value(key).toString());
        prof.insert(device, dev);
        out.insert(profile, prof);
    }
}

void mergeNamespaced(const QString& rootPrefix, const QJsonObject& in, const QString& localDevice)
{
    QSettings& s = store();
    for (auto pit = in.begin(); pit != in.end(); ++pit)
    {
        const QString profile = pit.key();
        const QJsonObject devices = pit.value().toObject();
        for (auto dit = devices.begin(); dit != devices.end(); ++dit)
        {
            const QString device = dit.key();
            if (device == localDevice) continue;            // never clobber our own live namespace
            const QString base = rootPrefix + QLatin1Char('/') + profile + QLatin1Char('/') + device;
            const QJsonObject ns = dit.value().toObject();

            // Freshness gate: keep a locally-fresher copy of this foreign namespace. lastWrite is stored as a
            // decimal-string scalar leaf (owner-stamped, then carried verbatim), so read both via toLongLong.
            const qint64 remoteLW = ns.value(QStringLiteral("lastWrite")).toString().toLongLong();
            s.beginGroup(base);
            const bool localPresent = !s.childKeys().isEmpty() || !s.childGroups().isEmpty();
            s.endGroup();
            if (localPresent)
            {
                const qint64 localLW = s.value(base + QStringLiteral("/lastWrite")).toString().toLongLong();
                if (remoteLW <= localLW) continue;          // our copy is as-fresh-or-fresher -> keep it
            }

            s.remove(base);                                 // verbatim replace: drop the stale copy first
            for (auto kit = ns.begin(); kit != ns.end(); ++kit)
                s.setValue(base + QLatin1Char('/') + kit.key(), kit.value().toString());
        }
    }
}

} // namespace

void CloudMerge::serializeAll(QJsonObject& root)
{
    QJsonObject resume, recent, marks, favorites, playlists, stats, playstats;
    serializeResumeRecent(resume, recent);
    serializeMarks(marks);
    serializeFavorites(favorites);
    serializePlaylists(playlists);
    serializeNamespaced(QStringLiteral("stats"), stats);         // device-namespaced accumulators (mdsync T3)
    serializeNamespaced(QStringLiteral("playstats"), playstats);
    root.insert(QStringLiteral("resume"), resume);
    root.insert(QStringLiteral("recent"), recent);
    root.insert(QStringLiteral("marks"), marks);
    root.insert(QStringLiteral("favorites"), favorites);
    root.insert(QStringLiteral("playlists"), playlists);
    root.insert(QStringLiteral("stats"), stats);
    root.insert(QStringLiteral("playstats"), playstats);
}

void CloudMerge::mergeAll(const QJsonObject& root)
{
    mergeResume(root.value(QStringLiteral("resume")).toObject());
    mergeRecent(root.value(QStringLiteral("recent")).toObject());
    mergeMarks(root.value(QStringLiteral("marks")).toObject());
    mergeFavorites(root.value(QStringLiteral("favorites")).toObject());
    mergePlaylists(root.value(QStringLiteral("playlists")).toObject());
    const QString localDevice = Settings::deviceId();
    mergeNamespaced(QStringLiteral("stats"),     root.value(QStringLiteral("stats")).toObject(),     localDevice);
    mergeNamespaced(QStringLiteral("playstats"), root.value(QStringLiteral("playstats")).toObject(), localDevice);
    store().sync();
    ItemMarks::invalidate();      // the merge wrote marks/* under the ini directly; drop the stale static cache
    ConsumptionStats::invalidate(); // ditto for the summed-across-devices stats cache
    Tombstones::compact(30);      // keep the deleted/* footprint bounded (cheap; runs at every merge)
}
