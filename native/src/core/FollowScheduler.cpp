#include "FollowScheduler.h"
#include "FollowSnapshot.h"

#include <QCoreApplication>
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

bool FollowScheduler::userCheckNow()
{
    if (cycleActive_)
    {
        if (manualCycle_)
        {
            // A second press while a Check now runs. Ignored: no request, and NO pass queued behind this one
            // (checkNow's manualPending_ would ask every source again a minute later for nothing). The row
            // says so, once.
            if (!status_.repeatIgnored)
            {
                status_.repeatIgnored = true;
                emit statusChanged();
            }
            return false;
        }
        // A background pass is running. Its series are already queued; what the press changes is the pace
        // of the rest of them, since somebody is now watching the row.
        manualCycle_ = true;
        status_.manual = true;
        emit statusChanged();
        pump();
        return true;
    }
    checkNow();
    return true;
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
        status_ = follow::CheckStatus();
        status_.phase = follow::CheckPhase::Done;
        status_.manual = manual;
        emit statusChanged();
        return;
    }

    queue_.clear();
    lastSent_.clear();
    busy_.clear();
    failed_.clear();
    cycleChecked_ = 0;
    cycleNew_ = 0;
    cycleOk_ = 0;
    for (const FollowItem& it : items) queue_ << Job{ it, sourceOf(it) };
    cycleActive_ = true;
    manualCycle_ = manual;
    status_ = follow::CheckStatus();
    status_.phase = follow::CheckPhase::Checking;
    status_.manual = manual;
    status_.seriesTotal = int(items.size());
    emit statusChanged();
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
                // A Check now paces its own series (kManualGapSecs, woken by armWake); a background pass keeps
                // the background gap and the tick's pace, unchanged by #420.
                const follow::Admit a = follow::admit(now(), lastSent_.value(src, 0),
                                                      busy_.contains(src), failed_.contains(src),
                                                      manualCycle_ ? follow::kManualGapSecs
                                                                   : follow::kSourceGapSecs);
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
    else                                     armWake();
}

void FollowScheduler::armWake()
{
    // Only a Check now asks to be woken. What it waits for is the earliest moment a queued series' source is
    // out of its gap; a source with a request in flight needs no wake-up (its reply runs the pump), and a
    // failed source's series have already been dropped. A background pass leaves this at -1 and is paced by
    // the 60-second tick exactly as before.
    wakeAt_ = -1;
    if (cycleActive_ && manualCycle_)
    {
        for (const Job& j : queue_)
        {
            if (busy_.contains(j.sourceId) || failed_.contains(j.sourceId)) continue;
            const qint64 at = lastSent_.value(j.sourceId, 0) + follow::kManualGapSecs;
            if (wakeAt_ < 0 || at < wakeAt_) wakeAt_ = at;
        }
    }
    if (!periodic_) return;            // probe_follow reads wakeAt() and drives the clock itself
    if (wakeAt_ < 0)
    {
        if (wake_) wake_->stop();
        return;
    }
    if (!wake_)
    {
        wake_ = new QTimer(this);
        wake_->setSingleShot(true);
        connect(wake_, &QTimer::timeout, this, &FollowScheduler::tick);
    }
    // The clock is whole seconds, so wait the full remainder plus a margin: firing a hair early would only
    // find WaitGap and re-arm, but there is no reason to spend the extra pass.
    const qint64 ms = qMax<qint64>(0, (wakeAt_ - now()) * 1000) + 50;
    wake_->start(int(ms));
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
    ++cycleOk_;
    if (!found.isEmpty())
    {
        newFound_ += int(found.size());
        cycleNew_ += int(found.size());
        QStringList ids;
        for (const FollowSnapshot::Pending& p : found) ids << p.id;
        emit newItemsFound(job.item.itemId, int(found.size()), ids);
    }
    pump();
}

void FollowScheduler::endCycle()
{
    if (!cycleActive_) return;
    cycleActive_ = false;
    manualCycle_ = false;
    wakeAt_ = -1;
    if (wake_) wake_->stop();
    // A MANUAL pass stamps the cycle clock too. "Check now" is a check; making it not count would leave the
    // scheduled pass due immediately afterwards and ask every source twice.
    FollowSnapshot::setLastCycleAt(now());
    ++cyclesRun_;
    emit cycleFinished(cycleChecked_, cycleNew_);
    // #420: the pass is over, so the row says how it went. seriesTotal is the pass's list; anything not
    // answered (a failed source, a reaped fetch, a series dropped behind a failed source) counts as failed.
    status_.phase = (status_.seriesTotal > 0 && cycleOk_ == 0) ? follow::CheckPhase::Failed
                                                               : follow::CheckPhase::Done;
    status_.seriesFailed = qMax(0, status_.seriesTotal - cycleOk_);
    status_.newItems = cycleNew_;
    status_.repeatIgnored = false;
    emit statusChanged();
}

// ---- The Following row's sentences (issue #420) ----------------------------------------------------------
static QString trFollow(const char* s) { return QCoreApplication::translate("FollowStatus", s); }

QString follow::checkStatusText(const follow::CheckStatus& s)
{
    switch (s.phase)
    {
    case CheckPhase::Idle:
        return trFollow("Series you follow are checked in the background and anything new appears on the New "
                        "shelf. The check is skipped while something is playing, and one source is never asked "
                        "twice at once.");
    case CheckPhase::Checking:
        if (s.repeatIgnored)
            return trFollow("Already checking your followed series. The result will appear here when it finishes.");
        return trFollow("Checking your followed series…");
    case CheckPhase::Failed:
        return trFollow("Couldn't reach the source of any series you follow. They will be tried on the next check.");
    case CheckPhase::Done:
        break;
    }
    if (s.seriesTotal <= 0) return trFollow("You aren't following any series yet.");
    const int checked = qMax(0, s.seriesTotal - s.seriesFailed);
    QString line;
    if (s.newItems <= 0)
        line = trFollow("Checked %1 series: nothing new.").arg(checked);
    else if (s.newItems == 1)
        line = trFollow("Checked %1 series: 1 new item, on the New shelf.").arg(checked);
    else
        line = trFollow("Checked %1 series: %2 new items, on the New shelf.").arg(checked).arg(s.newItems);
    if (s.seriesFailed > 0)
        line += QLatin1Char(' ')
              + trFollow("%1 more couldn't be reached and will be tried on the next check.").arg(s.seriesFailed);
    return line;
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
