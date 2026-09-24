#pragma once
// The quit budget (issue #442): quitting never waits on the network, and nothing on the quit path waits unbounded
// on a thread.
//
// WHAT HELD THE APP OPEN. QCoreApplication's destructor calls QThreadPool::globalInstance()->waitForDone() with no
// timeout. The bundled catalog add-ons run on that pool (AddonManager::dispatch -> QtConcurrent::run), and an
// add-on's http call is a synchronous wait on a nested event loop with a 20 s timeout and three attempts. Close the
// window while one is in flight to a slow host (Open Library's /subjects/fiction.json answered in 17 s when this
// was measured) and the process sat in ~QCoreApplication until the fetch gave up, up to a minute later. The #409
// check sends its SIGTERM 5 s after a fresh start, which is exactly while the home's first catalogs are loading.
//
// THE RULES THIS UNIT GIVES THE REST OF THE APP.
//   * begin() marks the process as quitting. It runs on aboutToQuit. Every synchronous network wait that can run
//     on a pool thread arms its loop with a Guard (or calls exec() below); begin() ends each armed loop at once,
//     and the caller then abandons its request instead of retrying. Nothing is sent on the strength of it: the
//     work was a fetch for a screen that is closing. Ending those waits frees pool threads, so the pool's queue
//     is cleared BEFORE the quit begins (issue #445): begin(pool) and drain() do both, in that order.
//   * ExitGate is the pool's timeout. After the event loop ends, main() drains the global pool: tasks that have
//     not started are dropped, and the running ones get kPoolMs to notice the cancel and return. A task that
//     still has not returned when the window has been torn down is abandoned to the process exit: settings
//     updates still pending are delivered, the log line says so, and the process ends without the destructor's
//     unbounded wait. That is the only place the app exits early, and it is logged.
//   * QuitFlush is the network budget for the work that DOES want to send something on quit (the save flush and
//     the settings bundle push). The jobs run side by side under ONE budget, kNetworkMs, for all of them
//     together. A job that has not landed by then is abandoned, and its `abandoned` hook leaves it queued durably
//     for the next launch (the bundle push records itself owed in PendingPush; the save flush needs nothing,
//     because the next launch's full reconcile uploads any save newer than its baseline).
//
// QtCore only and header-only, so every target that compiles an add-on or fetch unit gets it without a new
// source file.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVector>
#include <QtDebug>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <utility>

namespace QuitBudget
{
// Everything that wants to send something on quit, together, gets at most this long. Then it is abandoned and
// left to its durable record.
constexpr int kNetworkMs = 1500;
// Pool tasks, cancelled, get this long to return once the event loop has ended.
constexpr int kPoolMs = 1000;

namespace detail
{
struct State
{
    std::atomic<bool>  quitting{ false };
    QMutex             mutex;
    QSet<QEventLoop*>  loops;   // armed nested loops, each on its own (pool) thread
};
inline State& state()
{
    static State s;
    return s;
}
} // namespace detail

inline bool quitting() { return detail::state().quitting.load(std::memory_order_acquire); }

// Mark the process as quitting and end every armed loop. Idempotent, and safe from any thread. The flag is set
// BEFORE the lock is taken, so a Guard constructed after this call's iteration reads it as set; one constructed
// before it is in the set and gets a queued quit. No armed loop can miss the quit either way.
inline void begin()
{
    detail::State& s = detail::state();
    s.quitting.store(true, std::memory_order_release);
    const QMutexLocker lock(&s.mutex);
    for (QEventLoop* loop : std::as_const(s.loops))
        QMetaObject::invokeMethod(loop, &QEventLoop::quit, Qt::QueuedConnection);
}

// The same, for a pool whose threads the quit is about to free (issue #445). The pool's queued tasks are dropped
// FIRST: begin() ends the pool's waits, and a thread that comes free while the queue still holds work starts the
// next task at once, inside the exit's short window, where ExitGate may end the process under it. Cleared first,
// a freed thread can only return. The aboutToQuit hook calls this with the global pool.
inline void begin(QThreadPool* pool)
{
    if (pool) pool->clear();
    begin();
}

// Probes only: a probe runs several quit sequences in one process.
inline void resetForTesting() { detail::state().quitting.store(false, std::memory_order_release); }

// Arms a nested loop for the lifetime of the guard. A quit posted to it is removed with the loop if the loop dies
// first (QObject's destructor drops its posted events), and the set is only touched under the lock, so begin()
// never posts to a loop that has gone.
class Guard
{
public:
    explicit Guard(QEventLoop& loop) : loop_(&loop)
    {
        detail::State& s = detail::state();
        const QMutexLocker lock(&s.mutex);
        already_ = s.quitting.load(std::memory_order_acquire);
        if (!already_) s.loops.insert(loop_);
    }
    ~Guard()
    {
        detail::State& s = detail::state();
        const QMutexLocker lock(&s.mutex);
        s.loops.remove(loop_);
    }
    // The quit had already begun when the guard was made: do not enter the loop at all.
    bool quitting() const { return already_; }

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    QEventLoop* loop_;
    bool        already_ = false;
};

// Run `loop` unless the quit has begun, and end it when the quit begins. Returns true when the caller must abandon
// its request because the app is quitting.
inline bool exec(QEventLoop& loop)
{
    {
        const Guard guard(loop);
        if (!guard.quitting()) loop.exec();
    }
    return quitting();
}

// The pool's exit, in the only safe order (issue #445): drop the queued tasks, then begin the quit, which ends the
// running tasks' waits, then wait at most `budgetMs` for them. Callers do not call begin() first; drain() does
// it. A begin() that already ran (the aboutToQuit hook) is harmless: the queue is cleared again and begin() is
// idempotent. True when the pool is idle.
inline bool drain(QThreadPool* pool, int budgetMs)
{
    begin(pool);
    return !pool || pool->waitForDone(budgetMs);
}

// The global pool's bounded wait, and its fallback. Declared in main() right after the QApplication, so it is
// destroyed after the window and before the QApplication, whose destructor waits on the pool without a limit.
class ExitGate
{
public:
    ExitGate() = default;
    ExitGate(const ExitGate&) = delete;
    ExitGate& operator=(const ExitGate&) = delete;

    // Call when the event loop has returned, before the window is destroyed.
    void drain(int exitCode, QThreadPool* pool = QThreadPool::globalInstance())
    {
        pool_ = pool;
        code_ = exitCode;
        QElapsedTimer t;
        t.start();
        drained_ = QuitBudget::drain(pool, kPoolMs);   // clears the queue, THEN begins the quit (#445)
        if (drained_)
            qInfo().noquote() << QStringLiteral("quit: thread pool idle after %1 ms").arg(t.elapsed());
        else
            qWarning().noquote() << QStringLiteral("quit: %1 pool task(s) still running after %2 ms; "
                                                   "they get the rest of the teardown, then are abandoned")
                                        .arg(pool ? pool->activeThreadCount() : 0).arg(t.elapsed());
        armed_ = true;
    }

    bool drained() const { return drained_; }

    ~ExitGate()
    {
        if (!armed_ || drained_ || !pool_ || pool_->waitForDone(0)) return;   // idle now: the ordinary exit
        // Settings writes made during the teardown are still waiting for their UpdateRequest; deliver them now,
        // because the static QSettings destructors that would otherwise write them are about to be skipped.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
        qWarning().noquote() << QStringLiteral("quit: %1 pool task(s) never returned; ending the process "
                                               "without waiting for them")
                                    .arg(pool_->activeThreadCount());
        std::fflush(nullptr);
        std::_Exit(code_);
    }

private:
    QPointer<QThreadPool> pool_;
    int  code_    = 0;
    bool armed_   = false;
    bool drained_ = false;
};

// The quit-time network work, run side by side under one budget. See the header comment for the rules.
class QuitFlush
{
public:
    using Done = std::function<void(bool ok)>;
    struct Job
    {
        QString                   name;
        std::function<void(Done)> start;       // begin the work; call done(ok) when it lands (may be synchronous)
        std::function<void()>     abandoned;   // the budget ran out first: leave the work queued for next launch
    };

    // `finished(abandonedNames)` runs exactly once, POSTED to `context`'s event loop and never from inside run(),
    // so a caller inside closeEvent can call close() from it. It runs when every job has called done, or when
    // `budgetMs` has elapsed, whichever is first; each job still outstanding then has its `abandoned` hook run
    // first. A done() that arrives after that is ignored by the flush (the job's own callback still runs).
    static void run(QObject* context, int budgetMs, QVector<Job> jobs,
                    std::function<void(const QStringList& abandoned)> finished)
    {
        struct St
        {
            QVector<Job>  jobs;
            QVector<bool> settled;
            int           pending = 0;
            bool          over    = false;
            std::function<void(const QStringList&)> finished;
            QPointer<QObject> context;
        };
        auto st      = std::make_shared<St>();
        st->jobs     = std::move(jobs);
        st->settled  = QVector<bool>(st->jobs.size(), false);
        st->pending  = int(st->jobs.size());
        st->finished = std::move(finished);
        st->context  = context;

        auto finishNow = [](const std::shared_ptr<St>& s) {
            if (s->over) return;
            s->over = true;
            QStringList abandoned;
            for (int i = 0; i < s->jobs.size(); ++i)
            {
                if (s->settled.at(i)) continue;
                abandoned << s->jobs.at(i).name;
                if (s->jobs.at(i).abandoned) s->jobs.at(i).abandoned();
            }
            if (!s->context) return;
            auto fin = s->finished;
            QTimer::singleShot(0, s->context.data(), [fin, abandoned] { if (fin) fin(abandoned); });
        };

        // The budget is armed BEFORE any job starts, so a job that blocks in start() still counts against it.
        QTimer::singleShot(budgetMs, Qt::PreciseTimer, context, [st, finishNow] { finishNow(st); });
        if (st->pending == 0) { finishNow(st); return; }
        for (int i = 0; i < st->jobs.size(); ++i)
        {
            const auto start = st->jobs.at(i).start;
            Done done = [st, i, finishNow](bool) {
                if (st->over || st->settled.at(i)) return;
                st->settled[i] = true;
                if (--st->pending == 0) finishNow(st);
            };
            if (start) start(std::move(done));
            else       done(true);
        }
    }
};
} // namespace QuitBudget
