#pragma once
#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QSharedPointer>
#include <QStringList>
#include "LocalLibrary.h"

class AddonManager;
class LocalResolveCache;
class QTimer;
struct MediaCatalog;   // struct (not class) — must match AddonModels.h so MSVC mangles the slot identically

class CatalogResolver : public QObject
{
    Q_OBJECT
public:
    CatalogResolver(AddonManager* addons, LocalResolveCache* cache, QObject* parent = nullptr);
    void enqueue(const QVector<LocalLibrary::VideoEntry>& entries);
    void clearCacheAndRequeue(const QVector<LocalLibrary::VideoEntry>& entries);

signals:
    void resolved();

private slots:
    void onCatalogReady(int reqId, const MediaCatalog& catalog);

private:
    struct Job {
        quint64 id = 0;
        LocalLibrary::VideoEntry movie;
        qint64 size = 0, mtime = 0;
        QSet<int> outstanding;      // in-flight search reqIds
        QStringList matchedIds;     // one per source that matched
        bool issued = false;        // at least one search was actually dispatched
        QTimer* timer = nullptr;
    };
    void pump();
    void startJob(const QSharedPointer<Job>& job);
    void finishJob(quint64 id);
    void scheduleResolvedSignal();

    AddonManager* addons_;
    LocalResolveCache* cache_;
    QHash<quint64, QSharedPointer<Job>> jobs_;
    QList<QSharedPointer<Job>> pending_;
    QHash<int, quint64> reqToJob_;
    QSet<QString> seen_;            // paths queued/run this session (dedup)
    quint64 nextId_ = 1;
    int maxActive_ = 2;
    QTimer* resolvedDebounce_ = nullptr;
    bool cacheDirty_ = false;
};
