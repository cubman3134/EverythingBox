// THE SCROBBLE SEAM (issue #192) — the whole of what a scrobbling service has to be able to do, and nothing
// that is common to all of them.
//
// SHAPED FOR TWO IMPLEMENTATIONS WHILE ONE IS BEING WRITTEN. ListenBrainz lands in increment 1; Last.fm lands
// in increment 2 and must not force this to change shape. The way to make that true is not to guess at
// Last.fm's API — it is to keep off this interface everything that is not a per-service act:
//
//   * the THRESHOLD, what counts as a track, the audiobook rule, the double-count coordination — Scrobble.h.
//     Written once, applied to whichever providers are configured.
//   * the OFFLINE QUEUE and its ordering/timestamps — ScrobbleQueue.h. A provider is handed listens to post
//     and is told whether they landed; it never decides what to keep.
//   * WHEN to retry — the orchestrator's. A provider reports an Outcome and stops.
//
// What is left is three verbs, and they are three because the protocols themselves distinguish them:
//
//   nowPlaying   ephemeral, fire-and-forget. Both services treat it as a hint that expires on its own. It is
//                NEVER queued and NEVER retried — a retry queue for it is a bug, not robustness: delivering
//                "now playing" four minutes late tells the service the user is listening to something they
//                finished. That rule is enforced by this interface's SHAPE (no completion callback, nothing
//                to report, nothing to retry) rather than by a comment somebody has to obey.
//   submit       the durable one. A BATCH, because both services accept batches and the offline queue's whole
//                job is to hand over a flight's worth at once when the network comes back.
//   love         the favourite action's extra call.
//
// WHY NOT A QObject. A provider is used through callbacks the orchestrator owns, and every implementation of
// it in the app already owns a QNetworkAccessManager of its own; making the seam a QObject would buy signals
// nobody wants and cost a probe the ability to substitute a two-line fake. probe_scrobble's fake provider is
// exactly that.
#pragma once
#include "Scrobble.h"

#include <QString>
#include <QVector>
#include <functional>

// How a submission ended. The three arms exist because they lead to three different fates for the listens.
struct ScrobbleResult
{
    enum class Outcome
    {
        Ok,         // accepted; the orchestrator may drop these listens from the queue
        Retryable,  // the network, a 5xx, a rate limit. KEEP them and try again later.
        Auth,       // the credential was refused. KEEP them (the user can fix the token and they still land),
                    // but stop pumping — retrying a bad token in a loop is how an account gets rate-limited.
        Rejected    // the service says these listens are malformed and will never be accepted. DROP them, or
                    // the queue jams for ever behind one bad row and nothing after it is ever delivered.
    };

    Outcome outcome = Outcome::Retryable;

    // Human-readable, for the settings surface's status line. MUST NOT contain any part of a credential:
    // implementations build it from the service's own message or from an exception, NEVER by echoing the
    // request that failed — an error path that logs "the failing request" logs the token in it. See
    // ListenBrainzClient.cpp, where that rule is enforced at the one place a message is made.
    QString message;

    static ScrobbleResult ok()                                { return { Outcome::Ok,        QString() }; }
    static ScrobbleResult retryable(const QString& m = {})    { return { Outcome::Retryable, m }; }
    static ScrobbleResult auth(const QString& m = {})         { return { Outcome::Auth,      m }; }
    static ScrobbleResult rejected(const QString& m = {})     { return { Outcome::Rejected,  m }; }
};

class ScrobbleProvider
{
public:
    virtual ~ScrobbleProvider() = default;

    // Stable id ("listenbrainz", "lastfm") — what a queued listen is filed under, so a queue written by one
    // provider is never handed to another.
    virtual QString id() const = 0;
    // What the settings surface calls it.
    virtual QString displayName() const = 0;

    // Is there enough credential here to try at all? False means the orchestrator does not even queue: a
    // listen kept for a service the user never configured is a queue that grows for ever.
    virtual bool configured() const = 0;

    // Ephemeral. No callback ON PURPOSE — see the header.
    virtual void nowPlaying(const Scrobble::Track& track) = 0;

    // Durable, batched. `cb` is called exactly once, on the GUI thread. An implementation that cannot reach
    // the network still calls it, with Retryable.
    virtual void submit(const QVector<Scrobble::Play>& plays,
                        std::function<void(ScrobbleResult)> cb) = 0;

    // Can this service be told a track is loved? Answered per provider because it genuinely differs: Last.fm's
    // track.love takes an artist and a title, while ListenBrainz's feedback is keyed on a MusicBrainz recording
    // and has to resolve one first. A `false` here is what lets the favourite path stay silent rather than
    // report a failure the user can do nothing about.
    virtual bool supportsLove() const { return false; }

    // Love (or un-love) a track. Only called when supportsLove(). `cb` is called exactly once.
    virtual void love(const Scrobble::Track& track, bool loved,
                      std::function<void(ScrobbleResult)> cb)
    {
        (void)track; (void)loved;
        if (cb) cb(ScrobbleResult::rejected(QStringLiteral("This service does not support loving a track.")));
    }
};
