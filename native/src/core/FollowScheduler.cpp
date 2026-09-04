#include "FollowScheduler.h"
#include "FollowSnapshot.h"

#include <QDateTime>
#include <QTimer>

namespace
{
    // How long an unanswered fetch holds its source's one slot before the source is written off for this
    // cycle. Without it a fetcher that never calls back (a dropped reply, a source removed mid-pass) wedges
    // the cycle for ever and every later cycle is refused as "already active" — the CatalogPrefetcher's
    // watchdog, for the same reason and at the same 60 seconds.
    constexpr qint64 kFetchTimeoutSecs = 60;
    // How often the pump runs while the app is up. One minute is far finer than any offered interval (six
    // hours is the shortest) and is what paces the per-source gap inside an active cycle.
    constexpr int kTickMs = 60 * 1000;
}

FollowScheduler::FollowScheduler(QObject* parent) : QObject(parent)
{
    nowFn_ = [] { return QDateTime::currentSecsSinceEpoch(); };
    list_  = [] { return FollowStore::list(); };
}

void FollowScheduler::setClock(std::function<qint64()> nowFn)
{
    if (nowFn) nowFn_ = std::move(nowFn);
}

void FollowScheduler::setIntervalHours(int hours)
{
    intervalHours_ = int(follow::clampIntervalHours(hours));
}

qint64 FollowScheduler::now() const { return nowFn_ ? nowFn_() : 0; }

qint64 FollowScheduler::jitter() const
{
    return follow::jitterSecs(qint64(intervalHours_) * follow::kHourSecs, jitterSeed_);
}

qint64 FollowScheduler::nextDueAt() const
{
    return follow::nextDueAt(FollowSnapshot::lastCycleAt(),
                             qint64(intervalHours_) * follow::kHourSecs, jitter());
}

void FollowScheduler::start()
{
    if (!periodic_) return;
    if (!timer_)
    {
        timer_ = new QTimer(this);
        timer_->setInterval(kTickMs);
        connect(timer_, &QTimer::timeout, this, &FollowScheduler::tick);
    }
    timer_->start();
}

void FollowScheduler::checkNow()
{
    manualPending_ = true;
    tick();
}

void FollowScheduler::tick()
{
    reapStalled();
    if (!cycleActive_) beginCycleIfDue();
    if (cycleActive_)  pump();
}

void FollowScheduler::beginCycleIfDue()
{
    const bool manual = manualPending_;
    if (!manual)
    {
        // A scheduled pass, and all three gates apply. Each one RETURNS WITHOUT STAMPING the cycle time, so
        // the pass is deferred to the next tick rather than consumed — a box that is playing something all
        // evening runs its pass when the film ends, it does not lose the day's check.
        if (!follow::dueNow(now(), FollowSnapshot::lastCycleAt(),
                            qint64(intervalHours_) * follow::kHourSecs, jitter()))
            return;
        if (playing_ && playing_())  { ++skippedPlaying_; return; }
        if (!allowMetered_ && metered_ && metered_()) { ++skippedMetered_; return; }
    }

    const QVector<FollowItem> items = list_ ? list_() : QVector<FollowItem>();
    manualPending_ = false;
    if (items.isEmpty())
    {
        // Nothing followed: the pass still COUNTS as run, so an install with no follows does not re-evaluate
        // the schedule on every tick for ever.
        FollowSnapshot::setLastCycleAt(now());
        ++cyclesRun_;
        emit cycleFinished(0, 0);
        return;
    }

    queue_.clear();
    lastSent_.clear();
    busy_.clear();
    failed_.clear();
    cycleChecked_ = 0;
    cycleNew_ = 0;
    for (const FollowItem& it : items) queue_ << Job{ it, sourceOf(it) };
    cycleActive_ = true;
}

void FollowScheduler::pump()
{
    if (inPump_) { pumpAgain_ = true; return; }
    inPump_ = true;
    do
    {
        pumpAgain_ = false;
        bool progressed = true;
        while (progressed)
        {
            progressed = false;
            for (int i = 0; i < queue_.size(); ++i)
            {
                const QString src = queue_[i].sourceId;
                const follow::Admit a = follow::admit(now(), lastSent_.value(src, 0),
                                                      busy_.contains(src), failed_.contains(src));
                if (a == follow::Admit::SourceFailed)
                {
                    // This source already failed in this cycle: drop its remaining series without asking.
                    queue_.remove(i);
                    ++deferred_;
                    progressed = true;
                    break;
                }
                if (a != follow::Admit::Send) continue;   // WaitGap / WaitInFlight: try another source
                const Job job = queue_.takeAt(i);
                dispatch(job);                            // may complete synchronously -> sets pumpAgain_
                progressed = true;
                break;
            }
        }
    } while (pumpAgain_);
    inPump_ = false;
    if (queue_.isEmpty() && busy_.isEmpty()) endCycle();
}

void FollowScheduler::dispatch(const Job& job)
{
    const qint64 t = now();
    lastSent_.insert(job.sourceId, t);
    busy_.insert(job.sourceId, t);
    ++issued_;
    ++cycleChecked_;
    if (!fetch_)
    {
        // No fetcher wired (a host that has not finished starting up). Treat it as a failed source rather
        // than as an empty child list, which would otherwise LEARN "this series has no children" and later
        // announce the whole catalogue as new.
        onFetched(job, false, {});
        return;
    }
    // The reply may arrive now or in a minute; either is safe. Bound by value so a late reply cannot read a
    // job that has been popped.
    fetch_(job.item, [this, job](bool ok, const QVector<follow::Child>& children) {
        onFetched(job, ok, children);
    });
}

void FollowScheduler::onFetched(const Job& job, bool ok, const QVector<follow::Child>& children)
{
    busy_.remove(job.sourceId);
    if (!ok)
    {
        failed_.insert(job.sourceId);
        pump();
        return;
    }

    const qint64 t = now();
    const FollowSnapshot::Snapshot prev = FollowSnapshot::get(job.item.itemId);
    const follow::Diff d = follow::diffChildren(prev.seen, prev.fingerprint, children, prev.neverChecked());

    QVector<FollowSnapshot::Pending> found;
    for (const follow::Child& c : d.newChildren) found << FollowSnapshot::fromChild(c, t);
    if (d.coarseChanged)
    {
        // THE DEGRADE (scope rule 5). A source that does not key its children cannot say WHICH one is new,
        // so it says the series changed — one row, filed under the series' own id so a second coarse change
        // before the user looks does not stack up a second identical row.
        FollowSnapshot::Pending p;
        p.id       = job.item.itemId;
        p.title    = job.item.title;
        p.subtitle = QStringLiteral("changed");
        p.thumbnailUrl = job.item.thumbnailUrl;
        p.type     = job.item.type;
        p.foundAt  = t;
        found << p;
    }

    FollowSnapshot::record(job.item.itemId, d.seenAfter, d.fingerprintAfter, found, t);
    if (!found.isEmpty())
    {
        newFound_ += int(found.size());
        cycleNew_ += int(found.size());
        emit newItemsFound(job.item.itemId, int(found.size()));
    }
    pump();
}

void FollowScheduler::endCycle()
{
    if (!cycleActive_) return;
    cycleActive_ = false;
    // A MANUAL pass stamps the cycle clock too. "Check now" is a check; making it not count would leave the
    // scheduled pass due immediately afterwards and ask every source twice.
    FollowSnapshot::setLastCycleAt(now());
    ++cyclesRun_;
    emit cycleFinished(cycleChecked_, cycleNew_);
}

void FollowScheduler::reapStalled()
{
    if (busy_.isEmpty()) return;
    const qint64 t = now();
    const QList<QString> srcs = busy_.keys();
    for (const QString& s : srcs)
        if (t - busy_.value(s) >= kFetchTimeoutSecs)
        {
            busy_.remove(s);
            failed_.insert(s);   // a source that never answers is a failed source, retried next cycle
        }
}
