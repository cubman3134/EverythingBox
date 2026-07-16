// Fans a single game's metadata request out across every enabled provider addon that declared
// `metaFor: ["game"]` (SteamGridDB, IGDB, ScreenScraper, TheGamesDB, ...), then MERGES their results into
// one best-of MediaDetail: each artwork role, video, audio track and fact is taken from the highest-priority
// provider that supplies it. Used by the themed live panel on hover so "the best result for each game you
// hover over" is a blend of all configured sources, not whichever one happens to answer first.
//
// Latest-hover-wins: starting a new fetch() abandons any in-flight one (its late results are ignored), so
// rapidly moving the selection never mixes two games' art.
#pragma once
#include "AddonModels.h"
#include <QHash>
#include <QObject>
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

    // Query all game meta providers for `title` on `console` (a platform name, may be empty). `done` is
    // called once, with the merged result, when every provider has answered or the timeout elapses. If no
    // providers are configured, `done` is not called (there is nothing to aggregate).
    void fetch(const MediaItem& item, const QString& console, Done done);

    // True when at least one provider addon is available to aggregate (so callers can skip the work).
    bool hasProviders() const;

private slots:
    void onMetaReady(int requestId, const MediaDetail& detail);

private:
    void finalize();
    int  priorityOf(const QString& manifestId) const; // lower = higher priority in the merge

    AddonManager* mgr_;
    QTimer* timeout_;
    quint64 gen_ = 0;                       // bumped per fetch; late results from an older gen are dropped
    QHash<int, QString> outstanding_;       // in-flight requestId -> provider manifest id
    QVector<QPair<int, MediaDetail>> results_; // (priority, detail) collected this round
    Done done_;
};
