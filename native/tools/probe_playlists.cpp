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

    if (failures == 0) { std::puts("PLAYLISTS-OK"); return 0; }
    std::fprintf(stderr, "PLAYLISTS: %d check(s) failed\n", failures);
    return 1;
}
