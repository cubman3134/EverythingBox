// Headless check of PlaylistStore's category migration (src/core/PlaylistStore) — the data-safety heart of the
// category-scoped-playlists feature. Playlists widened from per-catalogue (a single catalogKey) to per-category
// (video|audio|game|reading, via core/MediaCategories.h), and the user's REAL "Weekend Picks" list must survive
// that byte-meaningfully: same id, same items, now category-scoped with its old key preserved. QtCore-only (a
// QSettings + JSON wrapper, no Quick/Widgets), so it runs under the offscreen QPA in CI and pins the contract:
//
//   * migration: a v1 blob (catalogKey-shaped) of a Weekend-Picks-shaped movie playlist + a games one folds to
//     categoryKey video/game; legacyKey preserves the old catalogKey; ids + every item field are untouched;
//   * unknown/unrecognised catalogType -> "video" (mediaCategory's catch-all fallback);
//   * migrateToCategories() is one-shot + idempotent: it returns true the run it rewrites, false once stamped,
//     and re-running the transform over already-migrated data changes nothing (categoryKey stays put);
//   * forCategory() filters by bucket (a video playlist and a game playlist never cross);
//   * create(categoryKey, name) makes a playlist in that bucket (no legacyKey — it never had a catalogue).
//
// Prints PLAYLISTS-OK on success; any failure prints PLAYLISTS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: like the other core probes (probe_marks/probe_sync), AppPaths::dataDir() is the probe exe's own
// build-tree folder (portable app), so the mymediavault.ini it reads/writes sits next to the probe and never
// touches a deployed install. We wipe the "playlists" and "profiles" groups at start and SEED our own profile
// id via ProfileStore::setCurrent, so ProfileStore::currentId() can't leak a developer's real profile.
#include "PlaylistStore.h"
#include "MediaCategories.h"
#include "ShuffleBag.h"
#include "ProfileStore.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QSettings>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PLAYLISTS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The exact JSON key PlaylistStore stores a profile's playlists under (mirrors plKey() in the store).
static QString itemsKey(const QString& profile) { return QStringLiteral("playlists/") + profile + QStringLiteral("/items"); }
static QString schemaKey(const QString& profile) { return QStringLiteral("playlists/") + profile + QStringLiteral("/schema"); }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/mymediavault.ini");
    const QString profile = QStringLiteral("probePl");

    // ---- 0. mediaCategory oracle (the core function HomeView::mediaCategory now delegates to) ----------------
    CHECK(core::mediaCategory(QStringLiteral("movie")) == QStringLiteral("video"));
    CHECK(core::mediaCategory(QStringLiteral("track")) == QStringLiteral("audio"));
    CHECK(core::mediaCategory(QStringLiteral("game"))  == QStringLiteral("game"));   // SINGULAR
    CHECK(core::mediaCategory(QStringLiteral("comic")) == QStringLiteral("reading"));
    CHECK(core::mediaCategory(QStringLiteral("quux"))  == QStringLiteral("video"));  // unknown -> fallback
    CHECK(core::mediaCategory(QString())               == QStringLiteral("video"));  // empty  -> fallback

    // ---- Seed a v1 (catalogKey-shaped) blob straight into the ini, and clear the migration stamp -------------
    // Three playlists: a Weekend-Picks-shaped movie list (video), a games list (game), and one with an
    // unrecognised catalogType (must fall to video). QSettings shares one in-process QConfFile per path, so a
    // seed written here is seen by PlaylistStore's own store() on the next access.
    const QString weekendId = QStringLiteral("9076637e-3a3a-4356-97cd-19c702ef5e13");
    const QString gamesId   = QStringLiteral("games-0000-1111-2222-333344445555");
    const QString weirdId   = QStringLiteral("weird-0000-1111-2222-333344445555");
    const QString v1Blob = QStringLiteral(
        "[{\"catalogKey\":\"com.mymediavault.aiocatalog|movies|movie\",\"id\":\"") + weekendId + QStringLiteral("\","
        "\"items\":[{\"addonId\":\"com.mymediavault.aiocatalog\",\"expandable\":false,\"itemId\":\"tmdb:movie:1275779\","
        "\"subtitle\":\"2026\",\"thumbnailUrl\":\"https://image.tmdb.org/t/p/w342/AnJ8IQJI23hNpYXVNaythu061Ru.jpg\","
        "\"title\":\"Disclosure Day\",\"type\":\"movie\"}],\"name\":\"Weekend Picks\"},"
        "{\"catalogKey\":\"native|consoles|game\",\"id\":\"") + gamesId + QStringLiteral("\","
        "\"items\":[{\"itemId\":\"igdb:1128\",\"kind\":\"game\",\"path\":\"C:/roms/sotn.chd\","
        "\"title\":\"Castlevania: SotN\",\"type\":\"game\"}],\"name\":\"Retro Night\"},"
        "{\"catalogKey\":\"someaddon|weird|quux\",\"id\":\"") + weirdId + QStringLiteral("\","
        "\"items\":[],\"name\":\"Mystery\"}]");
    {
        QSettings reset(iniPath, QSettings::IniFormat);
        reset.remove(QStringLiteral("playlists"));
        reset.remove(QStringLiteral("profiles"));
        reset.setValue(itemsKey(profile), v1Blob);   // the un-migrated v1 blob
        reset.remove(schemaKey(profile));             // ensure no stale stamp
        reset.sync();
    }
    ProfileStore::setCurrent(profile);

    // ---- 1. First migrate: rewrites (returns true), stamps the profile ---------------------------------------
    CHECK(PlaylistStore::migrateToCategories() == true);
    // Second migrate is a no-op: the stamp short-circuits it.
    CHECK(PlaylistStore::migrateToCategories() == false);

    // ---- 2. Weekend Picks survived byte-meaningfully: id, items, name intact; now categoryKey=video ----------
    {
        Playlist p;
        CHECK(PlaylistStore::get(weekendId, p));
        CHECK(p.id == weekendId);                                                   // id stable
        CHECK(p.categoryKey == QStringLiteral("video"));                            // movie -> video
        CHECK(p.legacyKey == QStringLiteral("com.mymediavault.aiocatalog|movies|movie")); // old key preserved
        CHECK(p.name == QStringLiteral("Weekend Picks"));
        CHECK(p.items.size() == 1);
        if (p.items.size() == 1)
        {
            const PlaylistEntry& e = p.items[0];
            CHECK(e.itemId == QStringLiteral("tmdb:movie:1275779"));                // items untouched
            CHECK(e.addonId == QStringLiteral("com.mymediavault.aiocatalog"));
            CHECK(e.title == QStringLiteral("Disclosure Day"));
            CHECK(e.subtitle == QStringLiteral("2026"));
            CHECK(e.type == QStringLiteral("movie"));
            CHECK(e.thumbnailUrl == QStringLiteral("https://image.tmdb.org/t/p/w342/AnJ8IQJI23hNpYXVNaythu061Ru.jpg"));
            CHECK(e.expandable == false);
        }
    }

    // ---- 3. The games playlist folded to categoryKey=game; the unknown type fell to video -------------------
    {
        Playlist g;
        CHECK(PlaylistStore::get(gamesId, g));
        CHECK(g.categoryKey == QStringLiteral("game"));
        CHECK(g.legacyKey == QStringLiteral("native|consoles|game"));
        CHECK(g.items.size() == 1 && g.items[0].path == QStringLiteral("C:/roms/sotn.chd")); // local-file item intact
        Playlist w;
        CHECK(PlaylistStore::get(weirdId, w));
        CHECK(w.categoryKey == QStringLiteral("video"));                            // quux -> fallback
        CHECK(w.legacyKey == QStringLiteral("someaddon|weird|quux"));
    }

    // ---- 4. forCategory filters by bucket (video and game never cross) ---------------------------------------
    {
        const QVector<Playlist> video = PlaylistStore::forCategory(QStringLiteral("video"));
        CHECK(video.size() == 2);                                                   // Weekend Picks + Mystery
        bool sawWeekend = false, sawMystery = false, sawGames = false;
        for (const Playlist& p : video) {
            if (p.id == weekendId) sawWeekend = true;
            if (p.id == weirdId)   sawMystery = true;
            if (p.id == gamesId)   sawGames = true;
        }
        CHECK(sawWeekend && sawMystery && !sawGames);
        const QVector<Playlist> games = PlaylistStore::forCategory(QStringLiteral("game"));
        CHECK(games.size() == 1 && games[0].id == gamesId);
        CHECK(PlaylistStore::forCategory(QStringLiteral("audio")).isEmpty());       // nothing there yet
    }

    // ---- 5. create(categoryKey, name): a new list in that bucket, no legacyKey -------------------------------
    {
        const QString id = PlaylistStore::create(QStringLiteral("audio"), QStringLiteral("Chill Mix"));
        CHECK(!id.isEmpty());
        Playlist p;
        CHECK(PlaylistStore::get(id, p));
        CHECK(p.categoryKey == QStringLiteral("audio"));
        CHECK(p.legacyKey.isEmpty());                                              // never had a catalogue
        CHECK(p.name == QStringLiteral("Chill Mix") && p.items.isEmpty());
        const QVector<Playlist> audio = PlaylistStore::forCategory(QStringLiteral("audio"));
        CHECK(audio.size() == 1 && audio[0].id == id);
    }

    // ---- 6. Idempotency beyond the stamp: re-running the transform over migrated data changes nothing --------
    // Clear the stamp so migrate actually SCANS again; because every playlist already has a categoryKey, it
    // finds nothing to change (returns false) and no categoryKey is disturbed.
    {
        QSettings raw(iniPath, QSettings::IniFormat);
        raw.remove(schemaKey(profile));
        raw.sync();
        CHECK(PlaylistStore::migrateToCategories() == false);                       // scanned, nothing to fold
        Playlist p;
        CHECK(PlaylistStore::get(weekendId, p) && p.categoryKey == QStringLiteral("video")); // still video
        CHECK(p.legacyKey == QStringLiteral("com.mymediavault.aiocatalog|movies|movie"));    // still preserved
        CHECK(p.items.size() == 1 && p.items[0].itemId == QStringLiteral("tmdb:movie:1275779"));
    }

    // ---- 7. rename + remove: T2's menu is the first real caller of these store ops -------------------------
    {
        const QString id = PlaylistStore::create(QStringLiteral("video"), QStringLiteral("Throwaway"));
        CHECK(!id.isEmpty());
        // A mixed pair — an addon item + a local-file item — exactly what Play-random must resolve across.
        PlaylistEntry addonE; addonE.addonId = QStringLiteral("com.x"); addonE.itemId = QStringLiteral("tmdb:movie:42");
        addonE.type = QStringLiteral("movie"); addonE.title = QStringLiteral("A");
        PlaylistEntry localE; localE.itemId = QStringLiteral("local:1"); localE.path = QStringLiteral("C:/v.mp4");
        localE.kind = QStringLiteral("video"); localE.type = QStringLiteral("video"); localE.title = QStringLiteral("B");
        PlaylistStore::addItem(id, addonE);
        PlaylistStore::addItem(id, localE);

        // rename persists the new name and disturbs nothing else (id / category / items intact).
        PlaylistStore::rename(id, QStringLiteral("Renamed"));
        Playlist p;
        CHECK(PlaylistStore::get(id, p));
        CHECK(p.name == QStringLiteral("Renamed"));
        CHECK(p.categoryKey == QStringLiteral("video"));
        CHECK(p.items.size() == 2);
        CHECK(p.items[0].addonId == QStringLiteral("com.x") && p.items[0].path.isEmpty());        // addon entry
        CHECK(p.items[1].path == QStringLiteral("C:/v.mp4") && p.items[1].addonId.isEmpty());      // local-file entry

        // remove drops exactly this playlist; the bucket's other lists (Weekend Picks + Mystery) are untouched.
        const int videoBefore = PlaylistStore::forCategory(QStringLiteral("video")).size();
        PlaylistStore::remove(id);
        CHECK(!PlaylistStore::get(id, p));                                            // gone
        CHECK(PlaylistStore::forCategory(QStringLiteral("video")).size() == videoBefore - 1);
    }

    // ---- 8. ShuffleBag: Channel mode's random sequencer (core/ShuffleBag.h) ---------------------------------
    // A pure, header-only invariant net (no UI): (a) every bag pass is a permutation of 0..n-1 (no repeat until
    // exhausted); (b) a reshuffle never immediately repeats the last item of the prior pass when n>1; (c) n==1
    // is sane (always 0); (d) an empty bag yields -1. Uses the real global RNG, so we assert over many trials.
    {
        // (c) size-1: always 0, never crashes.
        ShuffleBag one(1);
        CHECK(one.valid() && one.size() == 1);
        for (int i = 0; i < 50; ++i) CHECK(one.next() == 0);

        // (d) empty bag: invalid, next() == -1.
        ShuffleBag none(0);
        CHECK(!none.valid());
        CHECK(none.next() == -1);
        ShuffleBag neg(-3);
        CHECK(!neg.valid() && neg.next() == -1);

        // (a)+(b): over many trials at several sizes, walk multiple full passes and check the invariants.
        for (int n : { 2, 3, 5, 8 })
        {
            for (int trial = 0; trial < 400; ++trial)
            {
                ShuffleBag bag(n);
                int prevPassLast = -1;
                for (int pass = 0; pass < 4; ++pass)   // several reshuffles per trial
                {
                    QVector<int> seen;
                    for (int d = 0; d < n; ++d)
                    {
                        const int idx = bag.next();
                        CHECK(idx >= 0 && idx < n);        // always in range
                        CHECK(!seen.contains(idx));        // (a) no repeat within a pass
                        if (d == 0 && pass > 0)
                            CHECK(idx != prevPassLast);    // (b) no immediate repeat across the boundary (n>1)
                        seen.push_back(idx);
                    }
                    CHECK(seen.size() == n);               // (a) a full permutation of 0..n-1
                    prevPassLast = seen.last();
                }
            }
        }
    }

    if (failures == 0) { std::puts("PLAYLISTS-OK"); return 0; }
    std::fprintf(stderr, "PLAYLISTS: %d check(s) failed\n", failures);
    return 1;
}
