// Headless check of the multi-device-sync FOUNDATION (mdsync T1): the device identity, the per-store write
// timestamps, and the deletion tombstones that the generalized CloudMerge pass (T2) will read. QtCore-only
// (QSettings + JSON wrappers over the shared portable mymediavault.ini), so it runs under the offscreen QPA in
// CI and pins the contract T2's serializers lean on:
//
//   * Settings::deviceId() — minted ONCE (a stable UUID), persisted at exactly key "device/id", and never
//     regenerated on a repeat read (write-once);
//   * every per-store write FUNNEL stamps a sane epoch timestamp readable from the blob — ItemMarks::saveItem
//     AND the removeTagEverywhere direct-rewrite path (updatedAt), FavoritesStore::save (ts), and every
//     PlaylistStore mutator (updatedAt), through the single setValue funnel;
//   * Tombstones::{record,all,compact} — record stamps now under a per-store namespace, all() returns
//     {original key, ts}, compact(30) drops ONLY entries older than 30 days (the boundary is kept);
//   * per-profile isolation — a tombstone recorded for profile A is invisible under profile B (the store
//     namespace mirrors each store's per-profile shape);
//   * the wired remove-sites tombstone (FavoritesStore::remove, PlaylistStore::remove, ItemMarks tag deletion),
//     while hiding an item is NOT a delete and records no tombstone.
//
// Prints CLOUDMERGE-OK on success; any failure prints CLOUDMERGE-FAIL <cond> (line) and exits non-zero.
//
// Isolation: like the other core probes (probe_marks/probe_sync), AppPaths::dataDir() is the probe exe's own
// build-tree folder (portable app), so the mymediavault.ini it reads/writes sits next to the probe and never
// touches a deployed install. We wipe the groups we use at start and seed our own profile ids via
// ProfileStore::setCurrent so a developer's real profile/data can't leak into the asserts.
#include "Settings.h"
#include "ItemMarks.h"
#include "FavoritesStore.h"
#include "PlaylistStore.h"
#include "Tombstones.h"
#include "CloudMerge.h"
#include "CloudSync.h"      // mdsync T4: the device-local carve-out + bundle-settings hands-off
#include "ProfileStore.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QSettings>
#include <QCryptographicHash>
#include <QDateTime>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVector>
#include <QPair>
#include <tuple>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CLOUDMERGE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static QString md5(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

static void useProfile(const QString& id)
{
    ProfileStore::setCurrent(id);
    ItemMarks::invalidate();
}

// A "sane" epoch stamp is within a generous window around the write (never 0, never absurdly future/past).
static bool saneTs(qint64 ts, qint64 before, qint64 after)
{
    return ts >= before && ts <= after;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/mymediavault.ini");

    // Reset every group we touch so a stale ini can't skew the asserts.
    {
        QSettings reset(iniPath, QSettings::IniFormat);
        for (const char* g : {"device", "marks", "favorites", "playlists", "deleted", "profiles", "stats", "playstats", "resume", "recent"})
            reset.remove(QLatin1String(g));
        reset.sync();
    }
    ItemMarks::invalidate();

    // ---- 1. deviceId: mint-once + persist + excluded key SHAPE ----------------------------------------------
    {
        const QString d1 = Settings::deviceId();
        CHECK(!d1.isEmpty());                       // always mints a non-empty id
        const QString d2 = Settings::deviceId();
        CHECK(d2 == d1);                            // write-once: a repeat read never regenerates
        QSettings raw(iniPath, QSettings::IniFormat);
        CHECK(raw.value(QStringLiteral("device/id")).toString() == d1); // persisted at exactly device/id
        // Excluded-shape: the ONLY key under "device" is "id" (the key name T4's carve-out pins on).
        raw.beginGroup(QStringLiteral("device"));
        const QStringList deviceKeys = raw.childKeys();
        raw.endGroup();
        CHECK(deviceKeys == QStringList{QStringLiteral("id")});
    }

    // ---- 2. Tombstone record / all / compact (incl. the 30-day boundary) ------------------------------------
    {
        const QString ts = QStringLiteral("tsprobe");
        Tombstones::record(ts, QStringLiteral("k1"));
        Tombstones::record(ts, QStringLiteral("https://x/y")); // URL-shaped key survives the hash round-trip
        const QVector<Tombstones::Entry> got = Tombstones::all(ts);
        CHECK(got.size() == 2);
        bool haveK1 = false, haveUrl = false;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (const Tombstones::Entry& e : got)
        {
            if (e.key == QStringLiteral("k1")) haveK1 = true;
            if (e.key == QStringLiteral("https://x/y")) haveUrl = true;
            CHECK(e.ts > 0 && e.ts <= now); // ORIGINAL key preserved + a sane recent stamp
        }
        CHECK(haveK1 && haveUrl);

        // Compaction boundary: inject three raw tombstones bracketing the 30-day line by an hour of slack, so
        // the assert never races the clock. fresh + just-inside(29.96d) survive; just-outside(30.04d) is dropped.
        const QString cs = QStringLiteral("compactprobe");
        auto injectRaw = [&](const QString& key, qint64 tsSecs) {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("deleted/") + cs + QLatin1Char('/') + md5(key),
                         QStringLiteral("{\"key\":\"%1\",\"ts\":%2}").arg(key).arg(tsSecs));
            raw.sync();
        };
        const qint64 day = 86400;
        injectRaw(QStringLiteral("fresh"),   now);
        injectRaw(QStringLiteral("inside"),  now - 30 * day + 3600); // 29.96 days old -> kept
        injectRaw(QStringLiteral("outside"), now - 30 * day - 3600); // 30.04 days old -> dropped
        CHECK(Tombstones::all(cs).size() == 3);
        const int dropped = Tombstones::compact(30);
        CHECK(dropped == 1);                                        // exactly the just-outside one
        const QVector<Tombstones::Entry> after = Tombstones::all(cs);
        CHECK(after.size() == 2);
        QStringList remaining;
        for (const Tombstones::Entry& e : after) remaining << e.key;
        CHECK(remaining.contains(QStringLiteral("fresh")));
        CHECK(remaining.contains(QStringLiteral("inside")));
        CHECK(!remaining.contains(QStringLiteral("outside")));      // >30d gone
    }

    // ---- 3. Per-profile isolation of tombstones -------------------------------------------------------------
    {
        Tombstones::record(QStringLiteral("favorites/pA"), QStringLiteral("only-A"));
        CHECK(Tombstones::all(QStringLiteral("favorites/pA")).size() == 1);
        CHECK(Tombstones::all(QStringLiteral("favorites/pB")).isEmpty()); // B's namespace can't see A's tombstone
    }

    // ---- 4. ItemMarks: saveItem stamps updatedAt; hide is NOT a delete --------------------------------------
    {
        useProfile(QStringLiteral("cmA"));
        const qint64 before = QDateTime::currentSecsSinceEpoch();
        ItemMarks::setTags(QStringLiteral("game:doom"), QStringList{QStringLiteral("fps"), QStringLiteral("classic")});
        const qint64 after = QDateTime::currentSecsSinceEpoch();
        const ItemMarks::Marks m = ItemMarks::get(QStringLiteral("game:doom"));
        CHECK(m.tags.contains(QStringLiteral("fps")));
        CHECK(saneTs(m.updatedAt, before, after));                 // the write funnel stamped it

        // Hiding is a mark, not a removal: it stamps updatedAt but records NO tombstone.
        ItemMarks::setHidden(QStringLiteral("game:doom"), true);
        CHECK(ItemMarks::get(QStringLiteral("game:doom")).hidden);
        CHECK(Tombstones::all(QStringLiteral("marks/cmA/tagVocab")).isEmpty());  // no tombstone from hiding
    }

    // ---- 5. removeTagEverywhere: stamps surviving items AND tombstones the tag name in vocab space ----------
    {
        useProfile(QStringLiteral("cmTag"));
        ItemMarks::setTags(QStringLiteral("itemA"), QStringList{QStringLiteral("shared"), QStringLiteral("keepA")});
        ItemMarks::setTags(QStringLiteral("itemB"), QStringList{QStringLiteral("shared")});
        const qint64 before = QDateTime::currentSecsSinceEpoch();
        ItemMarks::removeTagEverywhere(QStringLiteral("shared"));
        const qint64 after = QDateTime::currentSecsSinceEpoch();
        // itemA lost "shared" but keeps "keepA" (still a non-default blob) — its updatedAt must be re-stamped.
        const ItemMarks::Marks a = ItemMarks::get(QStringLiteral("itemA"));
        CHECK(!a.tags.contains(QStringLiteral("shared")));
        CHECK(a.tags.contains(QStringLiteral("keepA")));
        CHECK(saneTs(a.updatedAt, before, after));                 // the direct-rewrite path stamped too
        // The retired tag name is tombstoned in the profile's vocab space.
        const QVector<Tombstones::Entry> tv = Tombstones::all(QStringLiteral("marks/cmTag/tagVocab"));
        bool tagTombstoned = false;
        for (const Tombstones::Entry& e : tv) if (e.key == QStringLiteral("shared")) tagTombstoned = true;
        CHECK(tagTombstoned);
    }

    // ---- 6. FavoritesStore: save stamps ts; remove tombstones the itemId ------------------------------------
    {
        useProfile(QStringLiteral("cmFav"));
        FavoriteItem f; f.addonId = QStringLiteral("addon"); f.itemId = QStringLiteral("movie:matrix");
        f.title = QStringLiteral("The Matrix"); f.type = QStringLiteral("movie");
        const qint64 before = QDateTime::currentSecsSinceEpoch();
        FavoritesStore::add(f);
        const qint64 after = QDateTime::currentSecsSinceEpoch();
        const QVector<FavoriteItem> favs = FavoritesStore::list();
        CHECK(favs.size() == 1);
        CHECK(saneTs(favs.first().ts, before, after));             // save() stamped a per-item ts
        CHECK(Tombstones::all(QStringLiteral("favorites/cmFav")).isEmpty()); // adding tombstones nothing

        FavoritesStore::remove(QStringLiteral("movie:matrix"));
        CHECK(FavoritesStore::list().isEmpty());
        const QVector<Tombstones::Entry> ft = Tombstones::all(QStringLiteral("favorites/cmFav"));
        CHECK(ft.size() == 1 && ft.first().key == QStringLiteral("movie:matrix")); // removal tombstoned the id
    }

    // ---- 6b. Legacy ts==0 is NEVER backfilled (the cross-device resurrection guard) ------------------------
    // save() persists ts verbatim; the stamp is set at add(), not on every rewrite. A pre-upgrade favourite
    // (no ts field) must stay ts==0 (= oldest) through unrelated saves, so its rewrite can never out-date a
    // real deletion tombstone from another device and resurrect a deleted favourite.
    {
        useProfile(QStringLiteral("cmLegacy"));
        // Inject a pre-upgrade favourite with NO ts field straight into the ini.
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("favorites/cmLegacy/items"),
                QStringLiteral("[{\"addonId\":\"a\",\"itemId\":\"legacy:F\",\"title\":\"F\",\"type\":\"movie\"}]"));
            raw.sync();
        }
        {
            const QVector<FavoriteItem> l = FavoritesStore::list();
            CHECK(l.size() == 1 && l.first().ts == 0);          // legacy reads back as ts==0 (oldest)
        }
        // A save triggered by ADDING another favourite must not backfill the legacy item's ts.
        FavoriteItem g; g.addonId = QStringLiteral("a"); g.itemId = QStringLiteral("new:G");
        g.title = QStringLiteral("G"); g.type = QStringLiteral("movie");
        FavoritesStore::add(g);
        qint64 legacyTs = -1, gTs = -1;
        for (const FavoriteItem& f : FavoritesStore::list())
        {
            if (f.itemId == QStringLiteral("legacy:F")) legacyTs = f.ts;
            if (f.itemId == QStringLiteral("new:G"))     gTs = f.ts;
        }
        CHECK(legacyTs == 0);                                   // NOT backfilled by the unrelated save
        CHECK(gTs > 0);                                         // the genuine add() stamped now
        // Resurrection sequence: un-star the legacy F -> its tombstone (T1=now) is strictly newer than F's
        // ts (0), so a newest-wins-vs-tombstone merge resolves DELETED — F cannot resurrect from its rewrite.
        FavoritesStore::remove(QStringLiteral("legacy:F"));
        qint64 tomb = -1;
        for (const Tombstones::Entry& e : Tombstones::all(QStringLiteral("favorites/cmLegacy")))
            if (e.key == QStringLiteral("legacy:F")) tomb = e.ts;
        CHECK(tomb > 0);
        CHECK(tomb > legacyTs);                                 // tombstone beats ts==0 -> stays deleted
    }

    // ---- 7. PlaylistStore: every mutator stamps updatedAt; remove tombstones the id -------------------------
    {
        useProfile(QStringLiteral("cmPl"));
        qint64 before = QDateTime::currentSecsSinceEpoch();
        const QString pid = PlaylistStore::create(QStringLiteral("video"), QStringLiteral("Night In"));
        qint64 after = QDateTime::currentSecsSinceEpoch();
        Playlist p;
        CHECK(PlaylistStore::get(pid, p));
        CHECK(saneTs(p.updatedAt, before, after));                 // create stamped

        before = QDateTime::currentSecsSinceEpoch();
        PlaylistStore::rename(pid, QStringLiteral("Renamed"));
        after = QDateTime::currentSecsSinceEpoch();
        CHECK(PlaylistStore::get(pid, p) && p.name == QStringLiteral("Renamed"));
        CHECK(saneTs(p.updatedAt, before, after));                 // rename re-stamped

        PlaylistEntry e; e.addonId = QStringLiteral("a"); e.itemId = QStringLiteral("ep1"); e.title = QStringLiteral("Ep 1");
        before = QDateTime::currentSecsSinceEpoch();
        PlaylistStore::addItem(pid, e);
        after = QDateTime::currentSecsSinceEpoch();
        CHECK(PlaylistStore::get(pid, p) && p.items.size() == 1);
        CHECK(saneTs(p.updatedAt, before, after));                 // addItem re-stamped

        before = QDateTime::currentSecsSinceEpoch();
        PlaylistStore::removeItem(pid, QStringLiteral("ep1"));
        after = QDateTime::currentSecsSinceEpoch();
        CHECK(PlaylistStore::get(pid, p) && p.items.isEmpty());
        CHECK(saneTs(p.updatedAt, before, after));                 // removeItem re-stamped

        PlaylistStore::remove(pid);
        CHECK(!PlaylistStore::get(pid, p));
        const QVector<Tombstones::Entry> pt = Tombstones::all(QStringLiteral("playlists/cmPl"));
        CHECK(pt.size() == 1 && pt.first().key == pid);            // remove tombstoned the playlist id
    }

    // ========================================================================================================
    //  T2 — the generalized CloudMerge serialize/merge matrix. All pure ini-in/json-out: we inject a "remote"
    //  device's ini state, serialize it to a document, wipe, inject the "local" device's state, merge the
    //  remote document in, and assert the resulting local ini. Timestamps are anchored near `now` so the
    //  30-day compaction that mergeAll() runs at its tail never drops the fixtures mid-test.
    // ========================================================================================================
    const qint64 T = QDateTime::currentSecsSinceEpoch();
    auto setRaw = [&](const QString& key, const QString& val) {
        QSettings raw(iniPath, QSettings::IniFormat); raw.setValue(key, val); raw.sync();
    };
    auto compact = [](const QJsonArray& a) { return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)); };
    auto compactO = [](const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); };
    auto wipeStores = [&]() {
        QSettings raw(iniPath, QSettings::IniFormat);
        for (const char* g : {"marks", "favorites", "playlists", "deleted", "resume", "recent"})
            raw.remove(QLatin1String(g));
        raw.sync();
        ItemMarks::invalidate();
    };
    auto serializeNow = [&]() { QJsonObject r; CloudMerge::serializeAll(r); return r; };
    auto mergeDoc = [&](const QJsonObject& doc) { CloudMerge::mergeAll(doc); };

    // Injection helpers (raw ini, explicit ts).
    auto injFavs = [&](const QString& p, const QVector<QPair<QString, qint64>>& items) {
        QJsonArray a;
        for (const auto& it : items) { QJsonObject o; o["itemId"] = it.first; o["title"] = it.first; o["ts"] = double(it.second); a.append(o); }
        setRaw(QStringLiteral("favorites/") + p + QStringLiteral("/items"), compact(a));
    };
    // playlist: (id, name, updatedAt, itemCount)
    auto injPlaylists = [&](const QString& p, const QVector<std::tuple<QString, QString, qint64, int>>& pls) {
        QJsonArray a;
        for (const auto& pl : pls) {
            QJsonObject o; o["id"] = std::get<0>(pl); o["name"] = std::get<1>(pl);
            o["categoryKey"] = QStringLiteral("video"); o["updatedAt"] = double(std::get<2>(pl));
            QJsonArray items; for (int i = 0; i < std::get<3>(pl); ++i) { QJsonObject e; e["itemId"] = QStringLiteral("e") + QString::number(i); items.append(e); }
            o["items"] = items; a.append(o);
        }
        setRaw(QStringLiteral("playlists/") + p + QStringLiteral("/items"), compact(a));
    };
    auto injMarkItem = [&](const QString& p, const QString& key, const QStringList& tags, qint64 upd) {
        QJsonObject o; o["hidden"] = false; o["completion"] = QStringLiteral("none");
        QJsonArray t; for (const QString& x : tags) t.append(x); o["tags"] = t; o["updatedAt"] = double(upd);
        setRaw(QStringLiteral("marks/") + p + QStringLiteral("/items/") + md5(key), compactO(o));
    };
    auto injArr = [&](const QString& key, const QStringList& vals) {
        QJsonArray a; for (const QString& v : vals) a.append(v); setRaw(key, compact(a));
    };
    auto injTomb = [&](const QString& tstore, const QString& key, qint64 ts) {
        QJsonObject o; o["key"] = key; o["ts"] = double(ts);
        setRaw(QStringLiteral("deleted/") + tstore + QLatin1Char('/') + md5(key), compactO(o));
    };

    // Readback helpers (fresh QSettings each time -> always current on disk).
    auto favIds = [&](const QString& p) {
        QSettings raw(iniPath, QSettings::IniFormat); QStringList out;
        for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(QStringLiteral("favorites/") + p + QStringLiteral("/items")).toString().toUtf8()).array())
            out << v.toObject().value(QStringLiteral("itemId")).toString();
        out.sort(); return out;
    };
    auto favTs = [&](const QString& p, const QString& id) -> qint64 {
        QSettings raw(iniPath, QSettings::IniFormat);
        for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(QStringLiteral("favorites/") + p + QStringLiteral("/items")).toString().toUtf8()).array())
        { const QJsonObject o = v.toObject(); if (o.value(QStringLiteral("itemId")).toString() == id) return qint64(o.value(QStringLiteral("ts")).toDouble()); }
        return -1;
    };
    auto plField = [&](const QString& p, const QString& id) -> QPair<QString, qint64> {
        QSettings raw(iniPath, QSettings::IniFormat);
        for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(QStringLiteral("playlists/") + p + QStringLiteral("/items")).toString().toUtf8()).array())
        { const QJsonObject o = v.toObject(); if (o.value(QStringLiteral("id")).toString() == id) return { o.value(QStringLiteral("name")).toString(), qint64(o.value(QStringLiteral("items")).toArray().size()) }; }
        return { QString(), -1 };
    };
    auto plIds = [&](const QString& p) {
        QSettings raw(iniPath, QSettings::IniFormat); QStringList out;
        for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(QStringLiteral("playlists/") + p + QStringLiteral("/items")).toString().toUtf8()).array())
            out << v.toObject().value(QStringLiteral("id")).toString();
        out.sort(); return out;
    };
    auto readArr = [&](const QString& key) {
        QSettings raw(iniPath, QSettings::IniFormat); QStringList out;
        for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(key).toString().toUtf8()).array()) out << v.toString();
        out.sort(); return out;
    };
    auto markTags = [&](const QString& p, const QString& key) {
        QSettings raw(iniPath, QSettings::IniFormat); QStringList out;
        const QJsonObject o = QJsonDocument::fromJson(raw.value(QStringLiteral("marks/") + p + QStringLiteral("/items/") + md5(key)).toString().toUtf8()).object();
        for (const QJsonValue& v : o.value(QStringLiteral("tags")).toArray()) out << v.toString();
        out.sort(); return out;
    };

    // ---- 8. Favourites: newer-wins each direction, union, tombstone beats older / loses to newer re-add -----
    {
        // 8a remote newer wins.
        wipeStores(); injFavs(QStringLiteral("f8"), {{QStringLiteral("X"), T - 100}}); const QJsonObject remA = serializeNow();
        wipeStores(); injFavs(QStringLiteral("f8"), {{QStringLiteral("X"), T - 500}}); mergeDoc(remA);
        CHECK(favTs(QStringLiteral("f8"), QStringLiteral("X")) == T - 100);           // remote (newer) wins

        // 8b local newer wins.
        wipeStores(); injFavs(QStringLiteral("f8"), {{QStringLiteral("X"), T - 500}}); const QJsonObject remB = serializeNow();
        wipeStores(); injFavs(QStringLiteral("f8"), {{QStringLiteral("X"), T - 100}}); mergeDoc(remB);
        CHECK(favTs(QStringLiteral("f8"), QStringLiteral("X")) == T - 100);           // local (newer) wins

        // 8c union of disjoint items.
        wipeStores(); injFavs(QStringLiteral("f8"), {{QStringLiteral("A"), T - 100}}); const QJsonObject remC = serializeNow();
        wipeStores(); injFavs(QStringLiteral("f8"), {{QStringLiteral("B"), T - 100}}); mergeDoc(remC);
        CHECK(favIds(QStringLiteral("f8")) == (QStringList{QStringLiteral("A"), QStringLiteral("B")}));

        // 8d tombstone beats an OLDER item (resurrection prevented): remote deleted X (newer); local still has X.
        wipeStores(); injTomb(QStringLiteral("favorites/f8"), QStringLiteral("X"), T - 100); const QJsonObject remD = serializeNow();
        wipeStores(); injFavs(QStringLiteral("f8"), {{QStringLiteral("X"), T - 500}}); mergeDoc(remD);
        CHECK(!favIds(QStringLiteral("f8")).contains(QStringLiteral("X")));           // tombstone (newer) suppresses

        // 8e tombstone LOSES to a newer re-add: remote re-added X (newer) than local's delete.
        wipeStores(); injFavs(QStringLiteral("f8"), {{QStringLiteral("X"), T - 100}}); const QJsonObject remE = serializeNow();
        wipeStores(); injTomb(QStringLiteral("favorites/f8"), QStringLiteral("X"), T - 500); mergeDoc(remE);
        CHECK(favIds(QStringLiteral("f8")).contains(QStringLiteral("X")));            // strictly-newer re-add wins
    }

    // ---- 9. Playlists: WHOLE-OBJECT newest-updatedAt + tombstones -------------------------------------------
    {
        // 9a whole-object newest wins (remote's newer P replaces name AND item set wholesale — no entry merge).
        wipeStores(); injPlaylists(QStringLiteral("p9"), {{QStringLiteral("P"), QStringLiteral("New"), T - 100, 3}}); const QJsonObject rem9 = serializeNow();
        wipeStores(); injPlaylists(QStringLiteral("p9"), {{QStringLiteral("P"), QStringLiteral("Old"), T - 500, 1}}); mergeDoc(rem9);
        CHECK(plField(QStringLiteral("p9"), QStringLiteral("P")) == (QPair<QString, qint64>{QStringLiteral("New"), 3})); // whole object

        // 9b older whole-object loses (local newer kept).
        wipeStores(); injPlaylists(QStringLiteral("p9"), {{QStringLiteral("P"), QStringLiteral("Old"), T - 500, 1}}); const QJsonObject rem9b = serializeNow();
        wipeStores(); injPlaylists(QStringLiteral("p9"), {{QStringLiteral("P"), QStringLiteral("New"), T - 100, 3}}); mergeDoc(rem9b);
        CHECK(plField(QStringLiteral("p9"), QStringLiteral("P")) == (QPair<QString, qint64>{QStringLiteral("New"), 3}));

        // 9c tombstone beats an older playlist (delete honored, no resurrection).
        wipeStores(); injTomb(QStringLiteral("playlists/p9"), QStringLiteral("P"), T - 100); const QJsonObject rem9c = serializeNow();
        wipeStores(); injPlaylists(QStringLiteral("p9"), {{QStringLiteral("P"), QStringLiteral("Old"), T - 500, 1}}); mergeDoc(rem9c);
        CHECK(!plIds(QStringLiteral("p9")).contains(QStringLiteral("P")));

        // 9d tombstone loses to a newer edit.
        wipeStores(); injPlaylists(QStringLiteral("p9"), {{QStringLiteral("P"), QStringLiteral("Edited"), T - 100, 2}}); const QJsonObject rem9d = serializeNow();
        wipeStores(); injTomb(QStringLiteral("playlists/p9"), QStringLiteral("P"), T - 500); mergeDoc(rem9d);
        CHECK(plIds(QStringLiteral("p9")).contains(QStringLiteral("P")));
    }

    // ---- 9L. LEGACY (ts==0, NO tombstone) SURVIVES a full serialize->merge round-trip, BOTH orders ----------
    // Data-safety regression (mdsync Fable review): a pre-upgrade favourite/playlist has no ts/updatedAt field
    // -> reads back as 0. With NO tombstone at all, QHash::value(id,0) ALSO defaults to 0, so the buggy
    // `tombs.value(id,0) >= ts` suppressor evaluated 0>=0 -> TRUE and WIPED every legacy item on the 2nd launch
    // of a single upgraded device (push doc w/ legacy items -> pull+merge own doc). The fix suppresses ONLY when
    // a tombstone actually EXISTS (`tombs.contains(id) && tombs.value(id) >= ts`); a recorded tombstone ts is
    // always > 0, so tombstone-beats-equal is preserved for REAL tombstones. Against the buggy `>= ts` the
    // legacy-survives asserts below FAIL; with the fix they pass.
    {
        // Legacy favourite: itemId but NO ts field (pre-upgrade shape) injected raw.
        const QString legFav = QStringLiteral("[{\"itemId\":\"L\",\"title\":\"L\"}]");
        // Order 1: legacy is LOCAL, remote empty -> merge must keep L.
        wipeStores(); const QJsonObject fEmpty = serializeNow();        // remote has no favourites at all
        wipeStores(); setRaw(QStringLiteral("favorites/lf/items"), legFav); mergeDoc(fEmpty);
        CHECK(favIds(QStringLiteral("lf")).contains(QStringLiteral("L"))); // legacy (ts==0, no tomb) survived
        CHECK(favTs(QStringLiteral("lf"), QStringLiteral("L")) == 0);      // still the untouched legacy shape
        // Order 2: legacy is REMOTE, local empty -> merge must keep L.
        wipeStores(); setRaw(QStringLiteral("favorites/lf/items"), legFav); const QJsonObject fLeg = serializeNow();
        wipeStores(); mergeDoc(fLeg);
        CHECK(favIds(QStringLiteral("lf")).contains(QStringLiteral("L"))); // legacy survived from the remote doc too

        // Legacy playlist: id/name but NO updatedAt field.
        const QString legPl = QStringLiteral("[{\"id\":\"L\",\"name\":\"Leg\",\"categoryKey\":\"video\",\"items\":[]}]");
        // Order 1: legacy is LOCAL, remote empty.
        wipeStores(); const QJsonObject pEmpty = serializeNow();
        wipeStores(); setRaw(QStringLiteral("playlists/lp/items"), legPl); mergeDoc(pEmpty);
        CHECK(plIds(QStringLiteral("lp")).contains(QStringLiteral("L"))); // legacy playlist (updatedAt==0, no tomb) survived
        // Order 2: legacy is REMOTE, local empty.
        wipeStores(); setRaw(QStringLiteral("playlists/lp/items"), legPl); const QJsonObject pLeg = serializeNow();
        wipeStores(); mergeDoc(pLeg);
        CHECK(plIds(QStringLiteral("lp")).contains(QStringLiteral("L")));

        // ...and the fix does NOT over-preserve: a REAL tombstone (ts>0) still suppresses a legacy (ts==0) item.
        wipeStores(); injTomb(QStringLiteral("favorites/lf"), QStringLiteral("L"), T - 100); const QJsonObject fKill = serializeNow();
        wipeStores(); setRaw(QStringLiteral("favorites/lf/items"), legFav); mergeDoc(fKill);
        CHECK(!favIds(QStringLiteral("lf")).contains(QStringLiteral("L"))); // real tombstone (ts>0) still wins over ts==0
        wipeStores(); injTomb(QStringLiteral("playlists/lp"), QStringLiteral("L"), T - 100); const QJsonObject pKill = serializeNow();
        wipeStores(); setRaw(QStringLiteral("playlists/lp/items"), legPl); mergeDoc(pKill);
        CHECK(!plIds(QStringLiteral("lp")).contains(QStringLiteral("L")));
    }

    // ---- 10. Marks: items newest-updatedAt / never-delete; vocab+pinned union-minus-tombstoned -------------
    {
        // 10a item: newer updatedAt wins.
        wipeStores(); injMarkItem(QStringLiteral("m10"), QStringLiteral("H"), {QStringLiteral("y")}, T - 100); const QJsonObject r10a = serializeNow();
        wipeStores(); injMarkItem(QStringLiteral("m10"), QStringLiteral("H"), {QStringLiteral("x")}, T - 500); mergeDoc(r10a);
        CHECK(markTags(QStringLiteral("m10"), QStringLiteral("H")) == (QStringList{QStringLiteral("y")}));

        // 10b item: never delete (remote absent -> local survives).
        wipeStores(); const QJsonObject r10bEmpty = serializeNow(); // remote has no marks at all
        wipeStores(); injMarkItem(QStringLiteral("m10"), QStringLiteral("H"), {QStringLiteral("keep")}, T - 100); mergeDoc(r10bEmpty);
        CHECK(markTags(QStringLiteral("m10"), QStringLiteral("H")) == (QStringList{QStringLiteral("keep")}));

        // 10c vocab union.
        wipeStores(); injArr(QStringLiteral("marks/m10/tagVocab"), {QStringLiteral("b")}); const QJsonObject r10c = serializeNow();
        wipeStores(); injArr(QStringLiteral("marks/m10/tagVocab"), {QStringLiteral("a")}); mergeDoc(r10c);
        CHECK(readArr(QStringLiteral("marks/m10/tagVocab")) == (QStringList{QStringLiteral("a"), QStringLiteral("b")}));

        // 10d vocab minus tombstoned (a deleted tag stays gone, and drops from pinned too).
        wipeStores(); injTomb(QStringLiteral("marks/m10/tagVocab"), QStringLiteral("b"), T - 100); const QJsonObject r10d = serializeNow();
        wipeStores();
        injArr(QStringLiteral("marks/m10/tagVocab"), {QStringLiteral("a"), QStringLiteral("b")});
        injArr(QStringLiteral("marks/m10/pinnedTags"), {QStringLiteral("a"), QStringLiteral("b")});
        mergeDoc(r10d);
        CHECK(readArr(QStringLiteral("marks/m10/tagVocab")) == (QStringList{QStringLiteral("a")}));   // b deleted from vocab
        CHECK(readArr(QStringLiteral("marks/m10/pinnedTags")) == (QStringList{QStringLiteral("a")})); // ...and from pinned

        // 10e pinned union-minus-tombstoned: the UNPIN case. Local unpinned t (pinned-space tombstone); a peer
        // still pinning t must NOT resurrect the shelf on merge.
        wipeStores(); injArr(QStringLiteral("marks/m10/pinnedTags"), {QStringLiteral("s"), QStringLiteral("t")}); const QJsonObject r10e = serializeNow(); // peer still pins s,t
        wipeStores();
        injArr(QStringLiteral("marks/m10/pinnedTags"), {QStringLiteral("s")});   // local dropped t
        injTomb(QStringLiteral("marks/m10/pinnedTags"), QStringLiteral("t"), T - 100); // local unpin tombstone
        // t must NOT be in vocab-tombstone space -> the tag itself survives, only the shelf is retired.
        injArr(QStringLiteral("marks/m10/tagVocab"), {QStringLiteral("s"), QStringLiteral("t")});
        mergeDoc(r10e);
        CHECK(readArr(QStringLiteral("marks/m10/pinnedTags")) == (QStringList{QStringLiteral("s")}));           // t stays unpinned
        CHECK(readArr(QStringLiteral("marks/m10/tagVocab")) == (QStringList{QStringLiteral("s"), QStringLiteral("t")})); // tag t still exists
    }

    // ---- 10f. setPinned(unpin) records a pinned-space tombstone; re-pin clears it (store-owned) -------------
    {
        useProfile(QStringLiteral("m10f"));
        ItemMarks::setPinned(QStringLiteral("shelf"), true);
        ItemMarks::setPinned(QStringLiteral("shelf"), false); // the standalone unpin
        bool tombed = false;
        for (const Tombstones::Entry& e : Tombstones::all(QStringLiteral("marks/m10f/pinnedTags")))
            if (e.key == QStringLiteral("shelf")) tombed = true;
        CHECK(tombed);                                                            // unpin -> pinned-space tombstone
        CHECK(Tombstones::all(QStringLiteral("marks/m10f/tagVocab")).isEmpty());  // NOT a vocab deletion
        ItemMarks::setPinned(QStringLiteral("shelf"), true);                      // re-pin
        bool stillTombed = false;
        for (const Tombstones::Entry& e : Tombstones::all(QStringLiteral("marks/m10f/pinnedTags")))
            if (e.key == QStringLiteral("shelf")) stillTombed = true;
        CHECK(!stillTombed);                                                      // re-pin cleared the tombstone
    }

    // ---- 11. Resume never-delete + recents cap 40 ----------------------------------------------------------
    {
        // Resume: local newer kept; remote-only entry added; a local entry with no remote counterpart survives.
        wipeStores();
        setRaw(QStringLiteral("resume/h1/pos"), QStringLiteral("10")); setRaw(QStringLiteral("resume/h1/ts"), QString::number(T - 500));
        setRaw(QStringLiteral("resume/h2/pos"), QStringLiteral("20")); setRaw(QStringLiteral("resume/h2/ts"), QString::number(T - 100));
        const QJsonObject rres = serializeNow(); // remote: h1@older, h2@newer
        wipeStores();
        setRaw(QStringLiteral("resume/h1/pos"), QStringLiteral("99")); setRaw(QStringLiteral("resume/h1/ts"), QString::number(T - 100)); // local h1 newer
        setRaw(QStringLiteral("resume/h3/pos"), QStringLiteral("30")); setRaw(QStringLiteral("resume/h3/ts"), QString::number(T - 100)); // local-only
        mergeDoc(rres);
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            CHECK(raw.value(QStringLiteral("resume/h1/pos")).toDouble() == 99.0);  // local (newer) kept
            CHECK(raw.value(QStringLiteral("resume/h2/pos")).toDouble() == 20.0);  // remote-only added
            CHECK(raw.value(QStringLiteral("resume/h3/pos")).toDouble() == 30.0);  // local-only never deleted
        }

        // Recents cap: union of 25 local + 25 remote (disjoint ids) caps at 40, newest first.
        wipeStores();
        auto recArr = [&](int base, int n) { QJsonArray a; for (int i = 0; i < n; ++i) { QJsonObject o; o["key"] = QStringLiteral("r") + QString::number(base + i); o["ts"] = double(T - (base + i)); a.append(o); } return compact(a); };
        setRaw(QStringLiteral("recent/rp/items"), recArr(100, 25)); // remote 25 (older ts range)
        const QJsonObject rrec = serializeNow();
        wipeStores();
        setRaw(QStringLiteral("recent/rp/items"), recArr(0, 25));   // local 25 (newer ts range)
        mergeDoc(rrec);
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            const QJsonArray got = QJsonDocument::fromJson(raw.value(QStringLiteral("recent/rp/items")).toString().toUtf8()).array();
            CHECK(got.size() == 40);                                              // capped at 40 (of 50 unioned)
            CHECK(got.first().toObject().value(QStringLiteral("key")).toString() == QStringLiteral("r0")); // newest first
        }
    }

    // ---- 12. Three-way convergence: A-writes, B-writes, merge in BOTH orders -> identical final state -------
    {
        const QString p = QStringLiteral("conv");
        // Build device A's ini state, capture docA.
        auto buildA = [&]() {
            wipeStores();
            injFavs(p, {{QStringLiteral("X"), T - 900}, {QStringLiteral("Z"), T - 900}});
            injPlaylists(p, {{QStringLiteral("P"), QStringLiteral("A"), T - 900, 1}});
            injMarkItem(p, QStringLiteral("H"), {QStringLiteral("t1")}, T - 900);
            injArr(QStringLiteral("marks/") + p + QStringLiteral("/tagVocab"), {QStringLiteral("t1")});
            injArr(QStringLiteral("marks/") + p + QStringLiteral("/pinnedTags"), {QStringLiteral("t1")});
        };
        // Build device B's ini state (edits Z + P + H newer, deletes X, unpins t1), capture docB.
        auto buildB = [&]() {
            wipeStores();
            injFavs(p, {{QStringLiteral("Y"), T - 400}, {QStringLiteral("Z"), T - 300}});
            injTomb(QStringLiteral("favorites/") + p, QStringLiteral("X"), T - 350);
            injPlaylists(p, {{QStringLiteral("P"), QStringLiteral("B"), T - 300, 2}});
            injMarkItem(p, QStringLiteral("H"), {QStringLiteral("t2")}, T - 300);
            injArr(QStringLiteral("marks/") + p + QStringLiteral("/tagVocab"), {QStringLiteral("t1"), QStringLiteral("t2")});
            injTomb(QStringLiteral("marks/") + p + QStringLiteral("/pinnedTags"), QStringLiteral("t1"), T - 320);
        };
        buildA(); const QJsonObject docA = serializeNow();
        buildB(); const QJsonObject docB = serializeNow();

        // Order 1: local = A, merge docB.
        buildA(); mergeDoc(docB);
        const QStringList o1_favs = favIds(p);
        const QPair<QString, qint64> o1_pl = plField(p, QStringLiteral("P"));
        const QStringList o1_tags = markTags(p, QStringLiteral("H"));
        const QStringList o1_vocab = readArr(QStringLiteral("marks/") + p + QStringLiteral("/tagVocab"));
        const QStringList o1_pinned = readArr(QStringLiteral("marks/") + p + QStringLiteral("/pinnedTags"));

        // Order 2: local = B, merge docA.
        buildB(); mergeDoc(docA);
        const QStringList o2_favs = favIds(p);
        const QPair<QString, qint64> o2_pl = plField(p, QStringLiteral("P"));
        const QStringList o2_tags = markTags(p, QStringLiteral("H"));
        const QStringList o2_vocab = readArr(QStringLiteral("marks/") + p + QStringLiteral("/tagVocab"));
        const QStringList o2_pinned = readArr(QStringLiteral("marks/") + p + QStringLiteral("/pinnedTags"));

        // Convergent: both orders reach the SAME final state.
        CHECK(o1_favs == o2_favs);
        CHECK(o1_pl == o2_pl);
        CHECK(o1_tags == o2_tags);
        CHECK(o1_vocab == o2_vocab);
        CHECK(o1_pinned == o2_pinned);

        // ...and that state is the semantically-correct one (newest edits win; X deleted; t1 unpinned).
        CHECK(o1_favs == (QStringList{QStringLiteral("Y"), QStringLiteral("Z")}));   // X tombstoned away
        CHECK(favTs(p, QStringLiteral("Z")) == T - 300);                             // B's newer Z won
        CHECK(o1_pl == (QPair<QString, qint64>{QStringLiteral("B"), 2}));            // B's newer whole-object P
        CHECK(o1_tags == (QStringList{QStringLiteral("t2")}));                        // B's newer marks
        CHECK(o1_vocab == (QStringList{QStringLiteral("t1"), QStringLiteral("t2")})); // vocab union
        CHECK(o1_pinned.isEmpty());                                                   // t1 unpinned on B -> no shelf
    }

    // ---- 13. Device-namespaced accumulators: union VERBATIM on merge, never arithmetic, no double-count -----
    {
        const QString localDev = Settings::deviceId();
        const QString hx = md5(QStringLiteral("vid:X"));
        auto wipeAcc = [&]() { QSettings raw(iniPath, QSettings::IniFormat); raw.remove(QStringLiteral("stats")); raw.remove(QStringLiteral("playstats")); raw.sync(); };
        auto val = [&](const QString& key) { QSettings raw(iniPath, QSettings::IniFormat); return raw.value(key).toString(); };
        const QString remIt = QStringLiteral("stats/p13/remoteDev/items/") + hx;
        const QString remCat = QStringLiteral("stats/p13/remoteDev/cat/video/seconds");
        const QString locIt = QStringLiteral("stats/p13/") + localDev + QStringLiteral("/items/") + hx;
        const QString locCat = QStringLiteral("stats/p13/") + localDev + QStringLiteral("/cat/video/seconds");

        // A remote device's namespace serializes; merging it copies it verbatim while our namespace is untouched.
        wipeAcc(); setRaw(remIt, QStringLiteral("R")); setRaw(remCat, QStringLiteral("20")); const QJsonObject docR = serializeNow();
        wipeAcc(); setRaw(locIt, QStringLiteral("L")); setRaw(locCat, QStringLiteral("10")); mergeDoc(docR);
        CHECK(val(locCat) == QStringLiteral("10") && val(locIt) == QStringLiteral("L")); // local namespace untouched
        CHECK(val(remCat) == QStringLiteral("20") && val(remIt) == QStringLiteral("R")); // remote copied verbatim

        // Repeated merge NEVER double-counts (verbatim replace, not arithmetic add).
        mergeDoc(docR); mergeDoc(docR);
        CHECK(val(remCat) == QStringLiteral("20"));   // still 20, not 40/60
        CHECK(val(locCat) == QStringLiteral("10"));

        // A remote doc carrying a STALE copy of OUR namespace must not clobber it.
        wipeAcc(); setRaw(locCat, QStringLiteral("999")); setRaw(remCat, QStringLiteral("20")); const QJsonObject docStale = serializeNow();
        wipeAcc(); setRaw(locCat, QStringLiteral("10")); mergeDoc(docStale);
        CHECK(val(locCat) == QStringLiteral("10"));    // our live namespace wins over the peer's stale copy of it
        CHECK(val(remCat) == QStringLiteral("20"));

        // playstats travels the same generic path (the hash shape is irrelevant to the verbatim merge).
        const QString sg = md5(QStringLiteral("game:g"));
        const QString remTot = QStringLiteral("playstats/p13/remoteDev/") + sg + QStringLiteral("/total");
        const QString locTot = QStringLiteral("playstats/p13/") + localDev + QStringLiteral("/") + sg + QStringLiteral("/total");
        wipeAcc(); setRaw(remTot, QStringLiteral("50")); const QJsonObject docP = serializeNow();
        wipeAcc(); setRaw(locTot, QStringLiteral("9")); mergeDoc(docP); mergeDoc(docP);
        CHECK(val(locTot) == QStringLiteral("9") && val(remTot) == QStringLiteral("50")); // union verbatim, no double-count
    }

    // ---- 14. Equal-timestamp tie-break: same key, same ts, different values -> BOTH orders CONVERGE ---------
    // The uniform order-independent comparator (greater canonical value bytes) supersedes the divergent legacy
    // ties (four stores kept-local, recents `>=`). Proven on resume (a scalar) AND favourites (an object).
    {
        // resume: pos 10 vs 20 at the SAME ts.
        wipeStores(); setRaw(QStringLiteral("resume/hX/pos"), QStringLiteral("10")); setRaw(QStringLiteral("resume/hX/ts"), QString::number(T)); const QJsonObject eqA = serializeNow();
        wipeStores(); setRaw(QStringLiteral("resume/hX/pos"), QStringLiteral("20")); setRaw(QStringLiteral("resume/hX/ts"), QString::number(T)); const QJsonObject eqB = serializeNow();
        auto resPos = [&]() { QSettings raw(iniPath, QSettings::IniFormat); return raw.value(QStringLiteral("resume/hX/pos")).toDouble(); };
        wipeStores(); setRaw(QStringLiteral("resume/hX/pos"), QStringLiteral("10")); setRaw(QStringLiteral("resume/hX/ts"), QString::number(T)); mergeDoc(eqB); const double r1 = resPos();
        wipeStores(); setRaw(QStringLiteral("resume/hX/pos"), QStringLiteral("20")); setRaw(QStringLiteral("resume/hX/ts"), QString::number(T)); mergeDoc(eqA); const double r2 = resPos();
        CHECK(r1 == r2);          // convergent regardless of merge order
        CHECK(r1 == 20.0);        // deterministic winner: greater canonical value bytes ("20" > "10")

        // favourites: same itemId + ts, different title.
        auto injFav1 = [&](const QString& id, const QString& title, qint64 ts) {
            QJsonObject o; o[QStringLiteral("itemId")] = id; o[QStringLiteral("title")] = title; o[QStringLiteral("ts")] = double(ts);
            QJsonArray a; a.append(o); setRaw(QStringLiteral("favorites/f14/items"), compact(a));
        };
        auto favTitle = [&](const QString& id) -> QString {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(QStringLiteral("favorites/f14/items")).toString().toUtf8()).array())
            { const QJsonObject o = v.toObject(); if (o.value(QStringLiteral("itemId")).toString() == id) return o.value(QStringLiteral("title")).toString(); }
            return QString();
        };
        wipeStores(); injFav1(QStringLiteral("X"), QStringLiteral("alpha"), T); const QJsonObject fA = serializeNow();
        wipeStores(); injFav1(QStringLiteral("X"), QStringLiteral("beta"),  T); const QJsonObject fB = serializeNow();
        wipeStores(); injFav1(QStringLiteral("X"), QStringLiteral("alpha"), T); mergeDoc(fB); const QString t1 = favTitle(QStringLiteral("X"));
        wipeStores(); injFav1(QStringLiteral("X"), QStringLiteral("beta"),  T); mergeDoc(fA); const QString t2 = favTitle(QStringLiteral("X"));
        CHECK(t1 == t2);                          // convergent
        CHECK(t1 == QStringLiteral("beta"));      // greater canon ("beta" > "alpha") wins in both orders
    }

    // ---- 15. Per-namespace freshness: newest-wins per FOREIGN namespace (three-device stale-copy) -----------
    // mergeNamespaced must NOT verbatim-replace a foreign namespace with an OLDER copy. Owner device C stamps
    // stats/<p>/C/lastWrite at accrual; that stamp travels verbatim. Scenario: local A already holds a FRESH
    // copy of C; it merges peer B's document carrying a STALE copy of C -> A keeps its fresh C (no downgrade).
    {
        const QString localDev = Settings::deviceId();
        auto wipeAcc = [&]() { QSettings raw(iniPath, QSettings::IniFormat); raw.remove(QStringLiteral("stats")); raw.remove(QStringLiteral("playstats")); raw.sync(); };
        auto val = [&](const QString& key) { QSettings raw(iniPath, QSettings::IniFormat); return raw.value(key).toString(); };
        const QString cCat = QStringLiteral("stats/pf/devC/cat/video/seconds");
        const QString cLW  = QStringLiteral("stats/pf/devC/lastWrite");

        // Peer B carries a STALE copy of device C (older lastWrite) -> must not clobber A's fresh C.
        wipeAcc(); setRaw(cCat, QStringLiteral("50")); setRaw(cLW, QString::number(T - 500)); const QJsonObject docStaleC = serializeNow();
        wipeAcc(); setRaw(cCat, QStringLiteral("100")); setRaw(cLW, QString::number(T)); mergeDoc(docStaleC);
        CHECK(val(cCat) == QStringLiteral("100"));         // fresh C kept; the stale peer copy did NOT downgrade it
        CHECK(val(cLW)  == QString::number(T));

        // Symmetric: a strictly-FRESHER incoming C replaces a locally-stale copy.
        wipeAcc(); setRaw(cCat, QStringLiteral("100")); setRaw(cLW, QString::number(T)); const QJsonObject docFreshC = serializeNow();
        wipeAcc(); setRaw(cCat, QStringLiteral("50")); setRaw(cLW, QString::number(T - 500)); mergeDoc(docFreshC);
        CHECK(val(cCat) == QStringLiteral("100"));         // newer incoming wins over the local stale copy
        CHECK(val(cLW)  == QString::number(T));

        // No local copy at all -> a brand-new foreign namespace is imported regardless of the freshness edge.
        wipeAcc(); setRaw(cCat, QStringLiteral("77")); setRaw(cLW, QString::number(T - 900)); const QJsonObject docNewC = serializeNow();
        wipeAcc(); mergeDoc(docNewC);
        CHECK(val(cCat) == QStringLiteral("77"));          // absent local -> import
    }

    // ---- 16. T4 carve-out: device-local excluded BOTH ways; applyBundle hands off the per-item stores -------
    {
        const QString localDev = Settings::deviceId();
        // Seed a representative ini: one key of every excluded shape + their SIBLING syncing counterparts + a
        // plain synced key + per-item store keys. (device/id is already minted by Settings::deviceId.)
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const char* g : {"roms", "emulators", "player", "netplay", "display", "profiles",
                                  "emu", "sync", "downloads", "pcgames", "library", "stats", "marks", "resume"})
                raw.remove(QLatin1String(g));
            // device-local (excluded):
            raw.setValue(QStringLiteral("roms/folder"), QStringLiteral("D:/roms"));
            raw.setValue(QStringLiteral("emulators/root"), QStringLiteral("D:/emu"));
            raw.setValue(QStringLiteral("emulators/fullscreen"), QStringLiteral("1"));
            raw.setValue(QStringLiteral("player/externalPath"), QStringLiteral("C:/vlc.exe"));
            raw.setValue(QStringLiteral("player/external"), QStringLiteral("vlc"));
            raw.setValue(QStringLiteral("netplay/relay"), QStringLiteral("host:1"));
            raw.setValue(QStringLiteral("display/mode"), QStringLiteral("tv"));
            raw.setValue(QStringLiteral("display/tvPromptDone"), QStringLiteral("1"));
            raw.setValue(QStringLiteral("profiles/current"), QStringLiteral("alice"));
            raw.setValue(QStringLiteral("emu/virtualPadOpacity"), QStringLiteral("50"));
            raw.setValue(QStringLiteral("sync/files/abc/audio"), QStringLiteral("3"));
            raw.setValue(QStringLiteral("downloads/foo"), QStringLiteral("1"));
            raw.setValue(QStringLiteral("pcgames/bar"), QStringLiteral("1"));
            // SIBLING carve-outs that MUST still sync:
            raw.setValue(QStringLiteral("profiles/list"), QStringLiteral("[alice,bob]"));
            raw.setValue(QStringLiteral("sync/global/audio"), QStringLiteral("2"));
            raw.setValue(QStringLiteral("library/showHidden"), QStringLiteral("true"));
            // a plain synced key + a per-item store key (the latter travels IN the bundle but is not applied):
            raw.setValue(QStringLiteral("display/theme"), QStringLiteral("dark"));
            raw.setValue(QStringLiteral("stats/pX/") + localDev + QStringLiteral("/cat/video/seconds"), QStringLiteral("5"));
            raw.sync();
        }

        // 16a. buildSettingsJson (outbound): every device-local key is ABSENT; siblings + plain + per-item PRESENT.
        const QJsonObject b = QJsonDocument::fromJson(CloudSync::buildSettingsJson()).object();
        for (const char* ex : {"roms/folder", "emulators/root", "emulators/fullscreen", "player/externalPath",
                               "player/external", "netplay/relay", "display/mode", "display/tvPromptDone",
                               "profiles/current", "emu/virtualPadOpacity", "sync/files/abc/audio",
                               "device/id", "downloads/foo", "pcgames/bar"})
            CHECK(!b.contains(QLatin1String(ex)));                    // device-local carved out of the bundle
        CHECK(b.contains(QStringLiteral("profiles/list")));          // sibling still syncs
        CHECK(b.contains(QStringLiteral("sync/global/audio")));      // sync/global/* still syncs
        CHECK(b.contains(QStringLiteral("library/showHidden")));     // library/showHidden still syncs
        CHECK(b.value(QStringLiteral("display/theme")).toString() == QStringLiteral("dark"));
        CHECK(!b.contains(QStringLiteral("stats/pX/") + localDev + QStringLiteral("/cat/video/seconds"))); // per-item now CARVED OUT of the bundle (mdsync T5 cadence fix)
        for (const char* pi : {"resume/", "recent/", "marks/", "favorites/", "playlists/", "stats/", "playstats/", "deleted/"})
        {
            bool anyPerItem = false;
            for (const QString& bk : b.keys()) if (bk.startsWith(QLatin1String(pi))) { anyPerItem = true; break; }
            CHECK(!anyPerItem);                                      // no per-item store rides the heavy bundle outbound
        }

        // 16b. applySettingsJson (inbound): a peer's bundle must not overwrite device-local keys NOR write any
        // per-item store key (release-gating hands-off); only plain synced keys land.
        QJsonObject peer;
        peer[QStringLiteral("roms/folder")]  = QStringLiteral("PEER/roms");   // device-local
        peer[QStringLiteral("display/mode")] = QStringLiteral("desktop");     // device-local
        peer[QStringLiteral("device/id")]    = QStringLiteral("PEER-DEVICE"); // device-local (identity)
        peer[QStringLiteral("stats/pX/") + localDev + QStringLiteral("/cat/video/seconds")] = QStringLiteral("999"); // per-item: hands off
        peer[QStringLiteral("marks/pX/items/deadbeef")] = QStringLiteral("{\"peer\":1}");                            // per-item: hands off
        peer[QStringLiteral("display/theme")] = QStringLiteral("light");      // plain synced -> updates
        peer[QStringLiteral("some/newKey")]   = QStringLiteral("hello");      // plain synced (new) -> added
        CloudSync::applySettingsJson(QJsonDocument(peer).toJson(QJsonDocument::Compact));
        {
            QSettings raw(iniPath, QSettings::IniFormat); raw.sync();
            CHECK(raw.value(QStringLiteral("roms/folder")).toString() == QStringLiteral("D:/roms"));   // untouched
            CHECK(raw.value(QStringLiteral("display/mode")).toString() == QStringLiteral("tv"));       // untouched
            CHECK(raw.value(QStringLiteral("device/id")).toString() == localDev);                      // OUR id preserved
            CHECK(raw.value(QStringLiteral("stats/pX/") + localDev + QStringLiteral("/cat/video/seconds")).toString() == QStringLiteral("5")); // per-item untouched (release-gating)
            CHECK(!raw.contains(QStringLiteral("marks/pX/items/deadbeef")));                           // per-item never written
            CHECK(raw.value(QStringLiteral("display/theme")).toString() == QStringLiteral("light"));   // plain synced updated
            CHECK(raw.value(QStringLiteral("some/newKey")).toString() == QStringLiteral("hello"));     // plain synced added
        }

        // Good-citizen cleanup: probes share this portable ini (all live in the same build dir), and the
        // form-factor probes read emu/virtualPad* and display/mode as DEFAULTS. Leave none of our seeded
        // device-local keys behind or a later run would inherit them.
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const char* g : {"roms", "emulators", "player", "netplay", "display", "profiles", "emu",
                                  "sync", "downloads", "pcgames", "library", "stats", "marks", "resume", "some"})
                raw.remove(QLatin1String(g));
            raw.sync();
        }
    }

    // ---- 17. T5 cadence: per-item churn re-uploads NOTHING heavy (neither the bundle nor the stateHash gate) ---
    {
        const QString localDev = Settings::deviceId();
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const char* g : {"stats", "marks", "favorites", "playlists", "resume", "recent", "playstats", "deleted", "display"})
                raw.remove(QLatin1String(g));
            raw.setValue(QStringLiteral("display/theme"), QStringLiteral("dark")); // a genuinely bundle-synced key
            raw.sync();
        }
        const QByteArray bundle0 = CloudSync::buildSettingsJson();
        const QByteArray fp0 = CloudSync::stateFingerprint();

        // Mutate EVERY per-item store family (the exact churn a live device generates while watching/marking).
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("stats/pX/") + localDev + QStringLiteral("/cat/video/seconds"), QStringLiteral("1234"));
            raw.setValue(QStringLiteral("playstats/pX/") + localDev + QStringLiteral("/abc/total"), QStringLiteral("99"));
            raw.setValue(QStringLiteral("marks/pX/items/deadbeef"), QStringLiteral("{\"updatedAt\":42}"));
            raw.setValue(QStringLiteral("favorites/pX/items/f1"), QStringLiteral("{\"ts\":7}"));
            raw.setValue(QStringLiteral("playlists/pX/p1"), QStringLiteral("{\"updatedAt\":9}"));
            raw.setValue(QStringLiteral("resume/pX/r1"), QStringLiteral("{\"ts\":3}"));
            raw.setValue(QStringLiteral("recent/pX/items"), QStringLiteral("[1,2]"));
            raw.setValue(QStringLiteral("deleted/favorites/pX/xyz"), QStringLiteral("{\"ts\":5}"));
            raw.sync();
        }
        CHECK(CloudSync::buildSettingsJson() == bundle0);   // per-item churn -> bundle bytes UNCHANGED
        CHECK(CloudSync::stateFingerprint() == fp0);        // per-item churn -> localChanged gate stays FALSE (no heavy re-upload)

        // A genuinely bundle-synced setting DOES still move the fingerprint (the merge-doc-only decoupling didn't
        // silence real bundle changes).
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("display/theme"), QStringLiteral("light"));
            raw.sync();
        }
        CHECK(CloudSync::buildSettingsJson() != bundle0);   // a real synced setting -> bundle changes
        CHECK(CloudSync::stateFingerprint() != fp0);        // -> localChanged fires, bundle re-uploads (correct)

        {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const char* g : {"stats", "marks", "favorites", "playlists", "resume", "recent", "playstats", "deleted", "display"})
                raw.remove(QLatin1String(g));
            raw.sync();
        }
    }

    if (failures == 0) { std::puts("CLOUDMERGE-OK"); return 0; }
    std::fprintf(stderr, "CLOUDMERGE: %d check(s) failed\n", failures);
    return 1;
}
