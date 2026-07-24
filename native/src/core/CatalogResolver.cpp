#include "CatalogResolver.h"
#include "CatalogMatch.h"
#include "LocalResolveCache.h"
#include "Settings.h"
#include "AddonManager.h"
#include "AddonModels.h"

#include <QTimer>
#include <QFileInfo>
#include <QDateTime>

CatalogResolver::CatalogResolver(AddonManager* addons, LocalResolveCache* cache, QObject* parent)
    : QObject(parent), addons_(addons), cache_(cache)
{
    connect(addons_, &AddonManager::catalogReady, this, &CatalogResolver::onCatalogReady);
    resolvedDebounce_ = new QTimer(this);
    resolvedDebounce_->setSingleShot(true);
    connect(resolvedDebounce_, &QTimer::timeout, this, [this] {
        if (cacheDirty_) { cache_->save(); cacheDirty_ = false; }
        emit resolved();
    });
}

static bool isMovieCatalogSource(AddonManager* m, LoadedAddon* s)
{
    if (!s || !s->isMediaSource()) return false;
    for (const AddonCatalog& c : m->catalogs(s))
        if (c.type == QStringLiteral("movie") || c.type == QStringLiteral("mixed")) return true;
    return false;
}

void CatalogResolver::enqueue(const QVector<LocalLibrary::VideoEntry>& entries)
{
    if (!Settings::resolveOnline()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const LocalLibrary::VideoEntry& e : entries)
    {
        if (e.kind != LocalLibrary::Kind::Movie) continue;         // movies only this track
        if (seen_.contains(e.path)) continue;
        const QFileInfo fi(e.path);
        const qint64 size = fi.size();
        const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
        if (cache_->isFresh(e.path, size, mtime, now)) continue;   // already resolved / nomatch-in-window
        seen_.insert(e.path);
        auto job = QSharedPointer<Job>::create();
        job->id = nextId_++; job->movie = e; job->size = size; job->mtime = mtime;
        pending_.append(job);
    }
    pump();
}

void CatalogResolver::clearCacheAndRequeue(const QVector<LocalLibrary::VideoEntry>& entries)
{
    cache_->clear();     // empties the on-disk cache so every movie is re-resolved online
    seen_.clear();
    enqueue(entries);
}

void CatalogResolver::pump()
{
    while (jobs_.size() < maxActive_ && !pending_.isEmpty())
        startJob(pending_.takeFirst());
}

void CatalogResolver::startJob(const QSharedPointer<Job>& job)
{
    jobs_.insert(job->id, job);
    for (LoadedAddon* s : addons_->sources())
    {
        if (!isMovieCatalogSource(addons_, s)) continue;
        const int reqId = addons_->requestSearch(s, job->movie.title);
        if (reqId >= 0) { job->outstanding.insert(reqId); reqToJob_.insert(reqId, job->id); }
    }
    if (job->outstanding.isEmpty()) { const quint64 id = job->id; QTimer::singleShot(0, this, [this, id]{ finishJob(id); }); return; }
    job->timer = new QTimer(this); job->timer->setSingleShot(true);
    const quint64 id = job->id;
    connect(job->timer, &QTimer::timeout, this, [this, id]{ finishJob(id); });
    job->timer->start(12000);
}

void CatalogResolver::onCatalogReady(int reqId, const MediaCatalog& catalog)
{
    const auto jt = reqToJob_.constFind(reqId);
    if (jt == reqToJob_.constEnd()) return;   // not one of ours (normal browse search)
    const quint64 jobId = jt.value(); reqToJob_.erase(jt);
    const auto j = jobs_.constFind(jobId);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    job->outstanding.remove(reqId);
    const int idx = CatalogMatch::bestMatch(job->movie, catalog.items);
    if (idx >= 0) { const QString id = catalog.items[idx].id; if (!id.isEmpty() && !job->matchedIds.contains(id)) job->matchedIds << id; }
    if (job->outstanding.isEmpty()) finishJob(jobId);
}

void CatalogResolver::finishJob(quint64 id)
{
    const auto j = jobs_.constFind(id);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    jobs_.remove(id);
    if (job->timer) { job->timer->stop(); job->timer->deleteLater(); job->timer = nullptr; }
    for (int r : job->outstanding) reqToJob_.remove(r);   // drop lingering (timeout path)
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!job->matchedIds.isEmpty()) cache_->putMatched(job->movie.path, job->size, job->mtime, job->matchedIds, now);
    else                            cache_->putNoMatch(job->movie.path, job->size, job->mtime, now);
    cacheDirty_ = true;
    scheduleResolvedSignal();
    pump();
}

void CatalogResolver::scheduleResolvedSignal()
{ resolvedDebounce_->start(1500); }   // coalesce a batch of finishes into one index rebuild
