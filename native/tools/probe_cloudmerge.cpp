// Headless check of the multi-device-sync FOUNDATION (mdsync T1): the device identity, the per-store write
// timestamps, and the deletion tombstones that the generalized CloudMerge pass (T2) will read. QtCore-only
// (QSettings + JSON wrappers over the shared portable everythingbox.ini), so it runs under the offscreen QPA in
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
// It now also owns the #34 push-on-Save decision layer (§19-23), which belongs here rather than in a probe of
// its own: the policy is only meaningful against CloudSync's fingerprint/carve-out contract and SettingsTxn's
// scope predicate, and both are already linked into this target.
//   * PendingPush::backoffMs / due() — the retry schedule, the give-up cap, the auth park, the settings-visit
//     gate (nothing uploads state the user has not confirmed) and the backwards-clock rule;
//   * PendingPush::resolve() — the conflict plan, and half the property that makes it converge in one round;
//   * CloudSync::adoptSyncedBaseline — the OTHER half: a pull's fixed point, asserted without a network;
//   * PendingPush::onOutcome / classifyRefresh / classifyPush — the state transitions, and both directions of
//     "needs a human" vs "needs patience";
//   * the record's durability and SHAPE — three scalars, device-local, out of transaction scope, invisible to
//     the sync fingerprint, and provably carrying no settings value.
//
// Prints CLOUDMERGE-OK on success; any failure prints CLOUDMERGE-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so every store below
// opens an everythingbox.ini that starts empty and is removed at exit. The probe still seeds its own profile
// ids via ProfileStore::setCurrent -- that is the fixture the per-profile namespace asserts are written
// against, not a defence against what some other run left behind.
#include "Settings.h"
#include "ItemMarks.h"
#include "FavoritesStore.h"
#include "PlaylistStore.h"
#include "Tombstones.h"
#include "CloudMerge.h"
#include "MetaOverrides.h"  // issue #24: the per-item metadata corrections the merge document now carries
#include "CloudSync.h"      // mdsync T4: the device-local carve-out + bundle-settings hands-off
#include "BrandMigration.h" // #58 review: the stored-add-on-id repair, played against the merge (section 19)
#include "SettingsTxn.h"    // #26: applySettingsJson must close an open settings transaction
#include "PendingPush.h"    // #34: the durable pending-push record + the retry policy (§19-23)
#include "ProfileStore.h"
#include "AppPaths.h"
#include "AppBrand.h"

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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <tuple>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CLOUDMERGE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// #34: PendingPush::due() with no unconfirmed settings edits open — the ordinary case, and what the §19
// assertions are about. That gate is asserted on its own in §19b, where the flag is passed explicitly in both
// directions; naming the ordinary case once here keeps those assertions about the thing they were written for.
// Flipping this helper's argument does not silently weaken them: Deferred compares equal to none of the five
// verdicts §19 expects, so all fifteen fail at once.
static PendingPush::Due dueClosed(const PendingPush::State& s, bool signedIn, bool manual, qint64 nowMs)
{
    return PendingPush::due(s, signedIn, manual, nowMs, /*unconfirmed*/false);
}

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
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

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
        for (const char* g : {"marks", "favorites", "playlists", "deleted", "resume", "recent", "metaoverrides"})
            raw.remove(QLatin1String(g));
        raw.sync();
        ItemMarks::invalidate();
        MetaOverrides::invalidate();
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
            for (const char* g : {"roms", "emulators", "player", "netplay", "display", "profiles", "emu",
                                  "sync", "downloads", "pcgames", "library", "stats", "marks", "resume",
                                  "metaoverrides"})
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
            // A per-item metadata correction (issue #24). Seeded here so the "no per-item store rides the
            // heavy bundle" sweep below actually has a metaoverrides key to find — an unseeded prefix would
            // make that iteration of the loop pass on absence and no mutation could kill it.
            raw.setValue(QStringLiteral("metaoverrides/items/deadbeef"),
                         QStringLiteral("{\"title\":\"Local fix\",\"updatedAt\":1}"));
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
        // Local video library folder is device-local (each machine points at its own disk); the
        // library/showHidden sibling is a user preference and DOES sync (leaf-exact match, not group).
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("library/folder")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("library/showHidden")) == false);
        CHECK(b.value(QStringLiteral("display/theme")).toString() == QStringLiteral("dark"));
        CHECK(!b.contains(QStringLiteral("stats/pX/") + localDev + QStringLiteral("/cat/video/seconds"))); // per-item now CARVED OUT of the bundle (mdsync T5 cadence fix)
        for (const char* pi : {"resume/", "recent/", "marks/", "favorites/", "playlists/", "stats/", "playstats/",
                               "deleted/", "metaoverrides/"})
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
        peer[QStringLiteral("metaoverrides/items/deadbeef")] = QStringLiteral("{\"title\":\"Peer fix\",\"updatedAt\":9}"); // per-item: hands off
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
            // The correction the user made HERE is untouched by a peer's bundle: only the merge document (with
            // its newest-updatedAt rule) is allowed to move it, or a stale peer copy would silently win.
            CHECK(raw.value(QStringLiteral("metaoverrides/items/deadbeef")).toString()
                  == QStringLiteral("{\"title\":\"Local fix\",\"updatedAt\":1}"));
            CHECK(raw.value(QStringLiteral("display/theme")).toString() == QStringLiteral("light"));   // plain synced updated
            CHECK(raw.value(QStringLiteral("some/newKey")).toString() == QStringLiteral("hello"));     // plain synced added
        }

        // 16c. applySettingsJson CLOSES an open settings transaction (#26). A remote bundle writes in-scope
        // settings keys; if one lands while a settings visit is open, the snapshot predates it, so a later
        // Discard would read the PEER's values as "the user's changes" and put the local ones back —
        // silently reverting another device. The guard commits first: losing the ability to discard this
        // visit is the correct trade against clobbering a peer.
        //
        // This case lives here rather than in probe_settingstxn because the guard is in CloudSync's TU, and
        // probe_settingstxn does not link it — without this assertion, deleting the guard passed CI.
        {
            SettingsTxn::begin();
            CHECK(SettingsTxn::active() == true);            // precondition: a visit really is open
            QJsonObject mid;
            mid[QStringLiteral("display/theme")] = QStringLiteral("midnight"); // plain in-scope settings key
            CloudSync::applySettingsJson(QJsonDocument(mid).toJson(QJsonDocument::Compact));
            CHECK(SettingsTxn::active() == false);           // the apply closed the transaction
            SettingsTxn::commit();                           // belt-and-braces: leave no txn open for §17/18
        }

        // Good-citizen cleanup: probes share this portable ini (all live in the same build dir), and the
        // form-factor probes read emu/virtualPad* and display/mode as DEFAULTS. Leave none of our seeded
        // device-local keys behind or a later run would inherit them.
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const char* g : {"roms", "emulators", "player", "netplay", "display", "profiles", "emu",
                                  "sync", "downloads", "pcgames", "library", "stats", "marks", "resume", "some",
                                  "metaoverrides"})
                raw.remove(QLatin1String(g));
            raw.sync();
            MetaOverrides::invalidate();
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

    // ---- 18. save-sync T3: a SAVE write no longer moves the heavy-bundle fingerprint ---------------------
    // saves/ and states/ left buildBundle AND stateHash. This is the whole point of that change: while saves
    // were folded into the fingerprint, one F2 press read as "local changed" and re-uploaded addons, themes
    // and settings alongside the save. SaveSync owns those files per-file now.
    {
        const QString app = AppPaths::dataDir();
        const QString saveFile  = app + QStringLiteral("/saves/ProbeT3.srm");
        const QString stateFile = app + QStringLiteral("/states/ProbeT3.state");
        const QString themeFile = app + QStringLiteral("/themes/probeT3/theme.json");
        QDir().mkpath(QFileInfo(saveFile).absolutePath());
        QDir().mkpath(QFileInfo(stateFile).absolutePath());
        QDir().mkpath(QFileInfo(themeFile).absolutePath());
        const auto write = [](const QString& p, const QByteArray& b) {
            QFile f(p); if (f.open(QIODevice::WriteOnly)) { f.write(b); f.close(); }
        };

        const QByteArray fp0 = CloudSync::stateFingerprint();
        write(saveFile,  "SRAM-A");
        write(stateFile, "STATE-A");
        CHECK(CloudSync::stateFingerprint() == fp0);   // a save/state APPEARING doesn't flip the gate
        write(saveFile,  "SRAM-B");                    // ...and neither does overwriting one (the F2 case)
        write(stateFile, "STATE-B");
        CHECK(CloudSync::stateFingerprint() == fp0);

        // The fingerprint still notices a genuinely bundle-carried FILE, so we didn't silence it wholesale.
        write(themeFile, "{}");
        CHECK(CloudSync::stateFingerprint() != fp0);

        QFile::remove(saveFile); QFile::remove(stateFile); QFile::remove(themeFile);
        QDir(app + QStringLiteral("/saves")).removeRecursively();
        QDir(app + QStringLiteral("/states")).removeRecursively();
        QDir(app + QStringLiteral("/themes/probeT3")).removeRecursively();
        CHECK(CloudSync::stateFingerprint() == fp0);   // cleanup restored the baseline (no stray left behind)
    }

    // ---- 19. #58 review: a REPAIRED add-on id must survive the merge that follows it — two devices, two rounds
    //
    // BrandMigration::reconcileAddonRefs re-points a stored favourite/playlist reference at the add-on id that
    // actually loaded. It deliberately does NOT re-date the blob (a repair is not an edit; stamping it now
    // would let it beat a genuinely newer change made elsewhere) and it fires no store change-hook, so it arms
    // no push. Both are right — and together they landed the repaired blob in the merge at an EQUAL timestamp
    // against the cloud's unrepaired copy, differing in nothing but the id's SPELLING. Deciding that on raw
    // canonical bytes decided it on the spelling, and the previous namespace sorts greater, so it won every
    // tie: repair at launch, pull 1.5s later, reverted; "that favourite's source addon isn't available" for
    // the rest of the session; and any later push uploaded the REVERTED blob, so the cloud never converged
    // either. Whole-object newest-wins made it worse for playlists — the entire repaired playlist went back.
    //
    // Two changes answer it, and this section demonstrates BOTH as convergence rather than as a local symptom:
    //   * CloudMerge's tieKey — an add-on id's spelling is not a content difference, so blobs that differ only
    //     in it now TIE and neither replaces the other;
    //   * MainWindow::mergeProgress re-runs the repair after every merge — because a peer's blob that really
    //     IS newer still wins outright, spelling and all, and reload() has already been and gone.
    {
        const QString aioNow = QLatin1String(AppBrand::kAddonPrefix) + QStringLiteral("aio");
        const QString aioWas = QLatin1String(AppBrand::Legacy::kAddonPrefix) + QStringLiteral("aio");
        const QString p19    = QStringLiteral("dev19");
        const QString favK   = QStringLiteral("favorites/") + p19 + QStringLiteral("/items");
        const QString plK    = QStringLiteral("playlists/") + p19 + QStringLiteral("/items");

        // One favourite and one playlist ENTRY, both naming `addon`, both stamped `ts`. The playlist carries
        // the id one level deeper, which is where whole-object newest-wins does the most damage.
        auto injRefs = [&](const QString& addon, qint64 ts, const QString& title) {
            QJsonObject f;
            f[QStringLiteral("itemId")] = QStringLiteral("I");
            f[QStringLiteral("title")]  = title;
            f[QStringLiteral("addonId")] = addon;
            f[QStringLiteral("ts")] = double(ts);
            QJsonArray fa; fa.append(f);
            setRaw(favK, compact(fa));

            QJsonObject e;
            e[QStringLiteral("itemId")]  = QStringLiteral("I");
            e[QStringLiteral("addonId")] = addon;
            QJsonArray items; items.append(e);
            QJsonObject pl;
            pl[QStringLiteral("id")] = QStringLiteral("P");
            pl[QStringLiteral("name")] = title;
            pl[QStringLiteral("categoryKey")] = QStringLiteral("video");
            pl[QStringLiteral("updatedAt")] = double(ts);
            pl[QStringLiteral("items")] = items;
            QJsonArray pa; pa.append(pl);
            setRaw(plK, compact(pa));
        };
        auto favAddon = [&]() -> QString {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(favK).toString().toUtf8()).array())
                if (v.toObject().value(QStringLiteral("itemId")).toString() == QStringLiteral("I"))
                    return v.toObject().value(QStringLiteral("addonId")).toString();
            return QString();
        };
        auto favTitle19 = [&]() -> QString {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(favK).toString().toUtf8()).array())
                if (v.toObject().value(QStringLiteral("itemId")).toString() == QStringLiteral("I"))
                    return v.toObject().value(QStringLiteral("title")).toString();
            return QString();
        };
        auto plAddon = [&]() -> QString {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(plK).toString().toUtf8()).array())
                for (const QJsonValue& e : v.toObject().value(QStringLiteral("items")).toArray())
                    return e.toObject().value(QStringLiteral("addonId")).toString();
            return QString();
        };
        auto plUpdated = [&]() -> qint64 {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(plK).toString().toUtf8()).array())
                return qint64(v.toObject().value(QStringLiteral("updatedAt")).toDouble());
            return -1;
        };
        // The whole per-device state, as BYTES — so "nothing moved" is checked at the level the merge writes
        // at, not through a field-by-field reading that a reserialization could slip past.
        auto snapshot = [&]() {
            QSettings raw(iniPath, QSettings::IniFormat);
            return QPair<QString, QString>{ raw.value(favK).toString(), raw.value(plK).toString() };
        };
        auto restore = [&](const QPair<QString, QString>& s) { setRaw(favK, s.first); setRaw(plK, s.second); };
        auto reconcile = [&](const QStringList& installed) {
            return BrandMigration::reconcileAddonRefs(AppPaths::dataDir(), installed);
        };
        const QString title = QStringLiteral("Thing");

        // Devices A and B: the same pre-rebrand install on two machines, each of which renamed its local copy
        // of the add-on (migrateAddonIds), so the id that resolves on BOTH is the current spelling.
        const QStringList instA{ aioNow };
        const QStringList instB{ aioNow };

        // -- Round 1, device A: repair at launch, then the 1.5s startup pull lands the cloud's stale copy ----
        wipeStores(); injRefs(aioWas, T, title);
        const QJsonObject cloud0 = serializeNow();          // the cloud document, still unrepaired
        wipeStores(); injRefs(aioWas, T, title);            // device A, as it starts
        CHECK(reconcile(instA) == 2);                       // the favourite AND the playlist entry move
        CHECK(favAddon() == aioNow);
        CHECK(plAddon() == aioNow);
        CHECK(favTs(p19, QStringLiteral("I")) == T);        // ...at an UNCHANGED stamp. This is the premise the
        CHECK(plUpdated() == T);                            //    whole finding rests on; stated, not assumed.
        // A's document AS REPAIRED, captured before the pull lands — what a push carries if some unrelated
        // change armed the debounce in that window. B's round below merges THIS rather than A's post-merge
        // state, deliberately: a document built after the merge is a product of the same comparator under
        // test, so an assertion made against it could not fail however that comparator was broken.
        const QJsonObject docARepaired = serializeNow();
        mergeDoc(cloud0);                                   // pullAndMergeProgress, ~1.5s after launch
        CHECK(favAddon() == aioNow);   // was: reverted to the previous spelling by the raw-byte tie-break
        CHECK(plAddon() == aioNow);    // was: the ENTIRE repaired playlist replaced, whole-object newest-wins
        const QPair<QString, QString> stateA1 = snapshot();
        const QJsonObject docA1 = serializeNow();

        // -- Round 1, device B: pulls A's repaired doc, then repairs its own copy after the merge -----------
        wipeStores(); injRefs(aioWas, T, title);
        mergeDoc(docARepaired);
        // The tie is a no-op in BOTH directions, so B is not dragged onto A's bytes either...
        CHECK(favAddon() == aioWas);
        CHECK(plAddon() == aioWas);
        // ...and the post-merge repair is what makes B correct on THIS launch rather than the next one.
        CHECK(reconcile(instB) == 2);
        CHECK(favAddon() == aioNow);
        CHECK(plAddon() == aioNow);
        const QPair<QString, QString> stateB1 = snapshot();
        const QJsonObject docB1 = serializeNow();

        CHECK(stateA1 == stateB1);   // both ends agree — byte for byte, not merely semantically

        // -- Round 2: a SECOND exchange moves nothing on either end (no oscillation, nothing left stranded) --
        // The byte check goes BEFORE the reconcile, deliberately. After it, the two are indistinguishable:
        // a merge that wrecked the state and a repair that put it back leave exactly the bytes a merge that
        // did nothing leaves — that IS the perpetual-repair loop this whole finding is about. Checked first,
        // it says the merge itself was a no-op; the count after it says there was nothing left to do.
        wipeStores(); restore(stateA1); mergeDoc(docB1);
        CHECK(snapshot() == stateA1);
        CHECK(reconcile(instA) == 0);
        wipeStores(); restore(stateB1); mergeDoc(docA1);
        CHECK(snapshot() == stateB1);
        CHECK(reconcile(instB) == 0);

        // -- The ASYMMETRIC pair, which is why arming a push would not have been enough --------------------
        // Device C's copy of the add-on kept the previous namespace and always will (for a remote add-on the
        // id is identity, not branding — renaming it orphans every saved URL), while A's local copy was
        // renamed. The two ends are BOTH correct and they disagree, permanently. Arming a push after the
        // repair only re-runs the same equal-ts comparison at the other end; the answer has to be that the
        // comparison stops caring. Two full rounds each way, and neither is dragged onto the other's spelling.
        const QStringList instC{ aioWas };
        wipeStores(); injRefs(aioWas, T, title);
        // Takes a COMPOUND mutation to kill, and that is a property of the implementation rather than a
        // weakness here: two independent guards each answer "leave it alone" for C — the stored id already
        // resolves, AND its counterpart does not — so removing either one on its own still yields 0. Both
        // have to go. (The same shape as the third-party favourite in probe_brand section 12; noted so the
        // kill matrix's single entry here is not read as an oversight.)
        CHECK(reconcile(instC) == 0);                       // C is already correct: not so much as reserialized
        const QPair<QString, QString> stateC = snapshot();
        const QJsonObject docC = serializeNow();
        wipeStores(); injRefs(aioWas, T, title);
        CHECK(reconcile(instA) == 2);
        const QPair<QString, QString> stateA = snapshot();
        const QJsonObject docA = serializeNow();
        for (int round = 0; round < 2; ++round)
        {
            wipeStores(); restore(stateA); mergeDoc(docC);
            CHECK(snapshot() == stateA);                    // A keeps the id that resolves on A...
            CHECK(reconcile(instA) == 0);                   // ...and had nothing to repair to keep it
            wipeStores(); restore(stateC); mergeDoc(docA);
            CHECK(snapshot() == stateC);                    // ...and C keeps the id that resolves on C
            CHECK(reconcile(instC) == 0);
        }

        // -- The normalization must not BLUNT the tie-break on real content -------------------------------
        // Spelling and content deliberately point opposite ways: the previous-namespace copy carries the
        // LESSER title. Content has to decide, in both orders. (On the raw-byte comparator the spelling
        // decided instead, and "alpha" won.)
        wipeStores(); injRefs(aioWas, T, QStringLiteral("alpha"));
        const QJsonObject cA = serializeNow();
        wipeStores(); injRefs(aioNow, T, QStringLiteral("beta"));
        const QJsonObject cB = serializeNow();
        wipeStores(); injRefs(aioNow, T, QStringLiteral("beta"));  mergeDoc(cA);
        const QString w1 = favTitle19();
        wipeStores(); injRefs(aioWas, T, QStringLiteral("alpha")); mergeDoc(cB);
        const QString w2 = favTitle19();
        CHECK(w1 == w2);                          // still convergent
        CHECK(w1 == QStringLiteral("beta"));      // ...and decided by the title, not by the spelling

        // -- ...and it must not be TOO BROAD either -------------------------------------------------------
        // A playlist's legacyKey opens with an add-on id as well, and reconcileAddonRefs deliberately never
        // repairs it (nothing resolves segment 0). Folding it into the tie key would make two blobs tie that
        // no repair can ever bring together — and a tie means "keep local", so the two merge orders would
        // then land on DIFFERENT states. Order-independence is what catches an over-broad normalization.
        auto injPlLegacy = [&](const QString& lk) {
            QJsonObject pl;
            pl[QStringLiteral("id")] = QStringLiteral("P");
            pl[QStringLiteral("name")] = title;
            pl[QStringLiteral("categoryKey")] = QStringLiteral("video");
            pl[QStringLiteral("legacyKey")] = lk;
            pl[QStringLiteral("updatedAt")] = double(T);
            pl[QStringLiteral("items")] = QJsonArray();
            QJsonArray pa; pa.append(pl);
            setRaw(plK, compact(pa));
        };
        auto plLegacyKey = [&]() -> QString {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(plK).toString().toUtf8()).array())
                return v.toObject().value(QStringLiteral("legacyKey")).toString();
            return QString();
        };
        const QString lkWas = aioWas + QStringLiteral("|movie|top");
        const QString lkNow = aioNow + QStringLiteral("|movie|top");
        wipeStores(); injPlLegacy(lkWas); const QJsonObject lkDocWas = serializeNow();
        wipeStores(); injPlLegacy(lkNow); const QJsonObject lkDocNow = serializeNow();
        wipeStores(); injPlLegacy(lkNow); mergeDoc(lkDocWas); const QString lk1 = plLegacyKey();
        wipeStores(); injPlLegacy(lkWas); mergeDoc(lkDocNow); const QString lk2 = plLegacyKey();
        CHECK(lk1 == lk2);          // both orders still reach the SAME state
        CHECK(lk1 == lkWas);        // ...decided on the bytes, because this is not a field the repair moves

        // -- The walk reaches an addonId at any DEPTH ------------------------------------------------------
        // Not hypothetical: the document on the other end was written by whatever build THAT device runs,
        // which may be a newer one with a richer per-item shape. An addonId nested inside it still names the
        // same add-on under two names, and the tie must not be decided on it either.
        auto injNested = [&](const QString& addon) {
            QJsonObject inner; inner[QStringLiteral("addonId")] = addon;
            QJsonObject f;
            f[QStringLiteral("itemId")] = QStringLiteral("I");
            f[QStringLiteral("ts")] = double(T);
            f[QStringLiteral("origin")] = inner;
            QJsonArray fa; fa.append(f);
            setRaw(favK, compact(fa));
        };
        auto favNestedAddon = [&]() -> QString {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(favK).toString().toUtf8()).array())
                return v.toObject().value(QStringLiteral("origin")).toObject()
                        .value(QStringLiteral("addonId")).toString();
            return QString();
        };
        wipeStores(); injNested(aioWas); const QJsonObject nestedDoc = serializeNow();
        wipeStores(); injNested(aioNow); mergeDoc(nestedDoc);
        CHECK(favNestedAddon() == aioNow);   // the tie is a no-op; this device keeps its own spelling

        // -- A genuinely NEWER peer still wins outright, spelling and all ----------------------------------
        // Nothing should stop it — that blob really is newer. But it can land this device back on the
        // namespace it renamed away from, and reload() has already run for the session, which is exactly what
        // the post-merge repair is for. Note the repair STILL does not re-date: the peer's stamp survives it.
        wipeStores(); injRefs(aioWas, T + 100, title);
        const QJsonObject newerDoc = serializeNow();
        wipeStores(); injRefs(aioNow, T, title);
        mergeDoc(newerDoc);
        CHECK(favAddon() == aioWas);
        CHECK(plAddon() == aioWas);
        CHECK(reconcile(instA) == 2);
        CHECK(favAddon() == aioNow);
        CHECK(plAddon() == aioNow);
        CHECK(favTs(p19, QStringLiteral("I")) == T + 100);
        CHECK(plUpdated() == T + 100);

        wipeStores();
    }
    // ---- 20. #34 backoff + due(): when a retry runs, when it waits, and when it stops --------------------
    // The whole "durable retry" contract is here, and it is pure — no clock, no socket. Every Due value is
    // reachable and every one of them is a DIFFERENT behaviour at the call site, which is why they exist.
    {
        using namespace PendingPush;
        // Backoff doubles per consecutive failure and then flattens at the ceiling. The point of the cap is
        // that an offline handheld makes ~10 attempts a day, not thousands.
        CHECK(backoffMs(0) == 0);                       // nothing owed -> due immediately
        CHECK(backoffMs(1) == kBaseDelayMs);
        CHECK(backoffMs(2) == 2 * kBaseDelayMs);
        CHECK(backoffMs(3) == 4 * kBaseDelayMs);
        CHECK(backoffMs(7) == kMaxDelayMs);             // 64x base would exceed the ceiling -> clamped
        // A CORRUPTED count cannot produce a no-backoff storm. 64 is the value that matters and 1000 is not:
        // an unguarded `kBaseDelayMs << (attempts - 1)` has its shift masked to 6 bits by the hardware, so
        // attempts == 64 shifts by 63 and every set bit of the base falls off the top — the delay comes out
        // ZERO, sails under the ceiling clamp, and the retry hammers Drive as fast as the event loop allows.
        // attempts == 1000 masks down to a shift of 39, which overshoots the ceiling and gets clamped anyway;
        // it therefore proves nothing about the guard, which is why the interesting boundary is pinned here.
        CHECK(backoffMs(64) == kMaxDelayMs);
        CHECK(backoffMs(1000) == kMaxDelayMs);
        CHECK(backoffMs(-5) == 0);

        const qint64 t0 = 1'000'000'000'000LL;          // an arbitrary fixed "now"; nothing here reads a clock

        State clean;
        CHECK(owed(clean) == false);
        CHECK(gaveUp(clean) == false);
        CHECK(dueClosed(clean, /*signedIn*/true,  /*manual*/false, t0) == Due::Nothing);   // nothing owed -> no traffic
        // Signed out AND clean. Kept knowing it is INERT under single-mutation testing — two independent
        // guards each answer Nothing here, so breaking either one leaves it green, and it is the one
        // assertion in §20-23 that no mutation kills. It stays as the composite tripwire for the day both
        // guards move at once; the two dimensions it composes are each pinned on their own (line 926 for
        // "nothing owed", lines 932-933 for "signed out").
        CHECK(dueClosed(clean, /*signedIn*/false, /*manual*/false, t0) == Due::Nothing);

        State one; one.attempts = 1; one.lastAttemptMs = t0; one.failure = Failure::Offline;
        CHECK(owed(one) == true);
        // Signed out beats everything: a push owed to an account we no longer hold must not touch the network.
        CHECK(dueClosed(one, /*signedIn*/false, /*manual*/false, t0 + kMaxDelayMs) == Due::Nothing);
        CHECK(dueClosed(one, /*signedIn*/false, /*manual*/true,  t0 + kMaxDelayMs) == Due::Nothing);
        // Inside the window -> Wait. Exactly AT the deadline -> Attempt (the boundary is inclusive, so a
        // timer that fires precisely on time is not thrown away for one millisecond).
        CHECK(dueClosed(one, true, false, t0) == Due::Wait);
        CHECK(dueClosed(one, true, false, t0 + kBaseDelayMs - 1) == Due::Wait);
        CHECK(dueClosed(one, true, false, t0 + kBaseDelayMs) == Due::Attempt);
        // A user action overrides the backoff window.
        CHECK(dueClosed(one, true, /*manual*/true, t0) == Due::Attempt);

        // Give-up: kMaxAttempts consecutive failures parks the retry, and NO amount of waiting un-parks it.
        State capped; capped.attempts = kMaxAttempts; capped.lastAttemptMs = t0; capped.failure = Failure::Offline;
        CHECK(gaveUp(capped) == true);
        CHECK(dueClosed(capped, true, false, t0 + 100 * kMaxDelayMs) == Due::GaveUp);
        CHECK(dueClosed(capped, true, /*manual*/true, t0) == Due::Attempt);   // only a user action resumes it
        // One short of the cap is NOT parked — the boundary is the cap itself, not "nearly".
        State nearly; nearly.attempts = kMaxAttempts - 1; nearly.lastAttemptMs = t0; nearly.failure = Failure::Offline;
        CHECK(gaveUp(nearly) == false);
        CHECK(dueClosed(nearly, true, false, t0 + kMaxDelayMs) == Due::Attempt);

        // Sign-in expiry is NOT offline: it is parked immediately, at the FIRST failure, because no amount of
        // patience fixes it. This is the "retrying forever in a state that never resolves" the issue calls out.
        State expired; expired.attempts = 1; expired.lastAttemptMs = t0; expired.failure = Failure::AuthExpired;
        CHECK(dueClosed(expired, true, false, t0 + 100 * kMaxDelayMs) == Due::NeedsSignIn);
        CHECK(dueClosed(expired, true, false, t0) == Due::NeedsSignIn);       // not even Wait — never automatic
        CHECK(dueClosed(expired, true, /*manual*/true, t0) == Due::Attempt);
        // ...and it wins over give-up, so a stale account that also burned the cap reports the ACTIONABLE
        // state ("sign in again") rather than the generic one.
        State both; both.attempts = kMaxAttempts + 3; both.lastAttemptMs = t0; both.failure = Failure::AuthExpired;
        CHECK(gaveUp(both) == true);
        CHECK(dueClosed(both, true, false, t0) == Due::NeedsSignIn);
    }

    // ---- 20b. #34 review round 1: UNCONFIRMED edits hold every timer off; a FUTURE stamp is due now ------
    // Two ways the retry could touch the network at a moment it must not, or fail to touch it forever. Both
    // are decided in due(), the single choke point every timer and every panel button passes through.
    {
        using namespace PendingPush;
        const qint64 t0 = 1'000'000'000'000LL;

        State clean;
        State one;     one.attempts = 1;            one.lastAttemptMs = t0;     one.failure = Failure::Offline;
        State capped;  capped.attempts = kMaxAttempts; capped.lastAttemptMs = t0; capped.failure = Failure::Offline;
        State expired; expired.attempts = 1;        expired.lastAttemptMs = t0; expired.failure = Failure::AuthExpired;

        // (a) THE VISIT GATE. SettingsTxn writes THROUGH to the ini, so while a settings visit is open the
        // fingerprint checkStatus reads is a state the user has NOT confirmed and may be about to Discard.
        // Uploading it publishes rejected values; on the PullThenPush arm applySettingsJson would also
        // force-commit the transaction and take the Discard away entirely. So no record may produce an Attempt
        // while the flag is set — INCLUDING with manual == true, which is the case that matters, because the
        // post-Save timer is manual: the Save it inherited confirmed the state as it was three seconds ago,
        // not the edits the user went back in and started making. (The caller sets the flag for BOTH timers
        // and for neither of the panel's own buttons; that split is MainWindow::runPendingPush's, stated in
        // PendingPush decision 5, and it is why the flag is named for unconfirmed edits and not for the
        // transaction — the panel is reachable only from inside the settings area, so a transaction is open
        // by construction whenever "Retry sync" is pressed.)
        for (const State& s : { one, capped, expired })
            for (bool manual : { false, true })
                for (qint64 now : { t0, t0 + 100 * kMaxDelayMs })
                    CHECK(due(s, /*signedIn*/true, manual, now, /*unconfirmed*/true) == Due::Deferred);
        CHECK(due(clean, true, /*manual*/true, t0, /*unconfirmed*/true) == Due::Deferred);  // post-Save
        // Deferred is its OWN verdict, not Wait and not a park, because the three mean different things to the
        // caller: Wait re-arms on the backoff, NeedsSignIn/GaveUp stop the timer dead, and this one re-arms
        // shortly regardless of the record — so the owed push survives the visit instead of being dropped.
        //
        // The control for the block above — that these same records DO attempt with the flag clear, so the
        // gate is what stopped them and not some other guard answering first — is §20's own schedule
        // assertions, which already pin `one` and `capped` attempting on a manual trigger. Repeating them here
        // would add lines and no kill power. The one case §20 does not carry is a CLEAN record on a manual
        // trigger, which is exactly the post-Save push, so that one is asserted:
        CHECK(dueClosed(clean, true, /*manual*/true, t0) == Due::Attempt);
        // Signed out still wins: a visit open on a device with no account is Nothing, not a deferral poll...
        CHECK(due(one, /*signedIn*/false, false, t0, /*unconfirmed*/true) == Due::Nothing);
        // ...and neither is a clean record with nothing owed. An open settings visit is not by itself a reason
        // to keep waking up.
        CHECK(due(clean, true, /*manual*/false, t0, /*unconfirmed*/true) == Due::Nothing);

        // (b) THE CLOCK. lastAttemptMs is wall clock, and wall clock moves backwards (an RTC that read ahead,
        // then an NTP correction). A stamp in the FUTURE makes dueAtMs unreachable, so a plain
        // `now < dueAt -> Wait` re-arms forever WITHOUT EVER ATTEMPTING, behind a panel that says "retrying
        // automatically" — and give-up never rescues it, because give-up counts attempts and none are made.
        // Note the first line: the stall survives armPendingRetry's clamp, which bounds the WAIT at
        // kMaxDelayMs but cannot make a verdict of Wait into an attempt.
        State future; future.attempts = 1; future.failure = Failure::Offline;
        future.lastAttemptMs = t0 + 30LL * 24 * 3600 * 1000;         // an RTC a month ahead
        CHECK(dueAtMs(future) - t0 > kMaxDelayMs);                   // the wait outruns the timer's clamp...
        CHECK(dueClosed(future, true, false, t0) == Due::Attempt);   // ...and due() does not wait for it
        State ahead; ahead.attempts = 2; ahead.lastAttemptMs = t0 + 1; ahead.failure = Failure::Offline;
        CHECK(dueClosed(ahead, true, false, t0) == Due::Attempt);    // one millisecond ahead is already ahead
        // The boundary: a stamp EQUAL to now is the ordinary fresh failure and still backs off normally, so
        // this did not quietly delete the backoff.
        State atNow; atNow.attempts = 1; atNow.lastAttemptMs = t0; atNow.failure = Failure::Offline;
        CHECK(dueClosed(atNow, true, false, t0) == Due::Wait);
        // A future stamp does not UN-PARK either parked state: a clock correction is not a user action.
        State futureCapped = capped; futureCapped.lastAttemptMs = t0 + 1;
        CHECK(dueClosed(futureCapped, true, false, t0) == Due::GaveUp);
        State futureAuth = expired; futureAuth.lastAttemptMs = t0 + 1;
        CHECK(dueClosed(futureAuth, true, false, t0) == Due::NeedsSignIn);
    }

    // ---- 21. #34 resolve(): what an attempt actually does, and why it cannot oscillate -------------------
    {
        using namespace PendingPush;
        // Unreachable is UNPROVEN-EMPTY, not "nothing to do". Both halves matter: a failed folder query and a
        // failed bundle query each mean the push would upload against an empty file id and POST a DUPLICATE
        // bundle rather than PATCH the real one.
        CHECK(resolve(/*reached*/false, /*listReached*/true,  /*local*/true, /*remote*/false) == Plan::Unreachable);
        CHECK(resolve(true,  /*listReached*/false, true,  false) == Plan::Unreachable);
        CHECK(resolve(false, false, true, true) == Plan::Unreachable);
        // THE IDEMPOTENCE GATE. Whatever the record thinks it owes, the fingerprint is the authority: a retry
        // that finds local == the synced baseline uploads nothing and clears. This is what stops a retry from
        // re-sending state an exit push or a manual Sync now already got up there.
        CHECK(resolve(true, true, /*local*/false, /*remote*/false) == Plan::NothingToSend);
        CHECK(resolve(true, true, /*local*/false, /*remote*/true)  == Plan::NothingToSend);  // peer moved, we owe nothing
        // We have edits and the remote is where we left it -> a plain push.
        CHECK(resolve(true, true, true, false) == Plan::Push);
        // CONFLICT: a peer pushed while we were offline AND we have local edits. Take theirs first, then send
        // ours. Never a blind push — that would silently revert the peer's whole bundle.
        CHECK(resolve(true, true, true, true) == Plan::PullThenPush);
        // NON-OSCILLATION, and exactly how much of it this section proves. HALF: once localChanged has gone
        // false the plan is NothingToSend whatever the remote did (the two NothingToSend lines above are that
        // statement — both values of remoteChanged), so there is no input to resolve() for which a round
        // repeats itself. The OTHER half — that a pull actually MAKES localChanged false — is not a property
        // of resolve() at all, and restating these same inputs in a loop here would not touch it. It is
        // asserted where it lives, on the baseline write itself, in §24.
    }

    // ---- 22. #34 outcome transitions + failure classification -------------------------------------------
    {
        using namespace PendingPush;
        const qint64 t0 = 1'700'000'000'000LL;

        State s;
        s = onOutcome(s, Outcome::Offline, t0);
        CHECK(s.attempts == 1 && s.lastAttemptMs == t0 && s.failure == Failure::Offline);
        s = onOutcome(s, Outcome::Offline, t0 + 5);
        CHECK(s.attempts == 2 && s.lastAttemptMs == t0 + 5);          // consecutive failures accumulate
        // A later AUTH failure re-classifies the record without resetting the count — the attempts already
        // spent are still spent, and the park reason is now the actionable one.
        s = onOutcome(s, Outcome::AuthExpired, t0 + 9);
        CHECK(s.attempts == 3 && s.failure == Failure::AuthExpired);
        // Success ZEROES the whole record, not just the counter: a clean record must LOOK clean.
        const State cleared = onOutcome(s, Outcome::Success, t0 + 20);
        CHECK(cleared.attempts == 0);
        CHECK(cleared.lastAttemptMs == 0);
        CHECK(cleared.failure == Failure::None);
        CHECK(owed(cleared) == false);

        // classifyRefresh separates "needs a human" from "needs patience", and BOTH directions are pinned,
        // because getting either one wrong is its own user-visible failure: a transient error mislabelled
        // Expired parks the device behind a sign-in prompt that cannot clear it, and a real expiry mislabelled
        // Offline retries an account that will never come back.
        const QString noCode;
        CHECK(classifyRefresh(/*haveToken*/false, /*status*/200, noCode, /*haveAccess*/true) == Auth::Expired);
        CHECK(classifyRefresh(true, /*status*/0, noCode, false) == Auth::Offline);   // never heard back at all
        CHECK(classifyRefresh(true, 200, noCode, /*haveAccess*/true) == Auth::Ok);
        // No stored grant is Expired even when the network is fine — the fix is a sign-in, not a retry.
        CHECK(classifyRefresh(false, 0, noCode, false) == Auth::Expired);

        // PARK. A revoked/expired grant or a dead client is the ONLY shape allowed to reach a sign-in prompt,
        // and it is identified positively: RFC 6749 §5.2's code, from a 4xx.
        CHECK(classifyRefresh(true, 400, QStringLiteral("invalid_grant"), false) == Auth::Expired);
        CHECK(classifyRefresh(true, 401, QStringLiteral("invalid_client"), false) == Auth::Expired);
        // BACK OFF. The transient answers that ALSO arrive as a JSON object — which is exactly what the old
        // "a JSON body means rejected" rule mis-read. Every one of these used to park the device at the FIRST
        // failure: no backoff, no automatic recovery, and "your sign-in expired" about one that had not.
        CHECK(classifyRefresh(true, 429, QStringLiteral("rateLimitExceeded"), false) == Auth::Offline);
        CHECK(classifyRefresh(true, 500, QStringLiteral("internal_failure"), false) == Auth::Offline);
        CHECK(classifyRefresh(true, 503, noCode, false) == Auth::Offline);
        CHECK(classifyRefresh(true, 502, QStringLiteral("invalid_grant"), false) == Auth::Offline);  // 5xx wins
        // 429 is the one transient answer that shares a status class with a real rejection, so it is the one
        // that needs excluding by name — this is the assertion that makes that line load-bearing rather than
        // a second guard the 4xx test already covers.
        CHECK(classifyRefresh(true, 429, QStringLiteral("invalid_client"), false) == Auth::Offline);
        // A proxy or captive portal: a body that parses, a status that is not a grant rejection, and no code
        // this client recognises. Patience, not a prompt.
        CHECK(classifyRefresh(true, 200, noCode, /*haveAccess*/false) == Auth::Offline);
        CHECK(classifyRefresh(true, 407, QStringLiteral("proxy_auth_required"), false) == Auth::Offline);
        CHECK(classifyRefresh(true, 400, noCode, false) == Auth::Offline);          // a 4xx with no code at all
        // Codes that are NOT a dead grant: a malformed request or a misconfigured OAuth client is a bug in
        // this app, and signing in again fixes neither. They back off (and surface through the attempt cap as
        // something the user CAN act on) rather than accusing the account.
        CHECK(classifyRefresh(true, 400, QStringLiteral("invalid_request"), false) == Auth::Offline);
        CHECK(classifyRefresh(true, 400, QStringLiteral("unauthorized_client"), false) == Auth::Offline);
        CHECK(classifyRefresh(true, 400, QStringLiteral("unsupported_grant_type"), false) == Auth::Offline);
        // Matched EXACTLY — no prefix, no substring, no case folding — so a body that merely mentions the code
        // cannot park the device.
        CHECK(classifyRefresh(true, 400, QStringLiteral("invalid_grants"), false) == Auth::Offline);
        CHECK(classifyRefresh(true, 400, QStringLiteral("INVALID_GRANT"), false) == Auth::Offline);
        // A token beats everything: a reply that carried one is Ok even if the body also carried a code.
        CHECK(classifyRefresh(true, 200, QStringLiteral("invalid_grant"), /*haveAccess*/true) == Auth::Ok);

        // classifyPush: ONLY the token layer can declare an auth failure. Every other failure is retryable,
        // which is what keeps a plain Drive hiccup out of the "sign in again" prompt.
        CHECK(classifyPush(/*ok*/true,  Auth::Expired) == Outcome::Success);   // success is success regardless
        CHECK(classifyPush(false, Auth::Expired) == Outcome::AuthExpired);
        CHECK(classifyPush(false, Auth::Offline) == Outcome::Offline);
        CHECK(classifyPush(false, Auth::Ok)      == Outcome::Offline);         // a Drive error, not an auth one
    }

    // ---- 23. #34 durability, record SHAPE, and the carve-outs that keep credentials out of it ------------
    // The record must survive a restart, must never travel to another device, must never be undone by a
    // settings Discard, and must never itself look like a settings change. That last one is the load-bearing
    // anti-oscillation property at the STORAGE level: if writing the record moved the sync fingerprint, every
    // failed push would create the very "local changed" it is retrying, and the device would never converge.
    {
        using namespace PendingPush;

        // Credential-shaped rows, seeded with sentinels so the assertions below can prove no VALUE was copied
        // anywhere near the pending record. These are fake strings, not credentials.
        //
        // The tripwire looks for the shared MARKER, not for the seeded values, and that distinction is the
        // whole assertion: (a) below ROTATES both tokens, so by the time the record is written the live value
        // is not the one seeded here — a check against the seeded strings would sail past a leak of the
        // current token and prove nothing. Verified by mutation: appending the live trakt/access to the
        // persisted failure word is caught only by the marker form.
        const QString marker = QStringLiteral("SENTINEL");
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("trakt/access"), marker + QStringLiteral("-ACCESS-0000"));
            raw.setValue(QStringLiteral("trakt/refresh"), marker + QStringLiteral("-REFRESH-0000"));
            raw.setValue(QStringLiteral("ra/token"), marker + QStringLiteral("-RATOKEN-0000"));
            raw.sync();
        }

        // (a) THE PUSH TRIGGER. Push-on-Save fires off SettingsTxn's dirty count, and that count is in-scope
        // only — so a rotating credential landing from a background reply mid-visit cannot schedule a push.
        // This is what "the push respects the transaction's exclusions" means operationally.
        CHECK(SettingsTxn::inScope(QStringLiteral("trakt/access")) == false);
        CHECK(SettingsTxn::inScope(QStringLiteral("trakt/refresh")) == false);
        CHECK(SettingsTxn::inScope(QStringLiteral("ra/token")) == false);
        SettingsTxn::begin();
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("trakt/access"), marker + QStringLiteral("-ACCESS-ROTATED"));
            raw.setValue(QStringLiteral("ra/token"), marker + QStringLiteral("-RATOKEN-ROTATED"));
            raw.sync();
        }
        CHECK(SettingsTxn::dirtyCount() == 0);      // a token rotation is invisible to the Save prompt...
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("trakt/clientId"), QStringLiteral("typed-by-the-user"));
            raw.sync();
        }
        CHECK(SettingsTxn::dirtyCount() == 1);      // ...while a row the user TYPES is not (the trigger lives)
        SettingsTxn::commit();

        // (b) THE RECORD'S CARVE-OUTS. device/push/* is device-local (never travels) and out of transaction
        // scope (a Discard cannot resurrect a pending state the user has no way to see).
        for (const QString& k : { keyAttempts(), keyLastAttempt(), keyFailure() })
        {
            CHECK(CloudSync::isDeviceLocalKey(k) == true);
            CHECK(SettingsTxn::inScope(k) == false);
        }

        // (c) ROUND-TRIP through the real ini, which is what "survives a restart" reduces to here.
        const qint64 t0 = 1'700'000'123'456LL;
        State s; s.attempts = 3; s.lastAttemptMs = t0; s.failure = Failure::AuthExpired;
        save(s);
        // DURABILITY AT THE LEVEL THE WORD MEANS — read the ini FILE, not another QSettings. Two QSettings on
        // one path share Qt's in-process QConfFile cache, so the load() round-trip below passes just as well
        // with an UNFLUSHED write, and "survives a crash" is exactly the claim that cache cannot support.
        // Verified by mutation: deleting save()'s store().sync() leaves every other assertion in this section
        // green and is caught only here.
        {
            QFile f(iniPath);
            CHECK(f.open(QIODevice::ReadOnly));
            const QByteArray onDisk = f.readAll();
            CHECK(onDisk.contains("lastAttemptMs=1700000123456"));  // the exact stamp, on disk
            CHECK(onDisk.contains("failure=auth"));
        }
        const State back = load();
        CHECK(back.attempts == 3);
        CHECK(back.lastAttemptMs == t0);            // a 64-bit epoch survives the ini's string round-trip
        CHECK(back.failure == Failure::AuthExpired);

        // (d) SHAPE. Exactly three scalar keys, and the failure reason is a WORD (an ini is a file a user can
        // open, and a persisted enumerator is a number whose meaning changes the day someone edits the enum).
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.beginGroup(QStringLiteral("device/push"));
            QStringList pushKeys = raw.childKeys(); pushKeys.sort();
            raw.endGroup();
            CHECK(pushKeys == (QStringList{QStringLiteral("attempts"), QStringLiteral("failure"),
                                           QStringLiteral("lastAttemptMs")}));
            CHECK(raw.value(QStringLiteral("device/push/failure")).toString() == QStringLiteral("auth"));
            // NO settings value rides along: nothing in the record carries the credential marker, in any of
            // its seeded or rotated forms. Note this is asserted against the RAW ini rather than the State
            // struct — the struct has nowhere to put a string, which is the design, and the tripwire is here
            // to catch the day someone gives it one.
            for (const QString& k : { keyAttempts(), keyLastAttempt(), keyFailure() })
                CHECK(!raw.value(k).toString().contains(marker));
            // §1's invariant survives the new record: `push` is a GROUP under device, not a child KEY, so the
            // only direct child key of `device` is still `id`.
            raw.beginGroup(QStringLiteral("device"));
            const QStringList deviceKeys = raw.childKeys();
            raw.endGroup();
            CHECK(deviceKeys == QStringList{QStringLiteral("id")});
        }

        // (e) THE ANTI-OSCILLATION PROPERTY. Recording a failure must not move the sync fingerprint or the
        // bundle bytes — otherwise the act of remembering "I owe a push" would itself be an unsynced change,
        // and the retry would be chasing its own tail forever.
        const QByteArray fpWithRecord = CloudSync::stateFingerprint();
        const QByteArray bundleWithRecord = CloudSync::buildSettingsJson();
        CHECK(!bundleWithRecord.contains("device/push"));   // the record is not in the uploaded document
        State s2; s2.attempts = 7; s2.lastAttemptMs = t0 + 999; s2.failure = Failure::Offline;
        save(s2);
        CHECK(CloudSync::stateFingerprint() == fpWithRecord);      // a DIFFERENT record -> same fingerprint
        CHECK(CloudSync::buildSettingsJson() == bundleWithRecord);
        clear();
        CHECK(CloudSync::stateFingerprint() == fpWithRecord);      // and clearing it -> still the same
        CHECK(CloudSync::buildSettingsJson() == bundleWithRecord);

        // (f) CLEARING REMOVES the keys rather than writing zeros, so a synced device and a fresh install look
        // identical in the ini — and load() reports a clean record from an absent one.
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.beginGroup(QStringLiteral("device/push"));
            const QStringList afterClear = raw.childKeys();
            raw.endGroup();
            CHECK(afterClear.isEmpty());
        }
        CHECK(owed(load()) == false);

        // (g) A CORRUPTED record cannot arm a retry. Hand-edit a negative count (or a failure word with no
        // count beside it) and load() normalises to clean rather than handing due() a nonsense state.
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("device/push/attempts"), -4);
            raw.setValue(QStringLiteral("device/push/lastAttemptMs"), t0);
            raw.setValue(QStringLiteral("device/push/failure"), QStringLiteral("offline"));
            raw.sync();
        }
        const State fixed = load();
        CHECK(fixed.attempts == 0);
        CHECK(fixed.lastAttemptMs == 0);
        CHECK(fixed.failure == Failure::None);
        CHECK(dueClosed(fixed, /*signedIn*/true, /*manual*/false, t0) == Due::Nothing);
        clear();
    }

    // ---- 24. #34 review round 1: the pull's FIXED POINT — the half of no-oscillation §21 cannot see ------
    // The whole no-oscillation argument rests on "after a pull, this device's baseline IS the bytes on Drive,
    // so the next checkStatus reports localChanged == false". That claim lived in comments and in
    // network-coupled code, and no probe touched it. It is not actually network-coupled: the entire property
    // is the baseline WRITE, which is why applyRemote's two setValue lines were factored into
    // adoptSyncedBaseline. Called directly here, with no socket and no Drive account.
    {
        using namespace PendingPush;
        const QString iso1 = QStringLiteral("2026-01-01T00:00:00Z");
        const QString iso2 = QStringLiteral("2026-01-02T00:00:00Z");
        const auto write = [&](const QString& k, const QString& v) {
            QSettings raw(iniPath, QSettings::IniFormat); raw.setValue(k, v); raw.sync(); };

        // A device with local edits and a baseline it no longer matches — the state that plans PullThenPush.
        write(QStringLiteral("cloud/syncedHash"), QStringLiteral("stale-baseline"));
        write(QStringLiteral("ui/probeS23"), QStringLiteral("local-edit"));
        CHECK(CloudSync::localChangedSinceSync() == true);
        CHECK(resolve(true, true, CloudSync::localChangedSinceSync(), /*remote*/true) == Plan::PullThenPush);

        // THE PULL, stamped — the shape that actually runs. The remote's appProperties hash is the hash of the
        // state the PEER uploaded, so after applyBundle it is the hash of the state this device now holds; the
        // probe stands in for applyBundle by writing the peer's value and taking the fingerprint.
        write(QStringLiteral("ui/probeS23"), QStringLiteral("peer-value"));
        const QString peerStamp = QString::fromUtf8(CloudSync::stateFingerprint());
        CloudSync::adoptSyncedBaseline(iso2, peerStamp);
        // THE FIXED POINT: localChanged is now false, so the very next resolve() answers NothingToSend and the
        // round ends. This is the line §21's for-loop was standing in for and could not actually reach.
        CHECK(CloudSync::localChangedSinceSync() == false);
        CHECK(resolve(true, true, CloudSync::localChangedSinceSync(), /*remote*/true) == Plan::NothingToSend);
        // The legacy shape (a bundle with no hash stamp) reaches the same fixed point by a different route:
        // with nothing to adopt, the state just applied IS the baseline. Asserted from a DIRTY baseline so it
        // cannot pass by inheriting the one above.
        write(QStringLiteral("cloud/syncedHash"), QStringLiteral("stale-again"));
        CHECK(CloudSync::localChangedSinceSync() == true);
        CloudSync::adoptSyncedBaseline(iso1, QString());
        CHECK(CloudSync::localChangedSinceSync() == false);
        // ...and the modified stamp is recorded too: checkStatus falls back to it for a legacy bundle that
        // carries no hash, and a baseline adopted without it would report remoteChanged forever.
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            CHECK(raw.value(QStringLiteral("cloud/appliedModified")).toString() == iso1);
        }
        // The remote's stamp is adopted VERBATIM, never re-derived from local state. checkStatus compares the
        // remote's appProperties hash against this stored value, so a baseline written as "our own hash"
        // instead would read remoteChanged on every check from then on — a false conflict that never clears,
        // and one the fixed-point assertions above cannot see (there the two are equal by construction).
        CloudSync::adoptSyncedBaseline(iso2, QStringLiteral("peer-stamp-0123"));
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            CHECK(raw.value(QStringLiteral("cloud/syncedHash")).toByteArray() == QByteArray("peer-stamp-0123"));
        }
        CloudSync::adoptSyncedBaseline(iso1, QString());   // back to the fixed point for the checks below
        CHECK(CloudSync::localChangedSinceSync() == false);

        // NOT VACUOUS. The gate still sees a real edit made after the pull — which is the state the following
        // push exists to send, and the reason the round is Pull-THEN-Push rather than just Pull.
        write(QStringLiteral("ui/probeS23"), QStringLiteral("edited-after-pull"));
        CHECK(CloudSync::localChangedSinceSync() == true);
        CHECK(resolve(true, true, CloudSync::localChangedSinceSync(), /*remote*/false) == Plan::Push);
        // And this is the same discriminator the push funnel uses to refuse to inflate the pending record on a
        // failed push with nothing to send (#34 review, minor 4): a device whose state matches its baseline
        // owes nothing, whatever the last exit push did.
        CloudSync::adoptSyncedBaseline(iso2, QString());
        CHECK(CloudSync::localChangedSinceSync() == false);

        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.remove(QStringLiteral("ui/probeS23"));
            raw.remove(QStringLiteral("cloud/syncedHash"));
            raw.remove(QStringLiteral("cloud/appliedModified"));
            raw.sync();
        }
    }

    // ---- 20. Metadata overrides (issue #24): newest-wins, equal-ts convergence, and reset-is-not-a-deletion --
    //
    // The store is GLOBAL (no profile level, like resume) because a mis-scrape is wrong for the whole
    // household. The interesting case is the one the whole feature turns on: "reset to scraped" must not be a
    // row deletion. A deleted row is indistinguishable from "this device never saw that item", so the next
    // merge with a peer that still holds the old correction would put it straight back — the user would watch
    // their reset undo itself, with nothing to tell them why.
    {
        const QString k20 = QStringLiteral("igdb:24001");
        const QString h20 = md5(k20);
        const QString ikey = QStringLiteral("metaoverrides/items/") + h20;
        auto injOv = [&](const QString& title, qint64 ts) {
            QJsonObject o;
            if (!title.isEmpty()) o[QStringLiteral("title")] = title;   // omit-empty: the record's ONE spelling
            o[QStringLiteral("updatedAt")] = double(ts);
            setRaw(ikey, compactO(o));
        };
        auto ovTitle = [&]() -> QString {
            QSettings raw(iniPath, QSettings::IniFormat);
            return QJsonDocument::fromJson(raw.value(ikey).toString().toUtf8())
                .object().value(QStringLiteral("title")).toString();
        };
        auto ovPresent = [&]() {
            QSettings raw(iniPath, QSettings::IniFormat); raw.sync(); return raw.contains(ikey);
        };

        // 20a. The store rides the document at all, under its own top-level key and its own hash.
        wipeStores(); injOv(QStringLiteral("Bonk's Adventure"), T);
        const QJsonObject d20 = serializeNow();
        CHECK(d20.contains(QStringLiteral("metaoverrides")));
        CHECK(d20.value(QStringLiteral("metaoverrides")).toObject().contains(h20));
        CHECK(d20.value(QStringLiteral("metaoverrides")).toObject().value(h20).toObject()
                  .value(QStringLiteral("title")).toString() == QStringLiteral("Bonk's Adventure"));

        // 20b. Newest updatedAt wins, each direction.
        wipeStores(); injOv(QStringLiteral("Newer"), T);      const QJsonObject newer = serializeNow();
        wipeStores(); injOv(QStringLiteral("Older"), T - 500); mergeDoc(newer);
        CHECK(ovTitle() == QStringLiteral("Newer"));           // remote newer replaces
        wipeStores(); injOv(QStringLiteral("Older"), T - 500); const QJsonObject older = serializeNow();
        wipeStores(); injOv(QStringLiteral("Newer"), T);       mergeDoc(older);
        CHECK(ovTitle() == QStringLiteral("Newer"));           // local newer survives

        // 20c. An item only ONE device knows about is imported, not dropped.
        wipeStores(); injOv(QStringLiteral("Only theirs"), T); const QJsonObject theirs = serializeNow();
        wipeStores(); mergeDoc(theirs);
        CHECK(ovTitle() == QStringLiteral("Only theirs"));

        // 20d. Equal timestamps: BOTH merge orders reach the same record. Two people fixed the same bad
        // scrape in the same second, differently; convergence is the property, the winner is only the means.
        wipeStores(); injOv(QStringLiteral("alpha"), T); const QJsonObject tieA = serializeNow();
        wipeStores(); injOv(QStringLiteral("beta"),  T); const QJsonObject tieB = serializeNow();
        wipeStores(); injOv(QStringLiteral("alpha"), T); mergeDoc(tieB); const QString e1 = ovTitle();
        wipeStores(); injOv(QStringLiteral("beta"),  T); mergeDoc(tieA); const QString e2 = ovTitle();
        CHECK(e1 == e2);                                       // convergent regardless of merge order
        CHECK(e1 == QStringLiteral("beta"));                   // greater canonical bytes decides it

        // 20e. The SAME correction typed on both devices in the same second is not a conflict at all: the
        // record has one canonical spelling, so the two blobs tie as equal and neither device is disturbed.
        wipeStores(); injOv(QStringLiteral("Same fix"), T); const QJsonObject sameA = serializeNow();
        wipeStores(); injOv(QStringLiteral("Same fix"), T); mergeDoc(sameA);
        CHECK(ovTitle() == QStringLiteral("Same fix"));

        // 20f. RESET-TO-SCRAPED PROPAGATES. Local reset (an empty record, freshly stamped) vs a peer still
        // holding the old correction: the husk is newer, so it wins and the reset survives the merge.
        wipeStores(); injOv(QStringLiteral("The wrong game"), T - 500); const QJsonObject stale = serializeNow();
        wipeStores(); injOv(QString(), T);                     // the husk MetaOverrides::reset() writes
        mergeDoc(stale);
        CHECK(ovPresent());                                    // the row is still there to keep winning
        CHECK(ovTitle().isEmpty());                            // and it still composites as "show the scrape"

        // …and the reset travels the other way too: a peer that pulls the husk drops its own old correction.
        wipeStores(); injOv(QString(), T); const QJsonObject resetDoc = serializeNow();
        wipeStores(); injOv(QStringLiteral("The wrong game"), T - 500); mergeDoc(resetDoc);
        CHECK(ovTitle().isEmpty());

        // 20g. …and the reset does NOT become permanent. A later, genuine correction (strictly newer) beats
        // the husk in both directions — otherwise "reset" would quietly mean "never correctable again".
        wipeStores(); injOv(QStringLiteral("The right game"), T + 100); const QJsonObject redo = serializeNow();
        wipeStores(); injOv(QString(), T); mergeDoc(redo);
        CHECK(ovTitle() == QStringLiteral("The right game"));

        // 20i. mergeAll drops the store's lazy cache. The merge writes metaoverrides/* under the ini
        // DIRECTLY, so a warm cache would keep serving the correction the merge just replaced — for the rest
        // of the session, on every screen, with a pull having visibly happened.
        wipeStores(); injOv(QStringLiteral("Remote wins"), T); const QJsonObject fresh20 = serializeNow();
        wipeStores(); injOv(QStringLiteral("Local stale"), T - 500);
        CHECK(MetaOverrides::get(k20).title == QStringLiteral("Local stale")); // warms the cache first
        mergeDoc(fresh20);
        CHECK(MetaOverrides::get(k20).title == QStringLiteral("Remote wins"));

        // 20h. Via the STORE front-end rather than raw ini, end to end: set -> reset leaves a husk (a real,
        // newer record), not a removed row, and the item reads back as un-overridden.
        wipeStores();
        MetaOverrides::Override ov;
        ov.title = QStringLiteral("Corrected");
        MetaOverrides::set(k20, ov);
        CHECK(MetaOverrides::get(k20).title == QStringLiteral("Corrected"));
        // set() is the merge funnel: every content write stamps a fresh timestamp, or the correction could
        // never outrank the peer copy it was made to replace.
        {
            QSettings raw(iniPath, QSettings::IniFormat); raw.sync();
            const qint64 ts = qint64(QJsonDocument::fromJson(raw.value(ikey).toString().toUtf8())
                                         .object().value(QStringLiteral("updatedAt")).toDouble());
            CHECK(saneTs(ts, T, QDateTime::currentSecsSinceEpoch()));
        }
        CHECK(MetaOverrides::count() == 1);
        MetaOverrides::reset(k20);
        CHECK(ovPresent());                                    // the husk row exists...
        CHECK(!MetaOverrides::has(k20));                       // ...but nothing is overridden any more
        CHECK(MetaOverrides::count() == 0);                    // and a husk is not a correction to count
        CHECK(serializeNow().value(QStringLiteral("metaoverrides")).toObject().contains(h20)); // it still syncs

        wipeStores();
    }

    if (failures == 0) { std::puts("CLOUDMERGE-OK"); return 0; }
    std::fprintf(stderr, "CLOUDMERGE: %d check(s) failed\n", failures);
    return 1;
}
