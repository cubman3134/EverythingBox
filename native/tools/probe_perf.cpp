// Headless test for the PerfTrace span harness (phase-2 perf track): gating, line format,
// begin/end orphan semantics, and the disabled-path overhead budget. Prints PERF-OK.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include "../src/core/PerfTrace.h"
#include "../src/browse/SyntheticCatalogs.h"
#include "../src/browse/SearchAggregator.h"
#include "../src/media/StreamResolver.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

static QStringList lines(const QString& p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString log = tmp.filePath("perf.log");

    CHECK(!PerfTrace::enabled(), "disabled by default (no MMV_PERF)");
    { PERF_SPAN("dead.span"); }              // must be a no-op while disabled
    PerfTrace::begin("dead.b"); PerfTrace::end("dead.b");
    CHECK(lines(log).isEmpty(), "disabled emits nothing to the test log");
    // (Do NOT assert the app's real perf_trace.log is absent — a prior MMV_PERF run may have left one.)

    // Disabled-path overhead budget: 1M scoped spans well under 200ms (it's one branch each).
    { QElapsedTimer t; t.start();
      for (int i = 0; i < 1000000; ++i) { PERF_SPAN("dead.hot"); }
      CHECK(t.elapsed() < 200, "disabled span overhead under budget"); }

    PerfTrace::forceEnableForTest(log);
    CHECK(PerfTrace::enabled(), "forceEnableForTest enables");

    { PERF_SPAN("unit.scope"); QThread::msleep(12); }
    PerfTrace::begin("unit.be");
    QThread::msleep(5);
    PerfTrace::end("unit.be", QStringLiteral("n=3"));
    PerfTrace::end("unit.orphan");           // no begin -> silent no-op
    PerfTrace::begin("unit.restart");
    QThread::msleep(30);
    PerfTrace::begin("unit.restart");        // overwrite restarts the clock
    QThread::msleep(5);
    PerfTrace::end("unit.restart");

    const QStringList out = lines(log);
    CHECK(out.size() == 3, "exactly the three real spans logged");
    // Format: ISO-ts | span | ms | detail(optional)
    const QRegularExpression re(QStringLiteral(
        "^\\d{4}-\\d{2}-\\d{2}T[0-9:.]+ \\| [a-z.]+ \\| \\d+(?: \\| .*)?$"));
    bool fmt = true;
    for (const QString& l : out) fmt = fmt && re.match(l).hasMatch();
    CHECK(fmt, "line format ISO-ts | span | ms | detail");
    CHECK(out[0].contains("unit.scope"), "scope span logged");
    CHECK(out[1].contains("unit.be") && out[1].contains("n=3"), "begin/end span carries detail");
    bool restartOk = false;
    { const QStringList parts = out[2].split(QStringLiteral(" | "));
      restartOk = parts.size() >= 3 && parts[1] == QStringLiteral("unit.restart")
                  && parts[2].toLongLong() < 30; }   // 5ms run, NOT 35ms — overwrite restarted it (J20: 25->30,
                                                      // the 5ms nominal has enough scheduling jitter on a loaded box)
    CHECK(restartOk, "begin-overwrite restarts the clock");

    // ---- Component budgets: real hot-path builders/parsers over synthetic worst-case inputs ----------
    // Each budget = max(measured worst-of-3 on the 2026-07 dev box x 3, a 50ms floor so CI jitter can't
    // false-fail a low-single-digit-ms measurement). Numbers captured with MMV_PERF off, offscreen QPA.
    auto worstOf3 = [](const std::function<void()>& run) {
        qint64 w = 0;
        for (int r = 0; r < 3; ++r) { QElapsedTimer t; t.start(); run(); w = qMax(w, t.elapsed()); }
        return w;
    };

    // recentsCatalog over 5,000 synthetic RecentItems. NOTE: the item mapping calls MetaCache::displayImage
    // per item (disk-path existence checks for cached art), so the cost includes real filesystem stat's —
    // there's no injection point. The budget carries that I/O in, with headroom scaled up accordingly.
    {
        QList<RecentItem> recents;
        recents.reserve(5000);
        for (int i = 0; i < 5000; ++i) {
            RecentItem r;
            r.path = QStringLiteral("C:/games/rom_%1.nes").arg(i);
            r.title = QStringLiteral("Game %1").arg(i);
            r.kind = QStringLiteral("game");
            r.system = QStringLiteral("nes");
            r.ts = 1700000000 + i;
            recents << r;
        }
        int mapped = 0;
        const qint64 ms = worstOf3([&] { mapped = browse::recentsCatalog(recents, QStringLiteral("game")).items.size(); });
        printf("MEASURE recentsCatalog 5k: %lld ms (%d mapped)\n", (long long)ms, mapped);
        const int RECENTS_BUDGET_MS = 700; // measured worst 213ms on 2026-07 dev box (per-item MetaCache::displayImage disk stats dominate & vary run-to-run 183-213ms); 3x+ headroom, extra for slower CI disk
        CHECK(mapped == 5000 && ms < RECENTS_BUDGET_MS, "budget: recentsCatalog 5k under budget");
    }

    // steamGamesCatalog over 5,000 synthetic games with an injected pure poster fn (no librarycache I/O).
    {
        QList<SteamGame> steam;
        steam.reserve(5000);
        for (int i = 0; i < 5000; ++i) {
            SteamGame g;
            g.appid = QString::number(1000 + i);
            g.name = QStringLiteral("Steam Game %1").arg(i);
            steam << g;
        }
        const auto poster = [](const SteamGame& g) { return QStringLiteral("poster:") + g.appid; };
        int mapped = 0;
        const qint64 ms = worstOf3([&] { mapped = browse::steamGamesCatalog(steam, QString(), poster).items.size(); });
        printf("MEASURE steamGamesCatalog 5k: %lld ms (%d mapped)\n", (long long)ms, mapped);
        const int STEAM_BUDGET_MS = 50; // measured worst 3ms on 2026-07 dev box; 3x+ headroom (50ms floor dominates so CI jitter can't false-fail)
        CHECK(mapped == 5000 && ms < STEAM_BUDGET_MS, "budget: steamGamesCatalog 5k under budget");
    }

    // parseM3u on a generated 10,000-entry IPTV-style playlist string.
    {
        QString playlist = QStringLiteral("#EXTM3U\n");
        playlist.reserve(10000 * 64);
        for (int i = 0; i < 10000; ++i)
            playlist += QStringLiteral("#EXTINF:-1,Channel %1\nhttp://host.example/path/stream_%1.ts\n").arg(i);
        const QString src = QStringLiteral("http://host.example/lists/playlist.m3u8");
        int parsed = 0;
        const qint64 ms = worstOf3([&] { parsed = StreamResolver::parseM3u(playlist, src).size(); });
        printf("MEASURE parseM3u 10k: %lld ms (%d parsed)\n", (long long)ms, parsed);
        const int M3U_BUDGET_MS = 50; // measured worst 12ms on 2026-07 dev box; 3x+ headroom (50ms floor dominates)
        CHECK(parsed == 10000 && ms < M3U_BUDGET_MS, "budget: parseM3u 10k-entry under budget");
    }

    // acceptResult over 10,000 items with 50% duplicates (each title|type appears exactly twice).
    {
        QVector<MediaItem> items;
        items.reserve(10000);
        for (int i = 0; i < 10000; ++i) {
            MediaItem it;
            it.title = QStringLiteral("Result %1").arg(i / 2); // i/2 -> each unique title emitted twice
            it.type = QStringLiteral("game");
            items << it;
        }
        int accepted = 0;
        const qint64 ms = worstOf3([&] {
            QSet<QString> seen;
            accepted = 0;
            for (const MediaItem& it : items)
                if (SearchAggregator::acceptResult(it, seen)) ++accepted;
        });
        printf("MEASURE acceptResult 10k(50%% dup): %lld ms (%d accepted)\n", (long long)ms, accepted);
        const int DEDUP_BUDGET_MS = 50; // measured worst 3ms on 2026-07 dev box; 3x+ headroom (50ms floor dominates)
        CHECK(accepted == 5000 && ms < DEDUP_BUDGET_MS, "budget: acceptResult 10k 50%-dup under budget");
    }

    if (fails == 0) printf("PERF-OK\n");
    return fails == 0 ? 0 : 1;
}
