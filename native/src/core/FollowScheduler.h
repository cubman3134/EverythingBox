// The background pass behind "Follow a series" (issue #155, increment 1): once a cycle, ask each followed
// series' source what children it has now, diff that against what this device last saw, and file whatever is
// new. Politeness is the feature, not a nicety — see FollowPlan.h, which owns every RULE this class applies.
// What lives here is the state machine that applies them: the queue, the per-source slots, the cycle
// boundary, and the seams that make all of it drivable from a probe with no event loop and no network.
//
// EVERY INPUT IS INJECTED, and that is why this file has no Settings include, no AddonManager include and no
// clock:
//
//   * setClock       — the wall clock. probe_follow supplies a fake one it advances by hand, which is the
//                      only way to assert an interval, a jitter bound and a five-second per-source gap
//                      without a test that sleeps (a test that sleeps is a test that is flaky on CI).
//   * setFetcher     — "ask this source for this series' children". Production routes it to
//                      AddonManager::requestDetail, which already runs the addon OFF THE GUI THREAD and
//                      marshals its reply back — so the "all off the GUI thread" requirement is met by the
//                      plumbing that already exists rather than by a second thread pool here. The callback
//                      may fire synchronously (the probe does) or many seconds later (production); the pump
//                      is re-entrancy-safe either way.
//   * setListSource  — which series are followed. Defaults to FollowStore::list.
//   * setIsPlaying   — "is anything playing right now". A refresh mid-playback is exactly the hitch the
//                      CatalogPrefetcher's gameplay gate exists to avoid, and libretro's frame loop is on the
//                      main thread; a skipped cycle is retried on the next tick, never dropped.
//   * setIsMetered   — "is this connection metered". Skipped by default; the user can allow it.
//
// A CYCLE IS ATOMIC ABOUT FAILURE. A source that errors is recorded as failed FOR THIS CYCLE and every other
// series it holds is dropped from the queue rather than tried — "retried next cycle, not hammered" is
// otherwise a claim about one series, when the whole point is a user with forty followed shows on one dead
// addon. The failure set is cleared at the cycle boundary and nowhere else.
//
// SEAMS LEFT FOR THE LATER INCREMENTS, named here so they are not re-invented:
//   * newItemsFound(seriesId, count) — the signal increment 2's notifier consumes. Emitted once per series
//     per cycle, with the count, so a grouped notification ("4 new items across 2 series") is a matter of
//     collecting them until cycleFinished rather than of re-reading the store.
//   * cycleFinished(seriesChecked, newItems) — the end-of-pass hook the same notifier groups on, and where
//     increment 3's auto-download would start its queue.
#pragma once
#include "FollowPlan.h"
#include "FollowStore.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QTimer;

class FollowScheduler : public QObject
{
    Q_OBJECT
public:
    // The reply to one "what are this series' children now" question. `ok == false` marks the SOURCE failed
    // for the rest of the cycle; the children are ignored in that case.
    using FetchDone = std::function<void(bool ok, const QVector<follow::Child>& children)>;
    using Fetcher   = std::function<void(const FollowItem& item, FetchDone done)>;

    explicit FollowScheduler(QObject* parent = nullptr);

    void setClock(std::function<qint64()> nowFn);
    void setFetcher(Fetcher f)                                { fetch_ = std::move(f); }
    void setListSource(std::function<QVector<FollowItem>()> f) { list_ = std::move(f); }
    void setIsPlaying(std::function<bool()> f)                { playing_ = std::move(f); }
    void setIsMetered(std::function<bool()> f)                { metered_ = std::move(f); }

    void setIntervalHours(int hours);            // 0 = manual (only "Check now" runs a pass)
    int  intervalHours() const { return intervalHours_; }
    void setAllowMetered(bool allow)             { allowMetered_ = allow; }
    void setJitterSeed(quint32 seed)             { jitterSeed_ = seed; }
    qint64 jitter() const;                       // the deterministic offset this install runs at

    // Test seam (probe_follow): with periodic OFF nothing arms a QTimer, so the probe drives the whole
    // machine by calling tick() against its own clock. Production leaves it on.
    void setPeriodic(bool on)                    { periodic_ = on; }

    void start();        // arm the periodic tick (no-op when periodic is off)
    void checkNow();     // the manual verb: run a pass now, bypassing the playing/metered skips
    void tick();         // the pump. Idempotent; safe to call as often as you like.

    // ---- introspection, for probe_follow -------------------------------------------------------------
    bool cycleActive()   const { return cycleActive_; }
    int  issued()        const { return issued_; }         // fetches dispatched, cumulative
    int  cyclesRun()     const { return cyclesRun_; }       // cycles that reached their end
    int  skippedPlaying()const { return skippedPlaying_; }  // ticks a cycle was due but playback held it
    int  skippedMetered()const { return skippedMetered_; }  // ticks a cycle was due but the link was metered
    int  deferred()      const { return deferred_; }        // series dropped because their source had failed
    int  newFound()      const { return newFound_; }        // children announced, cumulative
    int  queued()        const { return int(queue_.size()); }
    int  inFlight()      const { return int(busy_.size()); }
    qint64 nextDueAt()   const;                             // -1 while manual

signals:
    // INCREMENT 2 SEAM. One emission per series that grew, carrying how many children it grew by.
    void newItemsFound(const QString& seriesId, int count);
    // INCREMENT 2/3 SEAM. The pass finished: how many series were asked, and how many new children in total.
    void cycleFinished(int seriesChecked, int newItems);

private:
    struct Job { FollowItem item; QString sourceId; };

    void  beginCycleIfDue();
    void  pump();
    void  dispatch(const Job& job);
    void  onFetched(const Job& job, bool ok, const QVector<follow::Child>& children);
    void  endCycle();
    void  reapStalled();
    qint64 now() const;
    // A source's identity for throttling. The addon id where there is one; a series with no source addon is
    // filed under a single "" bucket, which is the conservative answer — unknown sources share one slot
    // rather than each getting their own.
    static QString sourceOf(const FollowItem& it) { return it.addonId; }

    std::function<qint64()> nowFn_;
    Fetcher fetch_;
    std::function<QVector<FollowItem>()> list_;
    std::function<bool()> playing_;
    std::function<bool()> metered_;

    QTimer* timer_ = nullptr;
    int     intervalHours_ = 24;
    bool    allowMetered_ = false;
    bool    periodic_ = true;
    quint32 jitterSeed_ = 0;

    bool    cycleActive_ = false;
    bool    manualPending_ = false;   // a "Check now" waiting to start (bypasses the playing/metered gates)
    bool    inPump_ = false;
    bool    pumpAgain_ = false;
    QVector<Job>            queue_;
    QHash<QString, qint64>  lastSent_;    // source -> the second its last request went out
    QHash<QString, qint64>  busy_;        // source -> the second its in-flight request went out
    QSet<QString>           failed_;      // sources that failed in THIS cycle
    int cycleChecked_ = 0;
    int cycleNew_ = 0;

    int issued_ = 0, cyclesRun_ = 0, skippedPlaying_ = 0, skippedMetered_ = 0, deferred_ = 0, newFound_ = 0;
};
