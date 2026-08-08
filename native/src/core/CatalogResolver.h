#pragma once
#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QSharedPointer>
#include <QStringList>
#include "LocalLibrary.h"
#include "../addons/AddonModels.h"   // MediaItem / MediaDetail / MediaCatalog (the slot arg + the meta job's captured row)

class AddonManager;
class LoadedAddon;
class LocalResolveCache;
class QTimer;

class CatalogResolver : public QObject
{
    Q_OBJECT
public:
    CatalogResolver(AddonManager* addons, LocalResolveCache* cache, QObject* parent = nullptr);
    // forceMeta: refetch each resolved movie's metadata even when MetaCache already holds it — the Re-match
    // path passes true so a same-title-different-year mis-match is fixed end to end (issue #73).
    void enqueue(const QVector<LocalLibrary::VideoEntry>& entries, bool forceMeta = false);
    void clearCacheAndRequeue(const QVector<LocalLibrary::VideoEntry>& entries);

signals:
    void resolved();

private slots:
    void onCatalogReady(int reqId, const MediaCatalog& catalog);
    void onMetaReady(int reqId, const MediaDetail& detail);   // issue #73: the resolved movie's getMeta arrived

private:
    struct Job {
        quint64 id = 0;
        bool isShow = false;
        LocalLibrary::VideoEntry movie;             // movie jobs
        QString showKey, showTitle, seriesImdbId;   // show jobs
        qint64 size = 0, mtime = 0;
        QSet<int> outstanding;      // in-flight search reqIds
        QStringList matchedIds;     // one per source that matched
        bool issued = false;        // at least one search was actually dispatched
        QTimer* timer = nullptr;
        // Movie meta-fetch phase (issue #73): the first matched catalog row + its owning addon, so the same
        // queue slot can fetch getMeta and persist it to MetaCache under the local tile key.
        MediaItem     metaItem;
        LoadedAddon*  metaSource = nullptr;
        int           metaReq = -1;      // in-flight getMeta reqId (-1 = none)
        bool          forceMeta = false; // Re-match: refetch even if MetaCache already holds this item
    };
    void pump();
    void startJob(const QSharedPointer<Job>& job);
    void finishJob(quint64 id);       // resolve phase done → write the cache, then maybe fetch meta
    void startMetaFetch(const QSharedPointer<Job>& job);
    void completeJob(quint64 id);     // release the slot, coalesce the signal, pump the next
    void scheduleResolvedSignal();

    AddonManager* addons_;
    LocalResolveCache* cache_;
    QHash<quint64, QSharedPointer<Job>> jobs_;
    QList<QSharedPointer<Job>> pending_;
    QHash<int, quint64> reqToJob_;
    QHash<int, LoadedAddon*> reqSource_;   // catalog reqId → the source that issued it (to fetch its getMeta)
    QSet<QString> seen_;            // paths queued/run this session (dedup)
    quint64 nextId_ = 1;
    int maxActive_ = 2;
    QTimer* resolvedDebounce_ = nullptr;
    bool cacheDirty_ = false;
};
