#include "CloudMerge.h"
#include "AppPaths.h"
#include "Tombstones.h"
#include "ItemMarks.h"

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
    // For each item, keep whichever position was saved more recently (ts). Never delete a local entry.
    for (auto it = resume.begin(); it != resume.end(); ++it)
    {
        const QJsonObject re = it.value().toObject();
        const QString prefix = QStringLiteral("resume/") + it.key() + QLatin1Char('/');
        const double localTs = store().value(prefix + QStringLiteral("ts"), 0.0).toDouble();
        const bool haveLocal = store().contains(prefix + QStringLiteral("pos"));
        if (haveLocal && re.value(QStringLiteral("ts")).toDouble() <= localTs) continue; // local is newer/equal
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
        auto ingest = [&byId](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("key")).toString().isEmpty()
                                       ? o.value(QStringLiteral("path")).toString()
                                       : o.value(QStringLiteral("key")).toString();
                if (id.isEmpty()) continue;
                if (!byId.contains(id) || o.value(QStringLiteral("ts")).toDouble() >= byId[id].value(QStringLiteral("ts")).toDouble())
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(localKey).toString().toUtf8()).array()); // local first
        ingest(QJsonDocument::fromJson(it.value().toString().toUtf8()).array());              // then remote
        QList<QJsonObject> merged = byId.values();
        std::sort(merged.begin(), merged.end(), [](const QJsonObject& a, const QJsonObject& b) {
            return a.value(QStringLiteral("ts")).toDouble() > b.value(QStringLiteral("ts")).toDouble();
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
                const qint64 lTs = static_cast<qint64>(
                    QJsonDocument::fromJson(localRaw).object().value(QStringLiteral("updatedAt")).toDouble());
                if (rTs <= lTs) continue; // local is newer/equal
            }
            s.setValue(ikey, QString::fromUtf8(QJsonDocument(rblob).toJson(QJsonDocument::Compact)));
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
                else if (o.value(QStringLiteral("ts")).toDouble() > byId[id].value(QStringLiteral("ts")).toDouble())
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
                else if (o.value(QStringLiteral("updatedAt")).toDouble() > byId[id].value(QStringLiteral("updatedAt")).toDouble())
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

} // namespace

void CloudMerge::serializeAll(QJsonObject& root)
{
    QJsonObject resume, recent, marks, favorites, playlists;
    serializeResumeRecent(resume, recent);
    serializeMarks(marks);
    serializeFavorites(favorites);
    serializePlaylists(playlists);
    root.insert(QStringLiteral("resume"), resume);
    root.insert(QStringLiteral("recent"), recent);
    root.insert(QStringLiteral("marks"), marks);
    root.insert(QStringLiteral("favorites"), favorites);
    root.insert(QStringLiteral("playlists"), playlists);
}

void CloudMerge::mergeAll(const QJsonObject& root)
{
    mergeResume(root.value(QStringLiteral("resume")).toObject());
    mergeRecent(root.value(QStringLiteral("recent")).toObject());
    mergeMarks(root.value(QStringLiteral("marks")).toObject());
    mergeFavorites(root.value(QStringLiteral("favorites")).toObject());
    mergePlaylists(root.value(QStringLiteral("playlists")).toObject());
    store().sync();
    ItemMarks::invalidate();      // the merge wrote marks/* under the ini directly; drop the stale static cache
    Tombstones::compact(30);      // keep the deleted/* footprint bounded (cheap; runs at every merge)
}
