#include "CatalogPrefetcher.h"
#include "AddonManager.h"
#include "AddonModels.h"
#include "../core/AppPaths.h"

#include <QTimer>
#include <QFile>
#include <QDateTime>
#include <QRandomGenerator>

// One-line append to <app>/stream_debug.log, the shared addon trace log (mwLog idiom). Prefetch is best-effort
// background work, so a failure is only ever noted here — never surfaced to the UI, never retried in a sweep.
static void pfLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + QStringLiteral("  prefetch: ") + msg
                 + QStringLiteral("\n")).toUtf8());  // ms precision so "sweep start" is provably after firstpaint
}

CatalogPrefetcher::CatalogPrefetcher(AddonManager* mgr, QObject* parent)
    : QObject(parent), mgr_(mgr)
{
    // MMV_PREFETCH_WATCHDOG_S (seconds, >0) shortens the in-flight watchdog for tests; default 60s.
    const int wdOverrideS = qEnvironmentVariableIntValue("MMV_PREFETCH_WATCHDOG_S");
    if (wdOverrideS > 0) watchdogMs_ = qint64(wdOverrideS) * 1000;

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &CatalogPrefetcher::resweep);

    // Liveness watchdog: if a dispatched request's catalogReady never fires (a lost reply), the slot and the
    // dedupe key would otherwise leak forever — three such and the queue is wedged at the cap until restart.
    // A periodic reap expires jobs older than watchdogMs_ and pumps the queue onward; it only runs while
    // something is in flight (started in pump, stopped when the last slot frees).
    watchdog_ = new QTimer(this);
    watchdog_->setInterval(int(qBound<qint64>(qint64(250), watchdogMs_ / 4, qint64(15000))));
    connect(watchdog_, &QTimer::timeout, this, &CatalogPrefetcher::reapStalled);

    // Only OUR reqIds are acted on (see onCatalogReady) — user browsing shares the same signal but is ignored.
    // A zero-item result is a failed/empty fetch (the manager's cache lambda skips those too) -> ok=false.
    connect(mgr_, &AddonManager::catalogReady, this,
            [this](int reqId, const MediaCatalog& c) { onCatalogReady(reqId, !c.items.isEmpty()); });
    // The source list or a source's enabled state changed -> re-warm (a newly enabled source, a new catalog).
    connect(mgr_, &AddonManager::sourcesChanged, this, &CatalogPrefetcher::resweep);
    connect(mgr_, &AddonManager::sourceEnabledChanged, this, [this](const QString&, bool) { resweep(); });
}

QString CatalogPrefetcher::jobKey(LoadedAddon* src, const QString& catalogId) const
{
    return src->manifest.id + QLatin1Char('|') + catalogId;
}

void CatalogPrefetcher::start()
{
    // Distinct sweep-start marker so the trace can prove the warm-up begins AFTER the first paint: app-startup
    // catalog loads and prefetch loads both go through requestCatalog (both log "catalog fetch"), so only this
    // line unambiguously timestamps "prefetch activity started". Kicked post-first-paint from MainWindow.
    pfLog(QStringLiteral("sweep start"));
    enqueueSweep();
    pump();
    armTimer();
}

void CatalogPrefetcher::resweep()
{
    enqueueSweep();
    pump();
    armTimer();
}

void CatalogPrefetcher::enqueueSweep()
{
    if (!mgr_) return;
    for (LoadedAddon* src : mgr_->sources())
    {
        if (!src || !mgr_->isEnabled(src->manifest.id)) continue; // disabled sources are never warmed
        for (const AddonCatalog& cat : mgr_->catalogs(src))
        {
            const QString key = jobKey(src, cat.id);
            if (active_.contains(key)) continue; // already queued or in flight
            // Skip anything still comfortably within the cache TTL — a warm cache costs no requests. (The
            // resweep cadence is < TTL, so last sweep's entries are still valid here and get skipped.)
            if (mgr_->cachedCatalog(src, cat.id, QString(), 1, {}).has_value()) continue;
            active_.insert(key);
            queue_.enqueue({ src, cat.id, key });
        }
    }
}

void CatalogPrefetcher::pump()
{
    while (inFlight_.size() < kMaxInFlight && !queue_.isEmpty())
    {
        const Job job = queue_.dequeue();
        const int reqId = mgr_->requestCatalog(job.src, job.catalogId, QString(), 1, {});
        if (reqId < 0)
        {
            // Couldn't dispatch (source went away or was disabled between sweep and pump) — drop, log, move on.
            pfLog(QStringLiteral("dropped %1 (request refused)").arg(job.key));
            active_.remove(job.key);
            continue;
        }
        ++issued_;
        inFlight_.insert(reqId, { job.key, QDateTime::currentMSecsSinceEpoch() });
    }
    if (!inFlight_.isEmpty() && !watchdog_->isActive()) watchdog_->start();
    if (inFlight_.isEmpty() && watchdog_->isActive()) watchdog_->stop();
}

void CatalogPrefetcher::onCatalogReady(int reqId, bool ok)
{
    const auto it = inFlight_.constFind(reqId);
    if (it == inFlight_.constEnd()) return; // not one of ours (a user request, or a job the watchdog reclaimed)
    const QString key = it->key;
    inFlight_.erase(it);
    active_.remove(key);
    // A failed/empty fetch isn't cached and isn't retried within this sweep — note it once per job so a source
    // that's down is diagnosable from the log; the next resweep gives it another chance.
    if (!ok) pfLog(QStringLiteral("failed %1 (empty/failed result; will retry next sweep)").arg(key));
    pump();
}

void CatalogPrefetcher::reapStalled()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool reclaimed = false;
    for (auto it = inFlight_.begin(); it != inFlight_.end(); )
    {
        if (now - it->startedMs >= watchdogMs_)
        {
            pfLog(QStringLiteral("watchdog expired %1 after %2ms (no reply; slot reclaimed)")
                      .arg(it->key).arg(now - it->startedMs));
            active_.remove(it->key);
            it = inFlight_.erase(it);
            ++expired_;
            reclaimed = true;
        }
        else
            ++it;
    }
    if (reclaimed) pump(); // freed slots — keep the queue moving (pump also stops the watchdog when idle)
}

void CatalogPrefetcher::armTimer()
{
    if (!periodic_) return;
    // Cadence tracks the cache TTL: ~25 min at the 30-min default (5/6 of TTL), + 0..(TTL/6) jitter so many
    // installs don't resweep in lockstep. MMV_PREFETCH_TTL_S scales the manager's TTL, so this scales with it.
    const qint64 ttl = mgr_ ? mgr_->catalogCacheTtlMs() : 30 * 60 * 1000;
    const qint64 base = ttl * 5 / 6;
    const qint64 jitter = ttl > 0 ? qint64(QRandomGenerator::global()->bounded(qint64(ttl / 6 + 1))) : 0;
    timer_->start(int(qMax<qint64>(1, base + jitter)));
}
