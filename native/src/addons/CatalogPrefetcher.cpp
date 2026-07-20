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
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  prefetch: ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

CatalogPrefetcher::CatalogPrefetcher(AddonManager* mgr, QObject* parent)
    : QObject(parent), mgr_(mgr)
{
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &CatalogPrefetcher::resweep);

    // Only OUR reqIds are acted on (see onCatalogReady) — user browsing shares the same signal but is ignored.
    connect(mgr_, &AddonManager::catalogReady, this, [this](int reqId, const MediaCatalog&) { onCatalogReady(reqId); });
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
        if (reqId < 0) { active_.remove(job.key); continue; } // couldn't dispatch (null/invalid) - drop it
        ++issued_;
        inFlight_.insert(reqId, job.key);
    }
}

void CatalogPrefetcher::onCatalogReady(int reqId)
{
    const auto it = inFlight_.constFind(reqId);
    if (it == inFlight_.constEnd()) return; // not one of ours (a user request, or already handled)
    const QString key = it.value();
    inFlight_.erase(it);
    active_.remove(key);
    // Whether it filled the cache or came back empty (a failed fetch isn't cached), the slot is now free.
    // We don't inspect success here: failures are best-effort and simply get retried on the next resweep.
    pump();
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
