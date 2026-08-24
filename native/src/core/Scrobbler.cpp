#include "Scrobbler.h"
#include "ScrobbleQueue.h"
#include "Settings.h"

#include <QDateTime>
#include <QTimer>

namespace {

// The retry ladder, in seconds. Starts short (a phone stepping between wifi and mobile recovers in seconds)
// and tops out at five minutes, which is well under the shortest listening session anybody has and far above
// any rate limit either service applies. The listens are on disk throughout, so a long backoff costs nothing
// but latency.
constexpr int kRetryFirstSec = 15;
constexpr int kRetryMaxSec   = 300;

} // namespace

Scrobbler::Scrobbler(QObject* parent) : QObject(parent)
{
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, [this] { retrySec_ = 0; pump(); });
    // A launch after an offline stretch delivers what is waiting without the user doing anything. Deferred to
    // the event loop rather than run in the constructor: the provider is set immediately after us, and a pump
    // with no provider is a no-op that would then have to be re-armed from somewhere.
    QTimer::singleShot(0, this, [this] { pump(); });
}

Scrobbler::~Scrobbler()
{
    // A track that has passed its threshold when the app closes has been LISTENED TO, and the queue is on
    // disk — so finishing here is the difference between the last track of the evening landing tomorrow and
    // never landing at all. Nothing is submitted from a destructor; this only writes the row.
    finishCurrent();
    delete provider_;
    provider_ = nullptr;
}

void Scrobbler::setProvider(ScrobbleProvider* provider)
{
    if (provider_ == provider) return;
    delete provider_;
    provider_ = provider;
    emit statusChanged();
    pump();
}

Scrobble::Policy Scrobbler::policy()
{
    Scrobble::Policy p;
    p.enabled       = Settings::scrobbleEnabled();
    p.includeSpoken = Settings::scrobbleSpokenAudio();
    // FALSE, today, and the arm exists so it can become true without a redesign. EverythingBoxServer's
    // Subsonic endpoint records plays LOCALLY and forwards nothing, so a client that also scrobbles counts
    // each play once. The day the server grows upstream forwarding, this is the one line that changes — it
    // reads the server's own capability answer — and Scrobble::verdictFor already refuses a Server-origin play
    // when it is set. Nothing else in the feature moves.
    p.serverForwards = false;
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
    if (!watch_.counts) return;

    // "Now playing" is EPHEMERAL: sent on the way past, never queued, never retried. If it does not arrive,
    // nothing is owed — the listen itself is a separate, durable thing.
    if (provider_ && provider_->configured()) provider_->nowPlaying(track);
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
    if (provider_ && provider_->configured())
    {
        ScrobbleQueue::append(provider_->id(), play);
        emit statusChanged();
        pump();
    }
}

void Scrobbler::playbackStopped()
{
    finishCurrent();
    Scrobble::clear(watch_);
}

void Scrobbler::finishCurrent()
{
    if (!Scrobble::finish(watch_)) return;   // nothing owed: not eligible, not far enough, or already sent
    if (!provider_ || !provider_->configured()) return;
    Scrobble::Play play;
    play.track      = watch_.track;
    play.listenedAt = watch_.startedAt;
    ScrobbleQueue::append(provider_->id(), play);
    emit statusChanged();
    pump();
}

void Scrobbler::retryNow()
{
    retry_->stop();
    retrySec_ = 0;
    pump();
}

void Scrobbler::pump()
{
    if (pumping_ || !provider_ || !provider_->configured()) return;
    if (retry_->isActive()) return;          // backing off; the timer will call us
    const QVector<Scrobble::Play> batch = ScrobbleQueue::head(provider_->id(), ScrobbleQueue::kBatchSize);
    if (batch.isEmpty()) return;

    pumping_ = true;
    const int n = int(batch.size());
    provider_->submit(batch, [this, n](ScrobbleResult r) {
        pumping_ = false;
        recordResult(r, n);
    });
}

void Scrobbler::recordResult(const ScrobbleResult& r, int submitted)
{
    if (!provider_) return;
    const QString pid = provider_->id();
    switch (r.outcome)
    {
        case ScrobbleResult::Outcome::Ok:
            ScrobbleQueue::dropFront(pid, submitted);
            ScrobbleQueue::noteDelivered(pid, submitted);
            ScrobbleQueue::setLastError(pid, QString());
            retrySec_ = 0;
            emit statusChanged();
            pump();          // there may be more behind this batch
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
            scheduleRetry();
            emit statusChanged();
            return;
    }
}

void Scrobbler::scheduleRetry()
{
    retrySec_ = retrySec_ <= 0 ? kRetryFirstSec : qMin(retrySec_ * 2, kRetryMaxSec);
    retry_->start(retrySec_ * 1000);
}

int Scrobbler::deliveredCount() const
{ return provider_ ? ScrobbleQueue::delivered(provider_->id()) : 0; }

int Scrobbler::queuedCount() const
{ return provider_ ? ScrobbleQueue::count(provider_->id()) : 0; }

QString Scrobbler::statusLine() const
{
    if (!provider_) return tr("Scrobbling is not set up.");
    if (!Settings::scrobbleEnabled())
        return tr("Scrobbling is off. Nothing is being sent to %1.").arg(provider_->displayName());
    if (!provider_->configured())
        return tr("Enter your %1 token to start scrobbling.").arg(provider_->displayName());

    const QString pid = provider_->id();
    QString s = tr("Scrobbled %n track(s) to %1.", "", ScrobbleQueue::delivered(pid))
                    .arg(provider_->displayName());
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

void Scrobbler::noteFavorite(const Scrobble::Track& track, bool loved)
{
    if (!provider_ || !provider_->supportsLove() || !provider_->configured()) return;
    const Scrobble::Policy p = policy();
    // The SAME gate a listen passes. A user who has scrobbling switched off has not asked this app to tell
    // anybody what they like either, and an untagged file has nothing to love.
    if (!Scrobble::eligible(track, p)) return;
    const QString pid = provider_->id();
    provider_->love(track, loved, [this, pid](ScrobbleResult r) {
        // A love is not queued (see the header), so the only thing to do with a failure is SAY so. Silence
        // here is the exact failure the confidence indicator exists to prevent.
        if (r.outcome != ScrobbleResult::Outcome::Ok) ScrobbleQueue::setLastError(pid, r.message);
        emit statusChanged();
    });
}
