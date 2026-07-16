// Queued game-metadata service. Fans a game out across every enabled provider addon that declared
// metaFor:["game"] (SteamGridDB / IGDB / ScreenScraper / TheGamesDB), merges by per-role precedence into one
// best-of result, and CACHES it (MetaCache) so it renders instantly + offline next time. Jobs run through a
// small throttled queue (bounded concurrency) so entering a console can prefetch its whole game list in the
// background without hammering the providers' rate limits.
//
// Two entry points:
//   request()  - a hover: scrape ONE game at high priority (jumps the queue) and call back with the result.
//   prefetch() - entering a console: enqueue the whole game list at low priority to scrape + cache ahead of
//                the cursor, so by the time you hover a game its art is already there.
// Every job that finishes caches its result — even one whose hover was superseded — so scrolling never throws
// a scrape away, and a game is never scraped twice in a session (dedup + skip-if-cached).
#pragma once
#include "AddonModels.h"
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QVector>
#include <functional>

class AddonManager;
struct LoadedAddon;
class QTimer;

class GameMetaAggregator : public QObject
{
    Q_OBJECT
public:
    explicit GameMetaAggregator(AddonManager* mgr, QObject* parent = nullptr);
    using Done = std::function<void(const MediaDetail& merged)>;

    bool hasProviders() const; // at least one game meta provider is configured

    // Hover: scrape this game at HIGH priority (jumps the queue), then call `done` with the merged result.
    // If it's already cached, `done` fires with the cached card and no provider is hit.
    void request(const MediaItem& item, const QString& console, Done done);

    // Prefetch: enqueue a whole list to scrape + cache in the background at low priority. Skips games already
    // cached or already queued this session. Call it on entering a console so hovering any game is instant.
    void prefetch(const QVector<MediaItem>& items, const QString& console);

private slots:
    void onMetaReady(int requestId, const MediaDetail& detail);

private:
    struct Job
    {
        quint64 id = 0;
        MediaItem item;
        QString key;
        bool priority = false;
        Done done;
        QHash<int, QString> outstanding;          // in-flight provider reqId -> manifest id
        QVector<QPair<int, MediaDetail>> results;  // (priority, detail) collected so far
        QTimer* timer = nullptr;                   // per-job deadline
    };

    void enqueue(const MediaItem& item, const QString& console, bool priority, Done done);
    void pump();                               // start queued jobs up to the concurrency cap
    void startJob(const QSharedPointer<Job>& job);
    void finishJob(quint64 id);                // merge -> cache -> callback -> free the slot
    int  priorityOf(const QString& manifestId) const;

    AddonManager* mgr_;
    QHash<quint64, QSharedPointer<Job>> jobs_; // running jobs by id
    QList<QSharedPointer<Job>> pending_;       // queued jobs (priority ones at the front)
    QHash<int, quint64> reqToJob_;             // provider reqId -> job id
    QSet<QString> seen_;                       // keys queued/run this session (dedup)
    quint64 nextId_ = 1;
    int maxActive_ = 2;                        // throttle: providers have rate limits (esp. ScreenScraper)
};
