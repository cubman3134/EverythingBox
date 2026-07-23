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
#include "ProfileStore.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QSettings>
#include <QCryptographicHash>
#include <QDateTime>
#include <QStringList>
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
        for (const char* g : {"device", "marks", "favorites", "playlists", "deleted", "profiles"})
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

    if (failures == 0) { std::puts("CLOUDMERGE-OK"); return 0; }
    std::fprintf(stderr, "CLOUDMERGE: %d check(s) failed\n", failures);
    return 1;
}
