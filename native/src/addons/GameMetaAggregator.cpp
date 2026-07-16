#include "GameMetaAggregator.h"
#include "AddonManager.h"
#include "../core/GamelistStore.h"
#include "../core/MetaCache.h"
#include "../core/Settings.h"

#include <QTimer>
#include <algorithm>

GameMetaAggregator::GameMetaAggregator(AddonManager* mgr, QObject* parent)
    : QObject(parent), mgr_(mgr)
{
    connect(mgr_, &AddonManager::metaReady, this, &GameMetaAggregator::onMetaReady);
}

bool GameMetaAggregator::hasProviders() const
{
    return !mgr_->metaProvidersFor(QStringLiteral("game")).isEmpty();
}

// Fixed merge priority: the provider best-in-class for the most roles goes first, so mergeLowerPriority
// keeps its artwork where it has it and the others backfill the rest. SteamGridDB (logos/heroes/box), then
// IGDB (summary/screenshots/video/rating), then ScreenScraper (console-accurate media), then TheGamesDB
// (broad fallback). Unknown providers sort after the known four but still contribute.
int GameMetaAggregator::priorityOf(const QString& id) const
{
    if (id == QStringLiteral("com.mymediavault.steamgriddb"))   return 0;
    if (id == QStringLiteral("com.mymediavault.igdb"))          return 1;
    if (id == QStringLiteral("com.mymediavault.screenscraper")) return 2;
    if (id == QStringLiteral("com.mymediavault.thegamesdb"))    return 3;
    return 100;
}

void GameMetaAggregator::request(const MediaItem& item, const QString& console, Done done)
{
    enqueue(item, console, true, std::move(done));
}

void GameMetaAggregator::prefetch(const QVector<MediaItem>& items, const QString& console)
{
    if (!hasProviders()) return;
    for (const MediaItem& it : items)
        if (it.type == QStringLiteral("game"))
            enqueue(it, console, false, nullptr);
}

void GameMetaAggregator::enqueue(const MediaItem& item, const QString& console, bool priority, Done done)
{
    const QString key = MetaCache::keyFor(item);
    if (key.isEmpty() || !hasProviders()) { if (done) done(MediaDetail{}); return; }

    // Already scraped this game (a previous session, a download, or an earlier prefetch)? Don't re-hit the
    // providers — hand back the cached card.
    if (!MetaCache::loadArt(key).isEmpty()) { if (done) done(MetaCache::cachedDetail(key)); return; }

    // Already queued/running this session: a hover just bumps its priority + adopts the callback rather than
    // scraping the same game twice.
    if (seen_.contains(key))
    {
        if (priority)
        {
            for (int i = 0; i < pending_.size(); ++i)
                if (pending_[i]->key == key)
                { pending_[i]->priority = true; if (done) pending_[i]->done = std::move(done); pending_.move(i, 0); pump(); return; }
            for (const auto& j : jobs_)
                if (j->key == key) { if (done) j->done = std::move(done); return; } // already running -> adopt cb
        }
        if (done) done(MediaDetail{}); // queued but we're not the priority owner
        return;
    }

    seen_.insert(key);
    auto job = QSharedPointer<Job>::create();
    job->id = nextId_++;
    job->item = item;
    job->item.type = QStringLiteral("game");
    if (!console.isEmpty()) job->item.systemHint = console;
    job->key = key;
    job->priority = priority;
    job->done = std::move(done);
    if (priority) pending_.prepend(job); else pending_.append(job);
    pump();
}

void GameMetaAggregator::pump()
{
    // Priority (hover) jobs are at the front, so they start first when a slot frees.
    while (jobs_.size() < maxActive_ && !pending_.isEmpty())
        startJob(pending_.takeFirst());
}

void GameMetaAggregator::startJob(const QSharedPointer<Job>& job)
{
    jobs_.insert(job->id, job);
    const QVector<LoadedAddon*> providers = mgr_->metaProvidersFor(QStringLiteral("game"));
    for (LoadedAddon* p : providers)
    {
        const int reqId = mgr_->requestMeta(p, job->item);
        if (reqId >= 0) { job->outstanding.insert(reqId, p->manifest.id); reqToJob_.insert(reqId, job->id); }
    }
    if (job->outstanding.isEmpty()) // no provider took the request -> finish on the next tick (never re-enter pump)
    {
        const quint64 id = job->id;
        QTimer::singleShot(0, this, [this, id] { finishJob(id); });
        return;
    }
    job->timer = new QTimer(this);
    job->timer->setSingleShot(true);
    const quint64 id = job->id;
    connect(job->timer, &QTimer::timeout, this, [this, id] { finishJob(id); }); // answer with whatever arrived
    job->timer->start(12000);
}

void GameMetaAggregator::onMetaReady(int requestId, const MediaDetail& detail)
{
    const auto jt = reqToJob_.constFind(requestId);
    if (jt == reqToJob_.constEnd()) return; // not one of our jobs (the single-provider meta path owns it)
    const quint64 jobId = jt.value();
    reqToJob_.erase(jt);
    const auto j = jobs_.constFind(jobId);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    const QString providerId = job->outstanding.take(requestId);
    if (detail.valid || !detail.art.isEmpty()) job->results.push_back({ priorityOf(providerId), detail });
    if (job->outstanding.isEmpty()) finishJob(jobId);
}

void GameMetaAggregator::finishJob(quint64 id)
{
    const auto j = jobs_.constFind(id);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    jobs_.remove(id);
    if (job->timer) { job->timer->stop(); job->timer->deleteLater(); job->timer = nullptr; }
    for (auto it = job->outstanding.constBegin(); it != job->outstanding.constEnd(); ++it)
        reqToJob_.remove(it.key()); // drop lingering req mappings (timeout path)

    // Merge high-priority first: scalar fields take the first non-empty, facts union (first label wins), and
    // MediaArt::mergeLowerPriority keeps each role/video/audio/meta from the earliest provider that has it.
    std::stable_sort(job->results.begin(), job->results.end(),
                     [](const QPair<int, MediaDetail>& a, const QPair<int, MediaDetail>& b) { return a.first < b.first; });
    MediaDetail merged;
    QSet<QString> factLabels;
    for (const auto& pr : job->results)
    {
        const MediaDetail& d = pr.second;
        if (merged.title.isEmpty())        merged.title = d.title;
        if (merged.subtitle.isEmpty())     merged.subtitle = d.subtitle;
        if (merged.overview.isEmpty())     merged.overview = d.overview;
        if (merged.imageUrl.isEmpty())     merged.imageUrl = d.imageUrl;
        if (merged.imdbStreamId.isEmpty()) merged.imdbStreamId = d.imdbStreamId;
        for (const MediaFact& f : d.facts)
            if (!f.label.isEmpty() && !factLabels.contains(f.label)) { factLabels.insert(f.label); merged.facts.push_back(f); }
        merged.art.mergeLowerPriority(d.art);
    }
    merged.valid = !merged.title.isEmpty() || !merged.overview.isEmpty()
                   || !merged.facts.isEmpty() || !merged.art.isEmpty();

    // ALWAYS cache the result — even a superseded hover or a background prefetch — so it's instant + offline
    // next time and a scroll-past never throws the scrape away.
    if (merged.valid || !merged.art.isEmpty())
    {
        MetaCache::saveArt(job->key, merged.art);
        if (merged.valid) MetaCache::saveDetail(job->key, merged);
        // "Keep scraped data": persist a freshly-scraped game back into its ROM system's gamelist.xml +
        // media folders (ES/RetroBat layout), so it's reused from the folder next time. Only for local ROMs
        // (item.url is the ROM path); GamelistStore skips games already listed.
        if (Settings::keepScrapedData() && !job->item.url.isEmpty()
            && !job->item.url.startsWith(QStringLiteral("http")))
            GamelistStore::write(job->item.url, merged);
    }
    if (job->done) job->done(merged);
    pump(); // a slot freed -> start the next queued game
}
