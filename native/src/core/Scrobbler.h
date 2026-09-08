// THE SCROBBLE ORCHESTRATOR (issue #192, increment 1) — the one thing that holds the pure rules, the offline
// queue and the provider together, and the only object the app talks to.
//
// It is deliberately thin, and everything it does NOT do is somewhere else on purpose:
//
//   Scrobble.h          the threshold, what counts, the accumulator          (pure; probe_scrobble)
//   ScrobbleQueue.h     what is kept, in what order, with what timestamps    (per-profile store)
//   ScrobbleProvider.h  post this to that service                            (the seam Last.fm slots into)
//   THIS FILE           when to ask each of them, and what the user is told
//
// ==================================================================================================
// THE THREE THINGS THE HOST TELLS IT, AND WHY THEY ARE THESE THREE
// ==================================================================================================
//   trackStarted(track)   a track began. Fired from PlaybackSession::trackChanged, which is the ONE signal
//                         that crosses EVERY kind of boundary this app has: a fresh play, a manual jump, a
//                         crossfade promotion, and — the one that matters — a GAPLESS advance, where mpv walks
//                         its own playlist with no reload, no file open and no trip through the play sink.
//                         #193's fourth increment learned that the hard way: a per-track hook wired to the
//                         play sink misses gapless advances entirely, so the previous track's name stayed on
//                         screen for the whole of the next one. A scrobbler wired the same way would credit
//                         every listen to the wrong track, and would do it silently.
//   positionTick(sec)     the player's position. A property of the PLAYER, not of any page, so it keeps
//                         arriving while the music plays behind a browse surface (#193 increment 3).
//   playbackStopped()     the queue ended, was cleared, or the media was left. The last track of an album
//                         gets no trackChanged after it, so without this the final track of every album
//                         would be the one that never scrobbled.
//
// ==================================================================================================
// WHAT MAKES IT SAY SOMETHING WHEN IT FAILS
// ==================================================================================================
// "Scrobbling silently stops working" is the complaint every client that has ever implemented this attracts,
// and the issue calls it out by name. So there is a COUNTER of delivered listens and a LAST ERROR, both
// per-profile and both persisted, and `statusLine()` renders them into the one sentence both settings
// builders show. A user who suspects nothing is happening can look, and the answer is either a number that
// grows or a reason it does not.
#pragma once
#include "Scrobble.h"
#include "ScrobbleProvider.h"

#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

class Scrobbler : public QObject
{
    Q_OBJECT
public:
    explicit Scrobbler(QObject* parent = nullptr);
    ~Scrobbler() override;

    // Adopt a provider. Takes ownership. probe_scrobble calls it with an in-process fake, which is the whole
    // reason it is a setter and not a hard-wired member. Replacing the set finalises nothing and submits
    // nothing — the queue is filed per provider id, so listens waiting for one service stay waiting for that
    // service, whoever is installed here afterwards.
    void setProvider(ScrobbleProvider* provider);   // replace the whole set with this one
    // ...and add one BESIDE the others, which is what increment 2 needed and what the per-provider queue was
    // built for from the start. Two services are not a choice between them: someone with a decade of Last.fm
    // history and a new ListenBrainz account wants both, each gets its own queue, its own delivered counter,
    // its own backoff and its own last error, and one being refused never holds up the other.
    void addProvider(ScrobbleProvider* provider);
    // ...and REMOVE ONE, by its id (issue #299). A destination the user has deleted - a music server
    // removed in Settings - leaves a provider behind that answers configured() == false: it accepts nothing,
    // queues nothing and posts nothing, but it is still installed, still counted and still there until the
    // next launch.
    //
    // ONE PROVIDER, NOT A REBUILD, and that distinction is the whole of the fix. Rebuilding the set would
    // destroy and recreate the Last.fm provider alongside it, and LastFmClient carries connections made once
    // at startup plus an AUTHORISATION POLL that a recreation silently abandons mid-flight - a user in the
    // middle of linking Last.fm would watch it never finish, which is worse than a dormant object. So this
    // takes exactly the slot whose id matches and leaves every other slot's backoff, in-flight submission
    // and delivered counter untouched.
    //
    // The removed provider's QUEUE IS NOT TOUCHED. ScrobbleQueue is filed per provider id and outlives any
    // installation of it; deleting somebody's unsent listens because their server went away is a decision
    // this function is not entitled to make. Returns true when a slot was actually removed.
    bool removeProvider(const QString& id);
    // The installed providers, in the order they were added. Empty until one is set.
    QVector<ScrobbleProvider*> providers() const;

    // The user's answer, read fresh from the device-local settings each time it is needed. Not cached: the
    // settings surface can flip it while a track is playing, and a cached copy would keep scrobbling for the
    // rest of the album after the user switched it off.
    static Scrobble::Policy policy();

    // ---- what the host reports ------------------------------------------------------------------------
    // A track began. `track` with an empty artist or title is accepted and simply will not be submitted —
    // the caller does not have to know the eligibility rules, which is what keeps them in one place.
    void trackStarted(const Scrobble::Track& track);
    void positionTick(double positionSec);
    void playbackStopped();

    // The favourite action, mapped onto the service's love/unlove. No-op unless the provider supports it and
    // the track is usable; never blocks, never queues (a love is not a listen — it has no timestamp to
    // preserve and re-sending it later would be indistinguishable from the user pressing it again).
    void noteFavorite(const Scrobble::Track& track, bool loved);

    // ---- what the surfaces read -----------------------------------------------------------------------
    // "Scrobbled 412 tracks to ListenBrainz. 6 waiting to send. Last.fm is not connected yet."
    // ONE builder, shown by BOTH settings builders, so the two can never tell the user different things
    // about the same state — the same discipline as MainWindow::traktStatusLine. Every installed provider
    // gets its own sentence, because "scrobbling is working" can be true of one service and false of the
    // other, and a line that averaged them would be the silent-failure complaint all over again.
    QString statusLine() const;
    // One provider's sentence, exposed so both surfaces can render a single service on its own row and the
    // probe can assert each arm without arithmetic on a joined string.
    QString statusLineFor(const ScrobbleProvider* provider) const;
    int     deliveredCount() const;   // summed over every installed provider
    int     queuedCount() const;      // ditto

    // Try every provider's queue. Called on construction (a launch after an offline stretch delivers
    // immediately), when a listen is added, and by a retry timer. RESPECTS each provider's own backoff: a
    // submission that just failed is not retried again because another track finished a second later, and one
    // service backing off does not delay the other.
    void pump();

    // Try the queue NOW, cancelling any backoff first. This is what a SETTINGS CHANGE calls, and the
    // distinction is the whole reason there are two entry points: a wrong token produces an Auth refusal, the
    // ladder climbs to five minutes, and the user then fixes the token — at which point making them wait out a
    // backoff earned by the credential they have just corrected is indistinguishable, from the outside, from
    // the feature not working. Nothing in the background calls this; a real network outage still backs off.
    void retryNow();

signals:
    // The counter, the queue depth or the error moved. The settings surfaces re-render their status line.
    void statusChanged();

    // A LOVE THAT DID NOT LAND (issue #193, increment 6). A love is not queued and not retried (see the
    // header), so a failure has exactly one honest outcome: say so, once. The LOCAL favourite stands — the
    // press is recorded here and is the source of truth — because reverting the star the user just pressed
    // because a box on the landing is asleep is the worse of the two wrong answers, and it is the one that
    // looks like the button is broken.
    //
    // Only raised for a destination whose love is a LIBRARY EDIT. A Last.fm love that fails is a background
    // fact about a third-party service and belongs in the status line, not in a notification over whatever
    // the user is doing.
    void loveFailed(const QString& message);

private:
    // One provider and the delivery state that belongs to IT rather than to the app: whether a submission is
    // in flight, how far up the backoff ladder it has climbed, and the timer that will try again. Per
    // provider and not shared, because the alternative is a ListenBrainz outage stopping Last.fm from being
    // written to for five minutes at a time — two independent services sharing one backoff would make each
    // one's worst day the other's as well.
    struct Slot;

    // Which destinations the track being watched is owed to. See Scrobbler.cpp: three separate ways for a
    // listen to be undeliverable, and one of them (the double-count coordination) is per destination.
    bool owes(Slot* s, const Scrobble::Track& track) const;

    void finishCurrent();                       // the current watch owes a scrobble -> queue it, everywhere
    // The installed slot with this provider id, or null. Used where a POINTER would outlive its slot - a
    // submission callback that arrives after the destination was removed (issue #299).
    Slot* slotById(const QString& id);
    void pumpSlot(Slot* s);
    void scheduleRetry(Slot* s);
    void recordResult(Slot* s, const ScrobbleResult& r, int submitted);
    void clearProviders();

    QVector<Slot*>  slots_;
    // Latched when the watch began: the double-count coordination excludes this play from every destination
    // EXCEPT the one that served it. Scrobble::verdictForDestination is the rule; this is the answer for the
    // track currently being watched, held so that a mid-track settings change cannot retroactively decide
    // that a listen already heard was never owed.
    bool            watchServerOnly_ = false;
    Scrobble::Watch watch_;        // ONE accumulator for every provider: the threshold, what counts and the
                                   // gapless boundary are properties of the LISTENING, not of any service.
};
