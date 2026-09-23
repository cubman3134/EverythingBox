// Headless check of core/QuitBudget (issue #442): quitting never waits on the network, and nothing on the quit
// path waits unbounded on a thread.
//
// Every stalled dependency here is a loopback server that accepts the connection, reads the request and never
// answers, so the network is faked with no real host involved.
//
// What this probe pins:
//   A. POOL FETCHES END ON QUIT. Three add-on http calls (AddonContext::httpGet, the call that held the app open)
//      and one BoundedFetch::get are in flight on a pool, each connected to the stalled server. begin() plus
//      drain() leave the pool idle inside QuitBudget::kPoolMs. Every call answered as a failure, none retried
//      (the server saw no new connection afterwards), and a task still queued never started.
//      A loop armed after the quit began is never entered.
//   B. THE QUIT NETWORK BUDGET. QuitFlush runs a fast job and a job stalled on the server under one budget. The
//      completion arrives once, POSTED (never inline from run()), no later than the decided 1.5 s budget plus
//      slack, with only the stalled job abandoned. Its abandoned hook leaves the push owed in PendingPush, the
//      durable record the next launch retries from. A late done() does not finish it twice. With every job
//      landing at once, the completion comes at once and no abandoned hook runs.
//   C. THE EXIT GATE. In a child process, a pool task that ignores every cancel (a 30 s sleep) cannot hold the
//      exit: ExitGate::drain gives up after kPoolMs and the gate ends the process with the event loop's code.
//      With an idle pool the gate stands aside and the process returns normally.
// What it cannot see: that main.cpp and MainWindow::closeEvent use these. The release workflow's #409 SIGTERM
// check and the Windows close-to-exit timings in the #442 report are the end-to-end evidence for that.
//
// Prints QUITBUDGET-OK on success; any failure prints QUITBUDGET-FAIL <cond> (line) and exits non-zero.
#include "QuitBudget.h"
#include "BoundedFetch.h"
#include "PendingPush.h"
#include "AddonContext.h"
#include "AddonModels.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QThreadPool>

#include <atomic>
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "QUITBUDGET-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The decision (#442), restated here on purpose: the probe must fail if the constant drifts from it.
static constexpr int kDecidedNetworkBudgetMs = 1500;

// Accepts, reads, never answers.
class StallServer : public QTcpServer
{
public:
    StallServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = nextPendingConnection()) { ++accepted; held.push_back(s); }
        });
    }
    QString url(const QString& path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path);
    }
    int accepted = 0;
    QVector<QTcpSocket*> held;
};

static void pumpUntil(const std::function<bool()>& done, int maxMs)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < maxMs)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// ---- C: the child process ----------------------------------------------------------------------------------
static int exitGateChild(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        QThreadPool idle;
        QuitBudget::ExitGate gate;
        gate.drain(5, &idle);
        std::printf("EXITGATE-IDLE drained=%d\n", gate.drained() ? 1 : 0);
        std::fflush(stdout);
    }   // an idle pool: the gate stands aside and we get here
    std::printf("EXITGATE-ORDINARY\n");
    std::fflush(stdout);

    QThreadPool stuck;
    stuck.start([] { QThread::sleep(30); });   // ignores every cancel there is
    {
        QuitBudget::ExitGate gate;
        gate.drain(7, &stuck);
        std::printf("EXITGATE-STUCK drained=%d\n", gate.drained() ? 1 : 0);
        std::fflush(stdout);
    }   // must end the process here with code 7
    std::printf("EXITGATE-FELL-THROUGH\n");
    std::fflush(stdout);
    return 3;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--exitgate-child") == 0) return exitGateChild(argc, argv);

    QCoreApplication app(argc, argv);
    QNetworkProxyFactory::setUseSystemConfiguration(false);   // loopback, never a system proxy

    StallServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));

    // ---- A: pool fetches end on quit ------------------------------------------------------------------------
    {
        QuitBudget::resetForTesting();
        AddonManifest m;
        m.id = QStringLiteral("com.probe.quitbudget");
        m.permissions = QStringList{ QStringLiteral("network") };
        const QString storage = QDir::tempPath();

        QThreadPool pool;
        pool.setMaxThreadCount(4);
        std::atomic<int>  addonDone{ 0 }, addonEmpty{ 0 };
        std::atomic<bool> boundedDone{ false }, boundedQuit{ false }, queuedRan{ false };
        const QString boundedUrl = server.url(QStringLiteral("/bounded"));
        for (int i = 0; i < 3; ++i)
            pool.start([&, url = server.url(QStringLiteral("/addon/%1").arg(i))] {
                const AddonContext ctx(m, storage);
                const QString body = ctx.httpGet(url);
                if (body.isEmpty()) ++addonEmpty;
                ++addonDone;
            });
        pool.start([&] {
            const BoundedFetch::Result r = BoundedFetch::get(boundedUrl, 20000, 1 << 20);
            boundedQuit = (r.verdict == BoundedFetch::Result::Failed && r.error == QStringLiteral("quitting"));
            boundedDone = true;
        });
        pool.start([&] { queuedRan = true; });   // the pool is full: this one is still queued at the quit

        pumpUntil([&] { return server.accepted >= 4; }, 10000);
        CHECK(server.accepted == 4);
        CHECK(addonDone == 0 && !boundedDone);   // all four really are waiting on the stalled server

        QElapsedTimer t;
        t.start();
        QuitBudget::begin();
        const bool drained = QuitBudget::drain(&pool, QuitBudget::kPoolMs);
        const qint64 ms = t.elapsed();
        std::printf("A: pool drained=%d in %lld ms (budget %d ms)\n", drained ? 1 : 0, (long long)ms,
                    QuitBudget::kPoolMs);
        CHECK(drained);
        CHECK(ms < QuitBudget::kPoolMs);
        CHECK(addonDone == 3);
        CHECK(addonEmpty == 3);
        CHECK(boundedDone);
        CHECK(boundedQuit);
        CHECK(!queuedRan);

        // Abandoned, not retried: no new connection reaches the stalled server afterwards.
        pumpUntil([] { return false; }, 1500);
        CHECK(server.accepted == 4);

        // A loop armed once the quit has begun is never entered.
        QEventLoop late;
        {
            const QuitBudget::Guard g(late);
            CHECK(g.quitting());
        }
        QElapsedTimer lt;
        lt.start();
        CHECK(QuitBudget::exec(late));
        CHECK(lt.elapsed() < 100);
        pool.waitForDone(60000);   // RED builds only: let a stuck fetch give up before the pool is destroyed
    }

    // ---- B: the quit network budget -------------------------------------------------------------------------
    CHECK(QuitBudget::kNetworkMs <= kDecidedNetworkBudgetMs);
    {
        QuitBudget::resetForTesting();
        PendingPush::clear();
        CHECK(!PendingPush::owed(PendingPush::load()));

        QNetworkAccessManager nam;
        QPointer<QNetworkReply> stalled;
        int  finishedCalls = 0;
        bool finishedInline = false, fastAbandoned = false, stalledHook = false;
        qint64 finishedAt = -1;
        QStringList abandonedOut;
        QElapsedTimer t;

        QVector<QuitBudget::QuitFlush::Job> jobs;
        jobs.push_back({ QStringLiteral("save flush"),
                         [](QuitBudget::QuitFlush::Done done) { done(true); },   // lands inline
                         [&] { fastAbandoned = true; } });
        jobs.push_back({ QStringLiteral("settings push"),
                         [&](QuitBudget::QuitFlush::Done done) {
                             stalled = nam.get(QNetworkRequest(QUrl(server.url(QStringLiteral("/push")))));
                             QNetworkReply* r = stalled;
                             QObject::connect(r, &QNetworkReply::finished, r, [r, done] {
                                 done(r->error() == QNetworkReply::NoError);
                             });
                         },
                         [&] {
                             // What MainWindow's hook does through recordPushOutcome(false): the push is owed.
                             stalledHook = true;
                             PendingPush::save(PendingPush::onOutcome(PendingPush::load(), PendingPush::Outcome::Offline,
                                                                      QDateTime::currentMSecsSinceEpoch()));
                         } });
        QObject ctx;
        t.start();
        QuitBudget::QuitFlush::run(&ctx, QuitBudget::kNetworkMs, jobs, [&](const QStringList& abandoned) {
            ++finishedCalls;
            finishedAt   = t.elapsed();
            abandonedOut = abandoned;
        });
        finishedInline = (finishedCalls != 0);
        pumpUntil([&] { return finishedCalls > 0; }, kDecidedNetworkBudgetMs + 5000);
        std::printf("B: stalled push abandoned after %lld ms (budget %d ms): %s\n", (long long)finishedAt,
                    QuitBudget::kNetworkMs, qPrintable(abandonedOut.join(QStringLiteral(", "))));
        CHECK(!finishedInline);
        CHECK(finishedCalls == 1);
        CHECK(finishedAt >= QuitBudget::kNetworkMs - 50);
        CHECK(finishedAt >= 0 && finishedAt <= kDecidedNetworkBudgetMs + 500);
        CHECK(abandonedOut == QStringList{ QStringLiteral("settings push") });
        CHECK(!fastAbandoned);
        CHECK(stalledHook);
        CHECK(PendingPush::owed(PendingPush::load()));   // left queued, durably, for the next launch

        // A late answer does not finish it twice.
        if (stalled) stalled->abort();
        pumpUntil([] { return false; }, 200);
        CHECK(finishedCalls == 1);
        PendingPush::clear();
    }
    {
        // Everything lands at once: done at once, no hook, and still never inline.
        int finishedCalls = 0, hooks = 0;
        qint64 finishedAt = -1;
        QStringList abandonedOut{ QStringLiteral("sentinel") };
        QVector<QuitBudget::QuitFlush::Job> jobs;
        for (int i = 0; i < 2; ++i)
            jobs.push_back({ QStringLiteral("job%1").arg(i), [](QuitBudget::QuitFlush::Done d) { d(true); },
                             [&] { ++hooks; } });
        QObject ctx;
        QElapsedTimer t;
        t.start();
        QuitBudget::QuitFlush::run(&ctx, QuitBudget::kNetworkMs, jobs, [&](const QStringList& a) {
            ++finishedCalls;
            finishedAt   = t.elapsed();
            abandonedOut = a;
        });
        CHECK(finishedCalls == 0);
        pumpUntil([&] { return finishedCalls > 0; }, 2000);
        CHECK(finishedCalls == 1);
        CHECK(finishedAt >= 0 && finishedAt < 200);
        CHECK(abandonedOut.isEmpty());
        CHECK(hooks == 0);
        pumpUntil([] { return false; }, QuitBudget::kNetworkMs + 200);   // the budget timer fires into nothing
        CHECK(finishedCalls == 1);
        CHECK(hooks == 0);
    }

    // ---- C: the exit gate -----------------------------------------------------------------------------------
    {
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        QElapsedTimer t;
        t.start();
        child.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--exitgate-child") });
        CHECK(child.waitForStarted(10000));
        const bool ended = child.waitForFinished(QuitBudget::kPoolMs + 10000);
        const qint64 ms = t.elapsed();
        if (!ended) { child.kill(); child.waitForFinished(5000); }
        const QString out = QString::fromUtf8(child.readAll());
        std::printf("C: child ended=%d code=%d after %lld ms\n%s", ended ? 1 : 0, child.exitCode(), (long long)ms,
                    qPrintable(out));
        CHECK(ended);
        CHECK(child.exitStatus() == QProcess::NormalExit);
        CHECK(child.exitCode() == 7);
        CHECK(out.contains(QStringLiteral("EXITGATE-IDLE drained=1")));
        CHECK(out.contains(QStringLiteral("EXITGATE-ORDINARY")));
        CHECK(out.contains(QStringLiteral("EXITGATE-STUCK drained=0")));
        CHECK(!out.contains(QStringLiteral("EXITGATE-FELL-THROUGH")));
        CHECK(ms < QuitBudget::kPoolMs + 8000);   // a child that waited out the 30 s task would fail this
    }

    if (failures)
    {
        std::fprintf(stderr, "QUITBUDGET-FAIL %d check(s)\n", failures);
        return 1;
    }
    std::printf("QUITBUDGET-OK\n");
    return 0;
}
