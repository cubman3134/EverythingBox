#include "AudiobookMetaAggregator.h"
#include "AddonManager.h"
#include "../browse/AudiobookCatalogs.h"   // the row id a book's MetaCache bundle is keyed by
#include "../core/AudiobookMatchStore.h"
#include "../core/MetaCache.h"

#include <QTimer>
#include <algorithm>

namespace {
const char* kAudiobookMetaType = "audiobook";
}

AudiobookMetaAggregator::AudiobookMetaAggregator(AddonManager* mgr, QObject* parent)
    : QObject(parent), mgr_(mgr)
{
    connect(mgr_, &AddonManager::metaReady, this, &AudiobookMetaAggregator::onMetaReady);
}

bool AudiobookMetaAggregator::hasProviders() const
{
    return !mgr_->metaProvidersFor(QString::fromLatin1(kAudiobookMetaType)).isEmpty();
}

int AudiobookMetaAggregator::sweep(const AudiobookLibrary::Index& idx)
{
    if (!hasProviders()) return 0;
    int n = 0;
    // Over `authors` and not over the narrator/series vectors: those hold COPIES of books already filed
    // under an author (AudiobookLibrary.h), so sweeping all three would scrape a book up to three times.
    for (const AudiobookLibrary::Author& a : idx.authors)
        for (const AudiobookLibrary::Book& b : a.books)
        {
            if (b.key.isEmpty()) continue;
            if (!AudiobookMeta::wantsEnrichment(b)) continue;   // its tags already say everything
            if (AudiobookMatches::has(b.key)) continue;         // matched once, or refused once. Either way: no.
            enqueue(b);
            ++n;
        }
    pump();
    return n;
}

void AudiobookMetaAggregator::enqueue(const AudiobookLibrary::Book& b)
{
    if (seen_.contains(b.key)) return;
    seen_.insert(b.key);

    auto job = QSharedPointer<Job>::create();
    job->id      = nextId_++;
    job->bookKey = b.key;
    job->title   = AudiobookLibrary::displayBook(b);
    job->author  = b.author.trimmed();
    job->book    = b;
    // WHAT THE PROVIDER IS ASKED. `title` is the book's own title — which for an untagged book is its FOLDER
    // name, and that is the honest thing to search on rather than a guess derived from it. The author rides
    // `subtitle`, which is where every addon in this tree already looks for a secondary search term. There is
    // no id: the app has never resolved this book to any catalogue, and inventing one would be a lie about
    // provenance.
    job->item.type     = QString::fromLatin1(kAudiobookMetaType);
    job->item.title    = job->title;
    job->item.subtitle = job->author;
    pending_.append(job);
}

void AudiobookMetaAggregator::pump()
{
    while (jobs_.size() < maxActive_ && !pending_.isEmpty())
        startJob(pending_.takeFirst());
}

void AudiobookMetaAggregator::startJob(const QSharedPointer<Job>& job)
{
    jobs_.insert(job->id, job);
    const QVector<LoadedAddon*> providers = mgr_->metaProvidersFor(QString::fromLatin1(kAudiobookMetaType));
    for (LoadedAddon* p : providers)
    {
        const int reqId = mgr_->requestMeta(p, job->item);
        if (reqId >= 0) { job->outstanding.insert(reqId, p->manifest.id); reqToJob_.insert(reqId, job->id); }
    }
    if (job->outstanding.isEmpty())   // nobody took it -> finish on the next tick, never re-entering pump()
    {
        const quint64 id = job->id;
        QTimer::singleShot(0, this, [this, id] { finishJob(id); });
        return;
    }
    job->timer = new QTimer(this);
    job->timer->setSingleShot(true);
    const quint64 id = job->id;
    connect(job->timer, &QTimer::timeout, this, [this, id] { finishJob(id); });  // answer with what arrived
    job->timer->start(12000);
}

void AudiobookMetaAggregator::onMetaReady(int requestId, const MediaDetail& detail)
{
    const auto jt = reqToJob_.constFind(requestId);
    if (jt == reqToJob_.constEnd()) return;   // not one of ours (the game aggregator / single-provider path)
    const quint64 jobId = jt.value();
    reqToJob_.erase(jt);
    const auto j = jobs_.constFind(jobId);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    const QString providerId = job->outstanding.take(requestId);
    const AudiobookMeta::Match m = AudiobookMeta::fromDetail(detail, providerId);
    if (!m.isEmpty()) job->results.push_back({ AudiobookMeta::providerPriority(providerId), m });
    if (job->outstanding.isEmpty()) finishJob(jobId);
}

void AudiobookMetaAggregator::finishJob(quint64 id)
{
    const auto j = jobs_.constFind(id);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    jobs_.remove(id);
    if (job->timer) { job->timer->stop(); job->timer->deleteLater(); job->timer = nullptr; }
    for (auto it = job->outstanding.constBegin(); it != job->outstanding.constEnd(); ++it)
        reqToJob_.remove(it.key());   // drop lingering mappings (the timeout path)

    // Best provider first, then fold each lower one in UNDER the result so far: a match is a best-of by
    // field rather than one provider's whole answer.
    std::stable_sort(job->results.begin(), job->results.end(),
                     [](const QPair<int, AudiobookMeta::Match>& a,
                        const QPair<int, AudiobookMeta::Match>& b) { return a.first < b.first; });
    AudiobookMeta::Match merged;
    for (const auto& pr : job->results) merged = AudiobookMeta::mergeLowerPriority(merged, pr.second);
    merged.confidence = AudiobookMeta::confidenceFor(job->book, merged);

    // AN ITEM WITH NO MATCH IS LEFT EXACTLY AS THE SCAN FOUND IT. No record, no placeholder, no half-filled
    // row — the next sweep may try again, and until then the book is what its tags said.
    if (merged.confidence < AudiobookMeta::kAcceptThreshold || !merged.hasFields()) { pump(); return; }

    AudiobookMatches::set(job->bookKey, merged);

    // The HEAVY half — cover and description — into MetaCache under the book's BROWSE ROW ID, which is the
    // key every existing metadata surface already reads a card by. That is what makes the cover render
    // offline and the description appear on the card with no new display code.
    const QString metaKey = QString::fromLatin1(browse::kAudiobookBookPrefix) + job->bookKey;
    MediaDetail card;
    card.title    = job->title;
    card.subtitle = job->author;
    card.overview = merged.description;
    card.imageUrl = merged.coverUrl;
    if (!merged.narrator.isEmpty())
        card.facts.push_back({ QStringLiteral("Narrator"), merged.narrator });
    if (!merged.series.isEmpty())
        card.facts.push_back({ QStringLiteral("Series"), merged.series });
    if (merged.year > 0)
        card.facts.push_back({ QStringLiteral("Year"), QString::number(merged.year) });
    card.valid = !card.title.isEmpty() || !card.overview.isEmpty() || !card.facts.isEmpty();
    if (!merged.coverUrl.isEmpty()) card.art.addImage(QStringLiteral("poster"), merged.coverUrl);
    if (!card.art.isEmpty()) MetaCache::saveArt(metaKey, card.art);
    if (card.valid) MetaCache::saveDetail(metaKey, card);

    emit matchesChanged(job->bookKey);
    pump();
}
