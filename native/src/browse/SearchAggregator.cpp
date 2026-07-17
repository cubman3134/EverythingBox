#include "SearchAggregator.h"
#include "../addons/AddonManager.h"

SearchAggregator::SearchAggregator(AddonManager* mgr, QObject* parent)
    : QObject(parent), mgr_(mgr)
{
    // A second connection to catalogReady alongside HomeView's own handler: each filters by its own reqId set,
    // and AddonManager reqIds are globally unique, so the two never both claim a response.
    connect(mgr_, &AddonManager::catalogReady, this, &SearchAggregator::onCatalogReady);
}

// Verbatim port of the former HomeView::startSearchEverything fan-out loop: clear prior state, then fire one
// page-1 catalog request against every enabled media source's catalogs. (The UI-side resets — generation_,
// pendingReqId_, the initial populate/toast — stay in HomeView, which drives start().)
void SearchAggregator::start(const QString& query)
{
    reqs_.clear();
    reqSrc_.clear();
    seen_.clear();
    cat_ = MediaCatalog();
    cat_.title = tr("Search: %1").arg(query);
    query_ = query;
    for (LoadedAddon* s : mgr_->sources())
    {
        if (!s->isMediaSource() || !mgr_->isEnabled(s->manifest.id)) continue;
        for (const AddonCatalog& c : mgr_->catalogs(s))
        {
            const int req = mgr_->requestCatalog(s, c.id, query, 1, {});
            reqs_.insert(req);
            reqSrc_.insert(req, s->manifest.id);
        }
    }
}

void SearchAggregator::cancel()
{
    reqs_.clear();
    reqSrc_.clear();
}

// One source's response to a cross-addon search. Dedup + tag its rows and stream them out; when the last
// in-flight request drains, report the final count. (Filtered by reqs_ — everything else is another
// consumer's request and is ignored, exactly as the old in-handler search branch did.)
void SearchAggregator::onCatalogReady(int requestId, const MediaCatalog& cat)
{
    if (!reqs_.contains(requestId)) return;
    reqs_.remove(requestId);
    const QString srcId = reqSrc_.take(requestId);
    const bool firstBatch = cat_.items.isEmpty();
    MediaCatalog add; // just this source's fresh, de-duped items, appended so the grid doesn't reset
    for (MediaItem it : cat.items)
    {
        if (!acceptResult(it, seen_)) continue;
        it.sourceAddonId = srcId;              // remember which addon to re-open it through
        add.items.push_back(it);
        cat_.items.push_back(it);
    }
    emit resultsAppended(add, firstBatch);
    if (reqs_.isEmpty()) emit finished(cat_.items.size());
}
