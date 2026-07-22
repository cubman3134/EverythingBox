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
#include "ProfileStore.h"
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
        reset.remove(QStringLiteral("profiles"));
        reset.sync();
    }
    ConsumptionStats::invalidate();

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
            // External write straight to the ini (bypasses the store, so no invalidate happens).
            const QString ik = QStringLiteral("stats/probeCache/items/") + hash(QStringLiteral("k"));
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
        // No empty-hash junk item key was written.
        QSettings ini(iniPath, QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("stats/probeJunk/items"));
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

    if (failures == 0) { std::puts("STATS-OK"); return 0; }
    std::fprintf(stderr, "STATS: %d check(s) failed\n", failures);
    return 1;
}
