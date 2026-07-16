#include "GameMetaAggregator.h"
#include "AddonManager.h"

#include <QSet>
#include <QTimer>
#include <algorithm>

GameMetaAggregator::GameMetaAggregator(AddonManager* mgr, QObject* parent)
    : QObject(parent), mgr_(mgr)
{
    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    connect(timeout_, &QTimer::timeout, this, [this] {
        if (!outstanding_.isEmpty()) { outstanding_.clear(); finalize(); } // answer with whatever arrived
    });
    connect(mgr_, &AddonManager::metaReady, this, &GameMetaAggregator::onMetaReady);
}

bool GameMetaAggregator::hasProviders() const
{
    return !mgr_->metaProvidersFor(QStringLiteral("game")).isEmpty();
}

// Fixed merge priority: the provider best-in-class for the most roles goes first, so mergeLowerPriority
// keeps its artwork where it has it and the others backfill the rest. SteamGridDB (logos/heroes/box),
// then IGDB (summary/screenshots/video/rating), then ScreenScraper (console-accurate media), then
// TheGamesDB (broad fallback). Unknown providers sort after the known four but still contribute.
int GameMetaAggregator::priorityOf(const QString& id) const
{
    if (id == QStringLiteral("com.mymediavault.steamgriddb"))   return 0;
    if (id == QStringLiteral("com.mymediavault.igdb"))          return 1;
    if (id == QStringLiteral("com.mymediavault.screenscraper")) return 2;
    if (id == QStringLiteral("com.mymediavault.thegamesdb"))    return 3;
    return 100;
}

void GameMetaAggregator::fetch(const MediaItem& item, const QString& console, Done done)
{
    ++gen_;
    outstanding_.clear();
    results_.clear();
    done_ = std::move(done);

    const QVector<LoadedAddon*> providers = mgr_->metaProvidersFor(QStringLiteral("game"));
    if (providers.isEmpty()) { done_ = nullptr; return; }

    MediaItem q = item;
    q.type = QStringLiteral("game");
    if (!console.isEmpty()) q.systemHint = console;

    for (LoadedAddon* p : providers)
    {
        const int reqId = mgr_->requestMeta(p, q);
        if (reqId >= 0) outstanding_.insert(reqId, p->manifest.id);
    }
    if (outstanding_.isEmpty()) { done_ = nullptr; return; }
    timeout_->start(9000);
}

void GameMetaAggregator::onMetaReady(int requestId, const MediaDetail& detail)
{
    const auto it = outstanding_.constFind(requestId);
    if (it == outstanding_.constEnd()) return; // not one of ours (the normal single-provider path owns it)
    const QString providerId = it.value();
    outstanding_.erase(it);
    if (detail.valid || !detail.art.isEmpty())
        results_.push_back({ priorityOf(providerId), detail });
    if (outstanding_.isEmpty()) finalize();
}

void GameMetaAggregator::finalize()
{
    timeout_->stop();
    Done done = std::move(done_);
    done_ = nullptr;
    if (!done) return;

    // Merge high-priority first: scalar fields take the first non-empty, facts union (first label wins),
    // and MediaArt::mergeLowerPriority keeps each role/video/audio/meta from the earliest provider that has it.
    std::stable_sort(results_.begin(), results_.end(),
                     [](const QPair<int, MediaDetail>& a, const QPair<int, MediaDetail>& b) { return a.first < b.first; });

    MediaDetail merged;
    QSet<QString> factLabels;
    for (const auto& pr : results_)
    {
        const MediaDetail& d = pr.second;
        if (merged.title.isEmpty())    merged.title = d.title;
        if (merged.subtitle.isEmpty()) merged.subtitle = d.subtitle;
        if (merged.overview.isEmpty()) merged.overview = d.overview;
        if (merged.imageUrl.isEmpty()) merged.imageUrl = d.imageUrl;
        if (merged.imdbStreamId.isEmpty()) merged.imdbStreamId = d.imdbStreamId;
        for (const MediaFact& f : d.facts)
            if (!f.label.isEmpty() && !factLabels.contains(f.label)) { factLabels.insert(f.label); merged.facts.push_back(f); }
        merged.art.mergeLowerPriority(d.art);
    }
    merged.valid = !merged.title.isEmpty() || !merged.overview.isEmpty()
                   || !merged.facts.isEmpty() || !merged.art.isEmpty();
    results_.clear();
    done(merged);
}
