// Queued AUDIOBOOK-metadata service (issue #198). Fans one scanned book out across every enabled provider
// addon that declared `metaFor: ["audiobook"]`, merges the replies by provider precedence into one best-of
// Match, scores it, and — only if it clears AudiobookMeta::kAcceptThreshold — stores it (AudiobookMatches)
// and caches its cover + description (MetaCache) so the book's card renders instantly and offline.
//
// IT IS GameMetaAggregator WITH DIFFERENT NOUNS, deliberately, down to the bounded queue and the per-job
// deadline: the four game-artwork providers proved the shape, and a second fan-out idiom would be a second
// place for "two providers answered" to mean something different. What is NOT copied is the hover entry
// point — nobody hovers a book to scrape it. There is one entry point, `sweep`, and it runs after a scan.
//
// WHAT THE SWEEP WILL NOT TOUCH, which is most of a healthy library:
//   * a book whose tags already carry a narrator, a series, a year and a cover (AudiobookMeta::wantsEnrichment)
//   * a book that already has a stored match — matched once, never re-fetched
//   * a book the user REJECTED — permanently, across this re-scan and every later one (AudiobookMatchStore.h)
//   * anything at all, when no addon declares itself an audiobook meta provider (the feature is then dormant
//     and costs one QVector check per scan)
//
// NOTHING AUDIBLE-SHAPED IS COMPILED IN. The app ships Open Library and Google Books — official, public,
// keyless — and any addon the user installs is an equal provider. AudiobookMeta.h states the reason at
// length; it is repeated here because this is the file somebody would add the call to.
#pragma once
#include "../core/AudiobookLibrary.h"
#include "../core/AudiobookMeta.h"
#include "AddonModels.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QVector>

class AddonManager;
struct LoadedAddon;
class QTimer;

class AudiobookMetaAggregator : public QObject
{
    Q_OBJECT
public:
    explicit AudiobookMetaAggregator(AddonManager* mgr, QObject* parent = nullptr);

    bool hasProviders() const;   // at least one addon declared metaFor:["audiobook"]

    // Enqueue every book in the index that wants enrichment and carries no record yet. Returns how many
    // were enqueued (0 = nothing to do, which is the steady state after the first sweep).
    int sweep(const AudiobookLibrary::Index& idx);

signals:
    // A book gained (or was refused) a match and the browse index should be re-derived. Carries the BOOK KEY
    // — the caller re-applies from the store rather than from this signal's contents, so a listener that
    // missed one is still correct on the next.
    void matchesChanged(const QString& bookKey);

private slots:
    void onMetaReady(int requestId, const MediaDetail& detail);

private:
    struct Job
    {
        quint64 id = 0;
        QString bookKey;
        QString title;
        QString author;
        AudiobookLibrary::Book book;               // what confidenceFor scores against
        MediaItem item;                            // what the providers are asked
        QHash<int, QString> outstanding;           // in-flight provider reqId -> manifest id
        QVector<QPair<int, AudiobookMeta::Match>> results;   // (priority, match) collected so far
        QTimer* timer = nullptr;                   // per-job deadline
    };

    void enqueue(const AudiobookLibrary::Book& b);
    void pump();
    void startJob(const QSharedPointer<Job>& job);
    void finishJob(quint64 id);

    AddonManager* mgr_;
    QHash<quint64, QSharedPointer<Job>> jobs_;
    QList<QSharedPointer<Job>> pending_;
    QHash<int, quint64> reqToJob_;
    QSet<QString> seen_;                           // book keys queued/run this session (dedup)
    quint64 nextId_ = 1;
    int maxActive_ = 2;                            // the providers are free and public; do not hammer them
};
