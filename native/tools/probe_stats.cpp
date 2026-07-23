// Headless check of the ConsumptionStats store (src/core/ConsumptionStats) — the per-profile media/reading
// accrual foundation (seconds watched/listened + pages read, per title + per category) the Stats panel (T2)
// reads. QtCore-only (a QSettings wrapper over the shared mymediavault.ini), so it runs under the offscreen
// QPA in CI and pins the contract the panel + the two accrual seams lean on:
//
//   * forward-only media accrual — the STORE floors a non-positive Δ to a no-op (the seam clamps to [0,30]);
//   * high-water pages — a revisit doesn't accrue, a backward turn doesn't decrement;
//   * rollup coherence — the per-category rollup equals the sum of per-title metrics after N accruals;
//   * per-profile isolation — profile A's stats are invisible to profile B;
//   * invalidate — an external ini write is not seen until invalidate() (hot cache), then re-read;
//   * title-update on accrual — the last non-empty title wins and is stored (no reverse lookup at display);
//   * empty-key no-op + junk-free — no empty-hash item key, no negative/zero/stray-category writes.
//
// Prints STATS-OK on success; any failure prints STATS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: like the other core probes, AppPaths::dataDir() is the probe exe's own build-tree folder, so the
// mymediavault.ini it reads/writes sits next to the probe and never touches a deployed install. We wipe the
// "stats" and "profiles" groups at start and SEED profile ids via ProfileStore::setCurrent, so currentId()
// can't leak a developer's real profile into the asserts.
#include "ConsumptionStats.h"
#include "PlayStats.h"
#include "ProfileStore.h"
#include "Settings.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QSettings>
#include <QCryptographicHash>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "STATS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The same MD5-hex token ConsumptionStats uses internally, so the probe can address an item's raw ini blob and
// can predict the hashed keys topTitles() returns.
static QString hash(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

// PlayStats keys games by SHA1 of the identity (not MD5), so the probe can address a game's raw ini slot.
static QString sha1(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// A ConsumptionStats item blob exactly as the store serializes it (entryFromJson reads these fields).
static QString statsBlob(qint64 secs, qint64 pages, qint64 last, const QString& title, const QString& cat)
{
    return QStringLiteral("{\"mediaSeconds\":%1,\"pagesRead\":%2,\"lastActivity\":%3,\"title\":\"%4\",\"category\":\"%5\"}")
        .arg(secs).arg(pages).arg(last).arg(title, cat);
}

static void useProfile(const QString& id)
{
    ProfileStore::setCurrent(id);
    ConsumptionStats::invalidate();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/mymediavault.ini");
    {
        QSettings reset(iniPath, QSettings::IniFormat);
        reset.remove(QStringLiteral("stats"));
        reset.remove(QStringLiteral("playstats"));
        reset.remove(QStringLiteral("profiles"));
        reset.sync();
    }
    ConsumptionStats::invalidate();

    // This device's accumulator namespace (mdsync T3): every writer targets stats/<profile>/<dev>/... and
    // playstats/<profile>/<dev>/...; the public readers SUM across device namespaces. Fixtures that poke the
    // raw ini must address the device path.
    const QString dev = Settings::deviceId();

    // ---- 1. Forward-only media accrual + the store's own non-positive floor ---------------------------------
    useProfile(QStringLiteral("probeA"));
    // The SEAM clamps Δ to [0,30]; the STORE additionally floors secs<=0 to a no-op. Feed a couple of clamped
    // heartbeats and a junk non-positive one.
    ConsumptionStats::addMediaSeconds(QStringLiteral("vid:matrix"), QStringLiteral("video"), 30, QStringLiteral("The Matrix"));
    ConsumptionStats::addMediaSeconds(QStringLiteral("vid:matrix"), QStringLiteral("video"), 12, QStringLiteral("The Matrix"));
    ConsumptionStats::addMediaSeconds(QStringLiteral("vid:matrix"), QStringLiteral("video"), 0,  QStringLiteral("The Matrix")); // no-op
    ConsumptionStats::addMediaSeconds(QStringLiteral("vid:matrix"), QStringLiteral("video"), -5, QStringLiteral("The Matrix")); // no-op
    {
        const ConsumptionStats::Totals t = ConsumptionStats::get(QStringLiteral("vid:matrix"));
        CHECK(t.mediaSeconds == 42);                 // 30 + 12; the 0 and -5 accrued nothing
        CHECK(t.title == QStringLiteral("The Matrix"));
        CHECK(t.lastActivity > 0);
    }
    // A stray/unknown category is a no-op (junk-free).
    ConsumptionStats::addMediaSeconds(QStringLiteral("vid:matrix"), QStringLiteral("hologram"), 99, QStringLiteral("The Matrix"));
    CHECK(ConsumptionStats::get(QStringLiteral("vid:matrix")).mediaSeconds == 42);

    // A second video + an audio title, to exercise category separation + rollups.
    ConsumptionStats::addMediaSeconds(QStringLiteral("vid:tron"),  QStringLiteral("video"), 20, QStringLiteral("Tron"));
    ConsumptionStats::addMediaSeconds(QStringLiteral("aud:daft"),  QStringLiteral("audio"), 25, QStringLiteral("Daft Punk"));
    ConsumptionStats::addMediaSeconds(QStringLiteral("aud:daft"),  QStringLiteral("audio"), 25, QStringLiteral("Daft Punk"));

    // ---- 2. High-water pages: revisits don't count, regressions don't decrement -----------------------------
    ConsumptionStats::addPagesRead(QStringLiteral("book:dune"), 1, QStringLiteral("Dune")); // 0 -> 1 = +1
    ConsumptionStats::addPagesRead(QStringLiteral("book:dune"), 5, QStringLiteral("Dune")); // 1 -> 5 = +4
    ConsumptionStats::addPagesRead(QStringLiteral("book:dune"), 3, QStringLiteral("Dune")); // regression: no-op
    ConsumptionStats::addPagesRead(QStringLiteral("book:dune"), 5, QStringLiteral("Dune")); // revisit: no-op
    CHECK(ConsumptionStats::get(QStringLiteral("book:dune")).pagesRead == 5);
    ConsumptionStats::addPagesRead(QStringLiteral("book:dune"), 9, QStringLiteral("Dune")); // 5 -> 9 = +4
    CHECK(ConsumptionStats::get(QStringLiteral("book:dune")).pagesRead == 9);

    ConsumptionStats::addPagesRead(QStringLiteral("comic:tintin"), 12, QStringLiteral("Tintin")); // 0 -> 12 = +12

    // ---- 3. Rollup coherence: category rollup == sum of per-title metrics ------------------------------------
    {
        // video: matrix(42) + tron(20) = 62
        qint64 sum = 0;
        for (const auto& p : ConsumptionStats::topTitles(QStringLiteral("video"), 100)) sum += p.second.mediaSeconds;
        CHECK(sum == 62);
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video")) == 62);
        // audio: daft(50)
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("audio")) == 50);
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("audio")) ==
              ConsumptionStats::topTitles(QStringLiteral("audio"), 100).value(0).second.mediaSeconds);
        // reading: dune(9) + tintin(12) = 21
        qint64 pgs = 0;
        for (const auto& p : ConsumptionStats::topTitles(QStringLiteral("reading"), 100)) pgs += p.second.pagesRead;
        CHECK(pgs == 21);
        CHECK(ConsumptionStats::categoryPages() == 21);
    }

    // topTitles ordering (video by seconds desc: matrix 42 before tron 20) + it returns the HASHED key.
    {
        const auto top = ConsumptionStats::topTitles(QStringLiteral("video"), 2);
        CHECK(top.size() == 2);
        CHECK(top[0].first == hash(QStringLiteral("vid:matrix")));
        CHECK(top[0].second.mediaSeconds == 42);
        CHECK(top[1].second.mediaSeconds == 20);
        // reading by pages desc: tintin 12 before dune 9.
        const auto tr = ConsumptionStats::topTitles(QStringLiteral("reading"), 1);
        CHECK(tr.size() == 1);
        CHECK(tr[0].second.title == QStringLiteral("Tintin"));
    }

    // ---- 4. Per-profile isolation ----------------------------------------------------------------------------
    useProfile(QStringLiteral("probeB"));
    CHECK(ConsumptionStats::get(QStringLiteral("vid:matrix")).mediaSeconds == 0);
    CHECK(ConsumptionStats::get(QStringLiteral("book:dune")).pagesRead == 0);
    CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video")) == 0);
    CHECK(ConsumptionStats::categoryPages() == 0);
    CHECK(ConsumptionStats::topTitles(QStringLiteral("video"), 5).isEmpty());
    ConsumptionStats::addMediaSeconds(QStringLiteral("vid:matrix"), QStringLiteral("video"), 7, QStringLiteral("The Matrix"));
    CHECK(ConsumptionStats::get(QStringLiteral("vid:matrix")).mediaSeconds == 7); // B's own tally
    // Switching back restores A intact (untouched by B).
    useProfile(QStringLiteral("probeA"));
    CHECK(ConsumptionStats::get(QStringLiteral("vid:matrix")).mediaSeconds == 42);
    CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video")) == 62);

    // ---- 5. Title update on accrual --------------------------------------------------------------------------
    ConsumptionStats::addMediaSeconds(QStringLiteral("vid:matrix"), QStringLiteral("video"), 3, QStringLiteral("The Matrix (Remastered)"));
    CHECK(ConsumptionStats::get(QStringLiteral("vid:matrix")).title == QStringLiteral("The Matrix (Remastered)"));
    CHECK(ConsumptionStats::get(QStringLiteral("vid:matrix")).mediaSeconds == 45);

    // ---- 6. Cache is hot; invalidate() re-reads --------------------------------------------------------------
    {
        useProfile(QStringLiteral("probeCache"));
        ConsumptionStats::addPagesRead(QStringLiteral("k"), 4, QStringLiteral("K")); // writer invalidated
        CHECK(ConsumptionStats::get(QStringLiteral("k")).pagesRead == 4);            // primes cache
        {
            // External write straight to the ini (bypasses the store, so no invalidate happens). Targets THIS
            // device's namespace — the reader sums device namespaces, so a legacy-path write would be invisible.
            const QString ik = QStringLiteral("stats/probeCache/") + dev + QStringLiteral("/items/") + hash(QStringLiteral("k"));
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(ik, QStringLiteral("{\"mediaSeconds\":0,\"pagesRead\":99,\"lastActivity\":1,\"title\":\"K\",\"category\":\"reading\"}"));
            raw.sync();
        }
        CHECK(ConsumptionStats::get(QStringLiteral("k")).pagesRead == 4);  // still cached (hot)
        ConsumptionStats::invalidate();
        CHECK(ConsumptionStats::get(QStringLiteral("k")).pagesRead == 99); // now re-read
    }

    // ---- 7. Empty-key no-op + junk-free ----------------------------------------------------------------------
    {
        useProfile(QStringLiteral("probeJunk"));
        ConsumptionStats::addMediaSeconds(QString(), QStringLiteral("video"), 10, QStringLiteral("ghost"));
        ConsumptionStats::addPagesRead(QString(), 5, QStringLiteral("ghost"));
        CHECK(ConsumptionStats::get(QString()).mediaSeconds == 0);
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video")) == 0); // empty-key accrued nothing
        CHECK(ConsumptionStats::categoryPages() == 0);
        // No empty-hash junk item key was written (check THIS device's namespace, where writes land).
        QSettings ini(iniPath, QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("stats/probeJunk/") + dev + QStringLiteral("/items"));
        const QStringList itemKeys = ini.childKeys();
        ini.endGroup();
        CHECK(itemKeys.isEmpty());
        // Hashed-key independence: "a/b", "a//b" and a URL-shaped key resolve to distinct entries.
        ConsumptionStats::addPagesRead(QStringLiteral("a/b"),  2, QStringLiteral("ab"));
        ConsumptionStats::addPagesRead(QStringLiteral("a//b"), 3, QStringLiteral("aslashb"));
        ConsumptionStats::addPagesRead(QStringLiteral("https://x/y.cbz"), 4, QStringLiteral("url"));
        CHECK(ConsumptionStats::get(QStringLiteral("a/b")).pagesRead == 2);
        CHECK(ConsumptionStats::get(QStringLiteral("a//b")).pagesRead == 3);
        CHECK(ConsumptionStats::get(QStringLiteral("https://x/y.cbz")).pagesRead == 4);
    }

    // ---- 8. Category flip: one key accrued as video THEN audio (cover-art misfile / re-open as the other kind)
    // Defined coherent behavior: the per-category ROLLUPS accrue where the seconds were EARNED (video keeps its
    // 10, audio keeps its 5 — an earned ledger, never moved by a later flip); the per-title blob keeps ONE
    // lastCategory (the most recent) for display, so the title migrates to that category's topTitles and carries
    // its whole lifetime total there. The rollup SUM stays coherent with the title's lifetime mediaSeconds — but
    // a single category's topTitles-sum deliberately need NOT equal its rollup once a title has flipped (that is
    // the earned-ledger vs. single-display-category decoupling this assert pins).
    {
        useProfile(QStringLiteral("probeFlip"));
        ConsumptionStats::addMediaSeconds(QStringLiteral("flip:show"), QStringLiteral("video"), 10, QStringLiteral("Flip"));
        ConsumptionStats::addMediaSeconds(QStringLiteral("flip:show"), QStringLiteral("audio"), 5,  QStringLiteral("Flip"));
        // Rollups accrue where earned — neither is retro-moved by the flip.
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video")) == 10);
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("audio")) == 5);
        // The title's lifetime total spans both kinds; the rollup sum stays coherent with it.
        CHECK(ConsumptionStats::get(QStringLiteral("flip:show")).mediaSeconds == 15);
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video"))
              + ConsumptionStats::categorySeconds(QStringLiteral("audio")) == 15);
        // Per-title lastCategory = the most recent (audio): the title now shows under audio's topTitles (with its
        // whole 15s total), and is ABSENT from video's — a single display category, not a per-category split.
        CHECK(ConsumptionStats::topTitles(QStringLiteral("video"), 10).isEmpty());
        const auto aud = ConsumptionStats::topTitles(QStringLiteral("audio"), 10);
        CHECK(aud.size() == 1 && aud.value(0).second.title == QStringLiteral("Flip"));
        CHECK(aud.value(0).second.mediaSeconds == 15);
    }

    // ========================================================================================================
    //  mdsync T3 — device-namespaced accumulators. Writers target stats/<profile>/<dev>/... (playstats too);
    //  readers SUM across device namespaces; a one-time stamped migration folds pre-upgrade un-namespaced keys.
    // ========================================================================================================
    auto setRaw = [&](const QString& key, const QString& val) {
        QSettings raw(iniPath, QSettings::IniFormat); raw.setValue(key, val); raw.sync();
    };
    auto hasGroupKeys = [&](const QString& grp) {
        QSettings raw(iniPath, QSettings::IniFormat); raw.beginGroup(grp);
        const bool any = !raw.childKeys().isEmpty() || !raw.childGroups().isEmpty(); raw.endGroup(); return any;
    };

    // ---- 9. ConsumptionStats migration: legacy un-namespaced keys fold into THIS device's namespace, once ----
    {
        // Seed a pre-upgrade profile "mig" with un-namespaced items + category rollups (straight to the ini).
        setRaw(QStringLiteral("stats/mig/items/") + hash(QStringLiteral("vid:V")), statsBlob(40, 0, 111, QStringLiteral("V"), QStringLiteral("video")));
        setRaw(QStringLiteral("stats/mig/items/") + hash(QStringLiteral("book:B")), statsBlob(0, 7, 112, QStringLiteral("B"), QStringLiteral("reading")));
        setRaw(QStringLiteral("stats/mig/cat/video/seconds"), QStringLiteral("40"));
        setRaw(QStringLiteral("stats/mig/cat/reading/pages"), QStringLiteral("7"));

        ProfileStore::setCurrent(QStringLiteral("mig"));
        ConsumptionStats::migrate();               // fold (global; guarded per profile)

        // Legacy roots gone; the data now lives under this device's namespace.
        CHECK(!hasGroupKeys(QStringLiteral("stats/mig/items")));
        CHECK(!hasGroupKeys(QStringLiteral("stats/mig/cat")));
        CHECK(hasGroupKeys(QStringLiteral("stats/mig/") + dev + QStringLiteral("/items")));
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            CHECK(raw.value(QStringLiteral("stats/mig/schema")).toInt() >= 1); // stamped
        }
        // Readers see the folded totals unchanged (the public contract is preserved through the fold).
        ConsumptionStats::invalidate();
        CHECK(ConsumptionStats::get(QStringLiteral("vid:V")).mediaSeconds == 40);
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video")) == 40);
        CHECK(ConsumptionStats::categoryPages() == 7);

        // Idempotent: a second migrate() is a no-op (run twice == once — no doubling).
        ConsumptionStats::migrate();
        ConsumptionStats::invalidate();
        CHECK(ConsumptionStats::get(QStringLiteral("vid:V")).mediaSeconds == 40);
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video")) == 40);
    }

    // ---- 10. Three device namespaces -> the readers sum EXACTLY across them ---------------------------------
    {
        // Profile "multi" carries the SAME title X in three foreign device namespaces (A/B/C) + per-device
        // category rollups. No local writes here, so the reader sees exactly A+B+C.
        const QString hx = hash(QStringLiteral("vid:X"));
        setRaw(QStringLiteral("stats/multi/A/items/") + hx, statsBlob(10, 0, 100, QStringLiteral("X"), QStringLiteral("video")));
        setRaw(QStringLiteral("stats/multi/B/items/") + hx, statsBlob(20, 0, 300, QStringLiteral("X"), QStringLiteral("video")));
        setRaw(QStringLiteral("stats/multi/C/items/") + hx, statsBlob(5,  0, 200, QStringLiteral("X"), QStringLiteral("video")));
        setRaw(QStringLiteral("stats/multi/A/cat/video/seconds"), QStringLiteral("10"));
        setRaw(QStringLiteral("stats/multi/B/cat/video/seconds"), QStringLiteral("20"));
        setRaw(QStringLiteral("stats/multi/C/cat/video/seconds"), QStringLiteral("5"));

        ProfileStore::setCurrent(QStringLiteral("multi"));
        ConsumptionStats::invalidate();
        CHECK(ConsumptionStats::get(QStringLiteral("vid:X")).mediaSeconds == 35);          // 10 + 20 + 5
        CHECK(ConsumptionStats::categorySeconds(QStringLiteral("video")) == 35);           // rollups sum too
        const auto top = ConsumptionStats::topTitles(QStringLiteral("video"), 10);
        CHECK(top.size() == 1 && top[0].first == hx);
        CHECK(top[0].second.mediaSeconds == 35);                                           // summed in topTitles
        // The newest device's activity (B @300) owns the display title/category.
        CHECK(ConsumptionStats::get(QStringLiteral("vid:X")).lastActivity == 300);
    }

    // ---- 11. PlayStats migration + aggregate readers (sum totals/sessions, MAX last-played) -----------------
    {
        const QString g = QStringLiteral("game:doom");
        // Seed a pre-upgrade profile "pmig" with an un-namespaced game.
        setRaw(QStringLiteral("playstats/pmig/") + sha1(g) + QStringLiteral("/total"),    QStringLiteral("100"));
        setRaw(QStringLiteral("playstats/pmig/") + sha1(g) + QStringLiteral("/sessions"), QStringLiteral("2"));
        setRaw(QStringLiteral("playstats/pmig/") + sha1(g) + QStringLiteral("/last"),     QStringLiteral("500"));

        ProfileStore::setCurrent(QStringLiteral("pmig"));
        PlayStats::migrate();

        CHECK(!hasGroupKeys(QStringLiteral("playstats/pmig/") + sha1(g)));                 // legacy game folded away
        CHECK(hasGroupKeys(QStringLiteral("playstats/pmig/") + dev + QStringLiteral("/") + sha1(g)));
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            CHECK(raw.value(QStringLiteral("playstats/pmig/schema")).toInt() >= 1);
        }
        {
            const PlayStats::Stat st = PlayStats::get(g);
            CHECK(st.totalSeconds == 100 && st.sessions == 2 && st.lastPlayed == 500);     // folded verbatim
        }
        CHECK(PlayStats::profileTotalSeconds() == 100);
        PlayStats::migrate();                                                              // idempotent
        CHECK(PlayStats::get(g).totalSeconds == 100);
        CHECK(PlayStats::profileTotalSeconds() == 100);
    }

    // ---- 12. PlayStats three device namespaces sum; the local writer targets ITS OWN namespace --------------
    {
        const QString g = QStringLiteral("game:quake");
        const QString sg = sha1(g);
        setRaw(QStringLiteral("playstats/pmulti/devA/") + sg + QStringLiteral("/total"),    QStringLiteral("10"));
        setRaw(QStringLiteral("playstats/pmulti/devA/") + sg + QStringLiteral("/sessions"), QStringLiteral("1"));
        setRaw(QStringLiteral("playstats/pmulti/devA/") + sg + QStringLiteral("/last"),     QStringLiteral("100"));
        setRaw(QStringLiteral("playstats/pmulti/devB/") + sg + QStringLiteral("/total"),    QStringLiteral("20"));
        setRaw(QStringLiteral("playstats/pmulti/devB/") + sg + QStringLiteral("/sessions"), QStringLiteral("2"));
        setRaw(QStringLiteral("playstats/pmulti/devB/") + sg + QStringLiteral("/last"),     QStringLiteral("300"));
        setRaw(QStringLiteral("playstats/pmulti/devC/") + sg + QStringLiteral("/total"),    QStringLiteral("5"));
        setRaw(QStringLiteral("playstats/pmulti/devC/") + sg + QStringLiteral("/sessions"), QStringLiteral("1"));
        setRaw(QStringLiteral("playstats/pmulti/devC/") + sg + QStringLiteral("/last"),     QStringLiteral("200"));

        ProfileStore::setCurrent(QStringLiteral("pmulti"));
        {
            const PlayStats::Stat st = PlayStats::get(g);
            CHECK(st.totalSeconds == 35);                       // 10 + 20 + 5
            CHECK(st.sessions == 4);                            // 1 + 2 + 1
            CHECK(st.lastPlayed == 300);                        // MAX(100, 300, 200)
        }
        CHECK(PlayStats::profileTotalSeconds() == 35);

        // A local session writes ONLY this device's namespace; the aggregate then includes it (no double-count).
        PlayStats::addSession(g, 7);
        {
            QSettings raw(iniPath, QSettings::IniFormat);
            CHECK(raw.value(QStringLiteral("playstats/pmulti/") + dev + QStringLiteral("/") + sg + QStringLiteral("/total")).toLongLong() == 7);
        }
        const PlayStats::Stat st2 = PlayStats::get(g);
        CHECK(st2.totalSeconds == 42 && st2.sessions == 5);     // foreign 35 + local 7
        CHECK(PlayStats::profileTotalSeconds() == 42);
        CHECK(!PlayStats::formatLastPlayed(st2.lastPlayed).isEmpty());
    }

    if (failures == 0) { std::puts("STATS-OK"); return 0; }
    std::fprintf(stderr, "STATS: %d check(s) failed\n", failures);
    return 1;
}
