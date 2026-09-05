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

    CHECK(!PerfTrace::enabled(), "disabled by default (no EB_PERF)");
    { PERF_SPAN("dead.span"); }              // must be a no-op while disabled
    PerfTrace::begin("dead.b"); PerfTrace::end("dead.b");
    CHECK(lines(log).isEmpty(), "disabled emits nothing to the test log");
    // (Do NOT assert the app's real perf_trace.log is absent — a prior EB_PERF run may have left one.)

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

    // begin-overwrite restarts the clock. The discriminator here is STRUCTURAL, not a wall-clock
    // threshold (#164): we bracket the post-restart window with our OWN timer, started before the
    // overwriting begin() and read after the end(). That bracket strictly contains the interval the
    // restarted clock measures, so a working restart logs <= the bracket BY CONSTRUCTION; a broken
    // one also carries the whole pre-restart sleep and lands a head-length above it. A scheduling
    // stall in the tail inflates the logged span and the bracket by the same amount, so load widens
    // the margin instead of eating it — there is no jitter budget left to blow.
    qint64 tailBracketMs = 0, preRestartHeadMs = 0;
    {
        QElapsedTimer whole, tailBracket;
        whole.start();
        PerfTrace::begin("unit.restart");
        QThread::msleep(30);                 // the head a non-restarted clock would still be carrying
        tailBracket.start();                 // ...opened BEFORE the overwrite...
        PerfTrace::begin("unit.restart");    // overwrite restarts the clock
        QThread::msleep(5);
        PerfTrace::end("unit.restart");
        tailBracketMs = tailBracket.elapsed();          // ...and closed AFTER the end() that logs it
        preRestartHeadMs = whole.elapsed() - tailBracketMs;
    }

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
      const qint64 logged = parts.size() >= 3 ? parts[2].toLongLong() : -1;
      printf("MEASURE unit.restart: logged %lldms, post-restart bracket %lldms, pre-restart head %lldms\n",
             (long long)logged, (long long)tailBracketMs, (long long)preRestartHeadMs);
      restartOk = parts.size() >= 3 && parts[1] == QStringLiteral("unit.restart")
                  // The logged span fits inside the bracket that contains only the post-restart window,
                  // so the overwrite restarted the clock. Had it not, the logged span would also carry
                  // the pre-restart head and sit ~preRestartHeadMs ABOVE the bracket.
                  && logged >= 0 && logged <= tailBracketMs
                  // Guard against the assertion going vacuous if the head sleep is ever dropped: the head
                  // must clear millisecond truncation by a wide margin, else the two cases could round
                  // together. This is a resolution floor, not a jitter budget — load only raises it.
                  && preRestartHeadMs >= 10; }
    CHECK(restartOk, "begin-overwrite restarts the clock");

    // ---- Component budgets: real hot-path builders/parsers over synthetic worst-case inputs ----------
    // Each budget = max(the measured cost on the 2026-07 dev box x 3, a 50ms floor so CI jitter can't
    // false-fail a low-single-digit-ms measurement). Numbers captured with EB_PERF off, offscreen QPA.
    // EVERY budget below is CPU-bound and says so in its failure text; a measurement whose cost is dominated
    // by the filesystem is PRINTED, never gated - a gate on one of those fails on a busy machine instead of
    // on a bad branch, which is the whole of #270.
    //
    // BEST of three, not worst (#270). A budget here exists to catch an ALGORITHMIC regression, and a
    // regression is present in every run; what differs between the three runs is only how much unrelated
    // machine load landed in each. The best run is therefore the least-polluted estimate of OUR cost, and
    // the worst is the run most decided by whatever else was touching the disk or the scheduler - taking it
    // let one hiccup in three decide the verdict, and the same binary scored 400 ms quiet against 891 ms
    // inside a loaded gate. None of these budgets is measuring jitter, so none of them wants the worst run.
    // (The disabled-span overhead check above is a different measurement - one straight-line CPU loop, timed
    // once - and keeps its shape.)
    auto bestOf3 = [](const std::function<void()>& run) {
        qint64 best = -1;
        for (int r = 0; r < 3; ++r) {
            QElapsedTimer t; t.start(); run();
            const qint64 e = t.elapsed();
            best = (best < 0) ? e : qMin(best, e);
        }
        return best;
    };

    // recentsCatalog over 5,000 synthetic RecentItems, with the per-item artwork lookup INJECTED (#270).
    // Un-stubbed, the mapping calls MetaCache::displayImage per item and that opens the item's cached
    // meta.json once per art role, so what this budget used to time was 15,000 filesystem opens with our
    // mapping somewhere inside them - a number that moved with the disk instead of with the code (400 ms
    // quiet against 891 ms during a loaded gate, same binary). Stubbed, the budget times the MAPPING: the
    // kind/system filtering, the art-key choice, the title fallback and the list growth, which is where an
    // algorithmic regression in this builder would actually land. The real lookup still runs below, so the
    // seam cannot rot into a stub nobody exercises.
    QList<RecentItem> recents;
    recents.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        RecentItem r;
        r.path = QStringLiteral("C:/games/rom_%1.nes").arg(i);
        r.title = QStringLiteral("Game %1").arg(i);
        r.kind = QStringLiteral("game");
        r.system = QStringLiteral("nes");
        r.ts = 1700000000 + i;
        r.thumb = QStringLiteral("http://art.example/%1.png").arg(i);
        recents << r;
    }

    // The injected fn is genuinely the one the mapping goes through - pinned as a VALUE on two items, so an
    // implementation that took the parameter and then called MetaCache anyway is caught too, not only one
    // that never calls it at all.
    {
        QList<RecentItem> two = recents.mid(0, 2);
        const MediaCatalog cat = browse::recentsCatalog(
            two, QStringLiteral("game"),
            [](const QString& key, const QString& url) { return QStringLiteral("stub:") + key + QLatin1Char('|') + url; });
        CHECK(cat.items.size() == 2
              && cat.items[0].thumbnailUrl
                     == QStringLiteral("stub:C:/games/rom_0.nes|http://art.example/0.png"),
              "injected art fn is what recentsCatalog maps through");
    }

    {
        int mapped = 0;
        qint64 artCalls = 0;
        const auto stubArt = [&artCalls](const QString&, const QString& url) -> QString { ++artCalls; return url; };
        const qint64 ms = bestOf3([&] {
            mapped = browse::recentsCatalog(recents, QStringLiteral("game"), stubArt).items.size();
        });
        printf("MEASURE recentsCatalog 5k (art stubbed, CPU-bound): %lld ms (%d mapped, %lld art calls)\n",
               (long long)ms, mapped, (long long)artCalls);
        // 30x the 4-5ms this measures on the 2026-09 dev box under a deliberately loaded machine (20 CPU +
        // 4 disk workers), and far BELOW the 440-613ms the same mapping costs when a per-item filesystem
        // touch is in it - so the regression that matters most here, someone putting I/O back into this
        // builder, still trips the budget by 3x while ordinary machine load cannot reach it.
        const int RECENTS_BUDGET_MS = 150;
        // artCalls pins the stub as the thing the three budgeted runs actually called: 3 x 5,000, once per item.
        CHECK(mapped == 5000 && artCalls == 15000 && ms < RECENTS_BUDGET_MS,
              "budget: recentsCatalog 5k under budget (CPU-bound: artwork lookup stubbed)");
    }

    // ...and one run through the REAL MetaCache::displayImage, so the seam above cannot drift away from the
    // code the app runs. NO BUDGET, deliberately: this measurement is I/O-BOUND - it is dominated by per-item
    // filesystem opens and tracks whatever else is using the disk, not this repository's code. It is printed
    // so a human can watch it; it is not gated, because gating it is the mistake #270 records.
    {
        QElapsedTimer t; t.start();
        const int mappedReal = browse::recentsCatalog(recents, QStringLiteral("game")).items.size();
        printf("MEASURE recentsCatalog 5k (real MetaCache::displayImage, I/O-bound, NOT budgeted): %lld ms (%d mapped)\n",
               (long long)t.elapsed(), mappedReal);
        CHECK(mappedReal == 5000, "recentsCatalog real-artwork path still maps every item");
    }

    // pcGamesCatalog over 5,000 synthetic games with an injected pure poster fn (no librarycache I/O). This
    // replaced the steamGamesCatalog budget when the four per-launcher folders became one: the merged builder
    // does strictly more per entry (normalise the title, hash it into a group, sort the sources and then the
    // items), and it is now the ONLY thing between a launcher scan and the grid — plus the re-derivation path
    // runs it again on every Play. A regression here is felt on every PC-library open, so it keeps a budget.
    {
        QList<SteamGame> steam;
        steam.reserve(5000);
        for (int i = 0; i < 5000; ++i) {
            SteamGame g;
            g.appid = QString::number(1000 + i);
            g.name = QStringLiteral("Steam Game %1").arg(i);
            steam << g;
        }
        const auto poster = [](const QVector<pcgame::PcGameSource>& v) {
            return v.isEmpty() ? QString() : QStringLiteral("poster:") + v.first().launchId;
        };
        int mapped = 0;
        const qint64 ms = bestOf3([&] {
            mapped = browse::pcGamesCatalog(steam, {}, {}, {}, {}, QString(), QString(), poster).items.size();
        });
        printf("MEASURE pcGamesCatalog 5k: %lld ms (%d mapped)\n", (long long)ms, mapped);
        const int PCGAMES_BUDGET_MS = 200; // measured worst 23ms on 2026-07 dev box (per-title normalise + group + two sorts); 8x headroom for a slower CI box
        CHECK(mapped == 5000 && ms < PCGAMES_BUDGET_MS,
              "budget: pcGamesCatalog 5k under budget (CPU-bound: poster fn injected)");
    }

    // parseM3u on a generated 10,000-entry IPTV-style playlist string.
    {
        QString playlist = QStringLiteral("#EXTM3U\n");
        playlist.reserve(10000 * 64);
        for (int i = 0; i < 10000; ++i)
            playlist += QStringLiteral("#EXTINF:-1,Channel %1\nhttp://host.example/path/stream_%1.ts\n").arg(i);
        const QString src = QStringLiteral("http://host.example/lists/playlist.m3u8");
        int parsed = 0;
        const qint64 ms = bestOf3([&] { parsed = StreamResolver::parseM3u(playlist, src).size(); });
        printf("MEASURE parseM3u 10k: %lld ms (%d parsed)\n", (long long)ms, parsed);
        const int M3U_BUDGET_MS = 50; // measured worst 12ms on 2026-07 dev box; 3x+ headroom (50ms floor dominates)
        CHECK(parsed == 10000 && ms < M3U_BUDGET_MS,
              "budget: parseM3u 10k-entry under budget (CPU-bound: string parsing only)");
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
        const qint64 ms = bestOf3([&] {
            QSet<QString> seen;
            accepted = 0;
            for (const MediaItem& it : items)
                if (SearchAggregator::acceptResult(it, seen)) ++accepted;
        });
        printf("MEASURE acceptResult 10k(50%% dup): %lld ms (%d accepted)\n", (long long)ms, accepted);
        const int DEDUP_BUDGET_MS = 50; // measured worst 3ms on 2026-07 dev box; 3x+ headroom (50ms floor dominates)
        CHECK(accepted == 5000 && ms < DEDUP_BUDGET_MS,
              "budget: acceptResult 10k 50%-dup under budget (CPU-bound: in-memory set)");
    }

    if (fails == 0) printf("PERF-OK\n");
    return fails == 0 ? 0 : 1;
}
