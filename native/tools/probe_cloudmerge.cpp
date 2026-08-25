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
//     while hiding an item is NOT a delete and records no tombstone;
//   * the three shapes "cleared" can take without colliding with "never known" (§25-27): a HUSK for marks
//     (#132), a resume TOMBSTONE (#150) and a recents tombstone that a cap eviction is deliberately kept out
//     of (#150) — each asserted through the reader's answer after a merge, never through the row's presence;
//   * and the converse of that rule (§29, also #132): a NON-clear must not be spelled as a clear either, or
//     the false clear — newest, and never compacted — deletes another device's genuine correction.
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
#include "LaunchOptionsStore.h" // issue #51 / RetroPark Slice 2a: the per-game override the merge carries opaquely
#include "MissedDismiss.h"  // issue #25: the per-show "you missed" dismissal watermarks
#include "TraktMissed.h"    // issue #25: kMissedDismissTtlDays — the shelf life prune() enforces
#include "CloudSync.h"     // mdsync T4: the device-local carve-out + bundle-settings hands-off
#include "Scrobble.h"      // #192: the carve-out is asserted through the writer's own prefixes
#include "ScrobbleQueue.h" // ...and through the real key builders, not hand-typed literals
#include "BrandMigration.h" // #58 review: the stored-add-on-id repair, played against the merge (section 19)
#include "SettingsTxn.h"    // #26: applySettingsJson must close an open settings transaction
#include "PendingPush.h"    // #34: the durable pending-push record + the retry policy (§19-23)
#include "TraktSync.h"      // #23: backfillThroughKey/DoneKey — the per-profile import cursor is device-local
#include "ProfileStore.h"
#include "PcGameRemap.h"        // issue #166: the PC-game id repair, played against the merge (§30)
#include "ConsumptionStats.h"   // issue #166: the reader §30f asserts the accumulators through
#include "PlayStats.h"          // issue #166: ditto, the per-game play totals
#include "RecentStore.h"        // issue #150: the reader §27 asserts through (the list Home renders)
#include "PlaybackSession.h"    // issue #150: the reader §26 asserts through (the pending resume seek)
#include "FilterPresetStore.h"  // issue #184: the saved-filter preset store §33 asserts through (the accessor)
#include "StoredUrl.h"          // issue #200: the credential rule §34 drives as a pure function
#include "CredentialScrub.h"    // issue #200: the one-time sweep of what earlier builds already wrote (§35f)
#include "StoredIdentity.h"     // issue #203: the durable name a row is filed under, and its sweep (§36)
#include "Subsonic.h"           // issue #203: the reader that turns a stream url back into a track id (§36a)
#include "SubsonicServerStore.h" // issue #203: which servers exist, i.e. which urls can be re-identified
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

// Double comparison within a hair, for the speed-store rates §24c round-trips through JSON (issue #140).
static bool near(double a, double b) { return (a > b ? a - b : b - a) < 1e-9; }

// §30: PlayStats hashes with SHA-1, not MD5 (PlayStats.cpp:29) — the two accumulators genuinely disagree, and
// using the wrong one here would inject a record the store can never find and assert nothing.
static QString sha1(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
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
        for (const char* g : {"marks", "favorites", "bookmarks", "audiobookmarks", "playlists", "filterpresets",
                              "deleted", "resume", "recent", "metaoverrides", "launchopts", "speed", "lyricoffset",
                              "missed"})
            raw.remove(QLatin1String(g));
        raw.sync();
        ItemMarks::invalidate();
        MetaOverrides::invalidate();
        MissedDismiss::invalidate();
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
                                  "metaoverrides", "speed", "lyricoffset"})
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
            // Audio output (issue #69): device / passthrough / exclusive are per-device. A synced audio-device
            // id would point another machine at the wrong sound card, so the whole audio/* group is carved out.
            raw.setValue(QStringLiteral("audio/device"), QStringLiteral("wasapi/RECEIVER"));
            raw.setValue(QStringLiteral("audio/passthrough"), QStringLiteral("true"));
            raw.setValue(QStringLiteral("audio/exclusive"), QStringLiteral("true"));
            raw.setValue(QStringLiteral("downloads/foo"), QStringLiteral("1"));
            raw.setValue(QStringLiteral("pcgames/bar"), QStringLiteral("1"));
            // pcscan/* (issue #62): the persisted last-good installed-scan per launcher. A snapshot of what
            // THIS machine has installed, so it is device-local and must never ride the bundle.
            raw.setValue(QStringLiteral("pcscan/steam"), QStringLiteral("[{\"id\":\"440\",\"name\":\"TF2\"}]"));
            // Per-game launch HOOKS (issue #64): a command line that EXECUTES, so it is device-local and must
            // NOT ride the bundle — the deliberate contrast with launchopts/* (#51), which DOES sync.
            raw.setValue(QStringLiteral("launchhooks/items/deadbeef"),
                         QStringLiteral("{\"preLaunch\":\"joyprofile start\"}"));
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
            // A "you missed" dismissal (issue #25), seeded for the same reason as the correction above: the
            // per-item sweep below iterates a list of PREFIXES, and an unseeded one passes on absence.
            raw.setValue(QStringLiteral("missed/pX/shows/deadbeef"), QStringLiteral("1700000000"));
            // A per-item playback speed (issue #140), seeded for the same reason: the per-item sweep needs a
            // "speed/" key to find, or that iteration passes on absence and no mutation could kill it.
            raw.setValue(QStringLiteral("speed/items/deadbeef"),
                         QStringLiteral("{\"rate\":1.5,\"updatedAt\":1}"));
            raw.sync();
        }

        // 16a. buildSettingsJson (outbound): every device-local key is ABSENT; siblings + plain + per-item PRESENT.
        const QJsonObject b = QJsonDocument::fromJson(CloudSync::buildSettingsJson()).object();
        for (const char* ex : {"roms/folder", "emulators/root", "emulators/fullscreen", "player/externalPath",
                               "player/external", "netplay/relay", "display/mode", "display/tvPromptDone",
                               "profiles/current", "emu/virtualPadOpacity", "sync/files/abc/audio",
                               "audio/device", "audio/passthrough", "audio/exclusive",  // #69: audio out is per-device
                               "launchhooks/items/deadbeef",           // #64: hooks are device-local, never in the bundle
                               "device/id", "downloads/foo", "pcgames/bar",
                               "pcscan/steam"})                        // #62: persisted installed-scan is device-local
            CHECK(!b.contains(QLatin1String(ex)));                    // device-local carved out of the bundle
        CHECK(b.contains(QStringLiteral("profiles/list")));          // sibling still syncs
        CHECK(b.contains(QStringLiteral("sync/global/audio")));      // sync/global/* still syncs
        CHECK(b.contains(QStringLiteral("library/showHidden")));     // library/showHidden still syncs
        // Local video library folder is device-local (each machine points at its own disk); the
        // library/showHidden sibling is a user preference and DOES sync (leaf-exact match, not group).
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("library/folder")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("library/showHidden")) == false);
        // Device performance profile (#119): the detected hardware identity ("device/profile") and the manual
        // override ("device/profileOverride") are DEVICE-LOCAL — hardware is per-machine and a Deck's tuned
        // defaults would crush a weaker peer. They ride the EXISTING "device/" prefix (no new carve-out was
        // added); pinned here so a refactor of that prefix cannot silently start syncing hardware identity, and
        // so they are NOT double-counted as a per-item store.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("device/profile")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("device/profileOverride")) == true);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("device/profile")) == false);
        // Per-game launch HOOKS (issue #64) are device-local; per-game launch OVERRIDES (issue #51) are NOT —
        // they sync as a per-item store. This pair is the crux of #64: a hook is a command line that runs, so a
        // synced one would execute on a machine where the path/tool may not exist. The contrast is asserted both
        // ways so a mistaken filing (hooks into isPerItemStoreKey, or the prefix dropped) turns one of them red.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("launchhooks/items/deadbeef")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("launchopts/items/deadbeef"))  == false);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("launchhooks/items/deadbeef")) == false);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("launchopts/items/deadbeef"))  == true);
        // Per-item playback speed (issue #140) is PER-ITEM-SYNCED, NOT device-local — the inverse of #64/#75/#103's
        // device-local carve-outs. A narrator's ideal speed belongs to the content and should follow the user
        // across devices, exactly like resume/launchopts. Asserted both ways so a mis-filing turns one red.
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("speed/items/deadbeef")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("speed/items/deadbeef"))  == false);
        // Per-game pad2key profiles (issue #105) are PER-ITEM-SYNCED, NOT device-local — a game preference the
        // CloudMerge document owns; it must be excluded from the heavy bundle or it double-syncs. Asserted both ways.
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("pad2key/items/deadbeef")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("pad2key/items/deadbeef"))  == false);
        // Per-book bookmarks (issue #136) are PER-ITEM-SYNCED, NOT device-local — a reading POSITION the issue
        // wants to survive switching devices, exactly like resume/speed. Asserted both ways so a mis-filing
        // (dropped from the per-item set, or leaking into the device-local table) turns one of them red.
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("bookmarks/default/items")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("bookmarks/default/items"))  == false);
        // Per-item audio bookmarks (issue #140) are PER-ITEM-SYNCED, NOT device-local — a bookmarked POSITION the
        // issue wants to survive switching devices, exactly like #136's reading bookmarks. Asserted both ways so a
        // mis-filing (dropped from the per-item set, or leaking into device-local) turns one red. The SECOND check
        // is also the tripwire that the "audiobookmarks/" prefix is NOT swallowed by the device-local "audio/"
        // prefix — a key that only differs after "audio" — so a refactor of either table cannot break it silently.
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("audiobookmarks/default/items")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("audiobookmarks/default/items"))  == false);
        // iptv/* (issue #75): saved Live-TV sources are DEVICE-LOCAL — the playlist URL can embed provider
        // credentials, so it must never ride the heavy settings bundle. Asserted both ways.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("iptv/profileA")) == true);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("iptv/profileA")) == false);
        // opds/* (issue #146): saved OPDS book catalogs are DEVICE-LOCAL — each holds a self-hosted server URL
        // plus optional HTTP basic-auth credentials, so it must never ride the heavy settings bundle. Both ways.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("opds/profileA")) == true);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("opds/profileA")) == false);
        // subsonic/* (issue #193): saved Subsonic music servers. Same carve-out, one notch more serious —
        // a Subsonic server authenticates EVERY request, so the stored password is not optional the way an
        // OPDS catalog's is, and a synced bundle is a zip in somebody's Drive folder.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("subsonic/profileA/servers")) == true);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("subsonic/profileA/servers")) == false);
        // scrobble/* and scrobblestate/* (issue #192): music scrobbling, BOTH key families, and the first one
        // is the reason the carve-out exists at all — scrobble/<profile>/lb/token is the user's ListenBrainz
        // credential, and a synced bundle is a zip on somebody's Drive. The state family is this DEVICE's
        // accumulator (the delivered counter and the undelivered listens); merged across devices it would
        // report a number neither of them scrobbled and would submit the same listens twice.
        //
        // Asserted through the pure layer's OWN key builders rather than through hand-typed literals, so a
        // rename of the prefix cannot leave this gate green while the writer moves out from under it.
        CHECK(CloudSync::isDeviceLocalKey(Scrobble::settingsKeyPrefix()
                                          + QStringLiteral("default/lb/token")) == true);
        CHECK(CloudSync::isDeviceLocalKey(Scrobble::settingsKeyPrefix()
                                          + QStringLiteral("default/enabled")) == true);
        CHECK(CloudSync::isDeviceLocalKey(ScrobbleQueue::queueKey(QStringLiteral("default"),
                                                                  QStringLiteral("listenbrainz"))) == true);
        CHECK(CloudSync::isDeviceLocalKey(ScrobbleQueue::counterKey(QStringLiteral("default"),
                                                                    QStringLiteral("listenbrainz"))) == true);
        // ...and NOT in the per-item set: the progress merge document must not claim these, or it would carry
        // the token into the one file that IS merged between devices.
        CHECK(CloudSync::isPerItemStoreKey(Scrobble::settingsKeyPrefix()
                                           + QStringLiteral("default/lb/token")) == false);
        // A NEIGHBOUR that must stay synced, so the prefix cannot have been widened into a whole-tree sweep.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("scrobbling/somethingElse")) == false);

        // pcscan/* (issue #62): the persisted per-launcher installed-scan is DEVICE-LOCAL (a snapshot of what
        // this machine has installed, and it churns every refresh) — never in the per-item set, never synced.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("pcscan/steam")) == true);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("pcscan/steam")) == false);
        // emugfx* (issue #103): per-game standalone-emulator graphics are DEVICE-LOCAL (hardware-dependent) —
        // both key spellings the store uses must be carved out, and never in the per-item set.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("emugfx/items/deadbeef")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("\x01") + QStringLiteral("emugfx-system:ps2")) == true);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("emugfx/items/deadbeef")) == false);
        // shaderpreset* (issue #99): per-game/per-system slang-shader preset is DEVICE-LOCAL for the same reason
        // as emugfx (a shader's cost is hardware-dependent) — both key spellings the store uses must be carved
        // out, and it must never be in the per-item (synced) set. Asserted both ways so a mistaken filing (into
        // isPerItemStoreKey, or the carve-out dropped) turns one of these red.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("shaderpreset/items/deadbeef")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("\x01") + QStringLiteral("shaderpreset-system:ps2")) == true);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("shaderpreset/items/deadbeef")) == false);
        // emulators/<systemId> (Unified Emulation Picker Task 2, Settings::emulatorFor): the per-system standalone
        // emulator DEFAULT is a user PREFERENCE — a property of the account, not the machine — so it rides the
        // synced settings bundle exactly like cores/<id> and backends/<id> (neither device-local NOR per-item).
        // This is a TRIPWIRE: the device-local table carries "emulators/root" + "emulators/fullscreen" as EXACT
        // leaves, so a future refactor that turned those into an "emulators/" PREFIX would silently stop syncing
        // every per-system emulator choice. Asserting cores/backends the same way pins the "rides the bundle"
        // classification for the whole trio; the two exact siblings confirm the leaf-not-prefix match holds.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("emulators/gc")) == false);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("emulators/gc")) == false);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("cores/gc")) == false);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("cores/gc")) == false);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("backends/gc")) == false);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("backends/gc")) == false);
        // The two EXACT device-local siblings stay device-local (leaf match, not an "emulators/" prefix).
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("emulators/root")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("emulators/fullscreen")) == true);
        CHECK(b.value(QStringLiteral("display/theme")).toString() == QStringLiteral("dark"));
        CHECK(!b.contains(QStringLiteral("stats/pX/") + localDev + QStringLiteral("/cat/video/seconds"))); // per-item now CARVED OUT of the bundle (mdsync T5 cadence fix)
        for (const char* pi : {"resume/", "recent/", "marks/", "favorites/", "playlists/", "stats/", "playstats/",
                               "deleted/", "metaoverrides/", "missed/", "filterpresets/", "speed/", "lyricoffset/"})
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
        // A dismissal in the HEAVY bundle must not land either, and here the hands-off is not just tidiness:
        // the bundle OVERWRITES, and this store's whole correctness argument is that the only write is a
        // max. A bundle carrying an older stamp would silently un-dismiss a show. (The stamp below is older
        // than the local one, which is exactly the case the merge document would reject and this path would
        // not.) Issue #25.
        peer[QStringLiteral("missed/pX/shows/deadbeef")] = QStringLiteral("1");
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
            // …and the dismissal likewise: the peer's older stamp did NOT land, so nothing this user waved
            // away has come back.
            CHECK(raw.value(QStringLiteral("missed/pX/shows/deadbeef")).toString()
                  == QStringLiteral("1700000000"));
            CHECK(raw.value(QStringLiteral("display/theme")).toString() == QStringLiteral("light"));   // plain synced updated
            CHECK(raw.value(QStringLiteral("some/newKey")).toString() == QStringLiteral("hello"));     // plain synced added
        }

        // 16b-trakt. The #23 read slice, in BOTH directions. The caches are cheap to re-fetch and would
        // flip the bundle's stateHash on every refresh; the IMPORT CURSOR is the one that would do real
        // harm, and it is the reason this block exists. Synced, one device's completed import suppresses
        // another device's FIRST one — that device reports "0 newly marked watched" and can only be
        // repaired by unlinking Trakt. The same failure across profiles is what made the cursor
        // per-profile in the first place, so the exclusion must survive the namespacing: it is asserted
        // through the SAME key builder the writer uses, never a literal, because a literal here would
        // stay green while the writer moved.
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(QStringLiteral("trakt/watchlistCache"), QStringLiteral("[]"));
            raw.setValue(QStringLiteral("trakt/watchlistCachedAt"), QStringLiteral("1700000000"));
            raw.setValue(QStringLiteral("trakt/collectionCachedAt"), QStringLiteral("1700000001"));
            raw.setValue(trakt::backfillThroughKey(QStringLiteral("pX")), QStringLiteral("1700000002"));
            raw.setValue(trakt::backfillDoneKey(QStringLiteral("pX")), QStringLiteral("true"));
            raw.setValue(QStringLiteral("trakt/clientId"), QStringLiteral("typed-by-the-user"));
            raw.sync();
        }
        for (const QString& k : { QStringLiteral("trakt/watchlistCache"),
                                  QStringLiteral("trakt/collectionCache"),
                                  QStringLiteral("trakt/watchlistCachedAt"),
                                  QStringLiteral("trakt/collectionCachedAt"),
                                  // the flat keys an earlier build of the branch wrote
                                  QStringLiteral("trakt/listsCachedAt"),
                                  QStringLiteral("trakt/backfillThrough"),
                                  QStringLiteral("trakt/backfillDone"),
                                  // ...and the live, per-profile ones, for three different profiles
                                  trakt::backfillThroughKey(QString()),
                                  trakt::backfillDoneKey(QString()),
                                  trakt::backfillThroughKey(QStringLiteral("pX")),
                                  trakt::backfillDoneKey(QStringLiteral("pX")),
                                  trakt::backfillThroughKey(QStringLiteral("pY")) })
            CHECK(CloudSync::isDeviceLocalKey(k) == true);
        // The sibling half, exactly as in probe_settingstxn §1b: the credentials the user TYPES are set
        // up once and are meant to reach every device. A "trakt/" prefix would take them too.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("trakt/clientId")) == false);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("trakt/clientSecret")) == false);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("trakt/backfillx")) == false);
        // RA login (#94): the session token + account name are DEVICE-LOCAL (per-device auth, never sync a
        // credential), but ra/hardcore is a preference that SYNCS. Exact leaves, not an "ra/" prefix.
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("ra/token")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("ra/user"))  == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("ra/hardcore")) == false);
        {
            // OUTBOUND: none of it is in the bundle, including the profile-scoped cursor just written.
            const QJsonObject bt = QJsonDocument::fromJson(CloudSync::buildSettingsJson()).object();
            CHECK(!bt.contains(QStringLiteral("trakt/watchlistCache")));
            CHECK(!bt.contains(QStringLiteral("trakt/watchlistCachedAt")));
            CHECK(!bt.contains(QStringLiteral("trakt/collectionCachedAt")));
            CHECK(!bt.contains(trakt::backfillThroughKey(QStringLiteral("pX"))));
            CHECK(!bt.contains(trakt::backfillDoneKey(QStringLiteral("pX"))));
            CHECK(bt.contains(QStringLiteral("trakt/clientId")));      // the sibling still travels
            // INBOUND: a peer's cursor cannot land on this device — the assertion that actually stops
            // one device's finished import from silencing another's first.
            QJsonObject peerT;
            peerT[trakt::backfillThroughKey(QStringLiteral("pX"))] = QStringLiteral("9999999999");
            peerT[QStringLiteral("trakt/watchlistCachedAt")]       = QStringLiteral("9999999999");
            CloudSync::applySettingsJson(QJsonDocument(peerT).toJson(QJsonDocument::Compact));
            QSettings raw(iniPath, QSettings::IniFormat); raw.sync();
            CHECK(raw.value(trakt::backfillThroughKey(QStringLiteral("pX"))).toString()
                  == QStringLiteral("1700000002"));                     // OUR cursor, untouched
            CHECK(raw.value(QStringLiteral("trakt/watchlistCachedAt")).toString()
                  == QStringLiteral("1700000000"));
        }

        // 16b-cal. #148: a CALENDAR cache write cannot move the sync fingerprint.
        //
        // Asserted through stateFingerprint(), deliberately NOT through "the key is absent from
        // buildSettingsJson()". An absence assertion is satisfied by an EMPTY bundle — break
        // buildSettingsJson outright and every "!contains" above it still passes — and absence was never
        // the property that cost anything here. What cost roughly 48 whole-zip uploads a day was the
        // fingerprint MOVING: checkStatus reads it as st.localChanged, and since #34 localChanged is a
        // debt the retry machinery keeps trying to pay, so a 30-minute cache stamp kept re-presenting
        // itself as an owed settings change. The property is therefore: write the cache, and the number
        // the push gate consults is bit-identical.
        //
        // The payload and the stamp are moved in SEPARATE steps, both compared against the SAME baseline.
        // Moving them together would make one assertion out of two: either half's exclusion deleted would
        // fail the combined check, so neither key would be pinned on its own, and an exclusion covering
        // only trakt/calendarCache would sail through while the clock — the half that actually ticks every
        // 30 minutes on an untouched machine — went on flipping the fingerprint.
        {
            {
                QSettings raw(iniPath, QSettings::IniFormat);
                raw.remove(QStringLiteral("trakt"));                                   // 16b-trakt's fixture is done with
                raw.setValue(QStringLiteral("display/theme"), QStringLiteral("dark")); // a genuinely bundle-synced key
                raw.setValue(QStringLiteral("trakt/calendarCache"), QStringLiteral("[{\"ep\":1}]"));
                raw.setValue(QStringLiteral("trakt/calendarCachedAt"), QStringLiteral("1700000000"));
                raw.sync();
            }
            const QByteArray fpCal = CloudSync::stateFingerprint();

            // (a) the PAYLOAD alone: a refresh that brought back a different episode list.
            {
                QSettings raw(iniPath, QSettings::IniFormat);
                raw.setValue(QStringLiteral("trakt/calendarCache"), QStringLiteral("[{\"ep\":2}]"));
                raw.sync();
            }
            CHECK(CloudSync::stateFingerprint() == fpCal);

            // (b) the STAMP alone, against the same baseline: the every-30-minutes case where the episode
            // list came back byte-identical and only the wall clock moved. This is the one the issue is
            // actually about — on an idle machine (a) never happens and (b) happens 48 times a day.
            {
                QSettings raw(iniPath, QSettings::IniFormat);
                raw.setValue(QStringLiteral("trakt/calendarCachedAt"), QStringLiteral("1700001800"));
                raw.sync();
            }
            CHECK(CloudSync::stateFingerprint() == fpCal);

            // (c) the POSITIVE CONTROL, and the reason (a) and (b) are not vacuous: the SAME fingerprint,
            // on the SAME ini, still moves for a real synced setting. Without this, a build whose
            // fingerprint was constant — isDeviceLocalKey returning true unconditionally, stateHash
            // returning early, an empty store — would pass everything above and report the churn fixed.
            {
                QSettings raw(iniPath, QSettings::IniFormat);
                raw.setValue(QStringLiteral("display/theme"), QStringLiteral("light"));
                raw.sync();
            }
            CHECK(CloudSync::stateFingerprint() != fpCal);

            // The predicate itself, both leaves. Not a restatement of (a)/(b): those are satisfied by an
            // exclusion in EITHER table, and isPerItemStoreKey is the wrong one — that table is a claim
            // that the merge document owns the key and will carry it between devices, which is false of a
            // cache no peer can use. These pin that the fix landed in the device-local table.
            CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("trakt/calendarCache")) == true);
            CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("trakt/calendarCachedAt")) == true);
            // NOT repeated here: the trakt/clientId and trakt/clientSecret sibling checks. §16b-trakt above
            // already runs them, isDeviceLocalKey is a pure function of the key string, so a copy in this
            // block would assert the identical thing against the identical input — killed by every mutation
            // that kills the original and by nothing else. Nor is there a calendar-specific sibling to
            // guard: unlike "trakt/", nothing a user types lives under "trakt/calendar".

            // INBOUND: a peer's calendar was fetched against ITS Trakt account and is worth nothing here;
            // landing it would also overwrite a fresher local fetch with an older one. Separate from the
            // fingerprint assertions — applySettingsJson consults the table independently, and a build that
            // stopped consulting it there keeps (a)-(c) green.
            {
                QJsonObject peerC;
                peerC[QStringLiteral("trakt/calendarCache")]    = QStringLiteral("[{\"peer\":1}]");
                peerC[QStringLiteral("trakt/calendarCachedAt")] = QStringLiteral("9999999999");
                CloudSync::applySettingsJson(QJsonDocument(peerC).toJson(QJsonDocument::Compact));
                QSettings raw(iniPath, QSettings::IniFormat); raw.sync();
                CHECK(raw.value(QStringLiteral("trakt/calendarCache")).toString()
                      == QStringLiteral("[{\"ep\":2}]"));            // OUR payload, untouched
                CHECK(raw.value(QStringLiteral("trakt/calendarCachedAt")).toString()
                      == QStringLiteral("1700001800"));              // OUR stamp, untouched
            }
            // No state is put back: (c) left display/theme on "light", which is exactly where 16b's
            // inbound case had left it, and §16's cleanup removes the whole "trakt" group either way.
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
                                  "trakt", "metaoverrides", "speed", "lyricoffset", "missed"})
                raw.remove(QLatin1String(g));
            raw.sync();
            MetaOverrides::invalidate();
            MissedDismiss::invalidate();
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

    // ---- 24b. Metadata overrides (issue #24): newest-wins, equal-ts convergence, and reset-is-not-a-deletion --
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

    // ---- 20j. Per-game backend override (RetroPark Slice 2a) survives the merge document --------------------
    //
    // serializeLaunchOpts/mergeLaunchOpts carry the launchopts blob OPAQUELY (whole JSON object per hash,
    // newest-updatedAt wins), so the `backend` field added in Slice 2a rides the merge with no field-specific
    // code. This pins that: a device holding only the peer's document ends up with the backend the peer set.
    // Without it, a future refactor that decomposed the blob into named fields could silently drop `backend`.
    {
        const QString bkey = QStringLiteral("romlib:C:/roms/gc/RetroParkGame.iso");
        wipeStores(); LaunchOpts::invalidate();
        LaunchOpts::Override bov; bov.backend = QStringLiteral("retropark");
        LaunchOpts::set(bkey, bov);                                        // device A pins RetroPark
        const QJsonObject docA = serializeNow();
        CHECK(docA.value(QStringLiteral("launchopts")).toObject().size() == 1); // it IS in the merge document

        wipeStores(); LaunchOpts::invalidate();                            // device B starts empty
        CHECK(LaunchOpts::get(bkey).backend.isEmpty());
        mergeDoc(docA);                                                    // mergeAll() invalidates the cache at its tail
        CHECK(LaunchOpts::get(bkey).backend == QStringLiteral("retropark")); // the backend field survived the merge

        // Adding a field changes no classification: launchopts/ stays per-item-synced, never device-local.
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("launchopts/items/deadbeef")) == true);
        CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("launchopts/items/deadbeef"))  == false);

        wipeStores(); LaunchOpts::invalidate();
    }

    // ---- 24c. Per-item playback speed (issue #140): rides the document, newest-updatedAt wins both orders ---
    //
    // The store is GLOBAL (no profile level, like resume/metaoverrides) because a narrator's ideal speed is a
    // property of the content, not the viewer. The merge is byte-for-byte the metaoverrides shape — newest
    // updatedAt wins per hash, no tombstones — so the interesting properties are the ones any per-item store
    // must have: it rides the document at all, newest wins each direction, and a one-device item is imported.
    {
        const QString k24 = QStringLiteral("audiobook:Mistborn");
        const QString h24 = md5(k24).left(10);                 // the 10-hex leaf SpeedStore uses (independent oracle)
        const QString ikey = QStringLiteral("speed/items/") + h24;
        auto injSpeed = [&](double rate, qint64 ts) {
            QJsonObject o; o[QStringLiteral("rate")] = rate; o[QStringLiteral("updatedAt")] = double(ts);
            setRaw(ikey, compactO(o));
        };
        auto speedRate = [&]() -> double {
            QSettings raw(iniPath, QSettings::IniFormat);
            return QJsonDocument::fromJson(raw.value(ikey).toString().toUtf8())
                .object().value(QStringLiteral("rate")).toDouble();
        };

        // 24c-a. It rides the document under its own top-level key and its own hash.
        wipeStores(); injSpeed(1.5, T);
        const QJsonObject d24 = serializeNow();
        CHECK(d24.contains(QStringLiteral("speed")));
        CHECK(d24.value(QStringLiteral("speed")).toObject().contains(h24));
        CHECK(near(d24.value(QStringLiteral("speed")).toObject().value(h24).toObject()
                       .value(QStringLiteral("rate")).toDouble(), 1.5));

        // 24c-b. Newest updatedAt wins, each direction.
        wipeStores(); injSpeed(1.75, T);      const QJsonObject sNewer = serializeNow();
        wipeStores(); injSpeed(1.25, T - 500); mergeDoc(sNewer);
        CHECK(near(speedRate(), 1.75));                        // remote newer replaces
        wipeStores(); injSpeed(1.25, T - 500); const QJsonObject sOlder = serializeNow();
        wipeStores(); injSpeed(1.75, T);       mergeDoc(sOlder);
        CHECK(near(speedRate(), 1.75));                        // local newer survives

        // 24c-c. A book only ONE device knows about is imported, not dropped.
        wipeStores(); injSpeed(2.0, T); const QJsonObject sTheirs = serializeNow();
        wipeStores(); mergeDoc(sTheirs);
        CHECK(near(speedRate(), 2.0));

        wipeStores();
    }

    // ---- 24c2. Per-item lyric offset (issue #142): rides the document, newest-updatedAt wins both orders ---
    //
    // The twin of 24c, and deliberately so: how far out a track's .lrc file runs is a property of the FILE
    // that came with the content, not of the viewer or the machine, so LyricOffsetStore is GLOBAL (no profile
    // level) with the metaoverrides/speed shape — newest updatedAt wins per hash, no tombstones. The one thing
    // worth stating separately is why there is no tombstone: CLEARING a nudge writes 0.0 rather than deleting
    // the row, so "back to no offset" propagates as an ordinary newer record. That is asserted below, because
    // a merge that treated 0.0 as "nothing to carry" would silently restore a nudge the user just cleared.
    {
        const QString k24l = QStringLiteral("music:Kiss Them For Me.flac");
        const QString h24l = md5(k24l).left(10);               // the 10-hex leaf LyricOffsetStore uses (independent oracle)
        const QString lkey = QStringLiteral("lyricoffset/items/") + h24l;
        auto injOffset = [&](double off, qint64 ts) {
            QJsonObject o; o[QStringLiteral("offset")] = off; o[QStringLiteral("updatedAt")] = double(ts);
            setRaw(lkey, compactO(o));
        };
        auto offsetVal = [&]() -> double {
            QSettings raw(iniPath, QSettings::IniFormat);
            return QJsonDocument::fromJson(raw.value(lkey).toString().toUtf8())
                .object().value(QStringLiteral("offset")).toDouble();
        };

        // 24c2-a. It rides the document under its own top-level key and its own hash.
        wipeStores(); injOffset(-0.5, T);
        const QJsonObject d24l = serializeNow();
        CHECK(d24l.contains(QStringLiteral("lyricoffset")));
        CHECK(d24l.value(QStringLiteral("lyricoffset")).toObject().contains(h24l));
        CHECK(near(d24l.value(QStringLiteral("lyricoffset")).toObject().value(h24l).toObject()
                       .value(QStringLiteral("offset")).toDouble(), -0.5));

        // 24c2-b. Newest updatedAt wins, each direction.
        wipeStores(); injOffset(1.5, T);       const QJsonObject lNewer = serializeNow();
        wipeStores(); injOffset(-1.0, T - 500); mergeDoc(lNewer);
        CHECK(near(offsetVal(), 1.5));                         // remote newer replaces
        wipeStores(); injOffset(-1.0, T - 500); const QJsonObject lOlder = serializeNow();
        wipeStores(); injOffset(1.5, T);        mergeDoc(lOlder);
        CHECK(near(offsetVal(), 1.5));                         // local newer survives

        // 24c2-c. A track only ONE device knows about is imported, not dropped.
        wipeStores(); injOffset(2.5, T); const QJsonObject lTheirs = serializeNow();
        wipeStores(); mergeDoc(lTheirs);
        CHECK(near(offsetVal(), 2.5));

        // 24c2-d. A CLEAR is a newer 0.0, and it must travel. This is the assertion the no-tombstone shape
        // rests on: a peer that still holds the old nudge is beaten by the newer zero, so clearing a nudge on
        // one device does not come back on the next merge.
        wipeStores(); injOffset(0.0, T); const QJsonObject lCleared = serializeNow();
        CHECK(lCleared.value(QStringLiteral("lyricoffset")).toObject().contains(h24l));
        wipeStores(); injOffset(1.0, T - 500); mergeDoc(lCleared);
        CHECK(near(offsetVal(), 0.0));

        // 24c2-e. Per-item-synced, NOT device-local — the same classification speed carries, for the same
        // reason (the value belongs to the content and should follow the user).
        CHECK(CloudSync::isPerItemStoreKey(lkey) == true);
        CHECK(CloudSync::isDeviceLocalKey(lkey)  == false);

        wipeStores();
    }

    // ---- 24d. Per-book bookmarks (issue #136): rides the document, newest-ts wins, delete tombstone holds ----
    //
    // The store is PER-PROFILE with the favourites {items, tombs} shape — union by a STABLE id keeping newest
    // ts, a tombstone at-or-after an item's ts suppressing it. A bookmark's id is its book+position, so the id
    // is opaque to the merge (it only unions/compares by it). Injected RAW (independent of BookmarkStore) so the
    // fixtures are not fixed points of the store. What this pins: it rides the document at all, newest wins each
    // direction, a one-device bookmark is imported, and — THE RAIL — a delete tombstone is not resurrected by a
    // peer's older copy while a strictly-newer re-add beats an older tombstone.
    {
        const QString p = QStringLiteral("bm24");
        const QString bmk = QStringLiteral("bookmarks/") + p + QStringLiteral("/items");
        // A bookmark row carries an opaque anchor blob — this section is about identity + time, so the anchor is
        // a constant. id/ts are the merge's inputs.
        auto injBm = [&](const QString& id, qint64 ts) {
            QJsonArray a; QJsonObject o;
            o[QStringLiteral("id")] = id; o[QStringLiteral("bookKey")] = QStringLiteral("/lib/B.epub");
            o[QStringLiteral("anchor")] = QJsonObject{ {QStringLiteral("kind"), 0}, {QStringLiteral("spine"), 1},
                                                       {QStringLiteral("offset"), 40} };
            o[QStringLiteral("label")] = QStringLiteral("mark"); o[QStringLiteral("ts")] = double(ts);
            a.append(o);
            setRaw(bmk, compact(a));
        };
        auto bmIds = [&]() {
            QSettings raw(iniPath, QSettings::IniFormat); QStringList out;
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(bmk).toString().toUtf8()).array())
                out << v.toObject().value(QStringLiteral("id")).toString();
            out.sort(); return out;
        };
        auto bmTs = [&](const QString& id) -> qint64 {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(bmk).toString().toUtf8()).array())
            { const QJsonObject o = v.toObject(); if (o.value(QStringLiteral("id")).toString() == id) return qint64(o.value(QStringLiteral("ts")).toDouble()); }
            return -1;
        };
        const QString ID = QStringLiteral("b0001");

        // 24d-a. It rides the document under "bookmarks", per profile, and the item survives a round-trip.
        wipeStores(); injBm(ID, T - 100); const QJsonObject d = serializeNow();
        CHECK(d.contains(QStringLiteral("bookmarks")));
        CHECK(d.value(QStringLiteral("bookmarks")).toObject().contains(p));

        // 24d-b. Newest ts wins, each direction.
        wipeStores(); injBm(ID, T - 100); const QJsonObject bNewer = serializeNow();
        wipeStores(); injBm(ID, T - 500); mergeDoc(bNewer);
        CHECK(bmTs(ID) == T - 100);                                    // remote newer replaces
        wipeStores(); injBm(ID, T - 500); const QJsonObject bOlder = serializeNow();
        wipeStores(); injBm(ID, T - 100); mergeDoc(bOlder);
        CHECK(bmTs(ID) == T - 100);                                    // local newer survives

        // 24d-c. A bookmark only ONE device knows about is imported, not dropped.
        wipeStores(); injBm(ID, T - 100); const QJsonObject bTheirs = serializeNow();
        wipeStores(); mergeDoc(bTheirs);
        CHECK(bmIds() == (QStringList{ID}));

        // 24d-d. THE RAIL. A delete tombstone at-or-after the item's ts keeps it deleted when a peer's stale
        // copy is merged back (no resurrection); a strictly-newer re-add beats an older tombstone.
        wipeStores(); injBm(ID, T - 100); const QJsonObject bStale = serializeNow(); // the peer still has it, no tomb
        wipeStores(); injTomb(QStringLiteral("bookmarks/") + p, ID, T - 50); mergeDoc(bStale);
        CHECK(bmIds().isEmpty());                                      // the tombstone (newer) suppresses the stale copy
        wipeStores(); injTomb(QStringLiteral("bookmarks/") + p, ID, T - 500); injBm(ID, T - 100); mergeDoc(bStale);
        CHECK(bmIds() == (QStringList{ID}));                          // a newer re-add beats the older tombstone

        wipeStores();
    }

    // ---- 24e. Per-item audio bookmarks (issue #140): rides the document, newest-ts wins, delete tombstone holds -
    //
    // Byte-for-byte the reading-bookmarks section above, for the audio TIME-anchor store: PER-PROFILE with the
    // favourites {items, tombs} shape — union by a STABLE id keeping newest ts, a tombstone at-or-after an item's
    // ts suppressing it. A bookmark's id is its item+whole-second, so it is opaque to the merge (it only unions/
    // compares by it). Injected RAW (independent of AudioBookmarkStore) so the fixtures are not fixed points of
    // the store. What this pins: it rides the document under its OWN root key, newest wins each direction, a
    // one-device bookmark is imported, and — THE RAIL — a delete tombstone is not resurrected by a peer's older
    // copy while a strictly-newer re-add beats an older tombstone.
    {
        const QString p = QStringLiteral("abm24");
        const QString abmk = QStringLiteral("audiobookmarks/") + p + QStringLiteral("/items");
        auto injAbm = [&](const QString& id, qint64 ts) {
            QJsonArray a; QJsonObject o;
            o[QStringLiteral("id")] = id; o[QStringLiteral("itemKey")] = QStringLiteral("audiobook:X");
            o[QStringLiteral("posSec")] = 305.5; o[QStringLiteral("label")] = QStringLiteral("mark");
            o[QStringLiteral("ts")] = double(ts);
            a.append(o);
            setRaw(abmk, compact(a));
        };
        auto abmIds = [&]() {
            QSettings raw(iniPath, QSettings::IniFormat); QStringList out;
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(abmk).toString().toUtf8()).array())
                out << v.toObject().value(QStringLiteral("id")).toString();
            out.sort(); return out;
        };
        auto abmTs = [&](const QString& id) -> qint64 {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const QJsonValue& v : QJsonDocument::fromJson(raw.value(abmk).toString().toUtf8()).array())
            { const QJsonObject o = v.toObject(); if (o.value(QStringLiteral("id")).toString() == id) return qint64(o.value(QStringLiteral("ts")).toDouble()); }
            return -1;
        };
        const QString ID = QStringLiteral("a0001");

        // 24e-a. It rides the document under "audiobookmarks", per profile — a DISTINCT root key from #136's
        // "bookmarks" (so the two stores never share a section).
        wipeStores(); injAbm(ID, T - 100); const QJsonObject d = serializeNow();
        CHECK(d.contains(QStringLiteral("audiobookmarks")));
        CHECK(d.value(QStringLiteral("audiobookmarks")).toObject().contains(p));
        CHECK(!d.value(QStringLiteral("bookmarks")).toObject().contains(p));   // not mixed into reading bookmarks

        // 24e-b. Newest ts wins, each direction.
        wipeStores(); injAbm(ID, T - 100); const QJsonObject aNewer = serializeNow();
        wipeStores(); injAbm(ID, T - 500); mergeDoc(aNewer);
        CHECK(abmTs(ID) == T - 100);                                   // remote newer replaces
        wipeStores(); injAbm(ID, T - 500); const QJsonObject aOlder = serializeNow();
        wipeStores(); injAbm(ID, T - 100); mergeDoc(aOlder);
        CHECK(abmTs(ID) == T - 100);                                   // local newer survives

        // 24e-c. A bookmark only ONE device knows about is imported, not dropped.
        wipeStores(); injAbm(ID, T - 100); const QJsonObject aTheirs = serializeNow();
        wipeStores(); mergeDoc(aTheirs);
        CHECK(abmIds() == (QStringList{ID}));

        // 24e-d. THE RAIL. A delete tombstone at-or-after the item's ts keeps it deleted when a peer's stale copy
        // is merged back (no resurrection); a strictly-newer re-add beats an older tombstone.
        wipeStores(); injAbm(ID, T - 100); const QJsonObject aStale = serializeNow(); // the peer still has it, no tomb
        wipeStores(); injTomb(QStringLiteral("audiobookmarks/") + p, ID, T - 50); mergeDoc(aStale);
        CHECK(abmIds().isEmpty());                                     // the tombstone (newer) suppresses the stale copy
        wipeStores(); injTomb(QStringLiteral("audiobookmarks/") + p, ID, T - 500); injAbm(ID, T - 100); mergeDoc(aStale);
        CHECK(abmIds() == (QStringList{ID}));                         // a newer re-add beats the older tombstone

        wipeStores();
    }

    // ---- 25. Marks: a CLEAR is a husk, so a peer's stale copy cannot resurrect it (issue #132) -------------
    //
    // Section 10 pins that the marks pass never DELETES a local row; this pins the other half — that clearing
    // an item does not delete it either. ItemMarks used to remove an all-default blob, which made "the user
    // cleared this" and "this device has never seen that item" the same fact on disk. mergeMarks cannot tell
    // those apart (it is handed two records or one), so a peer still holding the marks won every time and the
    // clear came back, on both devices. saveItem now leaves a stamped husk instead.
    //
    // EVERY assertion below reads through ItemMarks — "is this still marked?" — and never through the row's
    // presence. That is deliberate and it is the trap this family sets: an assertion that the row is ABSENT
    // after a clear passes on the broken build too, because deletion is exactly what the broken build does.
    // The reader's answer is the only thing that separates them. (The one presence check is on the SERIALIZED
    // document, which is about the husk propagating at all, not about how a clear is spelled.)
    {
        useProfile(QStringLiteral("m25"));
        const QString k25 = QStringLiteral("show:tt42");
        const QString h25 = md5(k25);
        // Marks as a PEER would hold them — hidden is the flag whose resurrection the user actually sees.
        auto injMark25 = [&](bool hidden, const QStringList& tags, qint64 upd) {
            QJsonObject o; o[QStringLiteral("hidden")] = hidden;
            o[QStringLiteral("completion")] = QStringLiteral("none");
            QJsonArray t; for (const QString& x : tags) t.append(x);
            o[QStringLiteral("tags")] = t; o[QStringLiteral("updatedAt")] = double(upd);
            setRaw(QStringLiteral("marks/m25/items/") + h25, compactO(o));
            ItemMarks::invalidate();
        };
        auto docHasItem = [&](const QJsonObject& doc) {
            return doc.value(QStringLiteral("marks")).toObject().value(QStringLiteral("m25")).toObject()
                      .value(QStringLiteral("items")).toObject().contains(h25);
        };
        // Mark, then clear, through the STORE — saveItem is what decides how a clear is spelled.
        auto markThenClear = [&]() {
            ItemMarks::setHidden(k25, true);
            ItemMarks::setTags(k25, QStringList{QStringLiteral("seen")});
            ItemMarks::setHidden(k25, false);
            ItemMarks::setTags(k25, QStringList{});
        };

        // 25a. THE ISSUE, end to end. Clear on this device; merge a peer that still holds the old marks.
        wipeStores(); injMark25(true, {QStringLiteral("seen")}, T - 500);
        const QJsonObject peerStale = serializeNow();   // the peer's document, made before the clear
        wipeStores();
        markThenClear();
        CHECK(!ItemMarks::get(k25).hidden);             // cleared here...
        mergeDoc(peerStale);
        CHECK(!ItemMarks::get(k25).hidden);             // ...and the peer's stale copy does NOT bring it back
        CHECK(ItemMarks::get(k25).tags.isEmpty());
        CHECK(!ItemMarks::anyHidden());                 // and no shelf/filter surface thinks otherwise

        // 25b. The clear TRAVELS: a peer pulling this device's husk drops its own copy of the marks.
        wipeStores();
        markThenClear();
        const QJsonObject clearedDoc = serializeNow();
        CHECK(docHasItem(clearedDoc));                  // the husk rides the sync document at all
        wipeStores(); injMark25(true, {QStringLiteral("seen")}, T - 500);
        CHECK(ItemMarks::get(k25).hidden);              // the peer's starting state
        mergeDoc(clearedDoc);
        CHECK(!ItemMarks::get(k25).hidden);             // the clear propagated
        CHECK(!ItemMarks::anyHidden());

        // 25c. …and a clear is not permanent. A genuinely newer re-mark beats the husk, or "clear" would
        // quietly mean "this item can never be hidden again".
        wipeStores(); injMark25(true, {}, T + 100); const QJsonObject reMark = serializeNow();
        wipeStores(); markThenClear();
        mergeDoc(reMark);
        CHECK(ItemMarks::get(k25).hidden);

        // 25d. A husk arriving from a newer device reads as "no marks" on a device that cannot WRITE one: a
        // husk IS an all-default blob, so every reader answers for it by construction, on this build and on
        // every build already shipped — which is what makes a mixed-version fleet converge instead of split.
        // Injected raw, exactly as the merge would land it.
        wipeStores(); injMark25(false, {}, T);
        {
            const ItemMarks::Marks m = ItemMarks::get(k25);
            CHECK(!m.hidden && m.tags.isEmpty());
            CHECK(!ItemMarks::anyHidden());
        }

        // 25e. The other side of the mixed fleet: a peer still on the OLD build DELETES on clear, so its
        // document simply omits the hash. The marks pass never deletes, so this device's husk survives and
        // goes on winning. (Inert unless mergeMarks is made to delete a locally-present, remotely-absent row —
        // which is the change that would silently re-open this issue from the merge side.)
        wipeStores(); injArr(QStringLiteral("marks/m25/tagVocab"), {QStringLiteral("other")});
        const QJsonObject oldBuildDoc = serializeNow();
        CHECK(!docHasItem(oldBuildDoc));                // the old build's clear is an absence
        wipeStores(); markThenClear();
        mergeDoc(oldBuildDoc);
        CHECK(!ItemMarks::get(k25).hidden);
        CHECK(docHasItem(serializeNow()));              // the husk is still here to keep carrying the clear

        // 25f. Equal timestamps: a husk and a mark stamped in the same second converge in BOTH merge orders.
        // The winner is the greater canonical bytes ("true" > "false", so the mark wins), NOT a rule that
        // clear beats mark — convergence is the property, the winner is only the means. Two devices racing
        // inside one second is a race the user already lost; two devices disagreeing forever is a bug.
        wipeStores(); injMark25(true,  {}, T); const QJsonObject tieMark = serializeNow();
        wipeStores(); injMark25(false, {}, T); const QJsonObject tieHusk = serializeNow();
        wipeStores(); injMark25(false, {}, T); mergeDoc(tieMark); const bool tie1 = ItemMarks::get(k25).hidden;
        wipeStores(); injMark25(true,  {}, T); mergeDoc(tieHusk); const bool tie2 = ItemMarks::get(k25).hidden;
        CHECK(tie1 == tie2);
        CHECK(tie1);

        // 25g. The husk is stamped at the time of the CLEAR, never with the stamp of the record it cleared.
        // Adopt a peer's mark by merge, then clear it — the ordinary shape of "sync, then tidy up". A husk
        // that kept the adopted record's timestamp would TIE with the peer's unchanged copy, and 25f is
        // exactly why a tie is not good enough here: the byte tie-break hands it to the mark, and the item
        // un-clears itself on the very next sync with a peer that has done nothing at all.
        wipeStores(); injMark25(true, {}, T - 100); const QJsonObject peerMark = serializeNow();
        wipeStores(); mergeDoc(peerMark);               // this device adopts the peer's mark, stamp and all
        CHECK(ItemMarks::get(k25).hidden);
        ItemMarks::setHidden(k25, false);               // ...and the user clears it
        mergeDoc(peerMark);                             // next sync, same unchanged peer
        CHECK(!ItemMarks::get(k25).hidden);

        wipeStores();
    }

    // ---- 26. resume: a CLEAR is a dated TOMBSTONE, so no stale copy resurrects it (issue #150) -------------
    //
    // The same defect as §25 in a store that must NOT take §25's answer. PlaybackSession::finishResume and
    // HomeView::clearResume both removed the whole resume group, and mergeResume's haveLocal gate reads
    // "pos" — so a cleared row fell through to the wholesale write-back. Worse than the marks case: the cloud
    // document holds THIS device's own pre-finish row, so it self-resurrects with no second device at all.
    //
    // A husk would be wrong here because clearing fires on EVERY finished episode: husks would grow with
    // playback rather than with deliberate user actions and every one would ride the document for ever. So the
    // shape is a tombstone bounded by compact(30) — whose cost, a position a 31-day-dormant peer brings back,
    // is asserted at the end rather than only asserted in prose.
    //
    // EVERY assertion reads through PlaybackSession — "what position would the app resume from?" — and never
    // through the row's presence, and every clear goes through the REAL clear site rather than a remove() the
    // probe spells itself. That is the trap this family sets: after a clear, "the row is absent" is true on the
    // broken build too, because deletion is exactly what the broken build does. Only the answer AFTER a merge
    // separates them. (The two document checks are about the tombstone propagating at all, which is a different
    // question from how a clear is spelled.)
    //
    // §26 and §27 were mutation-tested against 28 mutants of the fix — every one killed. Fifteen assertions
    // across the two sections are killed by NONE of them, and every one of those is a line taken BEFORE a merge:
    // "the position is gone here", "the peer starts out holding it", "the cap dropped it". They are fixture
    // preconditions, and the five of the shape "…and it is gone right after the clear" are the very trap named
    // above — kept, because a section whose setup is not stated is a section nobody can read, but never
    // load-bearing. Nothing else here is inert; if you add an assertion, mutate the line it is about.
    {
        const QString k26 = QStringLiteral("X:/Shows/S01E03.mkv");
        // Re-derived here rather than read from ResumeStore: an assertion that asks the code under test how it
        // spells its own key would keep passing if the spelling and the document ever drifted apart.
        const QString h26 = QString::fromLatin1(
            QCryptographicHash::hash(k26.toUtf8(), QCryptographicHash::Md5).toHex().left(10));

        // THE READER. A fresh session is what the app builds when the file is opened again; the pending seek is
        // the answer the user sees as "it carried on where I left off".
        auto resumeSeek = [&](const QString& key) {
            PlaybackSession s; s.beginResume(key); return s.takeResumeSeek();
        };
        // THE WRITER. The throttled playback funnel, stamped now.
        auto playTo = [&](const QString& key, double pos) {
            PlaybackSession s; s.beginResume(key); s.setDuration(3600.0); s.setPosition(pos); s.persistResume();
        };
        // THE CLEAR SITE under test — what "finished the episode" does.
        auto finish = [&](const QString& key) {
            PlaybackSession s; s.beginResume(key); s.finishResume();
        };
        auto injResume = [&](const QString& hash, double pos, qint64 ts) {
            setRaw(QStringLiteral("resume/") + hash + QStringLiteral("/pos"), QString::number(pos));
            setRaw(QStringLiteral("resume/") + hash + QStringLiteral("/dur"), QStringLiteral("3600"));
            setRaw(QStringLiteral("resume/") + hash + QStringLiteral("/ts"),  QString::number(ts));
        };
        auto docTombs = [&](const QJsonObject& doc) {
            QStringList out;
            for (const QJsonValue& v : doc.value(QStringLiteral("resumeTombs")).toArray())
                out << v.toObject().value(QStringLiteral("key")).toString();
            return out;
        };

        // 26a. THE ISSUE, and it needs only ONE device. Finish an episode, then sync against the copy of this
        // device's own state that the cloud document was already holding.
        wipeStores();
        playTo(k26, 900.0);
        const QJsonObject ownPreFinish = serializeNow();   // what the cloud already has from before the finish
        finish(k26);
        CHECK(resumeSeek(k26) == 0.0);                     // finished here...
        mergeDoc(ownPreFinish);
        CHECK(resumeSeek(k26) == 0.0);                     // ...and its own stale copy does NOT bring it back

        // 26b. The clear TRAVELS: a peer that still holds the position drops it on pulling our document.
        wipeStores();
        playTo(k26, 900.0); finish(k26);
        const QJsonObject clearedDoc = serializeNow();
        CHECK(docTombs(clearedDoc).contains(h26));         // the tombstone rides the sync document at all
        wipeStores(); injResume(h26, 900.0, T - 500);
        CHECK(resumeSeek(k26) == 900.0);                   // the peer's starting state
        mergeDoc(clearedDoc);
        CHECK(resumeSeek(k26) == 0.0);                     // the clear propagated

        // 26c. …and a clear is not permanent. A genuinely newer position beats the tombstone, or "finished"
        // would quietly mean "this file can never be resumed again".
        wipeStores(); injResume(h26, 1200.0, T + 100);
        const QJsonObject reWatched = serializeNow();
        wipeStores(); playTo(k26, 900.0); finish(k26);
        mergeDoc(reWatched);
        CHECK(resumeSeek(k26) == 1200.0);

        // 26d. Mixed fleet, direction ONE: a peer still on the OLD build deletes on finish and serializes no
        // "resumeTombs" key AT ALL, so its document is silent about deletions and still carries the position it
        // never cleared. Our tombstone has to survive that and go on winning — the absent key must read as "no
        // deletions I know of", never as "no deletions exist".
        wipeStores(); injResume(h26, 900.0, T - 500);
        QJsonObject oldBuildDoc = serializeNow();
        oldBuildDoc.remove(QStringLiteral("resumeTombs")); // what a pre-#150 serializer emits
        CHECK(!oldBuildDoc.contains(QStringLiteral("resumeTombs")));
        wipeStores(); playTo(k26, 900.0); finish(k26);
        mergeDoc(oldBuildDoc);
        CHECK(resumeSeek(k26) == 0.0);
        mergeDoc(oldBuildDoc);                             // and it keeps losing, every sync, not just the first
        CHECK(resumeSeek(k26) == 0.0);
        CHECK(docTombs(serializeNow()).contains(h26));      // our tombstone is still here to keep carrying it

        // 26e. Mixed fleet, direction TWO: an OLD BINARY reading what we now write. It cannot act on the
        // tombstone (it has never heard of resumeTombs) — that is stated in the report, not wished away — but
        // it must go on reading the rest of the document exactly as before. So the deletions ride a SEPARATE
        // root key: "resume" is still a flat <hash> -> {pos,dur,ts,title} map, unshaped, which is the one
        // property that cannot be fixed after the fact if it is got wrong.
        wipeStores(); playTo(k26, 900.0);
        {
            const QJsonObject doc = serializeNow();
            const QJsonObject res = doc.value(QStringLiteral("resume")).toObject();
            CHECK(res.contains(h26));
            CHECK(res.value(h26).toObject().value(QStringLiteral("pos")).toDouble() == 900.0);
            CHECK(doc.value(QStringLiteral("resumeTombs")).isArray()); // a sibling of "resume", not inside it
        }

        // 26f. A tombstone is only written where there was a position to clear. Finishing something that never
        // accrued one (opened and closed inside a second, a file resumed to its end on the previous device)
        // records nothing — otherwise deleted/* would grow with every finished file rather than with the
        // clears that actually happened, which is the cost this shape is supposed to bound.
        wipeStores();
        finish(k26);
        CHECK(docTombs(serializeNow()).isEmpty());

        // …but a PARTIAL row is still a record, and clearing it still dates the clear. mergeResume writes a
        // remote entry field by field, so a document entry without "pos" lands exactly this shape; asking only
        // whether "pos" is present would call it "never played" and delete it silently — which is the narrow
        // gate that let this bug through on the merge side, repeated on the write side.
        wipeStores();
        setRaw(QStringLiteral("resume/") + h26 + QStringLiteral("/ts"), QString::number(T - 100));
        setRaw(QStringLiteral("resume/") + h26 + QStringLiteral("/title"), QStringLiteral("Ep 3"));
        finish(k26);
        CHECK(docTombs(serializeNow()).contains(h26));

        // 26g. Re-watching LIFTS the clear. persistResume drops the tombstone, so a peer merging our document
        // afterwards keeps our newer position instead of being told the item was forgotten.
        wipeStores(); playTo(k26, 900.0); finish(k26); playTo(k26, 300.0);
        CHECK(resumeSeek(k26) == 300.0);
        CHECK(docTombs(serializeNow()).isEmpty());
        const QJsonObject reResumed = serializeNow();
        wipeStores(); injResume(h26, 900.0, T - 500);
        mergeDoc(reResumed);
        CHECK(resumeSeek(k26) == 300.0);                   // the peer takes our newer position, not a deletion

        // 26h. Order-independence, which is what makes two devices CONVERGE rather than take turns. A holds a
        // clear and no row; B holds the position it never finished. Merge each way; both must end cleared.
        wipeStores(); playTo(k26, 900.0); finish(k26);     const QJsonObject sideA = serializeNow();
        wipeStores(); injResume(h26, 900.0, T - 500);      const QJsonObject sideB = serializeNow();
        wipeStores(); playTo(k26, 900.0); finish(k26);     mergeDoc(sideB);
        const double order1 = resumeSeek(k26);
        wipeStores(); injResume(h26, 900.0, T - 500);      mergeDoc(sideA);
        const double order2 = resumeSeek(k26);
        CHECK(order1 == order2);
        CHECK(order1 == 0.0);

        // 26j. EQUAL stamps: a clear in the same second as the position it cleared still wins. It has to be
        // `>=` and not `>` — you cannot finish what you never saved, so at an equal stamp the clear is the
        // later of the two events, and `>` would let the position a peer saved that second come back for ever.
        wipeStores();
        injResume(h26, 900.0, T - 200);
        injTomb(QStringLiteral("resume"), h26, T - 200);   // finished in the very second the position was saved
        mergeDoc(serializeNow());
        CHECK(resumeSeek(k26) == 0.0);

        // 26k. The ON-DISK spelling of the namespace, pinned. Everything above would go on passing if the
        // store name were renamed, because the writer and the merge both read it from ResumeStore — a
        // self-consistent rename is invisible to a behavioural test and would strand the tombstones of every
        // install that had already written some. So this one assertion re-derives the layout independently:
        // deleted/<store>/<md5 of the key>, which is Tombstones' documented shape (§2).
        wipeStores();
        playTo(k26, 900.0); finish(k26);
        {
            QSettings raw(iniPath, QSettings::IniFormat); raw.sync();
            CHECK(raw.contains(QStringLiteral("deleted/resume/") + md5(h26)));
        }

        // 26i. THE COST OF THIS SHAPE, asserted rather than only described. compact(30) runs at every merge, so
        // a tombstone older than 30 days is gone and a peer that has been dark since then resurrects the
        // position. That is the trade #132 refused for a mark and this store accepts: a month-old playback
        // point is stale anyway. If this ever stops being true, this assertion is where it is said.
        wipeStores();
        injTomb(QStringLiteral("resume"), h26, T - 31 * 86400);
        mergeDoc(QJsonObject());                           // any merge; mergeAll's tail compacts
        {
            const QJsonObject dark = [&]{ QJsonObject d; QJsonObject r; QJsonObject e;
                e[QStringLiteral("pos")] = 900.0; e[QStringLiteral("dur")] = 3600.0;
                e[QStringLiteral("ts")] = double(T - 40 * 86400); r[h26] = e;
                d[QStringLiteral("resume")] = r; return d; }();
            mergeDoc(dark);
            CHECK(resumeSeek(k26) == 900.0);               // the expired clear no longer suppresses it
        }
        wipeStores();
    }

    // ---- 27. recents: an explicit REMOVE is dated, a cap EVICTION is not (issue #150) ----------------------
    //
    // The third shape in this family. RecentStore::remove/clear deleted from a list and recorded nothing, so
    // the union pass — which is handed two lists and cannot read a reason out of an absence — took the entry
    // back from any peer that still had it. A tombstone fixes that, but only if it stays off the cap:
    // evicting the 41st entry is the list running out of room, not the user saying "forget this", and dating
    // an eviction would make the cap PERMANENT.
    //
    // Every assertion reads through RecentStore::list() — the list the Home screen renders — and every removal
    // goes through the real store verb.
    {
        useProfile(QStringLiteral("r27"));
        auto rec = [](const QString& id, qint64 ts) {
            RecentItem it; it.key = id; it.path = QStringLiteral("X:/media/") + id + QStringLiteral(".mkv");
            it.title = id; it.kind = QStringLiteral("video"); it.ts = ts; return it;
        };
        auto ids = [&]() {
            QStringList out; for (const RecentItem& it : RecentStore::list()) out << it.key; return out;
        };
        auto docTombIds = [&](const QJsonObject& doc) {
            QStringList out;
            for (const QJsonValue& v : doc.value(QStringLiteral("recentTombs")).toObject()
                                          .value(QStringLiteral("r27")).toArray())
                out << v.toObject().value(QStringLiteral("key")).toString();
            return out;
        };

        // 27a. THE ISSUE. Remove one entry; sync with a peer that still lists it.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        const QJsonObject peerHasA = serializeNow();
        RecentStore::remove(QStringLiteral("A"));
        CHECK(ids() == QStringList{QStringLiteral("B")});
        mergeDoc(peerHasA);
        CHECK(ids() == QStringList{QStringLiteral("B")});   // the peer's stale list does NOT bring it back

        // 27b. The removal TRAVELS: a peer pulling our document drops its own copy.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        RecentStore::remove(QStringLiteral("A"));
        const QJsonObject weRemovedA = serializeNow();
        CHECK(docTombIds(weRemovedA).contains(QStringLiteral("A")));
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        CHECK(ids().size() == 2);                           // the peer's starting state
        mergeDoc(weRemovedA);
        CHECK(ids() == QStringList{QStringLiteral("B")});

        // 27c. clear() is "remove everything", one explicit action per entry, so the whole list stays cleared —
        // and the profile's tombstones still reach the document even though clear() leaves NO recents key for
        // the data half to serialize. A pass driven off the data half alone would drop them silently.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        const QJsonObject peerHasBoth = serializeNow();
        RecentStore::clear();
        CHECK(ids().isEmpty());
        const QJsonObject clearedDoc = serializeNow();
        CHECK(!clearedDoc.value(QStringLiteral("recent")).toObject()
                          .contains(QStringLiteral("r27/items")));   // nothing left in the data half
        CHECK(docTombIds(clearedDoc).size() == 2);                   // …and the deletions still travel
        mergeDoc(peerHasBoth);
        CHECK(ids().isEmpty());

        // 27d/27e. THE DISTINCTION, as a matched pair: the SAME entry, the SAME timestamp, the SAME peer — one
        // left by the cap, one taken away by the user.
        //
        // The fixture makes insertion order disagree with ts order (ev0 is added FIRST, so it sits at the tail
        // the cap trims, but carries the NEWEST ts), which is the only way an evicted entry can be shown coming
        // back: the merge re-caps by ts, so an entry that is still among the newest 40 has a slot waiting.
        const int kCap = 40;
        auto fillLocal = [&](int n) {                       // ev0 first (oldest slot), then ev1..evN-1
            RecentStore::add(rec(QStringLiteral("ev0"), T - 50));
            for (int i = 1; i < n; ++i) RecentStore::add(rec(QStringLiteral("ev") + QString::number(i), T - 1000 + i));
        };
        wipeStores();
        RecentStore::add(rec(QStringLiteral("ev0"), T - 50));
        const QJsonObject peerHasEv0 = serializeNow();      // a peer that still lists ev0

        // 27d. EVICTION. A 41st entry pushes ev0 off the end. It records nothing, so the merge hands it back.
        wipeStores();
        fillLocal(kCap + 1);
        CHECK(ids().size() == kCap);
        CHECK(!ids().contains(QStringLiteral("ev0")));      // the cap dropped it
        CHECK(docTombIds(serializeNow()).isEmpty());        // and dated NOTHING — the cap is not a deletion
        mergeDoc(peerHasEv0);
        CHECK(ids().contains(QStringLiteral("ev0")));       // still among the newest 40, so it is back
        CHECK(!ids().contains(QStringLiteral("ev1")));      // and the genuinely-oldest took the cut instead
        CHECK(ids().size() == kCap);

        // 27e. EXPLICIT REMOVE, same entry, same ts, same peer document: it stays gone, even though its
        // timestamp would win it a slot outright.
        wipeStores();
        fillLocal(kCap);                                    // 40 entries: no eviction happens at all
        CHECK(ids().size() == kCap);
        RecentStore::remove(QStringLiteral("ev0"));
        CHECK(!ids().contains(QStringLiteral("ev0")));
        CHECK(docTombIds(serializeNow()) == QStringList{QStringLiteral("ev0")});
        mergeDoc(peerHasEv0);
        CHECK(!ids().contains(QStringLiteral("ev0")));

        // 27l. The other direction of a full clear: the PEER emptied its list, we still hold the entries. Its
        // document then names the profile ONLY in the tombstone half — clear() leaves no recents key to
        // serialize — so a merge driven off the data half alone would never look at the profile at all and the
        // clear would arrive and be dropped in silence.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        RecentStore::clear();
        const QJsonObject peerClearedAll = serializeNow();
        CHECK(!peerClearedAll.value(QStringLiteral("recent")).toObject().contains(QStringLiteral("r27/items")));
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        mergeDoc(peerClearedAll);
        CHECK(ids().isEmpty());

        // 27m. EQUAL stamps, as in §26j: a removal recorded in the same second as the entry's own timestamp
        // suppresses it. `>` here would leave a same-second removal permanently losing to the entry it removed.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 100));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        const QJsonObject bothStamped = serializeNow();
        injTomb(QStringLiteral("recent/r27"), QStringLiteral("A"), T - 100);
        mergeDoc(bothStamped);
        CHECK(ids() == QStringList{QStringLiteral("B")});

        // 27n. A removal is filed under the ENTRY'S identity, never under the string the caller happened to
        // hand in. remove() matches on path OR key and callers pass whichever they hold — HomeView's
        // "Remove from Recent" passes the url, uninstallGameItem passes both in turn — while the merge
        // de-duplicates on key-else-path. Tombstoning the argument would file a streamed item's removal under
        // a URL that changes every session, and the merge would never look it up.
        wipeStores();
        {
            RecentItem streamed;
            streamed.key = QStringLiteral("strm:tt99");                    // the stable identity
            streamed.path = QStringLiteral("https://cdn.example/one.m3u8"); // …which its URL is not
            streamed.title = QStringLiteral("Streamed"); streamed.kind = QStringLiteral("video");
            streamed.ts = T - 500;
            RecentStore::add(streamed);
            const QJsonObject peerHasStream = serializeNow();
            RecentStore::remove(streamed.path);            // removed BY PATH
            CHECK(ids().isEmpty());
            mergeDoc(peerHasStream);
            CHECK(ids().isEmpty());                        // and it stays gone, because the key was tombstoned
        }

        // 27f. Re-opening an item UNDOES the removal of it — a removal is not a ban. add() lifts the tombstone,
        // so the entry survives a later sync with the document that carried the removal.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::remove(QStringLiteral("A"));
        const QJsonObject removalDoc = serializeNow();
        RecentStore::add(rec(QStringLiteral("A"), T + 200));  // …and the user opens it again
        CHECK(ids() == QStringList{QStringLiteral("A")});
        CHECK(docTombIds(serializeNow()).isEmpty());
        mergeDoc(removalDoc);                                 // the peer still carrying our own older removal
        CHECK(ids() == QStringList{QStringLiteral("A")});

        // 27g. Mixed fleet, direction ONE: an un-upgraded peer serializes no "recentTombs" at all and still
        // lists the entry we removed. The absent key must read as silence, not as "no deletions exist".
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        QJsonObject oldPeerDoc = serializeNow();
        oldPeerDoc.remove(QStringLiteral("recentTombs"));
        RecentStore::remove(QStringLiteral("A"));
        mergeDoc(oldPeerDoc);
        CHECK(ids() == QStringList{QStringLiteral("B")});
        mergeDoc(oldPeerDoc);                                 // every sync, not just the first
        CHECK(ids() == QStringList{QStringLiteral("B")});

        // 27h. Mixed fleet, direction TWO: the half an old binary reads is untouched. "recent" is still keyed
        // "<profile>/items" and its value is still the list JSON as a STRING — re-shaping it into
        // {items,tombs} would have made every shipped build read an empty list and stop merging recents.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::remove(QStringLiteral("A"));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        {
            const QJsonObject doc = serializeNow();
            const QJsonValue v = doc.value(QStringLiteral("recent")).toObject().value(QStringLiteral("r27/items"));
            CHECK(v.isString());
            const QJsonArray arr = QJsonDocument::fromJson(v.toString().toUtf8()).array();
            CHECK(arr.size() == 1 && arr.at(0).toObject().value(QStringLiteral("key")).toString() == QStringLiteral("B"));
            CHECK(doc.value(QStringLiteral("recentTombs")).isObject()); // a sibling, not inside "recent"
        }

        // 27i. Order-independence: A removed the entry, B still lists it. Both merge orders converge.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        RecentStore::remove(QStringLiteral("A"));
        const QJsonObject sideRemoved = serializeNow();
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        const QJsonObject sideKept = serializeNow();
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        RecentStore::remove(QStringLiteral("A"));
        mergeDoc(sideKept);
        const QStringList way1 = ids();
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::add(rec(QStringLiteral("B"), T - 400));
        mergeDoc(sideRemoved);
        const QStringList way2 = ids();
        CHECK(way1 == way2);
        CHECK(way1 == QStringList{QStringLiteral("B")});

        // 27j. Per-profile isolation: removing an id under one profile says nothing about the same id under
        // another. The tombstone namespace mirrors the store's own, exactly as favourites' does.
        // The second profile adds FIRST and never touches the entry again, deliberately: an add AFTER the other
        // profile's removal would lift the tombstone under a namespace that had wrongly gone global, and the
        // isolation this asserts would pass for the wrong reason.
        wipeStores();
        useProfile(QStringLiteral("r27b"));
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        useProfile(QStringLiteral("r27"));
        RecentStore::add(rec(QStringLiteral("A"), T - 500));
        RecentStore::remove(QStringLiteral("A"));
        const QJsonObject twoProfiles = serializeNow();
        mergeDoc(twoProfiles);
        useProfile(QStringLiteral("r27b"));
        CHECK(ids() == QStringList{QStringLiteral("A")});    // r27b keeps it
        useProfile(QStringLiteral("r27"));
        CHECK(ids().isEmpty());                              // r27 does not

        // 27k. The cost, again asserted rather than described: compact(30) expires a removal, and a peer dark
        // for longer than that hands the entry back. Bounded deleted/* is what buys it, and this is the price.
        wipeStores();
        RecentStore::add(rec(QStringLiteral("A"), T - 40 * 86400));
        const QJsonObject darkPeer = serializeNow();
        RecentStore::remove(QStringLiteral("A"));
        injTomb(QStringLiteral("recent/r27"), QStringLiteral("A"), T - 31 * 86400); // as if removed 31 days ago
        mergeDoc(QJsonObject());                              // any merge; mergeAll's tail compacts
        mergeDoc(darkPeer);
        CHECK(ids() == QStringList{QStringLiteral("A")});
    }
    // ---- 28. "you missed" dismissals (issue #25): merge by MAX, and the front-end that keeps it monotone --
    // Every other section of this document needs a timestamp, a tombstone space and an equal-value
    // tie-break. This one needs none of the three, and the assertions below are what that claim rests on:
    // the merge is a lattice join, so order does not matter, repetition does not matter, and there is no
    // "equal but different" case to decide.
    {
        wipeStores();
        const QString k26 = QStringLiteral("tt2500001");
        const QString other26 = QStringLiteral("tt2500002");
        // The stamps are AIR TIMES, not write times, so they are ordinary unix seconds and the test can pick
        // them freely; the merge never consults a clock.
        const qint64 lo = 1700000000, hi = 1700009999;

        // 26a. The store front-end is MONOTONE. A lower write is a no-op — not "last write wins" — because
        // a peer replaying a stale dismissal must not un-dismiss episodes the user has already dealt with.
        MissedDismiss::dismissThrough(k26, hi);
        CHECK(MissedDismiss::through(k26) == hi);
        MissedDismiss::dismissThrough(k26, lo);
        CHECK(MissedDismiss::through(k26) == hi);
        MissedDismiss::dismissThrough(k26, hi + 1);
        CHECK(MissedDismiss::through(k26) == hi + 1);
        // Not a record: no key, or a non-positive stamp. 0 is "never dismissed" and writing it must not
        // create a row that then reads back as one — the #132 rule for a store whose absent value is 0.
        MissedDismiss::dismissThrough(QString(), hi);
        MissedDismiss::dismissThrough(other26, 0);
        MissedDismiss::dismissThrough(other26, -5);
        CHECK(MissedDismiss::through(other26) == 0);
        CHECK(MissedDismiss::through(QString()) == 0);
        // …and none of the three left a ROW behind. Asserted through the serializer rather than through
        // through(), because an empty key hashes to a perfectly valid leaf that through() would never look
        // under — so the reader cannot see the row it wrote, but the merge document can, and it would be
        // pushed to every other device for ever.
        auto missedRowCount = [&]() {
            int n = 0;
            const QJsonObject sec = serializeNow().value(QStringLiteral("missed")).toObject();
            for (auto pit = sec.begin(); pit != sec.end(); ++pit) n += pit.value().toObject().size();
            return n;
        };
        CHECK(missedRowCount() == 1);   // only k26's

        // 26b. It is IN the merge document, per profile, and comes back through a round trip. Serialized as
        // a number, so a device that reads it back as a string would compare "9" > "10" and lose stamps.
        wipeStores();
        MissedDismiss::dismissThrough(k26, hi);
        const QJsonObject doc26 = serializeNow();
        CHECK(doc26.contains(QStringLiteral("missed")));
        const QJsonObject missedSec = doc26.value(QStringLiteral("missed")).toObject();
        CHECK(missedSec.size() == 1);
        const QJsonObject profSec = missedSec.begin().value().toObject();
        CHECK(profSec.size() == 1);
        CHECK(static_cast<qint64>(profSec.begin().value().toDouble()) == hi);

        // 26c. MAX, in both directions and in both orders — the whole convergence argument in four lines.
        wipeStores(); MissedDismiss::dismissThrough(k26, lo); const QJsonObject docLo = serializeNow();
        wipeStores(); MissedDismiss::dismissThrough(k26, hi); const QJsonObject docHi = serializeNow();
        // remote NEWER than local -> local rises.
        wipeStores(); MissedDismiss::dismissThrough(k26, lo); mergeDoc(docHi);
        const qint64 aWins = MissedDismiss::through(k26);
        // remote OLDER than local -> local holds. This is the leg that matters: a peer that has been off
        // for a month still carries the stamp from before the user extended it, and a plain "remote wins"
        // would hand back the episodes they dismissed since.
        wipeStores(); MissedDismiss::dismissThrough(k26, hi); mergeDoc(docLo);
        const qint64 bWins = MissedDismiss::through(k26);
        CHECK(aWins == hi);
        CHECK(bWins == hi);
        CHECK(aWins == bWins);   // ORDER-INDEPENDENT: both devices land on the same value

        // 26d. IDEMPOTENT: merging the same document again changes nothing, so the 15-second push loop
        // cannot walk a stamp anywhere.
        mergeDoc(docLo); mergeDoc(docLo); mergeDoc(docHi);
        CHECK(MissedDismiss::through(k26) == hi);

        // 26d-bis. A REPEAT press writes nothing and arms nothing. The store's change hook is what re-arms
        // the debounced Drive push, so a dismissal that changes no value must not fire it — otherwise
        // pressing "caught up" on an already-caught-up show uploads a merge document identical to the one
        // already there. Asserted through the hook because the STORED VALUE is the same either way, so no
        // reader can tell the two apart and nothing else in this probe would notice.
        {
            wipeStores();
            int fired = 0;
            MissedDismiss::setChangeHook([&fired] { ++fired; });
            MissedDismiss::dismissThrough(k26, hi);
            CHECK(fired == 1);                       // a real dismissal arms the push
            MissedDismiss::dismissThrough(k26, hi);  // the same stamp again
            MissedDismiss::dismissThrough(k26, lo);  // ...and an older one
            CHECK(fired == 1);                       // neither is a change, so neither arms anything
            MissedDismiss::dismissThrough(k26, hi + 1);
            CHECK(fired == 2);
            MissedDismiss::setChangeHook({});
        }

        // 26e. A remote non-record cannot land. There is no guard for it and there must not be: an absent
        // local row reads as 0, which is the floor of the max, so a 0 (or a negative, or a value that was
        // never a number) is rejected by the merge rule itself. Asserted against a hash the local store has
        // NEVER seen, because that is the only case where a separate guard could have made a difference.
        {
            wipeStores();
            QJsonObject zeroOnly;
            QJsonObject shows;
            shows.insert(QStringLiteral("00000000000000000000000000000000"), 0.0);
            QJsonObject sec;
            sec.insert(QStringLiteral("default"), shows);
            zeroOnly.insert(QStringLiteral("missed"), sec);
            mergeDoc(zeroOnly);
            CHECK(missedRowCount() == 0);   // nothing was created for it
            MissedDismiss::dismissThrough(k26, hi);
        }

        // 26e-bis. …and it cannot clobber a real one either.
        {
            QJsonObject zeroDoc = docHi;
            QJsonObject sec = zeroDoc.value(QStringLiteral("missed")).toObject();
            const QString pid = sec.begin().key();
            QJsonObject shows = sec.value(pid).toObject();
            const QString showHash = shows.begin().key();
            shows.insert(showHash, 0.0);
            sec.insert(pid, shows);
            zeroDoc.insert(QStringLiteral("missed"), sec);
            mergeDoc(zeroDoc);
            CHECK(MissedDismiss::through(k26) == hi);
        }

        // 26f. A document with NO "missed" section — every peer still on the previous build — merges as
        // empty and leaves the local store alone. The section is optional by the document's own contract.
        {
            QJsonObject noSec = docHi;
            noSec.remove(QStringLiteral("missed"));
            mergeDoc(noSec);
            CHECK(MissedDismiss::through(k26) == hi);
        }

        // 26g. mergeAll drops the store's lazy cache. The merge writes missed/* through the ini directly,
        // so a cache built before it would keep answering with the pre-merge stamp and the user would be
        // nagged about a show they dismissed on the other box until the next profile switch.
        wipeStores();
        MissedDismiss::dismissThrough(k26, lo);
        CHECK(MissedDismiss::through(k26) == lo);   // build the cache
        mergeDoc(docHi);
        CHECK(MissedDismiss::through(k26) == hi);   // ...and it must reflect the merge with no other prompting

        // 26g-bis. PER PROFILE, and the cache self-heals across a switch. The rows this store suppresses are
        // one viewer's, so a household where the parent has caught up on a show must not silence it for the
        // kid — and the failure mode if the cache did not notice the switch is exactly that, with no way to
        // see it except by wondering where a row went.
        {
            wipeStores();
            const QString before = ProfileStore::currentId();
            ProfileStore::setCurrent(QStringLiteral("p26a"));
            MissedDismiss::invalidate();
            MissedDismiss::dismissThrough(k26, hi);
            CHECK(MissedDismiss::through(k26) == hi);
            ProfileStore::setCurrent(QStringLiteral("p26b"));
            CHECK(MissedDismiss::through(k26) == 0);      // NOT the other profile's dismissal
            MissedDismiss::dismissThrough(k26, lo);
            CHECK(MissedDismiss::through(k26) == lo);
            ProfileStore::setCurrent(QStringLiteral("p26a"));
            CHECK(MissedDismiss::through(k26) == hi);     // ...and the first profile's is where it was
            // Both profiles ride the document, under their own names.
            const QJsonObject twoProfiles = serializeNow().value(QStringLiteral("missed")).toObject();
            CHECK(twoProfiles.contains(QStringLiteral("p26a")));
            CHECK(twoProfiles.contains(QStringLiteral("p26b")));
            ProfileStore::setCurrent(before);
            MissedDismiss::invalidate();
            wipeStores();
        }

        // 26h. prune() collects what can no longer suppress anything, and NOTHING else. The stamps here are
        // relative to a synthetic "now" so the test does not depend on the wall clock.
        {
            wipeStores();
            const qint64 now26 = 1800000000;
            const qint64 kDay = 86400;
            const QString liveKey = QStringLiteral("tt2500010");
            const QString deadKey = QStringLiteral("tt2500011");
            const QString badKey  = QStringLiteral("tt2500012");
            // The store's own group and hash, rebuilt here so a raw seed lands exactly where the store
            // looks — the point of the corrupt-row cases below is that the READER meets them.
            const QString pid = ProfileStore::currentId();
            const QString grp = QStringLiteral("missed/")
                              + (pid.isEmpty() ? QStringLiteral("default") : pid) + QStringLiteral("/shows");
            auto rawShowKey = [&](const QString& showKey) {
                return grp + QLatin1Char('/')
                     + QString::fromLatin1(QCryptographicHash::hash(showKey.toUtf8(),
                                                                    QCryptographicHash::Md5).toHex());
            };
            MissedDismiss::dismissThrough(liveKey, now26 - 5 * kDay);                              // fresh
            MissedDismiss::dismissThrough(deadKey, now26 - (trakt::kMissedDismissTtlDays + 5) * kDay); // long dead
            // A key under this root that is NOT a show stamp — whatever a later version of the app starts
            // writing there. The sweep matches the SHAPE, not the prefix, so it leaves that alone; a prefix
            // match would silently eat a future feature's state and the failure would surface as data loss
            // on a downgrade, which is the hardest kind to trace back here.
            setRaw(QStringLiteral("missed/default/somethingElse/x"), QStringLiteral("1"));
            // …and, from the other side, a key of the RIGHT shape under a DIFFERENT root. Both halves of
            // the sweep's filter have to be there: this one is what stops it walking the whole ini.
            setRaw(QStringLiteral("notmissed/default/shows/x"), QStringLiteral("1"));
            // Corrupt rows, of both shapes an ini can carry: one that is not a number at all, and one that
            // is a NEGATIVE number. Both read back as "never dismissed" — the negative one matters, because
            // a store that passed it through would hand planMissed a cut before the epoch and would carry
            // it to every other device for ever. Neither is serialized, and both are collected.
            const QString badKey2 = QStringLiteral("tt2500013");
            setRaw(rawShowKey(badKey),  QStringLiteral("-5"));
            setRaw(rawShowKey(badKey2), QStringLiteral("not-a-number"));
            MissedDismiss::invalidate();
            CHECK(MissedDismiss::through(badKey) == 0);
            CHECK(MissedDismiss::through(badKey2) == 0);
            CHECK(missedRowCount() == 2);   // live + dead; neither corrupt row is serialized
            // Read the two real stamps BEFORE the prune, so the store's cache is warm when it runs. A prune
            // that removed the rows and left the cache alone would otherwise be invisible here — the reads
            // after it would rebuild from the swept ini and agree by accident.
            CHECK(MissedDismiss::through(deadKey) == now26 - (trakt::kMissedDismissTtlDays + 5) * kDay);
            CHECK(MissedDismiss::prune(now26) == 3);   // the dead stamp AND both corrupt rows
            CHECK(MissedDismiss::through(deadKey) == 0);
            CHECK(MissedDismiss::through(liveKey) == now26 - 5 * kDay);
            {
                QSettings raw(iniPath, QSettings::IniFormat); raw.sync();
                CHECK(raw.value(QStringLiteral("missed/default/somethingElse/x")).toString()
                      == QStringLiteral("1"));   // untouched by the sweep
                CHECK(raw.value(QStringLiteral("notmissed/default/shows/x")).toString()
                      == QStringLiteral("1"));   // ditto, from outside the root
                raw.remove(QStringLiteral("missed/default/somethingElse/x"));
                raw.remove(QStringLiteral("notmissed"));
                raw.sync();
            }
            // Running it again removes nothing: a prune is not a state machine, it is a filter.
            CHECK(MissedDismiss::prune(now26) == 0);
            // A collected record that a peer still holds comes STRAIGHT BACK on the next merge, and that is
            // fine — it is the property that makes collecting safe in a store that syncs. The assertion is
            // here so that "the prune and the merge disagree" is a failure rather than a surprise.
            const QJsonObject docDead = serializeNow();   // (post-prune: dead is gone from ours)
            CHECK(!docDead.value(QStringLiteral("missed")).toObject().isEmpty());
        }

        wipeStores();
    }

    // ---- 29. Metadata overrides: a husk is a CLEAR, so a NON-clear must not write one (issue #132) ---------
    //
    // §24b pins the half of the rule everyone remembers — a reset must not DELETE the row, because "cleared"
    // and "never known" must not share a representation. This is the converse half, and it costs more when it
    // is wrong. A husk is a clear dated NOW; husks here are never compacted, so it is permanent, it rides the
    // sync document to every device, and being newest it outranks everything. Spelling a non-event that way
    // does not merely bloat the store: it deletes another device's real correction.
    //
    // It was reachable by the ordinary route. The "Fix info" editor writes back through MetaOverrides::set()
    // after every OSK, and typing a field back to exactly what the scraper found is deliberately NOT an
    // override (it would pin the item against a later, better scrape), so it normalizes to an empty value. On
    // an item carrying no correction, "open the editor, confirm a field unchanged, back out" therefore stored
    // a fresh clear. ItemMarks::saveItem has always refused the equivalent write; set() now does too.
    //
    // The assertions read through MetaOverrides::get() — what the item SHOWS — for §25's reason. Asserting
    // that the row is absent would be the wrong instrument in the other direction here: on the broken build
    // the row is present and the damage is a peer's edit, not a local symptom. Each sub-test uses its own
    // item key so that no assertion depends on one static QSettings noticing another's removal.
    {
        auto ovTitle29 = [&](const QString& key) { return MetaOverrides::get(key).title; };
        // What a peer holds: a genuine correction, stamped BEFORE anything set() can write. Hand-built as a
        // document (the shape is §20a's) so no ini round-trip is needed to produce it.
        auto peerDoc29 = [&](const QString& key) {
            QJsonObject blob;
            blob[QStringLiteral("title")] = QStringLiteral("Corrected");
            blob[QStringLiteral("updatedAt")] = double(T - 500);
            QJsonObject items; items.insert(md5(key), blob);
            QJsonObject root; root.insert(QStringLiteral("metaoverrides"), items);
            return root;
        };
        auto injPeer29 = [&](const QString& key) {
            QJsonObject o;
            o[QStringLiteral("title")] = QStringLiteral("Corrected");
            o[QStringLiteral("updatedAt")] = double(T - 500);
            setRaw(QStringLiteral("metaoverrides/items/") + md5(key), compactO(o));
            MetaOverrides::invalidate();
        };
        // The editor's write-back for "I opened Fix info and confirmed the field unchanged": an all-empty
        // override through the store's own funnel. This one call is what the issue is about.
        auto confirmUnchanged29 = [&](const QString& key) { MetaOverrides::set(key, MetaOverrides::Override{}); };

        // 29a. THE DATA LOSS. This device has never corrected the item; a peer has. The peer's correction is
        // the only edit anyone made, so it must be what both devices end up showing.
        wipeStores();
        const QString kA29 = QStringLiteral("igdb:29001");
        confirmUnchanged29(kA29);
        mergeDoc(peerDoc29(kA29));
        CHECK(ovTitle29(kA29) == QStringLiteral("Corrected"));

        // 29b. …and the direction that makes it permanent rather than local: the peer PULLS this device's
        // document, so a clear written here for a non-event deletes the correction over there too. Same
        // outcome from the opposite end is what "converged" means; one end keeping the edit would not be a
        // fix, it would be a disagreement that the next sync resolves against the user.
        wipeStores();
        const QString kB29 = QStringLiteral("igdb:29002");
        confirmUnchanged29(kB29);
        const QJsonObject quietDoc29 = serializeNow();
        wipeStores();
        injPeer29(kB29);
        mergeDoc(quietDoc29);
        CHECK(ovTitle29(kB29) == QStringLiteral("Corrected"));

        // 29c. The guard is "was there a record to clear", NOT "never write an empty record". A GENUINE clear
        // still husks and still wins, through set() with an emptied override — the editor's other path
        // (blanking the last corrected field), which §24b's reset() does not exercise. A guard that over-fired
        // would pass 29a/29b and silently re-open #24 here.
        wipeStores();
        const QString kC29 = QStringLiteral("igdb:29003");
        MetaOverrides::Override real29; real29.title = QStringLiteral("Mine");
        MetaOverrides::set(kC29, real29);
        CHECK(ovTitle29(kC29) == QStringLiteral("Mine"));
        MetaOverrides::set(kC29, MetaOverrides::Override{});   // blanked: a real clear, on a real record
        CHECK(ovTitle29(kC29).isEmpty());
        mergeDoc(peerDoc29(kC29));                             // a peer still holding an older correction
        CHECK(ovTitle29(kC29).isEmpty());                      // the clear is a fact with a time on it, and wins

        // 29d. A non-event is not carried at all — the assertion nothing else can make. On THIS device the
        // item reads the same either way, so the cost of getting it wrong (one permanent row per item the
        // user merely looked at, in a store that is never compacted, downloaded by every device) is invisible
        // until the document is opened.
        wipeStores();
        const QString kD29 = QStringLiteral("igdb:29004");
        confirmUnchanged29(kD29);
        CHECK(!serializeNow().value(QStringLiteral("metaoverrides")).toObject().contains(md5(kD29)));
        // …while a real clear IS carried. The exact mirror of the line above, and the pair is what separates
        // "wrote nothing" from "wrote nothing that propagates" — a reset that stopped riding the document
        // would be #24 again, and would pass every assertion in 29a-29c.
        wipeStores();
        const QString kE29 = QStringLiteral("igdb:29005");
        MetaOverrides::set(kE29, real29);
        MetaOverrides::set(kE29, MetaOverrides::Override{});
        CHECK(serializeNow().value(QStringLiteral("metaoverrides")).toObject().contains(md5(kE29)));

        wipeStores();
    }

    // ---- 30. A PC-game REMAP raced against a peer that still holds the old key (issue #166) ----------------
    //
    // §25-27 and §29 are about a CLEAR: how one is represented so a merge cannot mistake it for ignorance, and
    // how a non-clear must not borrow that representation. This section is about the third thing a per-item
    // row can stop holding a value for, and the one that is NEITHER: a device-local REPAIR. PcGameRemap moves
    // everything the user accrued against a game from its per-launcher id ("steam:1145360") onto the merged
    // id the catalog now builds, and it retires the source row. It is the last place in the tree that removes
    // a synced per-item row outright, and #132's shape applied to it mechanically would be a data-loss bug of
    // its own — so the three questions had to be answered, not reflexed:
    //
    //   IS A REMAP DATED? No. A repair is not a statement by the user, so it has nothing true to say about the
    //   old key; a husk stamped `now` would claim to be newer than every peer's genuine data and delete it.
    //   WHAT DOES A PEER THAT HAS NOT REMAPPED SEE? Nothing — its rows stand until it runs the repair itself
    //   (30a). That is the decision, and 30c pins that it is a decision and not an accident of who went first:
    //   both devices land on the same answer whichever of them remapped.
    //   DOES IT DIFFER FOR THE ACCUMULATORS? Yes, and not by taste — stats/playstats merge per DEVICE
    //   NAMESPACE under a `lastWrite` gate, not per row, so a row husk is not something that merge can
    //   compare at all. There the whole rule is "never stamp lastWrite", and 30f is what says so.
    //
    // The price of not dating the retirement is that the never-delete pass re-imports the old row here on the
    // next sync — an absence has no timestamp, so it reads as ignorance. 30b is that round trip, and it is the
    // defect: the remap's collision merge is monotone add-only (hidden ORs, tags union), so folding the stale
    // copy back in RE-HIDES a game the user just un-hid and re-stamps the result as the newest thing in the
    // fleet. The fix is that the newer side, when it is a clear, wins outright — both directions (30b, 30e),
    // and only when it really is a clear (30d).
    //
    // Every verdict below is read through the store's own accessor — ItemMarks::get, ConsumptionStats::get,
    // PlayStats::get — never through row presence, for §25's reason: a row is present on the broken build too,
    // and what the user loses is what the reader answers.
    {
        const QString oldId30 = QStringLiteral("steam:1145360");
        const QString newId30 = QStringLiteral("pcgame:hades");
        const QString prof30  = QStringLiteral("cm30");
        // A device that is NOT this one, so mergeNamespaced treats its namespace as foreign (it skips the
        // local device's own namespace by id, which would make 30f assert nothing).
        const QString peerDev30 = QStringLiteral("11111111-2222-3333-4444-555555555555");

        auto wipe30 = [&]() {
            QSettings raw(iniPath, QSettings::IniFormat);
            for (const char* g : {"marks", "favorites", "playlists", "deleted", "resume", "recent",
                                  "metaoverrides", "missed", "stats", "playstats", "pcgameremap"})
                raw.remove(QLatin1String(g));
            raw.sync();
            ItemMarks::invalidate();
            MetaOverrides::invalidate();
            MissedDismiss::invalidate();
            ConsumptionStats::invalidate();
        };
        auto marksBlob30 = [&](bool hidden, const QString& completion, const QStringList& tags, qint64 upd) {
            QJsonObject o;
            o.insert(QStringLiteral("hidden"), hidden);
            o.insert(QStringLiteral("completion"), completion);
            QJsonArray t; for (const QString& x : tags) t.append(x);
            o.insert(QStringLiteral("tags"), t);
            o.insert(QStringLiteral("updatedAt"), double(upd));
            return compactO(o);
        };
        auto injMark30 = [&](const QString& id, bool hidden, const QString& completion,
                             const QStringList& tags, qint64 upd) {
            setRaw(QStringLiteral("marks/") + prof30 + QStringLiteral("/items/") + md5(id),
                   marksBlob30(hidden, completion, tags, upd));
            ItemMarks::invalidate();
        };
        // The repair itself, driven through the real entry point. applyRemap takes the table directly, so the
        // id shapes above are the fixture and pcgame::itemId's normalisation is not on trial here (that is
        // probe_pcgames' subject) — what is on trial is what the repair does to a SYNCED row.
        auto remap30 = [&]() {
            QHash<QString, QString> t;
            t.insert(oldId30, newId30);
            pcgame::applyRemap(t);
            ItemMarks::invalidate();
            ConsumptionStats::invalidate();
        };
        auto marks30 = [&](const QString& id) { return ItemMarks::get(id); };

        // 30a. THE PEER SEES NOTHING. This device runs the repair and pushes; the peer has not run it and is
        // still reading the old id. Its record must survive the pull intact — a repair that dated the
        // retirement would arrive as a clear newer than anything the peer holds and delete it.
        //
        // The fixture carries a hide AND a completion on ONE item deliberately. It kills the obvious husk
        // (stamped `now`, which simply outranks the peer) and also the subtle one that stamps the SOURCE's own
        // updatedAt: at equal stamps CloudMerge falls to the lexical tie key, where a husk's "hidden":false
        // loses to "hidden":true but its "completion":"none" BEATS "finished" ('n' > 'f'). One field would
        // have let one of the two mutants through.
        wipe30();
        useProfile(prof30);
        injMark30(oldId30, /*hidden*/true, QStringLiteral("finished"), QStringList{}, T - 500);
        remap30();
        CHECK(marks30(newId30).hidden);                                    // premise: the record did move
        CHECK(marks30(newId30).completion == ItemMarks::Completion::Finished);
        // …and MOVED, not copied. This reads through the same accessor as everything else in §30 — no row
        // presence — so it is the section's own instrument, not an exception to it. Without it a repair that
        // retires nothing passes every other check here, because on THIS device a duplicate under the old id
        // answers the same as a genuine one; only asking about the old id can tell them apart.
        CHECK(!marks30(oldId30).hidden);
        const QJsonObject docRemapped30 = serializeNow();                  // what this device now pushes

        wipe30();
        useProfile(prof30);
        injMark30(oldId30, true, QStringLiteral("finished"), QStringList{}, T - 500);  // the peer, unremapped
        mergeDoc(docRemapped30);
        CHECK(marks30(oldId30).hidden);                                    // …still marked where the peer looks
        CHECK(marks30(oldId30).completion == ItemMarks::Completion::Finished);

        // 30b. THE ROUND TRIP, which is the defect. Undated retirement means the peer's copy of the old row is
        // re-imported here (an absence reads as ignorance, never as a deletion). The user has meanwhile
        // cleared the mark on the combined tile, so the next library refresh folds a stale marked copy into a
        // newer husk — and the add-only merge re-hides the game, at a stamp that then beats every device.
        wipe30();
        useProfile(prof30);
        injMark30(oldId30, true, QStringLiteral("none"), QStringList{}, T - 500);
        remap30();
        ItemMarks::setHidden(newId30, false);                              // the user un-hides the merged game
        CHECK(!marks30(newId30).hidden);
        {
            // What the peer, still on the old id, pushes: the very record this device retired, unchanged.
            QJsonObject items, po, marks, root;
            items.insert(md5(oldId30), QJsonDocument::fromJson(
                marksBlob30(true, QStringLiteral("none"), QStringList{}, T - 500).toUtf8()).object());
            po.insert(QStringLiteral("items"), items);
            marks.insert(prof30, po);
            root.insert(QStringLiteral("marks"), marks);
            mergeDoc(root);
        }
        CHECK(marks30(oldId30).hidden);                                    // premise: it really did come back
        remap30();                                                         // the next library refresh
        CHECK(!marks30(newId30).hidden);                                   // THE CLEAR SURVIVES

        // 30c. …and the peer reaches the SAME verdict when it runs the repair, which is what makes this a
        // decision rather than a race. The peer never saw the un-hide happen and has no record of what this
        // device already absorbed — so a fix built on a device-local ledger would pass 30b and fail here,
        // leaving the game hidden on one device and visible on the other for ever.
        const QJsonObject docCleared30 = serializeNow();
        wipe30();
        useProfile(prof30);
        injMark30(oldId30, true, QStringLiteral("none"), QStringList{}, T - 500);   // the peer's own old row
        mergeDoc(docCleared30);
        CHECK(marks30(oldId30).hidden);                                    // premise: the peer still reads it
        remap30();                                                         // the peer repairs, first time
        CHECK(!marks30(newId30).hidden);                                   // same answer as 30a's device

        // 30d. THE GUARD IS "IS IT A CLEAR", NOT "IS IT NEWER". Two LIVE launcher records collapsing into one
        // game must still merge generously — that rule is what stops a collision from deleting a completion
        // mark or a shelf (probe_pcgames §9), and a guard that fired on any newer destination would pass
        // 30b/30c while silently re-opening it.
        wipe30();
        useProfile(prof30);
        injMark30(oldId30, false, QStringLiteral("finished"), QStringList{QStringLiteral("rpg")}, T - 500);
        injMark30(newId30, true,  QStringLiteral("none"),     QStringList{QStringLiteral("indie")}, T - 200);
        remap30();
        {
            const ItemMarks::Marks m = marks30(newId30);
            CHECK(m.hidden);                                                       // hidden ORs
            CHECK(m.completion == ItemMarks::Completion::Finished);                // a verdict beats "none"
            CHECK(m.tags.contains(QStringLiteral("rpg")));                         // …and the tags UNION
            CHECK(m.tags.contains(QStringLiteral("indie")));
        }

        // 30e. THE MIRROR DIRECTION, which is equally reachable and which a destination-only guard misses
        // entirely. Here the CLEAR arrives under the OLD id — written by a peer that has not remapped, so the
        // only tile it can clear from is the per-launcher one — and the record this device already moved to
        // the merged id is the stale side.
        wipe30();
        useProfile(prof30);
        injMark30(newId30, true,  QStringLiteral("none"), QStringList{}, T - 500);  // already moved here
        injMark30(oldId30, false, QStringLiteral("none"), QStringList{}, T - 100);  // the peer's husk, newer
        remap30();
        CHECK(!marks30(newId30).hidden);                                   // the newer clear wins as a SOURCE

        // 30f. THE ACCUMULATORS, whose answer differs because their merge does. stats and playstats are copied
        // per DEVICE NAMESPACE under a `lastWrite` freshness gate, so the repair is invisible to a peer for a
        // different reason than marks: it rewrites the rows and does NOT stamp the namespace, so the owner's
        // own copy stays the freshest and mergeNamespaced keeps it. The namespace here is a FOREIGN one — the
        // case that matters, because that is a namespace this device does not own and must not date.
        wipe30();
        useProfile(prof30);
        auto injAccum30 = [&](const QString& id) {
            QJsonObject e;
            e.insert(QStringLiteral("mediaSeconds"), 120.0);
            e.insert(QStringLiteral("pagesRead"),    0.0);
            e.insert(QStringLiteral("lastActivity"), double(T - 500));
            e.insert(QStringLiteral("title"),        QStringLiteral("Hades"));
            e.insert(QStringLiteral("category"),     QStringLiteral("video"));
            setRaw(QStringLiteral("stats/") + prof30 + QLatin1Char('/') + peerDev30
                       + QStringLiteral("/items/") + md5(id), compactO(e));
            setRaw(QStringLiteral("stats/") + prof30 + QLatin1Char('/') + peerDev30
                       + QStringLiteral("/lastWrite"), QString::number(T - 500));
            const QString g = QStringLiteral("playstats/") + prof30 + QLatin1Char('/') + peerDev30
                            + QLatin1Char('/') + sha1(id);
            setRaw(g + QStringLiteral("/total"),    QStringLiteral("600"));
            setRaw(g + QStringLiteral("/sessions"), QStringLiteral("3"));
            setRaw(g + QStringLiteral("/last"),     QString::number(T - 500));
            setRaw(QStringLiteral("playstats/") + prof30 + QLatin1Char('/') + peerDev30
                       + QStringLiteral("/lastWrite"), QString::number(T - 500));
            ConsumptionStats::invalidate();
        };
        injAccum30(oldId30);
        CHECK(ConsumptionStats::get(oldId30).mediaSeconds == 120);         // premise: the fixture is readable
        CHECK(PlayStats::get(oldId30).totalSeconds == 600);
        remap30();
        CHECK(ConsumptionStats::get(newId30).mediaSeconds == 120);         // …and the records followed the id
        CHECK(PlayStats::get(newId30).totalSeconds == 600);
        const QJsonObject docAccum30 = serializeNow();

        wipe30();
        useProfile(prof30);
        injAccum30(oldId30);                                               // the peer, which has not remapped
        mergeDoc(docAccum30);
        // Unchanged, because nothing in the pulled document is fresher than the peer's own namespace. A remap
        // that stamped lastWrite would make the repair outrank the owner's genuine accrual, and mergeNamespaced
        // would replace the namespace WHOLESALE with the rekeyed copy — both readers would then answer 0 for
        // the id this device is still showing tiles for.
        CHECK(ConsumptionStats::get(oldId30).mediaSeconds == 120);
        CHECK(PlayStats::get(oldId30).totalSeconds == 600);

        wipe30();
        useProfile(QStringLiteral("cmA"));   // leave no §30 profile selected for anything appended after this
    }

    // ---- 31. TWO launcher ids onto ONE game: the fold order is the DATA's, not QHash's (issue #176) --------
    //
    // §30 pins WHAT the collision merge decides. This pins that the decision is not drawn from a hat. #166's
    // clear-wins rule is a pure function of the two blobs it is handed, which makes each PAIRWISE fold
    // deterministic — but it is not ASSOCIATIVE, and PcGameRemap folds N sources into one destination one at
    // a time. With a husk stamped strictly BETWEEN two live records the two fold orders answer differently:
    //
    //   husk FIRST : dest(live, oldest) + husk  -> the husk is newer and is a clear, so it CLEARS the older
    //                marks; the newest live record then merges into that clear and is all that survives.
    //   husk LAST  : dest(live, oldest) + newest live -> a plain union at the newest stamp, which the husk
    //                is now older than and cannot beat; the older marks SURVIVE.
    //
    // Both survivors carry the same stamp, so the fleet still converges (CloudMerge's lexical tie key picks
    // one side everywhere) — the defect is not a split brain, it is that WHICH answer a device reaches was
    // decided by QHash's iteration order, i.e. by a per-process hash seed. #176's fix is to fold in sorted
    // source-id order, so every device reaches one answer for the same reason instead of by convergence.
    //
    // THE FIXTURE PICKS ITS OWN IDS, AND THAT IS THE POINT. QHash's iteration order depends on a hash seed
    // that Qt randomises per process, so a pair of ids hard-coded here would agree with sorted order on
    // roughly half of all runs — and on those runs the unsorted code passes this section, which is a test
    // that proves nothing wearing a green tick. So the loop below SEARCHES the candidate ids for one pair
    // whose QHash order matches sorted order and one pair whose QHash order is the reverse of it, and runs
    // the collision on both. The second pair is the one that makes unsorted iteration fold the husk LAST and
    // return the other answer, on every seed, deterministically. It tunes the fixture to be HARDER, never to
    // agree: the asserted answer below is fixed by the id spelling ("epic:…" sorts before "steam:…"), not by
    // whatever the search found.
    {
        const QString prof31 = QStringLiteral("cm31");
        const QString dest31 = QStringLiteral("pcgame:foldorder");

        auto wipe31 = [&]() {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.remove(QStringLiteral("marks"));
            raw.remove(QStringLiteral("pcgameremap"));
            raw.sync();
            ItemMarks::invalidate();
        };
        auto inj31 = [&](const QString& id, bool hidden, const QString& completion,
                         const QStringList& tags, qint64 upd) {
            QJsonObject o;
            o.insert(QStringLiteral("hidden"), hidden);
            o.insert(QStringLiteral("completion"), completion);
            QJsonArray t; for (const QString& x : tags) t.append(x);
            o.insert(QStringLiteral("tags"), t);
            o.insert(QStringLiteral("updatedAt"), double(upd));
            setRaw(QStringLiteral("marks/") + prof31 + QStringLiteral("/items/") + md5(id), compactO(o));
            ItemMarks::invalidate();
        };
        auto rawAt31 = [&](const QString& id) {
            QSettings raw(iniPath, QSettings::IniFormat);
            return QJsonDocument::fromJson(
                       raw.value(QStringLiteral("marks/") + prof31 + QStringLiteral("/items/") + md5(id))
                          .toString().toUtf8()).object();
        };
        auto sig31 = [](const ItemMarks::Marks& m) {
            static const char* kComp[] = { "none", "inprogress", "finished", "abandoned", "planned" };
            QStringList tags = m.tags;
            tags.sort();                                   // the tag ORDER is not what this section is about
            return QStringLiteral("hidden=%1 completion=%2 tags=[%3]")
                       .arg(m.hidden ? 1 : 0)
                       .arg(QLatin1String(kComp[int(m.completion)]))
                       .arg(tags.join(QLatin1Char(',')));
        };

        // Which source id does a two-entry QHash hand out first? This is the order applyRemap's own table is
        // walked in on the unsorted build: applyRemap copies the caller's table into its `safe` hash and
        // remapMarks walks THAT, but both hold the same two keys under the same process seed, so both iterate
        // the same way. Read through the public container rather than assumed — the point of the search is to
        // find a pair where this disagrees with sorted order, and assuming which one that is defeats it.
        auto qhashFirst = [&](const QString& lo, const QString& hi) {
            QHash<QString, QString> h;
            h.insert(lo, dest31);
            h.insert(hi, dest31);
            return h.cbegin().key();
        };

        QString loSame, hiSame, loFlip, hiFlip;
        for (int i = 0; i < 64 && (loSame.isEmpty() || loFlip.isEmpty()); ++i)
        {
            // "epic:" < "steam:" by code unit, so `lo` is always the sorted-first source whatever i is.
            const QString lo = QStringLiteral("epic:fold%1").arg(i);
            const QString hi = QStringLiteral("steam:fold%1").arg(i);
            if (qhashFirst(lo, hi) == lo) { if (loSame.isEmpty()) { loSame = lo; hiSame = hi; } }
            else                          { if (loFlip.isEmpty()) { loFlip = lo; hiFlip = hi; } }
        }
        // Both must exist or the section is not testing what it says. 64 independent coin flips landing the
        // same way is a 1-in-2^63 event, so this firing means the hash stopped mixing, not bad luck.
        CHECK(!loSame.isEmpty());
        CHECK(!loFlip.isEmpty());

        // ONE collision, run end to end through the real entry point. The VERDICT is answered through
        // ItemMarks::get and never through row presence, for §25's reason; only the husk premise below drops
        // to the raw row, and it says there why it has to.
        //   destination : the OLDEST record, live (hidden + a verdict + a tag)
        //   `lo` source : the HUSK, stamped strictly between the other two
        //   `hi` source : the NEWEST record, live, carrying a different tag
        auto fold31 = [&](const QString& lo, const QString& hi, bool insertLoFirst) {
            wipe31();
            useProfile(prof31);
            inj31(dest31, true,  QStringLiteral("inProgress"), QStringList{QStringLiteral("alpha")}, T - 900);
            inj31(lo,     false, QStringLiteral("none"),     QStringList{},                        T - 600);
            inj31(hi,     false, QStringLiteral("finished"), QStringList{QStringLiteral("omega")}, T - 300);
            // Premises. Without these a mistyped key injects nothing, all four runs answer identically, and
            // the equality checks below pass over a fixture that is a fixed point of the function under test.
            CHECK(ItemMarks::get(dest31).tags.contains(QStringLiteral("alpha")));
            CHECK(ItemMarks::get(hi).tags.contains(QStringLiteral("omega")));
            // The husk's premise is read RAW, not through get(). ItemMarks deliberately renders a husk as no
            // marks (ensureCache's `if (isDefault(m)) continue`), so every reader-level question about it
            // answers the same as it would for a row that is not there — an assertion through get() could not
            // tell "the husk landed" from "the husk was never written", which is the one thing this premise
            // exists to say. The stamp is part of it: a husk at the wrong time is not the fixture.
            {
                const QJsonObject h = rawAt31(lo);
                CHECK(qint64(h.value(QStringLiteral("updatedAt")).toDouble()) == T - 600);
                CHECK(!h.value(QStringLiteral("hidden")).toBool()
                      && h.value(QStringLiteral("completion")).toString() == QStringLiteral("none")
                      && h.value(QStringLiteral("tags")).toArray().isEmpty());
            }

            QHash<QString, QString> t;
            if (insertLoFirst) { t.insert(lo, dest31); t.insert(hi, dest31); }
            else               { t.insert(hi, dest31); t.insert(lo, dest31); }
            pcgame::applyRemap(t);
            ItemMarks::invalidate();
            return sig31(ItemMarks::get(dest31));
        };

        const QString r31a = fold31(loSame, hiSame, /*insertLoFirst*/true);
        const QString r31b = fold31(loSame, hiSame, /*insertLoFirst*/false);
        const QString r31c = fold31(loFlip, hiFlip, /*insertLoFirst*/true);
        const QString r31d = fold31(loFlip, hiFlip, /*insertLoFirst*/false);

        // Four tables holding the same collision, built two ways and hashing two ways. One answer.
        CHECK(r31b == r31a);
        CHECK(r31c == r31a);
        CHECK(r31d == r31a);

        // …and it is the SORTED answer, NAMED. Equality alone would be satisfied by any rule that ignores the
        // data (fold nothing, clear everything), so the section says which fold actually ran: "epic:" sorts
        // first, so the husk goes in first and clears the destination's older marks — the newest live record
        // is then all that is left. `alpha` present, or `hidden` back, is the unsorted answer.
        CHECK(r31a == QStringLiteral("hidden=0 completion=finished tags=[omega]"));

        wipe31();
        useProfile(QStringLiteral("cmA"));   // leave no §31 profile selected for anything appended after this
    }

    // ---- 32. A no-op "confirm unchanged" must not RESTAMP and out-date a peer's genuine newer edit (#167) ---
    //
    // §29 pinned that a NON-clear must not be spelled as a CLEAR. This is the neighbouring hazard the #132
    // review found and left for its own issue, one notch up: on an item that DOES already carry a correction,
    // opening "Fix info", confirming a field UNCHANGED and pressing OK writes the same values back through
    // MetaOverrides::set(). set() used to stamp updatedAt on EVERY write, so that no-op confirm restamped the
    // whole record with `now` — and on the next merge the fresh stamp beat another device's genuinely newer
    // edit of the same item and silently deleted it. A non-event claiming to be newer than a real correction.
    // The fix stamps `now` only when the write actually CHANGES the stored content; a byte-equal write is a
    // no-op that leaves updatedAt alone (MetaOverrides::set, gated on contentEqual). The chosen semantics
    // (#167 option 1): a deliberate re-affirmation of unchanged values carries no weight — documented at the
    // gate in set() so the next reader does not restore the unconditional stamp.
    //
    // Read through MetaOverrides::get() — what the item SHOWS after the merge — for §25/§29's reason. The
    // local "already carries a correction, stamped BEFORE now" premise is established by MERGING a dated peer
    // document (CloudMerge writes the record with the peer's own timestamp), never by a live set() — a live
    // set() could only stamp `now`, which is the very value under test, and the read it triggers also refreshes
    // MetaOverrides' view of the just-merged record so the confirm-unchanged set() below sees it.
    {
        auto ovTitle32 = [&](const QString& key) { return MetaOverrides::get(key).title; };
        // A metaoverrides merge document carrying ONE item's correction at an explicit title + timestamp.
        auto doc32 = [&](const QString& key, const QString& title, qint64 ts) {
            QJsonObject blob;
            blob[QStringLiteral("title")]     = title;
            blob[QStringLiteral("updatedAt")] = double(ts);
            QJsonObject items; items.insert(md5(key), blob);
            QJsonObject root;  root.insert(QStringLiteral("metaoverrides"), items);
            return root;
        };

        // 32a. THE #167 DATA LOSS. Both devices corrected the item long ago (T-500). THIS device then opens
        // Fix info and confirms a field UNCHANGED — a byte-equal set(). The peer has since made a GENUINELY
        // LATER edit (T-100). That later edit is the only real change anyone made after the shared baseline, so
        // it must be what the merge keeps. On the broken build the no-op confirm restamped this record to `now`
        // (>> T-100) and the peer's later edit lost.
        wipeStores();
        const QString kA32 = QStringLiteral("igdb:32001");
        mergeDoc(doc32(kA32, QStringLiteral("Shared"), T - 500));         // the shared baseline, dated in the past
        CHECK(ovTitle32(kA32) == QStringLiteral("Shared"));
        MetaOverrides::Override affirm32; affirm32.title = QStringLiteral("Shared"); // exactly what is stored
        MetaOverrides::set(kA32, affirm32);                              // "confirm unchanged" -> must NOT restamp
        mergeDoc(doc32(kA32, QStringLiteral("Better"), T - 100));         // the peer's genuinely later edit
        CHECK(ovTitle32(kA32) == QStringLiteral("Better"));              // BROKEN: the restamp beats T-100 -> "Shared"

        // 32b. RAIL — a REAL edit still stamps `now` and still WINS. Same T-500 baseline; this device makes a
        // genuine change ("MineNow"), which must stamp `now` and out-date the peer's T-100 edit. This is the
        // load-bearing existing behaviour: the fix gates the stamp, it does not remove it. A mutant that turned
        // every set() into a no-op (contentEqual always true) passes 32a and fails HERE.
        wipeStores();
        const QString kB32 = QStringLiteral("igdb:32002");
        mergeDoc(doc32(kB32, QStringLiteral("Shared"), T - 500));
        CHECK(ovTitle32(kB32) == QStringLiteral("Shared"));
        MetaOverrides::Override real32; real32.title = QStringLiteral("MineNow");
        MetaOverrides::set(kB32, real32);                               // genuine change -> stamps now (~T)
        mergeDoc(doc32(kB32, QStringLiteral("Better"), T - 100));         // a peer edit, still older than now
        CHECK(ovTitle32(kB32) == QStringLiteral("MineNow"));            // the real edit is newest and wins

        // 32c. RAIL — a genuine CLEAR still husks and still WINS (unchanged from #132/§29c), through the
        // rewritten set(). The clear CHANGES the stored content (a title -> empty), so it stamps `now` and
        // beats the peer's older correction. A gate that mistook the clear for a no-op would leave the
        // correction in place and fail here — and would re-open #24.
        wipeStores();
        const QString kC32 = QStringLiteral("igdb:32003");
        mergeDoc(doc32(kC32, QStringLiteral("Shared"), T - 500));
        CHECK(ovTitle32(kC32) == QStringLiteral("Shared"));
        MetaOverrides::set(kC32, MetaOverrides::Override{});            // blank every field: a real clear
        CHECK(ovTitle32(kC32).isEmpty());
        mergeDoc(doc32(kC32, QStringLiteral("Better"), T - 100));         // a peer still holding a correction
        CHECK(ovTitle32(kC32).isEmpty());                              // the husk is newer -> the clear wins

        wipeStores();
        useProfile(QStringLiteral("cmA"));   // leave no §32 profile selected for anything appended after this
    }

    // ---- 33. Saved filter presets: newest-ts + delete tombstone, id-stable rename, routing (issue #184) ------
    //
    // #184 wires FilterPresetStore (the #63 saved game-library filters) into this document as a per-profile
    // {items, tombs} store, the same shape as favourites — union by a STABLE id keeping newest ts, a tombstone
    // at-or-after an item's ts suppressing it. What that shape has to buy here, and what this section pins:
    //   * a preset edited on one device and deleted on the other resolves by the newer ts (33a/33b);
    //   * THE #1 RAIL — a delete is NOT resurrected by a peer's older copy (33c): the tombstone out-dates the
    //     stale copy the peer still holds, so merging that copy back in leaves the preset gone;
    //   * a strictly-newer edit of the same id DOES beat an older delete (33d) — a delete is not a permanent ban;
    //   * a rename is an id-stable NAME edit, not a delete+add (33e): it folds onto the one id and leaves NO
    //     tombstone, so a peer's concurrent copy converges instead of the rename spawning a duplicate;
    //   * a non-event does not enter the outbound document (33f): an empty store serializes empty, and removing a
    //     name that isn't there tombstones nothing;
    //   * routing (33g): the store is NOT in the device-local carve-out (it SYNCS), IS a per-item store (owned by
    //     THIS document, off the heavy bundle), and a real preset shows up in the serialized merge document.
    // Every outcome is asserted THROUGH FilterPresetStore's accessor (list/exists/get), not by reading the row.
    {
        const QString p33 = QStringLiteral("cmP33");
        useProfile(p33);
        auto wipe33 = [&]() {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.remove(QStringLiteral("filterpresets"));
            raw.remove(QStringLiteral("deleted/filterpresets"));
            raw.sync();
        };
        // Inject a preset row with an explicit id/name/ts (empty filter — this section is about identity + time,
        // not the filter body), so the newest-ts fixtures are not fixed points of save()'s "stamp now".
        auto injPreset = [&](const QString& id, const QString& name, qint64 ts) {
            QJsonArray a; QJsonObject o;
            o[QStringLiteral("id")] = id; o[QStringLiteral("name")] = name;
            o[QStringLiteral("filter")] = QJsonObject{}; o[QStringLiteral("ts")] = double(ts);
            a.append(o);
            setRaw(QStringLiteral("filterpresets/") + p33 + QStringLiteral("/items"), compact(a));
        };
        // Read back THROUGH the store: does the active profile's preset list contain <name>?
        auto hasPreset = [&](const QString& name) { return FilterPresetStore::exists(name); };
        auto presetCount = [&]() { return FilterPresetStore::list().size(); };
        // The tombs the outbound document would carry for this profile (what "enters the document").
        auto docTombCount = [&]() {
            return serializeNow().value(QStringLiteral("presets")).toObject()
                       .value(p33).toObject().value(QStringLiteral("tombs")).toArray().size();
        };

        const QString U = QStringLiteral("11111111-1111-4111-8111-111111111111"); // a fixed preset id for the matrix

        // 33a. Edit newer than delete -> the edit survives. Remote deleted U (tombstone at T-500); local edited U
        // (T-100, newer). Newest wins -> present.
        wipe33(); injTomb(QStringLiteral("filterpresets/") + p33, U, T - 500); const QJsonObject r33a = serializeNow();
        wipe33(); injPreset(U, QStringLiteral("SNES backlog"), T - 100); mergeDoc(r33a);
        CHECK(hasPreset(QStringLiteral("SNES backlog")));                 // newer edit beats older delete
        CHECK(presetCount() == 1);

        // 33b. Delete newer than edit -> the delete wins. Remote deleted U (T-100); local's edit is older (T-500).
        wipe33(); injTomb(QStringLiteral("filterpresets/") + p33, U, T - 100); const QJsonObject r33b = serializeNow();
        wipe33(); injPreset(U, QStringLiteral("SNES backlog"), T - 500); mergeDoc(r33b);
        CHECK(!hasPreset(QStringLiteral("SNES backlog")));                // newer delete beats older edit
        CHECK(presetCount() == 0);

        // 33c. THE #1 RAIL — no resurrection. THIS device deleted U (tombstone at T-100, at-or-after the copy the
        // peer holds); the peer NEVER saw the delete and still serializes U at its original T-300. Merging the
        // peer's stale copy must leave U deleted, not bring it back. (Peer's document carries the item, no tomb.)
        wipe33(); injPreset(U, QStringLiteral("SNES backlog"), T - 300); const QJsonObject r33c = serializeNow();
        wipe33(); injTomb(QStringLiteral("filterpresets/") + p33, U, T - 100); mergeDoc(r33c);
        CHECK(!hasPreset(QStringLiteral("SNES backlog")));                // stale peer copy does NOT resurrect the delete
        CHECK(presetCount() == 0);

        // 33d. A strictly-newer edit of the same id DOES beat an older delete (a delete is not a ban). Remote
        // edited U at T-100; local deleted U at T-500 (older). The newer edit resurrects.
        wipe33(); injPreset(U, QStringLiteral("SNES backlog"), T - 100); const QJsonObject r33d = serializeNow();
        wipe33(); injTomb(QStringLiteral("filterpresets/") + p33, U, T - 500); mergeDoc(r33d);
        CHECK(hasPreset(QStringLiteral("SNES backlog")));                // strictly-newer edit wins over older delete
        CHECK(presetCount() == 1);

        // 33e. A RENAME is an id-stable name edit, NOT a delete+add. Device A creates a preset (random id) and
        // renames it X->Y through the real store; that write leaves NO tombstone (proving it is not a delete),
        // and it merges onto the SAME id a peer still holds under the old name, converging to ONE preset named Y
        // rather than a duplicate. Driven through FilterPresetStore so the id-stability is the store's, not the
        // fixture's.
        wipe33();
        FilterPresetStore::save({ QString(), QStringLiteral("X"), gamefilter::Filter{}, 0 }); // mints a random id
        CHECK(FilterPresetStore::rename(QStringLiteral("X"), QStringLiteral("Y")));            // id-stable rename
        const QString renamedId = FilterPresetStore::get(QStringLiteral("Y")).id;
        CHECK(!renamedId.isEmpty());
        CHECK(docTombCount() == 0);                                      // a rename tombstones NOTHING (not a delete+add)
        const QJsonObject r33e = serializeNow();                          // device A's document: Y at ~now, no tomb
        wipe33(); injPreset(renamedId, QStringLiteral("X"), T - 500); mergeDoc(r33e); // peer still holds the OLD name
        CHECK(hasPreset(QStringLiteral("Y")));                           // the rename won (newer)…
        CHECK(!hasPreset(QStringLiteral("X")));                          // …and did not leave the old name behind
        CHECK(presetCount() == 1);                                       // one preset, not a rename-spawned duplicate

        // 33f. Non-events do not enter the outbound document. An empty store serializes to an empty presets
        // section (no profile key), and removing a name that is not present tombstones nothing.
        wipe33();
        CHECK(serializeNow().value(QStringLiteral("presets")).toObject().isEmpty()); // empty store -> nothing carried
        injPreset(U, QStringLiteral("Keep"), T - 200);
        FilterPresetStore::remove(QStringLiteral("ghost"));             // a no-op remove: no such preset
        CHECK(hasPreset(QStringLiteral("Keep")));                       // the real one is untouched…
        CHECK(docTombCount() == 0);                                     // …and the no-op remove tombstoned nothing

        // 33g. ROUTING. The store SYNCS (never in the device-local carve-out), is owned by THIS merge document
        // (a per-item store, so it is off the heavy settings bundle), and a real preset appears in the serialized
        // merge document under its profile. The three predicates are the seam #184 had to get right.
        const QString pk = QStringLiteral("filterpresets/") + p33 + QStringLiteral("/items");
        CHECK(CloudSync::isDeviceLocalKey(pk) == false);                // NOT device-local -> it syncs
        CHECK(CloudSync::isPerItemStoreKey(pk) == true);                // owned by the merge document
        const QByteArray b33 = CloudSync::buildSettingsJson();
        CHECK(!QJsonDocument::fromJson(b33).object().contains(pk));     // …so it does NOT ride the heavy bundle
        wipe33(); injPreset(U, QStringLiteral("InDoc"), T - 100);
        const QJsonObject doc33 = serializeNow();
        const QJsonArray items33 = doc33.value(QStringLiteral("presets")).toObject().value(p33).toObject()
                                       .value(QStringLiteral("items")).toArray();
        bool inSyncedDoc = false;
        for (const QJsonValue& v : items33)
            if (v.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("InDoc")) inSyncedDoc = true;
        CHECK(inSyncedDoc);                                             // present in the synced merge document

        // 33h. THE RAIL, END TO END THROUGH THE STORE. 33c injects the tombstone raw; this drives the REAL
        // delete path — FilterPresetStore::remove must LEAVE a tombstone — so a peer that still holds the preset
        // (serialized before the delete, no tombstone) cannot resurrect it on merge. This is the assertion that
        // fails if remove() forgets to record the tombstone: the peer's stale copy would come straight back.
        wipe33();
        injPreset(U, QStringLiteral("Doomed"), T - 300);                 // the shared baseline both devices held
        const QJsonObject peer33 = serializeNow();                        // the peer's document: still has it, no tomb
        CHECK(hasPreset(QStringLiteral("Doomed")));
        FilterPresetStore::remove(QStringLiteral("Doomed"));            // the user deletes it on THIS device
        CHECK(!hasPreset(QStringLiteral("Doomed")));
        mergeDoc(peer33);                                               // fold the peer's stale copy back in
        CHECK(!hasPreset(QStringLiteral("Doomed")));                    // the store's tombstone keeps it deleted
        CHECK(presetCount() == 0);

        wipe33();
        useProfile(QStringLiteral("cmA"));   // leave no §33 profile selected for anything appended after this
    }

    // ---- 34. StoredUrl: the credential rule, as a pure function (issue #200) --------------------------------
    //
    // NO REAL CREDENTIAL APPEARS IN THIS FILE. Every token below is invented for the probe; the live finding
    // that motivated the issue is recorded only by its SHAPE (…/dld/<uuid>?token=<36 chars>).
    //
    // The rule these assertions defend: a value written into a store that SYNCS must not carry a credential,
    // and the only way to be sure of that without a list of parameter names nobody can maintain is to keep no
    // query at all on a stored playback location. Each case below is one way that went wrong or could.
    {
        using namespace StoredUrl;
        const QString tok = QStringLiteral("?token=nOtaReAlToKeN000000000000000000000");

        // 34a. A LOCAL PATH IS NOT A URL and comes back byte for byte — drive letter, spaces, backslashes,
        // dots in the name, all of it. This is the assertion that stops the fix from eating the ordinary case:
        // most recents are local files, and a sanitiser that "normalises" them breaks every one of them.
        const QString win = QStringLiteral("C:\\Users\\me\\My Videos\\a b.c d.mkv");
        CHECK(location(win) == win);
        CHECK(artwork(win) == win);
        CHECK(!isNetworkUrl(win));
        const QString unc = QStringLiteral("\\\\server\\share\\Some Show\\S01E01.mkv");
        CHECK(location(unc) == unc);
        const QString fileUrl = QStringLiteral("file:///C:/x/y.mkv");
        CHECK(location(fileUrl) == fileUrl);            // file:// is not a fetch scheme: nothing to strip
        CHECK(location(QString()) == QString());        // empty in, empty out (add() rejects it before this)

        // 34b. A url with NO query is untouched, and one WITH a query loses all of it — including a query that
        // is not a credential. That is the deliberate half of the rule: `?page=2` is dropped too, because the
        // alternative is a list of credential-shaped names that a debrid token (`token`), a Subsonic
        // credential (`u`/`t`/`s`) and a CDN signature (`sig`/`Expires`) would each have to be on, and the
        // next scheme would not be. Nothing of value is lost — a stored playback url is not re-signed.
        const QString bare = QStringLiteral("https://host.example/dld/6f1e/movie.mkv");
        CHECK(location(bare) == bare);
        CHECK(location(bare + QStringLiteral("?page=2")) == bare);
        CHECK(location(bare + tok) == bare);
        CHECK(carriesCredential(bare + tok));
        CHECK(!carriesCredential(bare));
        CHECK(location(location(bare + tok)) == location(bare + tok));   // idempotent

        // 34c. THE LIVE SHAPE, and the one that proves the strip is by DELIMITER and not by "the last thing
        // that looks like a file". The query holds an ENCODED '/' and a dot, so any rule reaching for the last
        // separator would cut inside the credential and keep half of it.
        const QString enc = QStringLiteral("https://nexus-236.cnam.example/dld/909107ff-1811-4952-fec4")
                            + QStringLiteral("?token=aaa%2Fbbb.ccc&exp=1799999999");
        CHECK(location(enc) == QStringLiteral("https://nexus-236.cnam.example/dld/909107ff-1811-4952-fec4"));
        CHECK(!location(enc).contains(QStringLiteral("aaa")));

        // 34d. USERINFO — the one credential outside the query with an unambiguous syntax — goes, and it goes
        // at the LAST '@' in the authority, not the first (a password may legally carry an encoded '@'; cutting
        // at the first would leave its tail sitting in the host position, which is worse than not cutting).
        // Two literal '@' — a password containing one, which servers do accept. lastIndexOf leaves the host;
        // indexOf would leave "ss@h.example" as the host, i.e. half the password still in the stored url.
        CHECK(location(QStringLiteral("http://user:p@ss@h.example/live/1.ts"))
              == QStringLiteral("http://h.example/live/1.ts"));
        CHECK(location(QStringLiteral("http://user:pa%40ss@h.example/live/1.ts"))
              == QStringLiteral("http://h.example/live/1.ts"));
        // …and a fragment goes with it, wherever it falls relative to the query.
        CHECK(location(QStringLiteral("https://h.example/p#frag?x=1")) == QStringLiteral("https://h.example/p"));
        CHECK(location(QStringLiteral("https://h.example/p?x=1#frag")) == QStringLiteral("https://h.example/p"));

        // 34e. A CREDENTIAL IN THE PATH IS DELIBERATELY LEFT ALONE, and the assertion says so out loud rather
        // than leaving the gap undocumented. The Xtream shape …/live/<user>/<pass>/<id>.ts is indistinguishable
        // from a content path by any rule that does not know the provider, and a heuristic that guessed would
        // mangle every legitimate stream url in the store — the path is what keeps a row identifiable and
        // re-openable. Userinfo (34d) is the exception because its syntax is not a guess.
        const QString xtream = QStringLiteral("http://iptv.example/live/someuser/somepass/12345.ts");
        CHECK(location(xtream) == xtream);              // NOT stripped — a stated limit, not an oversight

        // 34f. The scheme ALLOW-list. Streaming schemes are scrubbed (an IPTV source arrives on rtsp/rtmp as
        // readily as on http, with the credential in the same place); LAUNCHER uris are not, because their
        // query is a launch INSTRUCTION — com.epicgames.launcher://apps/X?action=launch — and rewriting it to
        // protect a value that was never a credential would break the relaunch.
        CHECK(isNetworkUrl(QStringLiteral("rtsp://h/x")) && isNetworkUrl(QStringLiteral("rtmps://h/x"))
              && isNetworkUrl(QStringLiteral("srt://h?passphrase=x")));
        const QString epic = QStringLiteral("com.epicgames.launcher://apps/Fortnite?action=launch&silent=true");
        CHECK(!isNetworkUrl(epic));
        CHECK(location(epic) == epic);
        const QString steam = QStringLiteral("steam://rungameid/440");
        CHECK(location(steam) == steam);
        // A TITLE that merely contains "://" is prose, not a url: the scheme test requires a real scheme token
        // before it. Without this a film called "Re: //Slashers" would be truncated on its way into recents.
        CHECK(!isNetworkUrl(QStringLiteral("Re: //Slashers")));
        CHECK(label(QStringLiteral("Re: //Slashers")) == QStringLiteral("Re: //Slashers"));

        // 34g. ARTWORK TAKES THE OTHER RULE, and the live install is why. A real cover url on that machine is
        // https://books.google.com/books/content?id=…&printsec=frontcover&img=1&zoom=1&edge=curl — its query
        // IS the image. location()'s rule would blank the cover of every Google Books row to protect a value
        // that was never minted by the stream-signing path. So artwork() keeps the query and drops only the
        // parameters whose NAME says credential.
        const QString gb = QStringLiteral("https://books.google.com/books/content")
                           + QStringLiteral("?id=506EEQAAQBAJ&printsec=frontcover&img=1&zoom=1&edge=curl");
        CHECK(artwork(gb) == gb);                        // a real artwork query survives intact
        CHECK(artwork(QStringLiteral("https://h/cover.png?w=300&token=nope&h=300"))
              == QStringLiteral("https://h/cover.png?w=300&h=300"));   // …minus the credential-named one
        // The Subsonic triple: u (user), t (the salted token), s (the salt). The one artwork url in this tree
        // known to carry a credential, and the reason single letters are on the list at all.
        CHECK(artwork(QStringLiteral("https://music.example/rest/getCoverArt?id=al-1&u=bob&t=abcd&s=efgh&f=json"))
              == QStringLiteral("https://music.example/rest/getCoverArt?id=al-1&f=json"));
        CHECK(artwork(QStringLiteral("https://h/c.png?token=x")) == QStringLiteral("https://h/c.png")); // nothing left -> no '?'
        CHECK(artwork(QStringLiteral("http://u:p@h/c.png")) == QStringLiteral("http://h/c.png"));       // userinfo still goes
        CHECK(isCredentialParam(QStringLiteral("access_token")) && isCredentialParam(QStringLiteral("Signature"))
              && !isCredentialParam(QStringLiteral("zoom")));

        // 34h. THE completeBaseName TRAP, which is what #193 hit and what this generalises. QFileInfo splits a
        // string on the last '/' and then on the last '.', so for a url it returns a slice of the QUERY — and
        // whether that slice contains the token depends on where the last dot happens to fall, i.e. on the
        // server's id format. The live resume store held exactly the resulting shape: "<uuid>?token=<36>",
        // which has no scheme, so nothing that reasons about urls would ever have cleaned it.
        const QString slice = QStringLiteral("909107ff-1811-4952?token=nOtaReAlToKeN0000000000000000000000");
        CHECK(!isNetworkUrl(slice));                     // it is not a url — this is why label() exists
        CHECK(label(slice) == QStringLiteral("909107ff-1811-4952"));
        CHECK(label(QStringLiteral("Who Framed Roger Rabbit?")) == QStringLiteral("Who Framed Roger Rabbit?"));
        CHECK(label(QStringLiteral("Whose Line Is It Anyway? The Movie"))
              == QStringLiteral("Whose Line Is It Anyway? The Movie"));   // a query tail is `name=`, not prose
        CHECK(label(bare + tok) == bare);                // a label that is a url outright loses its query too

        // 34i. title(): a supplied title wins (scrubbed), a network url with none is labelled from its PATH
        // and never from its query, and a file keeps the completeBaseName() every call site already used.
        CHECK(title(QStringLiteral("Dungeon Crawler Carl"), bare + tok) == QStringLiteral("Dungeon Crawler Carl"));
        CHECK(title(QString(), bare + tok) == QStringLiteral("movie.mkv"));
        // …and a url whose ENTIRE content past the host is the credential still yields an honest label rather
        // than an empty one: the host. (The row keeps a name, so it is never a blank tile.)
        CHECK(title(QString(), QStringLiteral("https://h.example/") + tok) == QStringLiteral("h.example"));
        // The base-name half is HOST-SPECIFIC and only here: `title()` ends in QFileInfo::completeBaseName(),
        // and '\' is a path separator on Windows but an ordinary, legal character in a POSIX file name — so
        // on Linux `win` is one long file name and its complete base name is all of it but the ".mkv". Both
        // spellings assert the same rule; neither host is left without the assertion (issue #205). Every
        // other `win` check above is separator-blind and needs no guard.
#ifdef Q_OS_WIN
        CHECK(title(QString(), win) == QStringLiteral("a b.c d"));
#else
        CHECK(title(QString(), win) == QStringLiteral("C:\\Users\\me\\My Videos\\a b.c d"));
        CHECK(title(QString(), QStringLiteral("/home/me/My Videos/a b.c d.mkv")) == QStringLiteral("a b.c d"));
#endif
        CHECK(title(slice, win) == QStringLiteral("909107ff-1811-4952"));  // a caller's OWN completeBaseName slice
    }

    // ---- 35. #200 end to end: the writers, the sweep, and what a peer can send --------------------------------
    {
        useProfile(QStringLiteral("r35"));
        const QString tokQ = QStringLiteral("?token=nOtaReAlToKeN000000000000000000000");
        const QString signedUrl = QStringLiteral("https://store-034.example/zip/aa9a74fd-2bbb-470b") + tokQ;
        const QString cleanUrl  = QStringLiteral("https://store-034.example/zip/aa9a74fd-2bbb-470b");
        auto rawVal = [&](const QString& k) {
            QSettings raw(iniPath, QSettings::IniFormat); return raw.value(k).toString();
        };
        const QString recKey = QStringLiteral("recent/r35/items");

        // 35a. THE WRITE PATH. Playing an addon-resolved stream records the row, and the row holds no token —
        // in ANY of its four url-shaped fields. Asserted against the RAW INI, not the struct: the ini is the
        // artefact that syncs and the artefact a bug report attaches, so that is where the absence has to hold.
        wipeStores();
        {
            RecentItem it;
            it.path = signedUrl;
            it.key  = QStringLiteral("openlibrary:/works/OL24848193W");
            it.title = QStringLiteral("The Gate of the Feral Gods");
            it.thumb = QStringLiteral("https://covers.example/b/id/15232581-M.jpg") + tokQ;
            it.kind = QStringLiteral("audio"); it.ts = T - 100;
            RecentStore::add(it);
        }
        CHECK(!rawVal(recKey).contains(QStringLiteral("token=")));      // nothing in the stored row carries it
        CHECK(rawVal(recKey).contains(cleanUrl));                       // …and the location itself survived
        {
            const QVector<RecentItem> got = RecentStore::list();
            CHECK(got.size() == 1);
            // THE ROW STILL RE-OPENS. Every input openRecent dispatches on is intact: the kind (which picks
            // the route), the key (which is the identity, and what resume is keyed by), and a url that is
            // still a url — `path.contains("://")` is the test openRecent makes, and it still passes, so the
            // entry routes to the stream player rather than to the "file can no longer be found" branch.
            CHECK(got[0].path == cleanUrl);
            CHECK(got[0].path.contains(QStringLiteral("://")));
            CHECK(got[0].key == QStringLiteral("openlibrary:/works/OL24848193W"));
            CHECK(got[0].kind == QStringLiteral("audio"));
            CHECK(got[0].title == QStringLiteral("The Gate of the Feral Gods"));
            CHECK(got[0].thumb == QStringLiteral("https://covers.example/b/id/15232581-M.jpg"));
            CHECK(RecentStore::relaunchFor(got[0].kind) == RecentStore::Relaunch::Audio);
        }
        // …and it is still removable by the url the caller has in hand, tokenised or not.
        RecentStore::remove(signedUrl);
        CHECK(RecentStore::list().isEmpty());

        // 35b. A KEYLESS catalog stream recorded the URL AS ITS KEY (MainWindow's `rkey = item.id.isEmpty() ?
        // url : item.id`), which put a second copy of the token in the same row under a different field name.
        // The identity survives as the scrubbed url, so the entry is still there and still de-dups.
        wipeStores();
        {
            RecentItem it;
            it.path = signedUrl; it.key = signedUrl;     // both fields, both tokenised
            it.title = QStringLiteral("Obsession"); it.kind = QStringLiteral("video"); it.ts = T - 90;
            RecentStore::add(it);
        }
        CHECK(!rawVal(recKey).contains(QStringLiteral("token=")));
        CHECK(RecentStore::list().size() == 1);
        CHECK(RecentStore::list().at(0).key == cleanUrl);   // an identity remains — the row is not orphaned

        // 35c. A LOCAL FILE IS STORED EXACTLY AS IT WAS. The security fix must not touch the common case, and
        // this is the assertion that fails if a future "tidy up the path" creeps into the scrub.
        wipeStores();
        {
            RecentItem it;
            it.path = QStringLiteral("C:/EverythingBox-app/roms/nes/Super Mario Bros. 3.7z");
            it.title = QStringLiteral("Super Mario Bros. 3"); it.kind = QStringLiteral("game");
            it.key = QStringLiteral("igdb:1068"); it.ts = T - 80;
            RecentStore::add(it);
        }
        CHECK(RecentStore::list().at(0).path
              == QStringLiteral("C:/EverythingBox-app/roms/nes/Super Mario Bros. 3.7z"));

        // 35d. NO TOKEN LEAVES THIS DEVICE. The whole issue is that "recent/" is a per-item store owned by the
        // merge document, so the row does not merely sit in the ini — it is uploaded. Assert against the
        // serialized document itself, and against the routing predicates that put it there.
        wipeStores();
        {
            RecentItem it; it.path = signedUrl; it.key = QStringLiteral("tmdb:movie:1339713");
            it.title = QStringLiteral("Obsession"); it.kind = QStringLiteral("video"); it.ts = T - 70;
            RecentStore::add(it);
        }
        CHECK(CloudSync::isPerItemStoreKey(recKey) == true);    // owned by THIS document (unchanged by #200)
        CHECK(CloudSync::isDeviceLocalKey(recKey) == false);    // and NOT carved out — recents still sync
        CHECK(!compactO(serializeNow()).contains(QStringLiteral("token=")));

        // 35e. WHAT A PEER SENDS IS SCRUBBED ON THE WAY IN. A device still running an older build serializes
        // the signed url it played; mergeRecent writes the winning row straight into this ini, so a fix
        // confined to the writer would be undone by the first sync with any un-upgraded device. Note the
        // remote row and the local row are the SAME item: scrubbing before the identity is taken is what
        // collapses them to one entry instead of leaving a tokenised twin beside the clean one.
        wipeStores();
        {
            QJsonObject peerRow;
            peerRow.insert(QStringLiteral("path"), signedUrl);
            peerRow.insert(QStringLiteral("title"), QStringLiteral("909107ff?token=nOtaReAlToKeN0000000"));
            peerRow.insert(QStringLiteral("thumb"), QStringLiteral("https://covers.example/c.jpg?token=nope"));
            peerRow.insert(QStringLiteral("kind"), QStringLiteral("audio"));
            peerRow.insert(QStringLiteral("key"), QStringLiteral("openlibrary:/works/OL24848193W"));
            peerRow.insert(QStringLiteral("ts"), double(T - 60));
            QJsonArray peerList; peerList.append(peerRow);
            QJsonObject recentSec; recentSec.insert(QStringLiteral("r35/items"), compact(peerList));
            QJsonObject doc; doc.insert(QStringLiteral("recent"), recentSec);
            mergeDoc(doc);
        }
        CHECK(!rawVal(recKey).contains(QStringLiteral("token=")));
        {
            const QVector<RecentItem> got = RecentStore::list();
            CHECK(got.size() == 1);                                  // the peer's row arrived…
            CHECK(got[0].path == cleanUrl);                          // …cleaned
            CHECK(got[0].title == QStringLiteral("909107ff"));       // including the completeBaseName slice
            CHECK(got[0].thumb == QStringLiteral("https://covers.example/c.jpg"));
        }

        // 35e2. THE PEER'S TOMBSTONES COME IN THE SAME WAY. A keyless row's identity IS its url, so a peer's
        // removal of one arrives filed under the TOKENISED spelling — which would write a credential into
        // deleted/* (a per-item store, so it syncs straight back out) and, worse, name an entry the scrubbed
        // list no longer contains, so the peer's removal would be silently ignored. Both halves asserted: the
        // stored identity carries no token, AND the removal still lands on the entry.
        wipeStores();
        {
            RecentItem it;                                  // this device has the item, keyless
            it.path = signedUrl; it.title = QStringLiteral("Pasted link");
            it.kind = QStringLiteral("video"); it.ts = T - 600;
            RecentStore::add(it);
            CHECK(RecentStore::list().size() == 1);
            QJsonObject tomb;                               // …the peer removed it, at a later second
            tomb.insert(QStringLiteral("key"), signedUrl);
            tomb.insert(QStringLiteral("ts"), double(T - 300));
            QJsonArray tombs; tombs.append(tomb);
            QJsonObject tombSec; tombSec.insert(QStringLiteral("r35"), tombs);
            QJsonObject doc; doc.insert(QStringLiteral("recentTombs"), tombSec);
            mergeDoc(doc);
        }
        CHECK(RecentStore::list().isEmpty());               // the peer's removal was understood…
        for (const Tombstones::Entry& e : Tombstones::all(QStringLiteral("recent/r35")))
            CHECK(!e.key.contains(QStringLiteral("token="))); // …without importing the credential with it

        // 35e3. A PEER'S RESUME TITLE, same entrance. resume/<hash>/title is the one field of a resume row kept
        // in the clear, and a peer on an older build derives it with completeBaseName() — a slice of the query.
        // The POSITION is what the row is for and must arrive untouched; only the label is scrubbed.
        wipeStores();
        {
            QJsonObject row;
            row.insert(QStringLiteral("pos"), 742.5);
            row.insert(QStringLiteral("dur"), 3600.0);
            row.insert(QStringLiteral("ts"), double(T - 50));
            row.insert(QStringLiteral("title"), QStringLiteral("909107ff-1811?token=nOtaReAlToKeN00000"));
            QJsonObject resumeSec; resumeSec.insert(QStringLiteral("869ea9c7b3"), row);
            QJsonObject doc; doc.insert(QStringLiteral("resume"), resumeSec);
            mergeDoc(doc);
        }
        CHECK(rawVal(QStringLiteral("resume/869ea9c7b3/title")) == QStringLiteral("909107ff-1811"));
        CHECK(rawVal(QStringLiteral("resume/869ea9c7b3/pos")).toDouble() == 742.5);

        // 35e4. AND THE LOCAL WRITER OF THAT FIELD, generalised past #193's http/https pair. A remote track is
        // titled from the queue's display title on EVERY network scheme — an IPTV source arrives on rtsp as
        // readily as on http, with the credential in the same place — and never from the url's own text.
        wipeStores();
        {
            const QString rtsp = QStringLiteral("rtsp://iptv.example/ch/9?token=nOtaReAlToKeN000000");
            PlaybackSession s;
            s.setQueue({ rtsp }, 0, { QStringLiteral("BBC One HD") });
            s.beginResume(rtsp);
            s.setDuration(3600.0);
            s.setPosition(120.0);
            s.persistResume();
        }
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            bool sawTitle = false;
            raw.beginGroup(QStringLiteral("resume"));
            const QStringList groups = raw.childGroups();
            raw.endGroup();
            for (const QString& g : groups)
            {
                const QString t = raw.value(QStringLiteral("resume/") + g + QStringLiteral("/title")).toString();
                if (t.isEmpty()) continue;
                CHECK(!t.contains(QStringLiteral("token=")));
                if (t == QStringLiteral("BBC One HD")) sawTitle = true;
            }
            CHECK(sawTitle);   // the display title, not a slice of the url
        }

        // 35e5. THE CONSUMPTION-STATS TITLE, at its own writer. The reader seams title an item from its path
        // and the media seam from the queue's display title, so a streamed item's title is a url or a slice of
        // one — and it lands in stats/*, another per-item store. The counters must survive untouched; only the
        // label is scrubbed. Driven through the public writer with a deliberately raw title, because the
        // in-app callers are now scrubbed upstream and would assert nothing about THIS store's own guard.
        {
            const QString sk = QStringLiteral("stats-probe-200");
            ConsumptionStats::addMediaSeconds(sk, QStringLiteral("audio"), 42,
                                              QStringLiteral("aa9a74fd?token=nOtaReAlToKeN00000000"));
            const ConsumptionStats::Totals got = ConsumptionStats::get(sk);
            CHECK(got.mediaSeconds == 42);                                   // the accrual is the user's data
            CHECK(got.title == QStringLiteral("aa9a74fd"));                  // …the label is not a credential
            CHECK(!got.title.contains(QStringLiteral("token=")));
        }

        // 35f. THE SWEEP OF WHAT IS ALREADY STORED. Every install that has ever played one of these still
        // holds the token today; a fix that only guards new writes leaves the problem exactly where it is.
        // Seeded RAW — as an older build wrote it — then cleaned in place, and the entry still usable after.
        wipeStores();
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.remove(QLatin1String(CredentialScrub::stampKey()));   // un-stamp: this fixture must be swept
            raw.sync();
        }
        {
            QJsonObject a, b;
            a.insert(QStringLiteral("path"), signedUrl);
            a.insert(QStringLiteral("title"), QStringLiteral("Dungeon Crawler Carl"));
            a.insert(QStringLiteral("kind"), QStringLiteral("audio"));
            a.insert(QStringLiteral("key"), QStringLiteral("openlibrary:/works/OL24593432W"));
            a.insert(QStringLiteral("ts"), double(T - 500));
            b.insert(QStringLiteral("path"), QStringLiteral("C:/music/Probe Artist/01 Track One.wav"));
            b.insert(QStringLiteral("title"), QStringLiteral("01 Track One"));
            b.insert(QStringLiteral("kind"), QStringLiteral("audio"));
            b.insert(QStringLiteral("ts"), double(T - 600));
            QJsonArray seed; seed.append(a); seed.append(b);
            setRaw(recKey, compact(seed));
            setRaw(QStringLiteral("resume/869ea9c7b3/title"),
                   QStringLiteral("909107ff-1811-4952?token=nOtaReAlToKeN000000000000000000000"));
            setRaw(QStringLiteral("resume/869ea9c7b3/pos"), QStringLiteral("742.5"));
            QJsonObject blob;
            blob.insert(QStringLiteral("mediaSeconds"), 900.0);
            blob.insert(QStringLiteral("title"), QStringLiteral("aa9a74fd?token=nOtaReAlToKeN00000000"));
            setRaw(QStringLiteral("stats/r35/devX/items/deadbeef"), compactO(blob));
            // A keyless row's tombstone identity IS its url, so a removal filed one under the tokenised
            // spelling — a credential in deleted/*, which syncs like everything else here.
            injTomb(QStringLiteral("recent/r35"), signedUrl, T - 400);
        }
        CHECK(CredentialScrub::run() == true);                        // it found work to do…
        CHECK(!rawVal(recKey).contains(QStringLiteral("token=")));
        CHECK(rawVal(QStringLiteral("resume/869ea9c7b3/title")) == QStringLiteral("909107ff-1811-4952"));
        CHECK(rawVal(QStringLiteral("resume/869ea9c7b3/pos")) == QStringLiteral("742.5")); // the POSITION is untouched
        CHECK(!rawVal(QStringLiteral("stats/r35/devX/items/deadbeef")).contains(QStringLiteral("token=")));
        {
            QJsonObject blob = QJsonDocument::fromJson(
                rawVal(QStringLiteral("stats/r35/devX/items/deadbeef")).toUtf8()).object();
            CHECK(qint64(blob.value(QStringLiteral("mediaSeconds")).toDouble()) == 900); // counters survive
            CHECK(blob.value(QStringLiteral("title")).toString() == QStringLiteral("aa9a74fd"));
        }
        {
            // NOTHING WAS DROPPED. Both rows are still there, in order, and the cleaned one still carries the
            // key it re-resolves by — the sweep is a rewrite, never a deletion.
            const QVector<RecentItem> got = RecentStore::list();
            CHECK(got.size() == 2);
            CHECK(got[0].path == cleanUrl);
            CHECK(got[0].key == QStringLiteral("openlibrary:/works/OL24593432W"));
            CHECK(got[1].path == QStringLiteral("C:/music/Probe Artist/01 Track One.wav"));   // local, verbatim
        }
        {
            // The tombstone was RE-FILED, not merely emptied: the credential is gone from deleted/* AND the
            // removal still names the identity the merge holds, so a peer cannot resurrect the entry.
            bool sawClean = false;
            for (const Tombstones::Entry& e : Tombstones::all(QStringLiteral("recent/r35")))
            {
                CHECK(!e.key.contains(QStringLiteral("token=")));
                if (e.key == cleanUrl) { sawClean = true; CHECK(e.ts == T - 400); }  // …at its faithful ts
            }
            CHECK(sawClean);
        }
        // 35g. STAMPED AND IDEMPOTENT (PlaylistStore::migrateToCategories' shape), and the stamp is a REAL
        // gate rather than a decoration: a tokenised row injected raw AFTER the sweep is left alone, because
        // the sweep has already run on this install and is not a permanent background scrubber. That is the
        // design, not a gap — the writers refuse the credential at every in-app entrance and CloudMerge
        // refuses it at the sync entrance, so the only way to get one in past this point is to hand-edit the
        // ini, and a one-shot that re-scans every list on every startup would be paying for that for ever.
        {
            QJsonObject late;
            late.insert(QStringLiteral("path"), signedUrl);
            late.insert(QStringLiteral("title"), QStringLiteral("Late"));
            late.insert(QStringLiteral("kind"), QStringLiteral("video"));
            late.insert(QStringLiteral("ts"), double(T - 10));
            QJsonArray arr; arr.append(late);
            setRaw(recKey, compact(arr));
        }
        CHECK(CredentialScrub::run() == false);               // stamped: it does not look again…
        CHECK(rawVal(recKey).contains(QStringLiteral("token=")));  // …so the hand-injected row is untouched
        setRaw(recKey, QString());                            // (put the fixture back for the row assertions)
        {
            QJsonObject a, b;
            a.insert(QStringLiteral("path"), cleanUrl);
            a.insert(QStringLiteral("key"), QStringLiteral("openlibrary:/works/OL24593432W"));
            a.insert(QStringLiteral("kind"), QStringLiteral("audio"));
            a.insert(QStringLiteral("ts"), double(T - 500));
            b.insert(QStringLiteral("path"), QStringLiteral("C:/music/Probe Artist/01 Track One.wav"));
            b.insert(QStringLiteral("kind"), QStringLiteral("audio"));
            b.insert(QStringLiteral("ts"), double(T - 600));
            QJsonArray arr; arr.append(a); arr.append(b); setRaw(recKey, compact(arr));
        }
        CHECK(RecentStore::list().size() == 2);
        // The stamp is DEVICE-LOCAL: "this install's ini has been cleaned" is a fact about this install, and a
        // synced stamp would tell a machine that has never run the sweep that it had.
        CHECK(CloudSync::isDeviceLocalKey(QLatin1String(CredentialScrub::stampKey())) == true);
        CHECK(CloudSync::isPerItemStoreKey(QLatin1String(CredentialScrub::stampKey())) == false);

        wipeStores();
        useProfile(QStringLiteral("cmA"));
    }

    // ---- 36. #203: an IDENTITY that is a signed url --------------------------------------------------------
    //
    // NO REAL CREDENTIAL APPEARS HERE EITHER. The user is "listener", the token and salt are invented, and the
    // server is music.example.test.
    //
    // The rule these assertions defend, and it is NOT §34's: a stored playback LOCATION can afford to lose its
    // whole query, because a signed link has expired by the time anyone clicks it. An IDENTITY cannot — every
    // track on a Subsonic server streams from the same endpoint, so location()'s rule maps a fifty-track
    // playlist onto fifty copies of one string. The right answer is to store the track's durable name instead,
    // and to keep whatever distinguishes a row that has no durable name.
    {
        useProfile(QStringLiteral("r36"));
        wipeStores();
        auto rawVal36 = [&](const QString& k) {
            QSettings raw(iniPath, QSettings::IniFormat); return raw.value(k).toString();
        };
        const QString root  = QStringLiteral("https://music.example.test");
        const QString creds = QStringLiteral("u=listener&t=nOtaReAlToKeN0000000000000000000&s=fAkeSaLt")
                              + QStringLiteral("&v=1.16.1&c=EverythingBox&f=json");
        auto streamOf = [&](const QString& host, const QString& id) {
            return host + QStringLiteral("/rest/stream.view?") + creds + QStringLiteral("&id=") + id;
        };
        SubsonicServer srv;
        srv.name = QStringLiteral("Probe Navidrome");
        srv.url  = root;
        srv.username = QStringLiteral("listener");
        srv.password = QStringLiteral("fake-probe-password");
        const QString serverId = SubsonicServerStore::add(srv);
        CHECK(!serverId.isEmpty());
        const QVector<QPair<QString, QString>> roots = StoredIdentity::serverRoots();
        CHECK(roots.size() == 1 && roots.first().first == serverId && roots.first().second == root);

        // The ids are recomputed from Subsonic::qualify rather than spelled out, so a change to the id format
        // moves this probe with it instead of silently asserting a stale shape.
        const QString idA = Subsonic::qualify(serverId, Subsonic::Kind::Track, QStringLiteral("tr-1"));
        const QString idB = Subsonic::qualify(serverId, Subsonic::Kind::Track, QStringLiteral("tr-2"));
        CHECK(!idA.isEmpty() && idA != idB && Subsonic::isQualified(idA));

        // 36a. THE READER, on its own. It is the half that makes a MIGRATION possible at all: a row written by
        // an older build carries nothing but the url, so the id has to come back out of it.
        CHECK(Subsonic::trackIdFromStreamUrl(streamOf(root, QStringLiteral("tr-1")), roots) == idA);
        CHECK(Subsonic::trackIdFromStreamUrl(streamOf(root, QStringLiteral("tr-2")), roots) == idB);
        // Every refusal, and each is a way a wrong id could have been minted:
        CHECK(Subsonic::trackIdFromStreamUrl(streamOf(QStringLiteral("https://other.example"),
                                                      QStringLiteral("tr-1")), roots).isEmpty()); // not our server
        CHECK(Subsonic::trackIdFromStreamUrl(root + QStringLiteral("/rest/getCoverArt?") + creds
                                             + QStringLiteral("&id=al-1"), roots).isEmpty());     // not a track
        CHECK(Subsonic::trackIdFromStreamUrl(root + QStringLiteral("/rest/stream.view?") + creds,
                                             roots).isEmpty());                                   // no id
        CHECK(Subsonic::trackIdFromStreamUrl(streamOf(root, QStringLiteral("tr-1")), {}).isEmpty()); // no servers
        CHECK(Subsonic::trackIdFromStreamUrl(QStringLiteral("C:\\music\\01.flac"), roots).isEmpty());
        CHECK(Subsonic::trackIdFromStreamUrl(idA, roots).isEmpty());   // an id is not a url: no re-entry
        // An EXACT root match, not a prefix and not a host match. A look-alike host that merely starts with the
        // configured root would otherwise file the row under a server it does not belong to.
        CHECK(Subsonic::trackIdFromStreamUrl(streamOf(root + QStringLiteral(".evil.example"),
                                                      QStringLiteral("tr-1")), roots).isEmpty());
        CHECK(Subsonic::trackIdFromStreamUrl(streamOf(root + QStringLiteral("/sub"),
                                                      QStringLiteral("tr-1")), roots).isEmpty());
        // The query cannot fake the endpoint: the test is on the part BEFORE the '?'.
        CHECK(Subsonic::trackIdFromStreamUrl(root + QStringLiteral("/rest/getCoverArt?x=/rest/stream.view&id=1"),
                                             roots).isEmpty());
        // …and the endpoint has to be THERE. A bare server root carrying an `id` is not a stream url, and
        // accepting it would let any link to that host be filed as one of its tracks.
        CHECK(Subsonic::trackIdFromStreamUrl(root + QStringLiteral("?id=tr-1"), roots).isEmpty());

        // 36b. THE RULE. Three steps, each a fallback for the one above rather than an alternative.
        {
            using StoredIdentity::resolve;
            const QString sA = streamOf(root, QStringLiteral("tr-1"));
            const QString local = QStringLiteral("C:\\Users\\me\\Music\\Radiohead\\01 Airbag.flac");

            // A LOCAL FILE IS BYTE-IDENTICAL, with or without a hint. This is the no-regression assertion: a
            // local queue saved as a playlist must produce exactly the rows it produced before this issue.
            CHECK(resolve(local, QString(), roots) == local);
            CHECK(resolve(local, local, roots) == local);     // the hint IS the play path -> ignored, not doubled
            CHECK(resolve(QString(), QString(), roots).isEmpty());   // empty in, empty out

            // The hint wins when there is one — that is the running queue's own table (musicQueueIndexPaths_).
            CHECK(resolve(sA, idA, {}) == idA);               // …and needs no server list to be honoured
            // …but a hint cannot smuggle a credential in. A caller that hands us a url as a "durable name" gets
            // it scrubbed, exactly as StoredUrl::title refuses a caller's own completeBaseName().
            CHECK(!resolve(sA, streamOf(root, QStringLiteral("tr-9")), {}).contains(QStringLiteral("t=nOtaReAl")));

            // No hint: the id comes back out of the url.
            CHECK(resolve(sA, QString(), roots) == idA);

            // NO HINT AND NO SERVER — the row that cannot be named. It SURVIVES, credential-free, and this is
            // the pair of assertions the whole rule turns on: two different tracks stay two different rows,
            // where location()'s rule would make them the same string and take the playlist apart.
            const QString fbA = resolve(streamOf(root, QStringLiteral("tr-1")), QString(), {});
            const QString fbB = resolve(streamOf(root, QStringLiteral("tr-2")), QString(), {});
            CHECK(!fbA.contains(QStringLiteral("nOtaReAlToKeN")) && !fbA.contains(QStringLiteral("u=listener"))
                  && !fbA.contains(QStringLiteral("fAkeSaLt")));
            CHECK(fbA != fbB);                                                     // still distinguishable
            CHECK(StoredUrl::location(streamOf(root, QStringLiteral("tr-1")))
                  == StoredUrl::location(streamOf(root, QStringLiteral("tr-2")))); // …which location() is not
            CHECK(fbA.contains(QStringLiteral("id=tr-1")));                        // and still re-identifiable:
            CHECK(Subsonic::trackIdFromStreamUrl(fbA, roots) == idA);              // a later pass finishes the job
            // An addon-signed url that is nobody's stream endpoint takes the same road and keeps its own name.
            const QString audiobook = QStringLiteral("https://store-034.example/zip/aa9a74fd-2bbb-470b")
                                      + QStringLiteral("?token=nOtaReAlToKeN000000000000000000000");
            const QString fbC = resolve(audiobook, QString(), roots);
            CHECK(fbC == QStringLiteral("https://store-034.example/zip/aa9a74fd-2bbb-470b"));
            CHECK(!fbC.isEmpty());
            // Idempotent, and never empty for a non-empty input (a row with no identity is a row no reader
            // can reach — which is the one outcome worse than the leak).
            for (const QString& s : { sA, local, audiobook, idA, QStringLiteral("steam:440") })
            {
                const QString once = resolve(s, QString(), roots);
                CHECK(!once.isEmpty());
                CHECK(resolve(once, QString(), roots) == once);
            }
        }

        // 36c. THE SWEEP, over a store seeded exactly as an older build wrote it.
        const QString plKey36 = QStringLiteral("playlists/r36/items");
        auto entry = [&](const QString& id, const QString& path, const QString& title) {
            QJsonObject e;
            e.insert(QStringLiteral("itemId"), id);
            if (!path.isEmpty()) e.insert(QStringLiteral("path"), path);
            e.insert(QStringLiteral("title"), title);
            e.insert(QStringLiteral("type"), QStringLiteral("audio"));
            e.insert(QStringLiteral("kind"), QStringLiteral("audio"));
            return e;
        };
        auto seedPlaylists = [&](const QJsonArray& items, qint64 upd) {
            QJsonObject p;
            p.insert(QStringLiteral("id"), QStringLiteral("pl-36"));
            p.insert(QStringLiteral("categoryKey"), QStringLiteral("audio"));
            p.insert(QStringLiteral("name"), QStringLiteral("Weekend Picks"));
            p.insert(QStringLiteral("updatedAt"), double(upd));
            p.insert(QStringLiteral("items"), items);
            QJsonArray all; all.append(p);
            setRaw(plKey36, compact(all));
        };
        auto plItems = [&]() {
            QSettings raw(iniPath, QSettings::IniFormat);
            const QJsonArray all = QJsonDocument::fromJson(raw.value(plKey36).toString().toUtf8()).array();
            return all.isEmpty() ? QJsonArray()
                                 : all.first().toObject().value(QStringLiteral("items")).toArray();
        };
        const QString localTrack = QStringLiteral("C:\\Users\\me\\Music\\Kid A\\04 Idioteque.flac");
        const QString audiobookUrl = QStringLiteral("https://store-034.example/zip/aa9a74fd-2bbb-470b")
                                     + QStringLiteral("?token=nOtaReAlToKeN000000000000000000000");
        {
            QJsonArray items;
            const QString sA = streamOf(root, QStringLiteral("tr-1"));
            const QString sB = streamOf(root, QStringLiteral("tr-2"));
            items.append(entry(sA, sA, QStringLiteral("Airbag")));                 // mappable
            items.append(entry(localTrack, localTrack, QStringLiteral("Idioteque")));   // a local file
            items.append(entry(audiobookUrl, audiobookUrl, QStringLiteral("Chapter 3"))); // unmappable
            items.append(entry(sB, sB, QStringLiteral("Paranoid Android")));       // mappable
            items.append(entry(QStringLiteral("steam:440"), QString(), QStringLiteral("Team Fortress 2")));
            seedPlaylists(items, T - 5000);
            CHECK(rawVal36(plKey36).contains(QStringLiteral("nOtaReAlToKeN")));    // the leak, before
        }
        CHECK(StoredIdentity::sweepPlaylists() == true);                           // it found work to do…
        {
            // NO CREDENTIAL LEFT ANYWHERE IN THE STORE — asserted against the RAW INI, because the ini is what
            // syncs and what a bug report attaches.
            const QString rawNow = rawVal36(plKey36);
            CHECK(!rawNow.contains(QStringLiteral("nOtaReAlToKeN")));
            CHECK(!rawNow.contains(QStringLiteral("u=listener")));
            CHECK(!rawNow.contains(QStringLiteral("fAkeSaLt")));

            const QJsonArray got = plItems();
            CHECK(got.size() == 5);                                    // NOTHING was dropped, and the order held
            auto at = [&](int i, const char* f) { return got.at(i).toObject().value(QLatin1String(f)).toString(); };
            CHECK(at(0, "itemId") == idA && at(0, "path") == idA);      // the durable name, both fields
            CHECK(at(0, "title") == QStringLiteral("Airbag"));          // …and the row is otherwise untouched
            CHECK(at(1, "itemId") == localTrack && at(1, "path") == localTrack);   // local: byte for byte
            CHECK(at(2, "itemId") == QStringLiteral("https://store-034.example/zip/aa9a74fd-2bbb-470b"));
            CHECK(at(2, "title") == QStringLiteral("Chapter 3"));       // unmappable: survives, credential-free
            CHECK(at(3, "itemId") == idB);
            CHECK(at(4, "itemId") == QStringLiteral("steam:440"));      // a store-game row is not a url at all
            CHECK(!got.at(4).toObject().contains(QStringLiteral("path")));
            // The MERGE CLOCK is not touched. Raising it would make this cleaned-but-stale copy outrank a
            // genuinely newer edit made on another device — a security fix eating an edit.
            QSettings raw(iniPath, QSettings::IniFormat);
            const QJsonArray all = QJsonDocument::fromJson(raw.value(plKey36).toString().toUtf8()).array();
            CHECK(qint64(all.first().toObject().value(QStringLiteral("updatedAt")).toDouble()) == T - 5000);
        }
        // 36d. RUN TWICE EQUALS RUN ONCE, and the second run does not even write. This is the property that
        // lets it be repeatable rather than stamped — which it has to be, because what a row can be named
        // depends on which servers are configured, and because a peer on an older build can push a tokenised
        // playlist back at any time (36f).
        {
            const QString after1 = rawVal36(plKey36);
            CHECK(StoredIdentity::sweepPlaylists() == false);
            CHECK(rawVal36(plKey36) == after1);                        // byte-identical, and no write happened
        }
        // 36e. TWO ENTRIES THAT BECOME ONE ROW. The same track saved twice under two spellings of its url is
        // one track: itemId is the identity inside a playlist (contains/addItem/removeItem all key on it) and
        // addItem would never have allowed the pair. The FIRST is kept — a playlist's order is the user's own.
        {
            QJsonArray items;
            const QString sA1 = streamOf(root, QStringLiteral("tr-1"));
            const QString sA2 = root + QStringLiteral("/rest/stream.view?u=listener&t=aDiFfErEnTtOkEn")
                                + QStringLiteral("&s=oThErSaLt&v=1.16.1&c=EverythingBox&f=json&id=tr-1");
            items.append(entry(sA1, sA1, QStringLiteral("Airbag (first)")));
            items.append(entry(localTrack, localTrack, QStringLiteral("Idioteque")));
            items.append(entry(sA2, sA2, QStringLiteral("Airbag (second)")));
            seedPlaylists(items, T - 5000);
            CHECK(StoredIdentity::sweepPlaylists() == true);
            const QJsonArray got = plItems();
            CHECK(got.size() == 2);
            CHECK(got.at(0).toObject().value(QStringLiteral("itemId")).toString() == idA);
            CHECK(got.at(0).toObject().value(QStringLiteral("title")).toString()
                  == QStringLiteral("Airbag (first)"));                // the earlier position wins
            CHECK(got.at(1).toObject().value(QStringLiteral("itemId")).toString() == localTrack);
        }
        // 36f. THE ENTRANCE A WRITER-ONLY FIX CANNOT CLOSE: a peer still running an older build. The playlist
        // merge is whole-object newest-wins, so its tokenised copy lands in the local ini as the winner — and
        // is cleaned before anything reads it.
        {
            wipeStores();
            const QString sA = streamOf(root, QStringLiteral("tr-1"));
            QJsonObject e = entry(sA, sA, QStringLiteral("Airbag"));
            QJsonArray items; items.append(e);
            QJsonObject pl;
            pl.insert(QStringLiteral("id"), QStringLiteral("pl-remote"));
            pl.insert(QStringLiteral("categoryKey"), QStringLiteral("audio"));
            pl.insert(QStringLiteral("name"), QStringLiteral("From The Other Box"));
            pl.insert(QStringLiteral("updatedAt"), double(T - 100));
            pl.insert(QStringLiteral("items"), items);
            QJsonArray pls; pls.append(pl);
            QJsonObject po; po.insert(QStringLiteral("items"), pls); po.insert(QStringLiteral("tombs"), QJsonArray());
            QJsonObject fam; fam.insert(QStringLiteral("r36"), po);
            QJsonObject doc; doc.insert(QStringLiteral("playlists"), fam);
            mergeDoc(doc);
            CHECK(!rawVal36(plKey36).contains(QStringLiteral("nOtaReAlToKeN")));
            const QJsonArray got = plItems();
            CHECK(got.size() == 1);                                    // the peer's playlist ARRIVED…
            CHECK(got.at(0).toObject().value(QStringLiteral("itemId")).toString() == idA);   // …re-identified
        }
        // 36g. ANOTHER PROFILE'S PLAYLIST. Servers are per-profile, so a row belonging to a profile that is
        // not the active one cannot be re-qualified — matching it against THIS profile's servers would mint an
        // id that resolves to nothing on the profile that holds the row. It is still cleaned, and — this is
        // the half that makes the abstention safe rather than merely cautious — it is still RE-IDENTIFIABLE,
        // so the run made as that profile finishes the job. Monotone, never destructive.
        {
            const QString otherKey = QStringLiteral("playlists/r36other/items");
            const QString sA = streamOf(root, QStringLiteral("tr-1"));
            QJsonArray items; items.append(entry(sA, sA, QStringLiteral("Airbag")));
            QJsonObject p;
            p.insert(QStringLiteral("id"), QStringLiteral("pl-other"));
            p.insert(QStringLiteral("categoryKey"), QStringLiteral("audio"));
            p.insert(QStringLiteral("items"), items);
            QJsonArray all; all.append(p);
            setRaw(otherKey, compact(all));
            CHECK(StoredIdentity::sweepPlaylists() == true);
            const QString rawNow = rawVal36(otherKey);
            CHECK(!rawNow.contains(QStringLiteral("nOtaReAlToKeN")));      // cleaned…
            CHECK(!rawNow.contains(QStringLiteral("u=listener")));
            const QString got = QJsonDocument::fromJson(rawNow.toUtf8()).array().first().toObject()
                                    .value(QStringLiteral("items")).toArray().first().toObject()
                                    .value(QStringLiteral("itemId")).toString();
            CHECK(got != idA);                                             // …but NOT re-qualified…
            CHECK(Subsonic::trackIdFromStreamUrl(got, roots) == idA);      // …and still nameable later
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.remove(QStringLiteral("playlists/r36other")); raw.sync();
        }
        // 36h. A RESUME LABEL IS A NAME, NOT A MACHINE STRING. Playing a playlist entry keys its resume record
        // on the qualified id, which is credential-free — and is also not a title. QFileInfo hands one back
        // WHOLE (no '/' and no '.'), so the ini filled up with `sub<US><uuid><US>track<US>tr-1` where the
        // track name belongs. Found live, driven here through the real writer.
        {
            wipeStores();
            {
                PlaybackSession s;
                s.setQueue({ idA }, 0, { QStringLiteral("A Song With A Real Name") });
                s.beginResume(idA);
                s.setDuration(300.0);
                s.setPosition(90.0);
                s.persistResume();
            }
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.beginGroup(QStringLiteral("resume"));
            const QStringList groups = raw.childGroups();
            raw.endGroup();
            bool sawTitle = false;
            for (const QString& g : groups)
            {
                const QString t = raw.value(QStringLiteral("resume/") + g + QStringLiteral("/title")).toString();
                if (t.isEmpty()) continue;
                CHECK(!t.contains(Subsonic::idSep()));                        // never the id itself
                if (t == QStringLiteral("A Song With A Real Name")) sawTitle = true;
            }
            CHECK(sawTitle);
            // …and a local path is still titled from its OWN FILE NAME, which is the arm that must not move.
            // The queue title is deliberately different here: for a local file the base name wins, which is
            // the contract #193 and #200 both left alone, and an assertion that let the queue title through
            // could not tell "local files untouched" from "everything takes the queue title".
            wipeStores();
            {
                const QString local = QStringLiteral("C:/Users/me/Music/Kid A/04 Idioteque.flac");
                PlaybackSession s;
                s.setQueue({ local }, 0, { QStringLiteral("Idioteque (2000 Remaster)") });
                s.beginResume(local);
                s.setDuration(300.0);
                s.setPosition(90.0);
                s.persistResume();
            }
            QSettings raw2(iniPath, QSettings::IniFormat);
            raw2.beginGroup(QStringLiteral("resume"));
            const QStringList groups2 = raw2.childGroups();
            raw2.endGroup();
            bool sawLocal = false;
            for (const QString& g : groups2)
                if (raw2.value(QStringLiteral("resume/") + g + QStringLiteral("/title")).toString()
                    == QStringLiteral("04 Idioteque")) sawLocal = true;
            CHECK(sawLocal);
        }
        // 36i. AND THE STORE STILL SYNCS. The fix is not a carve-out: playlists are a per-item store and stay
        // one. What changed is what a row says, not where it goes.
        CHECK(CloudSync::isPerItemStoreKey(plKey36) == true);
        CHECK(CloudSync::isDeviceLocalKey(plKey36) == false);

        wipeStores();
        SubsonicServerStore::remove(serverId);
        useProfile(QStringLiteral("cmA"));
    }

    // ---- 37. #203, Live TV: the identity that cannot be rewritten, so the row stops travelling -------------
    //
    // An IPTV channel's credentials are commonly in its url's PATH (…/live/<user>/<pass>/<id>.ts) — which
    // StoredUrl deliberately does not touch (§34e) — and where they are in the query they are often what makes
    // the channel play. There is no durable name to move the favourite onto either: a tvg-id is optional and
    // names a channel in a guide, not a stream. So the identity is left ALONE and the row is kept out of the
    // synced document instead, which is the rule `iptv/*` (the same urls, in the source list) already follows.
    {
        useProfile(QStringLiteral("r37"));
        wipeStores();
        const QString chanUrl = QStringLiteral("http://iptv.example/live/someuser/somepass/12345.ts");
        const QString chanId  = QStringLiteral("livetv:") + chanUrl;
        const QString movieId = QStringLiteral("tt0111161");
        {
            QJsonArray a;
            QJsonObject c;
            c.insert(QStringLiteral("itemId"), chanId);
            c.insert(QStringLiteral("title"), QStringLiteral("Channel One"));
            c.insert(QStringLiteral("kind"), QStringLiteral("livetv"));
            c.insert(QStringLiteral("path"), chanUrl);
            c.insert(QStringLiteral("ts"), double(T - 300));
            QJsonObject m;
            m.insert(QStringLiteral("itemId"), movieId);
            m.insert(QStringLiteral("title"), QStringLiteral("The Shawshank Redemption"));
            m.insert(QStringLiteral("ts"), double(T - 400));
            a.append(c); a.append(m);
            setRaw(QStringLiteral("favorites/r37/items"), compact(a));
        }
        injTomb(QStringLiteral("favorites/r37"), QStringLiteral("livetv:") + chanUrl + QStringLiteral("-gone"),
                T - 200);
        injTomb(QStringLiteral("favorites/r37"), QStringLiteral("tt0000001"), T - 200);

        // 37a. WHAT LEAVES THE DEVICE. The channel is not in the document — and neither is its TOMBSTONE, which
        // carries the same url verbatim (Tombstones.h keeps the original key in the value). Un-starring a
        // channel would otherwise have put the credential back into the document under a different name, which
        // is the trap #200 hit with the recents tombstones.
        {
            const QJsonObject doc = serializeNow();
            const QJsonObject po = doc.value(QStringLiteral("favorites")).toObject()
                                      .value(QStringLiteral("r37")).toObject();
            QStringList sent;
            for (const QJsonValue& v : po.value(QStringLiteral("items")).toArray())
                sent << v.toObject().value(QStringLiteral("itemId")).toString();
            CHECK(sent == QStringList{ movieId });                     // the ordinary favourite still syncs
            QStringList tombs;
            for (const QJsonValue& v : po.value(QStringLiteral("tombs")).toArray())
                tombs << v.toObject().value(QStringLiteral("key")).toString();
            CHECK(tombs == QStringList{ QStringLiteral("tt0000001") });
            const QString whole = QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact));
            CHECK(!whole.contains(QStringLiteral("somepass")));        // …anywhere in the whole document
            CHECK(!whole.contains(QStringLiteral("livetv:")));
        }
        // 37b. AND IT IS STILL THERE. The filter is on the wire, never on the store: the user's starred channel
        // is untouched locally, still resolves, and still plays. A favourite that stopped working would be a
        // worse outcome than the leak it was meant to fix.
        CHECK(favIds(QStringLiteral("r37")).contains(chanId));
        // 37c. AND ON THE WAY IN, because a peer on an older build goes on sending them. Accepting one would
        // write the credential into this device's ini — the entrance a send-side filter alone cannot close.
        {
            const QString otherUrl = QStringLiteral("http://iptv.example/live/otheruser/otherpass/999.ts");
            QJsonObject rc;
            rc.insert(QStringLiteral("itemId"), QStringLiteral("livetv:") + otherUrl);
            rc.insert(QStringLiteral("title"), QStringLiteral("Channel Two"));
            rc.insert(QStringLiteral("ts"), double(T - 50));
            QJsonObject rm;
            rm.insert(QStringLiteral("itemId"), QStringLiteral("tt0068646"));
            rm.insert(QStringLiteral("title"), QStringLiteral("The Godfather"));
            rm.insert(QStringLiteral("ts"), double(T - 50));
            QJsonArray items; items.append(rc); items.append(rm);
            QJsonObject rt; rt.insert(QStringLiteral("key"), chanId); rt.insert(QStringLiteral("ts"), double(T));
            QJsonArray tombs; tombs.append(rt);
            QJsonObject po; po.insert(QStringLiteral("items"), items); po.insert(QStringLiteral("tombs"), tombs);
            QJsonObject fam; fam.insert(QStringLiteral("r37"), po);
            QJsonObject doc; doc.insert(QStringLiteral("favorites"), fam);
            mergeDoc(doc);
            const QStringList after = favIds(QStringLiteral("r37"));
            CHECK(!after.contains(QStringLiteral("livetv:") + otherUrl));   // the peer's channel is refused…
            CHECK(after.contains(QStringLiteral("tt0068646")));             // …and everything else lands
            CHECK(after.contains(movieId));
            // A peer's Live TV TOMBSTONE is refused too, and that is deliberate rather than incidental: it
            // carries the url, and honouring it would let a device that never should have had the row delete
            // the row this device DOES have.
            CHECK(after.contains(chanId));
            QSettings raw(iniPath, QSettings::IniFormat);
            CHECK(!raw.value(QStringLiteral("favorites/r37/items")).toString().contains(QStringLiteral("otherpass")));
        }
        // 37d. The favourites store as a whole is unchanged: still per-item, still synced. Only Live TV rows
        // are held back, and only in the document.
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("favorites/r37/items")) == true);

        wipeStores();
        useProfile(QStringLiteral("cmA"));
    }

    if (failures == 0) { std::puts("CLOUDMERGE-OK"); return 0; }
    std::fprintf(stderr, "CLOUDMERGE: %d check(s) failed\n", failures);
    return 1;
}
