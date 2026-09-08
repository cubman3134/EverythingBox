#include "Scrobbler.h"
#include "ScrobbleQueue.h"
#include "Settings.h"

#include <QDateTime>
#include <QStringList>
#include <QTimer>

namespace {

// The retry ladder, in seconds. Starts short (a phone stepping between wifi and mobile recovers in seconds)
// and tops out at five minutes, which is well under the shortest listening session anybody has and far above
// any rate limit either service applies. The listens are on disk throughout, so a long backoff costs nothing
// but latency.
constexpr int kRetryFirstSec = 15;
constexpr int kRetryMaxSec   = 300;

} // namespace

// One provider plus the delivery state that belongs to it alone. See Scrobbler.h for why the backoff and the
// in-flight latch are PER PROVIDER: sharing them would make a ListenBrainz outage stop Last.fm being written
// to for five minutes at a time, and the user's symptom would be the second service silently lagging.
struct Scrobbler::Slot
{
    ScrobbleProvider* provider = nullptr;
    bool    pumping  = false;   // one submission in flight at a time: the queue is a FIFO, and two
                                // overlapping submissions could drop the wrong prefix off the front
    int     retrySec = 0;       // current backoff, 0 == not backing off
    QTimer* retry    = nullptr; // owned by the Scrobbler (parented), stopped and dropped with the slot
};

Scrobbler::Scrobbler(QObject* parent) : QObject(parent)
{
    // A launch after an offline stretch delivers what is waiting without the user doing anything. Deferred to
    // the event loop rather than run in the constructor: the providers are installed immediately after us,
    // and a pump with none installed is a no-op that would then have to be re-armed from somewhere.
    QTimer::singleShot(0, this, [this] { pump(); });
}

Scrobbler::~Scrobbler()
{
    // A track that has passed its threshold when the app closes has been LISTENED TO, and the queue is on
    // disk — so finishing here is the difference between the last track of the evening landing tomorrow and
    // never landing at all. Nothing is submitted from a destructor; this only writes the rows.
    finishCurrent();
    clearProviders();
}

void Scrobbler::clearProviders()
{
    for (Slot* s : slots_)
    {
        if (s->retry) s->retry->stop();
        delete s->provider;
        delete s;
    }
    slots_.clear();
}

void Scrobbler::setProvider(ScrobbleProvider* provider)
{
    if (slots_.size() == 1 && slots_.first()->provider == provider) return;
    clearProviders();
    addProvider(provider);
}

void Scrobbler::addProvider(ScrobbleProvider* provider)
{
    if (!provider) return;
    for (const Slot* s : slots_) if (s->provider == provider) return;
    Slot* s = new Slot;
    s->provider = provider;
    s->retry = new QTimer(this);
    s->retry->setSingleShot(true);
    connect(s->retry, &QTimer::timeout, this, [this, s] { s->retrySec = 0; pumpSlot(s); });
    slots_.push_back(s);
    emit statusChanged();
    pump();
}

bool Scrobbler::removeProvider(const QString& id)
{
    if (id.isEmpty()) return false;
    for (int i = 0; i < slots_.size(); ++i)
    {
        Slot* s = slots_[i];
        if (!s || !s->provider || s->provider->id() != id) continue;
        // ONE SLOT. Every other slot keeps its backoff, its in-flight submission and its retry timer - see
        // the header for why a rebuild is not an option (an authorisation poll abandoned mid-flight).
        if (s->retry) s->retry->stop();
        slots_.remove(i);
        delete s->provider;      // its network callbacks are context-connected to it and go with it
        delete s;
        emit statusChanged();    // the status line stops naming a destination that no longer exists
        return true;
    }
    return false;
}

QVector<ScrobbleProvider*> Scrobbler::providers() const
{
    QVector<ScrobbleProvider*> out;
    out.reserve(slots_.size());
    for (const Slot* s : slots_) out.push_back(s->provider);
    return out;
}

Scrobble::Policy Scrobbler::policy()
{
    Scrobble::Policy p;
    p.enabled       = Settings::scrobbleEnabled();
    p.includeSpoken = Settings::scrobbleSpokenAudio();
    // THE COORDINATION, AND IT IS NOW A SETTING (issue #193, increment 6). The arm was written in increment
    // 1 against the day a music server would forward its own plays upstream; that day is here, because a
    // Navidrome or Airsonic box can be configured to scrobble to Last.fm and ListenBrainz on its own. The
    // issue's requirement is "a single clear setting rather than two that silently conflict", so this reads
    // that ONE setting, and the whole of the rest of the feature is unchanged: Scrobble::verdictFor already
    // decides what it means, and verdictForDestination decides who it applies to.
    p.serverForwards = Settings::scrobbleServerForwards();
    return p;
}

void Scrobbler::trackStarted(const Scrobble::Track& track)
{
    // ORDER MATTERS AND IS THE WHOLE GAPLESS STORY. The OUTGOING track is finished FIRST, using the play time
    // it accumulated and the timestamp it was stamped with when it began — because under gapless this is the
    // only notification the boundary produces at all, and beginning the new watch first would throw the old
    // one away unread.
    finishCurrent();

    const Scrobble::Policy p = policy();
    Scrobble::begin(watch_, track, QDateTime::currentSecsSinceEpoch(), p);

    // THE ONE ARM OF THE VERDICT THAT IS ABOUT THE DESTINATION, latched here for the whole of this track
    // (issue #193, increment 6). Scrobble::verdictForDestination sets out the argument; the short version is
    // that a server which forwards its own plays upstream only KNOWS about a play because this app reported
    // it, so suppressing the report to the server as well as to the upstream services counts the listen zero
    // times instead of once.
    //
    // Latched rather than re-read at queue time on purpose: everything else about this listen was settled
    // when it began, and a user who flips a setting during the last minute of a track they have already
    // listened to is not asking for that listen to be thrown away.
    watchServerOnly_ = Scrobble::verdictFor(track, p) == Scrobble::Verdict::SkipServerForwards;
    if (watchServerOnly_)
        // ...and the watch counts at all only if SOMEBODY would still take it. With no provider that owns
        // this play, the exclusion is total and there is nothing to accumulate.
        for (Slot* s : slots_)
            if (s->provider->configured() && s->provider->accepts(track) && s->provider->ownsSource(track))
                { watch_.counts = true; break; }
    if (!watch_.counts) return;

    // "Now playing" is EPHEMERAL: sent on the way past, never queued, never retried. If it does not arrive,
    // nothing is owed — the listen itself is a separate, durable thing. Announced to every service that
    // would be told about the listen, because the listener is listening to it on all of them.
    for (Slot* s : slots_)
        if (owes(s, track)) s->provider->nowPlaying(track);
}

// WHICH DESTINATIONS THIS TRACK IS OWED TO. Three questions, and each of them is a different way for a
// listen to be undeliverable: the service has no credential (`configured`), the service cannot be told about
// this track at all (`accepts` — a music server has no way to name a file on this disk), and the
// double-count coordination excludes this destination for this play (`watchServerOnly_`, above).
bool Scrobbler::owes(Slot* s, const Scrobble::Track& track) const
{
    if (!s || !s->provider || !s->provider->configured()) return false;
    if (!s->provider->accepts(track)) return false;
    if (watchServerOnly_ && !s->provider->ownsSource(track)) return false;
    return true;
}

void Scrobbler::positionTick(double positionSec)
{
    if (!watch_.active) return;
    if (!Scrobble::advance(watch_, positionSec)) return;
    // Crossed the threshold mid-track. Queue it NOW rather than at the boundary: a track that has been
    // listened to is owed whatever happens next, including the app being killed, the power going out, or the
    // user skipping the last thirty seconds.
    Scrobble::Play play;
    play.track      = watch_.track;
    play.listenedAt = watch_.startedAt;
    // ONE listen, filed once PER SERVICE. Each queue is keyed by provider id (ScrobbleQueue's whole reason
    // for taking one) so a listen delivered to ListenBrainz is still owed to Last.fm, with the SAME original
    // timestamp — which is what makes the two histories agree rather than differ by the length of an outage.
    bool queued = false;
    for (Slot* s : slots_)
        if (owes(s, watch_.track)) { ScrobbleQueue::append(s->provider->id(), play); queued = true; }
    if (queued) { emit statusChanged(); pump(); }
}

void Scrobbler::playbackStopped()
{
    finishCurrent();
    Scrobble::clear(watch_);
}

void Scrobbler::finishCurrent()
{
    if (!Scrobble::finish(watch_)) return;   // nothing owed: not eligible, not far enough, or already sent
    Scrobble::Play play;
    play.track      = watch_.track;
    play.listenedAt = watch_.startedAt;
    bool queued = false;
    for (Slot* s : slots_)
        if (owes(s, watch_.track)) { ScrobbleQueue::append(s->provider->id(), play); queued = true; }
    if (queued) { emit statusChanged(); pump(); }
}

void Scrobbler::retryNow()
{
    // EVERY provider's backoff is cancelled, not just one. This is what a settings change calls, and a user
    // who has just fixed a credential must not be made to wait out a delay earned before they fixed it — for
    // either service, since the surface does not make them press it twice.
    for (Slot* s : slots_) { s->retry->stop(); s->retrySec = 0; }
    pump();
}

void Scrobbler::pump()
{
    for (Slot* s : slots_) pumpSlot(s);
}

Scrobbler::Slot* Scrobbler::slotById(const QString& id)
{
    for (Slot* s : slots_) if (s && s->provider && s->provider->id() == id) return s;
    return nullptr;
}

void Scrobbler::pumpSlot(Slot* s)
{
    if (!s || s->pumping || !s->provider || !s->provider->configured()) return;
    if (s->retry->isActive()) return;        // this service is backing off; its own timer will call us
    const QVector<Scrobble::Play> batch = ScrobbleQueue::head(s->provider->id(), ScrobbleQueue::kBatchSize);
    if (batch.isEmpty()) return;

    s->pumping = true;
    const int n = int(batch.size());
    // BY ID, NOT BY POINTER (issue #299). A provider can be REMOVED while its submission is in flight - the
    // user deletes the music server mid-batch - and a callback holding the Slot* would then write through a
    // freed one. Looking the slot up when the answer arrives means a removed destination's reply is simply
    // dropped, which is the correct outcome: there is nothing left to record it against.
    const QString pid = s->provider->id();
    s->provider->submit(batch, [this, pid, n](ScrobbleResult r) {
        Slot* live = slotById(pid);
        if (!live) return;                  // removed while in flight: nothing to record, nothing dangling
        live->pumping = false;
        recordResult(live, r, n);
    });
}

void Scrobbler::recordResult(Slot* s, const ScrobbleResult& r, int submitted)
{
    if (!s || !s->provider) return;
    const QString pid = s->provider->id();
    switch (r.outcome)
    {
        case ScrobbleResult::Outcome::Ok:
            ScrobbleQueue::dropFront(pid, submitted);
            ScrobbleQueue::noteDelivered(pid, submitted);
            ScrobbleQueue::setLastError(pid, QString());
            s->retrySec = 0;
            emit statusChanged();
            pumpSlot(s);     // there may be more behind this batch, for THIS service
            return;

        case ScrobbleResult::Outcome::Rejected:
            // The service says this batch will never be accepted. DROP it, or the queue jams for ever behind
            // it and every listen after it is lost too — a silent total failure, which is strictly worse than
            // losing the batch that caused it. The reason is kept so the user is told which it was.
            ScrobbleQueue::dropFront(pid, submitted);
            ScrobbleQueue::setLastError(pid, r.message);
            emit statusChanged();
            return;

        case ScrobbleResult::Outcome::Auth:
            // KEEP the listens — the user can fix the token and they will still land, backdated — but stop
            // pumping. Retrying a refused credential in a loop is how an account gets rate-limited, and no
            // amount of waiting makes a wrong token right. The next pump comes from a settings change.
            ScrobbleQueue::setLastError(pid, r.message);
            emit statusChanged();
            return;

        case ScrobbleResult::Outcome::Retryable:
            ScrobbleQueue::setLastError(pid, r.message);
            scheduleRetry(s);
            emit statusChanged();
            return;
    }
}

void Scrobbler::scheduleRetry(Slot* s)
{
    if (!s) return;
    s->retrySec = s->retrySec <= 0 ? kRetryFirstSec : qMin(s->retrySec * 2, kRetryMaxSec);
    s->retry->start(s->retrySec * 1000);
}

int Scrobbler::deliveredCount() const
{
    int n = 0;
    for (const Slot* s : slots_) n += ScrobbleQueue::delivered(s->provider->id());
    return n;
}

int Scrobbler::queuedCount() const
{
    int n = 0;
    for (const Slot* s : slots_) n += ScrobbleQueue::count(s->provider->id());
    return n;
}

QString Scrobbler::statusLineFor(const ScrobbleProvider* provider) const
{
    if (!provider) return QString();
    // PROVIDER-NEUTRAL WORDING. Increment 1 said "Enter your %1 token", which is true of ListenBrainz and
    // false of Last.fm — there is no token to enter there, only an account to authorise. The orchestrator
    // must not know which is which (that is the whole point of the seam), so it says the one thing that is
    // true of both and leaves the how-to to the provider's own row in Settings.
    if (!provider->configured())
        return tr("%1 is not connected yet.").arg(provider->displayName());

    const QString pid = provider->id();
    QString s = tr("Scrobbled %n track(s) to %1.", "", ScrobbleQueue::delivered(pid))
                    .arg(provider->displayName());
    const int waiting = ScrobbleQueue::count(pid);
    if (waiting > 0) s += QLatin1Char(' ') + tr("%n waiting to send.", "", waiting);
    const int lost = ScrobbleQueue::dropped(pid);
    if (lost > 0) s += QLatin1Char(' ') + tr("%n older listen(s) were dropped to stay within the queue limit.",
                                             "", lost);
    const QString err = ScrobbleQueue::lastError(pid);
    // LAST, and only when there is one: a status line that leads with an error a successful submission has
    // since cleared is how a working feature gets reported as broken.
    if (!err.isEmpty()) s += QLatin1Char(' ') + tr("Last problem: %1").arg(err);
    return s;
}

QString Scrobbler::statusLine() const
{
    if (slots_.isEmpty()) return tr("Scrobbling is not set up.");
    if (!Settings::scrobbleEnabled()) return tr("Scrobbling is off. Nothing is being sent.");
    QStringList parts;
    // ONE SENTENCE PER SERVICE, never an average. "It is working" can be true of one and false of the other,
    // and a single merged number is exactly the shape in which a half-broken feature looks healthy.
    for (const Slot* s : slots_) parts << statusLineFor(s->provider);
    return parts.join(QLatin1Char(' '));
}

void Scrobbler::noteFavorite(const Scrobble::Track& track, bool loved)
{
    const Scrobble::Policy p = policy();
    for (Slot* s : slots_)
    {
        ScrobbleProvider* pr = s->provider;
        if (!pr->supportsLove() || !pr->configured() || !pr->accepts(track)) continue;
        // THE GATE A LOVE PASSES, WHICH IS NOT ALWAYS THE GATE A LISTEN PASSES (issue #193, increment 6).
        // For a service that PUBLISHES a love it is the same gate, and for the same reason increment 1 gave:
        // a user who has scrobbling switched off has not asked this app to tell anybody what they like. For a
        // destination whose love is an edit to the user's OWN library — starring a track on the very server
        // that is holding it — two of those arms are about something else entirely, and Scrobble.h says which
        // and why. The untagged and spoken arms still apply either way.
        if (Scrobble::loveVerdictFor(track, p, pr->loveIsLibraryEdit()) != Scrobble::Verdict::Submit)
            continue;
        const QString pid = pr->id();
        const bool libraryEdit = pr->loveIsLibraryEdit();
        pr->love(track, loved, [this, pid, libraryEdit](ScrobbleResult r) {
            // A love is not queued (see the header), so the only thing to do with a failure is SAY so.
            // Silence here is the exact failure the confidence indicator exists to prevent.
            if (r.outcome != ScrobbleResult::Outcome::Ok)
            {
                ScrobbleQueue::setLastError(pid, r.message);
                // ...and for a LIBRARY EDIT, say it where the user is rather than only in a settings panel
                // they are not looking at. They pressed a button and it half worked: the favourite is
                // recorded here and did not reach the server. Once, with the server's own words.
                if (libraryEdit && !r.message.isEmpty()) emit loveFailed(r.message);
            }
            emit statusChanged();
        });
    }
}
