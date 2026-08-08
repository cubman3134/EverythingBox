#include "CatalogResolver.h"
#include "CatalogMatch.h"
#include "LocalResolveCache.h"
#include "LocalMetaMerge.h"
#include "MetaCache.h"
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
    connect(addons_, &AddonManager::metaReady, this, &CatalogResolver::onMetaReady);   // issue #73
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

static bool isSeriesCatalogSource(AddonManager* m, LoadedAddon* s)
{
    if (!s || !s->isMediaSource()) return false;
    for (const AddonCatalog& c : m->catalogs(s))
        if (c.type == QStringLiteral("series") || c.type == QStringLiteral("tv") || c.type == QStringLiteral("mixed"))
            return true;
    return false;
}

void CatalogResolver::enqueue(const QVector<LocalLibrary::VideoEntry>& entries, bool forceMeta)
{
    if (!Settings::resolveOnline()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSet<QString> showsThisCall;   // dedup distinct shows within this enqueue
    for (const LocalLibrary::VideoEntry& e : entries)
    {
        if (e.kind == LocalLibrary::Kind::Movie)
        {
            if (seen_.contains(e.path)) continue;
            const QFileInfo fi(e.path);
            const qint64 size = fi.size();
            const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
            if (cache_->isFresh(e.path, size, mtime, now)) continue;   // already resolved / nomatch-in-window
            seen_.insert(e.path);
            auto job = QSharedPointer<Job>::create();
            job->id = nextId_++; job->movie = e; job->size = size; job->mtime = mtime;
            job->forceMeta = forceMeta;
            pending_.append(job);
        }
        else if (e.kind == LocalLibrary::Kind::Episode)
        {
            if (e.show.trimmed().isEmpty()) continue;   // M1: no cleaned show title → an empty search returns junk
            const QString sk = LocalLibrary::showKeyFor(e);
            const QString seenKey = QStringLiteral("show:") + sk;     // disjoint from movie paths
            if (showsThisCall.contains(sk) || seen_.contains(seenKey)) continue;
            if (cache_->isShowFresh(sk, now)) continue;               // resolved / nomatch-in-window
            showsThisCall.insert(sk); seen_.insert(seenKey);
            auto job = QSharedPointer<Job>::create();
            job->id = nextId_++; job->isShow = true; job->showKey = sk;
            job->showTitle = e.show; job->seriesImdbId = e.seriesImdbId;
            pending_.append(job);
        }
    }
    pump();
}

void CatalogResolver::clearCacheAndRequeue(const QVector<LocalLibrary::VideoEntry>& entries)
{
    cache_->clear();     // empties the on-disk cache so every movie is re-resolved online
    seen_.clear();
    enqueue(entries, /*forceMeta=*/true);   // Re-match also REFETCHES metadata, not just the id (issue #73)
}

void CatalogResolver::pump()
{
    // "Off ⇒ zero network" literally: toggling resolveOnline off drains queued work and issues
    // no further searches (enqueue already gates NEW work; this covers already-queued jobs).
    if (!Settings::resolveOnline()) { pending_.clear(); return; }
    while (jobs_.size() < maxActive_ && !pending_.isEmpty())
        startJob(pending_.takeFirst());
}

void CatalogResolver::startJob(const QSharedPointer<Job>& job)
{
    jobs_.insert(job->id, job);
    const QString query = job->isShow ? job->showTitle : job->movie.title;
    for (LoadedAddon* s : addons_->sources())
    {
        const bool ok = job->isShow ? isSeriesCatalogSource(addons_, s) : isMovieCatalogSource(addons_, s);
        if (!ok) continue;
        if (job->isShow)
        {
            // A show TITLE must be searched against a SERIES/TV catalog. requestSearch sends an UNTYPED
            // query, which aiocatalog's getCatalog defaults to its MOVIES catalog (`cat = a.catalog || "movies"`)
            // — so a show search returns movie-typed rows that bestSeriesMatch rejects (C1). Instead, query
            // each of the source's series/tv/mixed catalogs by id: aiocatalog's "tv" catalog runs TMDB
            // /search/tv (series-typed, id tmdb:tv:{N}); a Stremio source builds /catalog/series/<id>/search=<q>.json.
            for (const AddonCatalog& c : addons_->catalogs(s))
            {
                if (c.type != QStringLiteral("series") && c.type != QStringLiteral("tv")
                    && c.type != QStringLiteral("mixed")) continue;
                const int reqId = addons_->requestCatalog(s, c.id, query, 1);
                if (reqId >= 0) { job->issued = true; job->outstanding.insert(reqId); reqToJob_.insert(reqId, job->id); reqSource_.insert(reqId, s); }
            }
        }
        else
        {
            // Same rule as the show branch, and for a sharper reason: requestSearch on a RemoteHttp source is
            // dispatchRemoteCatalog with an EMPTY catalog id, which cannot name a Stremio route — the URL
            // builder falls through to its bare path and emits "<base>/catalog///search=<q>.json", a 404 fired
            // once per movie per addon while the real catalog is never asked. Since search-only catalogs are
            // now carried (they are the whole point of the Stremio track), that hit every such addon. Query
            // each movie/mixed catalog by its own id instead.
            for (const AddonCatalog& c : addons_->catalogs(s))
            {
                if (c.type != QStringLiteral("movie") && c.type != QStringLiteral("mixed")) continue;
                const int reqId = addons_->requestCatalog(s, c.id, query, 1);
                if (reqId >= 0) { job->issued = true; job->outstanding.insert(reqId); reqToJob_.insert(reqId, job->id); reqSource_.insert(reqId, s); }
            }
        }
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
    LoadedAddon* const source = reqSource_.take(reqId);   // the source that issued this search (for its getMeta)
    const auto j = jobs_.constFind(jobId);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    job->outstanding.remove(reqId);
    const int idx = job->isShow
        ? CatalogMatch::bestSeriesMatch(job->showTitle, job->seriesImdbId, catalog.items)
        : CatalogMatch::bestMatch(job->movie, catalog.items);
    if (idx >= 0)
    {
        const QString id = catalog.items[idx].id;
        if (!id.isEmpty() && !job->matchedIds.contains(id)) job->matchedIds << id;
        // Capture the FIRST matched movie row + its addon so finishJob can fetch its getMeta from the same
        // source (issue #73). Shows are out of scope here — episode-level artwork needs the tt:S:E route.
        if (!job->isShow && job->metaItem.id.isEmpty() && !id.isEmpty())
        { job->metaItem = catalog.items[idx]; job->metaSource = source; }
    }
    if (job->outstanding.isEmpty()) finishJob(jobId);
}

void CatalogResolver::finishJob(quint64 id)
{
    const auto j = jobs_.constFind(id);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    if (job->timer) { job->timer->stop(); job->timer->deleteLater(); job->timer = nullptr; }
    for (int r : job->outstanding) { reqToJob_.remove(r); reqSource_.remove(r); }   // drop lingering (timeout path)
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!job->matchedIds.isEmpty()) {
        if (job->isShow) cache_->putShowMatched(job->showKey, job->matchedIds, now);
        else             cache_->putMatched(job->movie.path, job->size, job->mtime, job->matchedIds, now);
        cacheDirty_ = true;
    }
    // Only stamp a 14-day nomatch when we genuinely "searched everywhere and found nothing":
    // at least one search was issued AND all issued searches replied (outstanding drained).
    // A timeout (outstanding still non-empty) or a no-source/offline run (nothing issued) must
    // leave the entry UNCACHED so the next scan/launch retries — seen_ already blocks a same-session
    // requeue, so we won't spin this session. This keeps offline/no-addon from poisoning the cache.
    else if (job->issued && job->outstanding.isEmpty()) {
        if (job->isShow) cache_->putShowNoMatch(job->showKey, now);
        else             cache_->putNoMatch(job->movie.path, job->size, job->mtime, now);
        cacheDirty_ = true;
    }
    // Movie meta-fetch phase (issue #73): a resolved movie now fetches its owning addon's getMeta and persists
    // it to MetaCache under the local tile key, so the tile renders poster/plot/year like any addon tile. This
    // reuses the SAME job slot — the fetch stays bounded by maxActive_, no second parallel request path — and
    // completeJob() runs when the meta arrives or times out. Shows are out of scope (episode-level artwork).
    if (!job->isShow && !job->matchedIds.isEmpty() && job->metaSource && !job->metaItem.id.isEmpty()
        && Settings::resolveOnline())
    {
        startMetaFetch(job);
        return;
    }
    completeJob(id);
}

void CatalogResolver::startMetaFetch(const QSharedPointer<Job>& job)
{
    const QString key = LocalLibrary::tileId(job->movie);
    // Idempotent: don't refetch metadata we already hold — UNLESS this is a Re-match, which MUST refetch so a
    // same-title-different-year mis-match is corrected end to end. On a forced refetch, drop the stale bundle
    // first so old art/facts can't outlive the new scrape (the user's #24 override lives in the ini, not here,
    // so it survives). The presence test is "detail" — the key MetaCache::saveDetail writes below.
    if (!job->forceMeta && MetaCache::load(key).contains(QStringLiteral("detail")))
    { completeJob(job->id); return; }
    if (job->forceMeta) MetaCache::remove(key);

    const int req = addons_->requestMeta(job->metaSource, job->metaItem);
    if (req < 0) { completeJob(job->id); return; }   // source can't answer getMeta → the id/badge still stand
    job->metaReq = req; reqToJob_.insert(req, job->id);
    job->timer = new QTimer(this); job->timer->setSingleShot(true);
    const quint64 id = job->id;
    connect(job->timer, &QTimer::timeout, this, [this, id] {
        const auto j = jobs_.constFind(id);                    // meta timed out → give up on it, release the slot
        if (j == jobs_.constEnd()) return;
        const QSharedPointer<Job> job = j.value();
        if (job->metaReq >= 0) { reqToJob_.remove(job->metaReq); job->metaReq = -1; }
        completeJob(id);
    });
    job->timer->start(12000);
}

void CatalogResolver::onMetaReady(int reqId, const MediaDetail& detail)
{
    const auto jt = reqToJob_.constFind(reqId);
    if (jt == reqToJob_.constEnd()) return;   // not ours (HomeView / the game aggregator own their own reqs)
    const quint64 jobId = jt.value();
    const auto j = jobs_.constFind(jobId);
    if (j == jobs_.constEnd()) { reqToJob_.erase(jt); return; }
    const QSharedPointer<Job> job = j.value();
    if (job->metaReq != reqId) return;        // defensive: this job is not fetching this reqId
    reqToJob_.erase(jt); job->metaReq = -1;
    // Compose the scrape with the file's .nfo (authoritative), persist under the LOCAL tile key so every
    // delegate renders it. The user's manual override composites LAST at MetaCache read time (issue #24), so
    // it is deliberately NOT baked in here — that keeps "reset to scraped" honest.
    if (detail.valid || !detail.art.isEmpty())
        MetaCache::saveDetail(LocalLibrary::tileId(job->movie), LocalMeta::baseDetail(job->movie, detail));
    completeJob(jobId);
}

void CatalogResolver::completeJob(quint64 id)
{
    const auto j = jobs_.constFind(id);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    jobs_.remove(id);
    if (job->timer) { job->timer->stop(); job->timer->deleteLater(); job->timer = nullptr; }
    if (job->metaReq >= 0) { reqToJob_.remove(job->metaReq); job->metaReq = -1; }
    scheduleResolvedSignal();
    pump();
}

void CatalogResolver::scheduleResolvedSignal()
{ resolvedDebounce_->start(1500); }   // coalesce a batch of finishes into one index rebuild
