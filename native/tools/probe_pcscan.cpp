// Headless test for the persisted PC-launcher scan cache (src/core/PcScanCache, issue #62 part 1) — the unit
// that keeps the last SUCCESSFUL installed-scan per store so a store that is briefly unreadable (launcher
// closed, drive not mounted, manifest dir locked) falls back to it instead of dropping every game it holds.
// QtCore-only (a JSON blob per source in the shared everythingbox.ini), so it runs under offscreen QPA and
// pins:
//
//   * merge() — THE mutation-tested heart:
//       - an OK scan is authoritative: its entries replace the cache, forced available=true, EVEN when they
//         arrive marked unavailable, and an OK-but-EMPTY scan returns empty (a genuinely empty library does
//         NOT resurrect a stale cache);
//       - an UNREADABLE scan returns the CACHED entries, forced available=false, and IGNORES its own entries.
//   * toJson/fromJson — a faithful three-field round trip, with the JSON SHAPE checked independently of
//     fromJson so a dropped/renamed key is caught.
//   * reconcile() — the ini-backed entry point: an OK scan persists (loadCached sees it next time); an
//     UNREADABLE scan does NOT persist (the good cache stands, still all-available); an OK-EMPTY scan clears.
//
// Prints PCSCAN-OK on success; any failure prints PCSCAN-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the everythingbox.ini
// starts empty and is removed at exit — no seam, no defensive reset.
//
// FIXTURES ARE INDEPENDENT of the code under test: every expected value is a hand-written literal, and the
// persisted blob is read back both through loadCached AND straight off the ini with QSettings, so an
// assertion cannot pass merely because it re-ran the function it is checking.
#include "PcScanCache.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PCSCAN-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using pcscan::ScanEntry;
using pcscan::ScanResult;
using pcscan::ScanStatus;

// Value equality on the three fields — kept here rather than as an operator== on the struct so the probe's
// notion of "equal" is its own, not something the production type could quietly redefine.
static bool eq(const ScanEntry& a, const ScanEntry& b)
{
    return a.id == b.id && a.name == b.name && a.available == b.available;
}
static bool eq(const QVector<ScanEntry>& a, const QVector<ScanEntry>& b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) if (!eq(a[i], b[i])) return false;
    return true;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- merge(): the pure decision ---------------------------------------------------------------------
    // Two distinct fixtures so a mutation that returns the wrong operand is always visibly wrong: the cache
    // and the fresh scan share NO ids.
    const QVector<ScanEntry> cached = {
        { QStringLiteral("10"), QStringLiteral("Alpha"), true },
        { QStringLiteral("20"), QStringLiteral("Beta"),  true },
    };

    // An OK, non-empty scan REPLACES the cache and every entry is available. The fresh entry deliberately
    // arrives available=false to prove the OK branch forces it true rather than trusting the input.
    {
        ScanResult fresh; fresh.status = ScanStatus::Ok;
        fresh.entries = { { QStringLiteral("30"), QStringLiteral("Gamma"), false } };
        const QVector<ScanEntry> got = pcscan::merge(cached, fresh);
        const QVector<ScanEntry> want = { { QStringLiteral("30"), QStringLiteral("Gamma"), true } };
        CHECK(eq(got, want));   // fresh replaces cache; available forced true
    }

    // An OK, EMPTY scan returns EMPTY — a genuinely empty library is empty; it does NOT fall back to cache.
    {
        ScanResult fresh; fresh.status = ScanStatus::Ok;   // no entries
        const QVector<ScanEntry> got = pcscan::merge(cached, fresh);
        CHECK(got.isEmpty());
    }

    // An UNREADABLE scan returns the CACHED entries, every one marked unavailable, and IGNORES its own
    // entries (seeded here with an id that is in neither list, so a mutant returning fresh.entries is caught).
    {
        ScanResult fresh; fresh.status = ScanStatus::Unreadable;
        fresh.entries = { { QStringLiteral("99"), QStringLiteral("Ghost"), true } };
        const QVector<ScanEntry> got = pcscan::merge(cached, fresh);
        const QVector<ScanEntry> want = {
            { QStringLiteral("10"), QStringLiteral("Alpha"), false },
            { QStringLiteral("20"), QStringLiteral("Beta"),  false },
        };
        CHECK(eq(got, want));   // cached, forced unavailable, fresh.entries ignored
    }

    // Unreadable with an EMPTY cache is empty — nothing to fall back to.
    {
        ScanResult fresh; fresh.status = ScanStatus::Unreadable;
        CHECK(pcscan::merge({}, fresh).isEmpty());
    }

    // ---- toJson / fromJson: faithful three-field round trip ---------------------------------------------
    {
        const QVector<ScanEntry> entries = {
            { QStringLiteral("a1"), QStringLiteral("One"), true },
            { QStringLiteral("b2"), QStringLiteral("Two"), false },   // the false is what catches a dropped flag
        };
        const QJsonArray arr = pcscan::toJson(entries);

        // SHAPE, checked WITHOUT fromJson: the exact keys and values the blob must carry. A renamed or
        // dropped key fails here even though a symmetric toJson/fromJson pair would round-trip past it.
        CHECK(arr.size() == 2);
        const QJsonObject o0 = arr.at(0).toObject(), o1 = arr.at(1).toObject();
        CHECK(o0.value(QStringLiteral("id")).toString() == QStringLiteral("a1"));
        CHECK(o0.value(QStringLiteral("name")).toString() == QStringLiteral("One"));
        CHECK(o0.value(QStringLiteral("available")).toBool() == true);
        CHECK(o1.value(QStringLiteral("id")).toString() == QStringLiteral("b2"));
        CHECK(o1.value(QStringLiteral("available")).toBool() == false);

        // Round trip: back to the identical entries.
        CHECK(eq(pcscan::fromJson(arr), entries));

        // A legacy blob with no "available" field reads back as available (a last-good scan is all-available).
        QJsonArray legacy;
        { QJsonObject o; o.insert(QStringLiteral("id"), QStringLiteral("c3"));
          o.insert(QStringLiteral("name"), QStringLiteral("Three")); legacy.push_back(o); }
        const QVector<ScanEntry> fromLegacy = pcscan::fromJson(legacy);
        CHECK(fromLegacy.size() == 1 && fromLegacy[0].available == true
              && fromLegacy[0].id == QStringLiteral("c3"));
    }

    // ---- reconcile(): the ini-backed entry point --------------------------------------------------------
    // The raw ini leaf, addressed the way the store addresses it, so persistence is proven independent of
    // loadCached. iniKey("steam") == "pcscan/steam"; QSettings treats '/' as a group separator.
    QSettings ini(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                  QSettings::IniFormat);
    CHECK(pcscan::iniKey(QStringLiteral("steam")) == QStringLiteral("pcscan/steam"));

    // 1. An OK scan persists. The returned list is the fresh one (all available), and it survives to disk.
    {
        ScanResult fresh; fresh.status = ScanStatus::Ok;
        fresh.entries = { { QStringLiteral("440"), QStringLiteral("TF2"), true },
                          { QStringLiteral("620"), QStringLiteral("Portal 2"), true } };
        const QVector<ScanEntry> got = pcscan::reconcile(QStringLiteral("steam"), fresh);
        CHECK(got.size() == 2 && got[0].id == QStringLiteral("440") && got[0].available);

        ini.sync();   // pick up what reconcile's QSettings just wrote (separate handle)
        const QByteArray raw = ini.value(QStringLiteral("pcscan/steam")).toString().toUtf8();
        const QJsonArray onDisk = QJsonDocument::fromJson(raw).array();
        CHECK(onDisk.size() == 2 && onDisk.at(0).toObject().value(QStringLiteral("id")).toString()
                                        == QStringLiteral("440"));
        CHECK(eq(pcscan::loadCached(QStringLiteral("steam")), got));
    }

    // 2. An UNREADABLE scan does NOT persist. It returns the cache marked unavailable, but the STORED cache
    //    is untouched — loadCached still reads the last good scan, all available. This is the assertion that
    //    catches a reconcile that persists unconditionally (it would bake the unavailable flag onto disk).
    {
        ScanResult fresh; fresh.status = ScanStatus::Unreadable;
        const QVector<ScanEntry> got = pcscan::reconcile(QStringLiteral("steam"), fresh);
        CHECK(got.size() == 2 && !got[0].available && !got[1].available);

        const QVector<ScanEntry> reloaded = pcscan::loadCached(QStringLiteral("steam"));
        CHECK(reloaded.size() == 2 && reloaded[0].available && reloaded[1].available);
    }

    // 3. An OK-EMPTY scan CLEARS the persisted cache (does not resurrect it on the next read).
    {
        ScanResult fresh; fresh.status = ScanStatus::Ok;   // no entries
        const QVector<ScanEntry> got = pcscan::reconcile(QStringLiteral("steam"), fresh);
        CHECK(got.isEmpty());
        CHECK(pcscan::loadCached(QStringLiteral("steam")).isEmpty());
    }

    // A never-written source reads back empty.
    CHECK(pcscan::loadCached(QStringLiteral("nonesuch")).isEmpty());

    if (failures == 0) { std::printf("PCSCAN-OK\n"); return 0; }
    std::fprintf(stderr, "PCSCAN had %d failure(s)\n", failures);
    return 1;
}
