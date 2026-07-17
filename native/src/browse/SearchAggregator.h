// Cross-addon "search everything" fan-out, extracted out of HomeView. Given a query, it fires one page-1
// requestCatalog against every enabled media source's catalogs and merges the streamed responses into one
// deduped, source-tagged catalog. It owns its own reqId set and connects itself to AddonManager::catalogReady,
// filtering by that set — AddonManager reqIds are globally-unique positive ints, so this second connection is
// exactly equivalent to HomeView's former in-handler search branch (and coexists with HomeView's own
// pendingReqId_-driven handler). HomeView keeps the UI concerns (the "_search" level, grid population,
// loading_ mirroring, the toasts, Back re-firing start()); this class owns only the async request lifecycle
// and the merge rules.
#pragma once
#include <QObject>
#include <QSet>
#include <QHash>
#include <QString>
#include "../addons/AddonModels.h"

class AddonManager;

class SearchAggregator : public QObject
{
    Q_OBJECT
public:
    SearchAggregator(AddonManager* mgr, QObject* parent = nullptr);

    // (Re)fire the fan-out for `query`: clear any prior state, then requestCatalog(page 1) against every
    // enabled media source's catalogs. Responses stream back via resultsAppended; finished fires when the
    // last one drains. Re-calling start() supersedes an in-flight search (its stale replies are then ignored).
    void start(const QString& query);
    // Forget the in-flight requests so their responses are ignored (no signals for them afterwards).
    void cancel();
    bool active() const { return !reqs_.isEmpty(); }         // requests still in flight
    const MediaCatalog& accumulated() const { return cat_; } // merged results so far (for (re)population)
    const QString& query() const { return query_; }          // the current search query (for the no-results toast)

    // The pure dedup/skip rule shared by the merge path (probe_browse covers this in isolation): reject the
    // synthetic (_open/info/rechdr) and empty-title rows, and any "title|type" (lowercased) already seen;
    // otherwise record the key and accept. Defined inline so the probe can exercise it without linking the
    // whole AddonManager request machinery.
    static bool acceptResult(const MediaItem& it, QSet<QString>& seen)
    {
        if (it.type == QStringLiteral("_open") || it.type == QStringLiteral("info")
            || it.type == QStringLiteral("rechdr") || it.title.isEmpty())
            return false;
        const QString dk = (it.title + QLatin1Char('|') + it.type).toLower();
        if (seen.contains(dk)) return false;
        seen.insert(dk);
        return true;
    }

signals:
    void resultsAppended(const MediaCatalog& add, bool firstBatch); // one reply's deduped, source-tagged rows
    void finished(int totalResults);                                // the req set drained (0 = no results)

private slots:
    void onCatalogReady(int requestId, const MediaCatalog& cat);

private:
    AddonManager* mgr_ = nullptr;
    QSet<int> reqs_;             // still-in-flight requestCatalog ids for this search
    QHash<int, QString> reqSrc_; // reqId -> source manifest id (to tag each result's origin)
    QSet<QString> seen_;         // dedup keys "title|type" already added
    MediaCatalog cat_;           // accumulated results (also used to re-populate)
    QString query_;              // the current search query
};
