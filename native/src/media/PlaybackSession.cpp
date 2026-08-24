#include "PlaybackSession.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/ConsumptionStats.h"
#include "../core/ResumeStore.h"   // issue #150: the key scheme + the tombstoned clear
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDateTime>
#include <algorithm>
#include <cmath>

// Stable, path-derived key prefix for one file's resume state (shared by video / audio / audiobooks).
// ResumeStore owns the hash so the clear can name this item to the tombstone store the same way the merge
// document names it; the trailing '/' is this file's own convenience for appending leaves.
static QString mediaResumeKey(const QString& path)
{
    return ResumeStore::groupFor(path) + QStringLiteral("/");
}

// Pre-generalization audiobooks were stored under "audiobook/"; read those too so in-progress books resume.
static QString legacyAudiobookKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("audiobook/") + QString::fromLatin1(h) + QStringLiteral("/");
}

PlaybackSession::PlaybackSession(const QString& settingsFile, QObject* parent)
    : QObject(parent), settingsFile_(settingsFile)
{
}

QSettings& PlaybackSession::store()
{
    if (!settings_)
    {
        const QString file = settingsFile_.isEmpty()
            ? AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile)
            : settingsFile_;
        settings_ = new QSettings(file, QSettings::IniFormat, this);
    }
    return *settings_;
}

QString PlaybackSession::titleAt(int index) const
{
    return QFileInfo(tracks_.value(index)).completeBaseName();
}

QStringList PlaybackSession::displayTitlesFor(const QStringList& titles) const
{
    QStringList displayTitles;
    for (int i = 0; i < tracks_.size(); ++i)
        displayTitles << (i < titles.size() && !titles[i].isEmpty()
                               ? titles[i] : QFileInfo(tracks_[i]).completeBaseName());
    return displayTitles;
}

void PlaybackSession::setQueue(const QStringList& files, int startIndex, const QStringList& titles,
                               const QString& resumeKey, const TrackHeaders& trackHeaders)
{
    tracks_ = files;
    // Assigned WITH the tracks, never appended to: a queue replaces the previous one wholesale, and headers
    // left over from the last queue would be indexed by position into a completely different track list.
    trackHeaders_ = trackHeaders;
    // #193: the display list is now KEPT, not just emitted. A later edit has to renumber the titles alongside
    // the paths, and the caller's list is only handed over here — recomputing base names at edit time would
    // silently swap the caller's titles for file names on the first insert.
    titles_ = displayTitlesFor(titles);
    emit queueChanged(titles_, startIndex, /*replaced*/ true);   // a whole new queue: the host may present it
    playIndex(startIndex);
    // playIndex resume-keyed the starting track by its file path; when the caller has a stable id (a catalog
    // stream / audiobook whose URL changes on re-resolve), re-key that starting track here instead — folded in
    // atomically so the re-key can't be forgotten or reordered by a caller (the old begin-after-setQueue bug).
    if (!resumeKey.isEmpty())
        beginResume(resumeKey);
}

void PlaybackSession::playIndex(int index)
{
    if (index < 0 || index >= tracks_.size()) return;
    persistResume();              // save where we were in the outgoing track (if any)
    trackIndex_ = index;
    beginResume(tracks_[index]);  // track the new file's position (and resume it if seen before)
    emit trackChanged(index, tracks_.size(), titleAt(index));
    // trackHeaders_ is allowed to be SHORTER than tracks_ (a local queue passes none at all), and a track
    // with no entry gets an empty set — which is the correct answer, and the one that makes the player clear
    // the previous track's headers rather than keep them.
    //
    // Spelled with an explicit bound and at(), not with value(): the log-discipline gate reads
    // `<name>Headers.value(…)` as pulling ONE header's value out into a local, and it is right to — that is
    // the hole no grep over log calls can see. This container is a LIST of header sets rather than a set, so
    // the match would be a false positive, and the fix for a false positive is not to teach the gate a
    // nuance it would then apply everywhere. It is to not write the shape.
    const StreamHeaders::Headers trackHeaders =
        index < trackHeaders_.size() ? trackHeaders_.at(index) : StreamHeaders::Headers();
    emit playRequested(tracks_[index], trackHeaders);
    // #141: a playRequested is a REPLACE-load — it resets mpv's playlist to this one entry — so (re-)arm the
    // gapless one-ahead feed from here. This is the single bootstrap for both the initial track (setQueue) and
    // every MANUAL jump (next/prev/a playlist-row click): a deliberate skip does a hard reload (nobody expects
    // gaplessness across a manual skip) and re-seeds the append frontier at the new position. AUTO-advance never
    // reaches here — it is owned by onPlaylistPos(), which moves trackIndex_ without a reload. No-op when off,
    // which is what keeps the gapless-off path byte-for-byte the pre-#141 machine.
    if (gapless_)
    {
        prevPos_ = 0;             // mpv's playlist-pos after a replace-load is 0 (one entry, about to grow)
        appendedThrough_ = index; // the entry mpv is now playing; its successor is the frontier to feed
        // #141 crossfade: with the deferral armed the host feeds this boundary itself, once it knows whether
        // the boundary is a crossfade (which needs the file mpv has not even loaded yet). Feeding here anyway
        // would hand mpv the entry it is then free to cross into on its own, and the crossfade would find the
        // boundary already spent. See setDeferAppend.
        if (!deferAppend_) maybeAppendNext(); // hand mpv the next track so its decoder crosses into it with no gap
    }
}

void PlaybackSession::setGapless(bool on)
{
    // Armed by the host BEFORE setQueue, and only for an audio queue with the setting on. Off is the default
    // and the no-regression guarantee: with it off, playIndex takes no gapless branch, onPlaylistPos()
    // early-returns, and appendRequested() is never emitted — the EOF -> handleTrackEnd advance is unchanged.
    gapless_ = on;
}

void PlaybackSession::armGaplessLive()
{
    // See the header for why merely setting gapless_ here would arm a feed that can never fire.
    if (gapless_ || trackIndex_ < 0) return;
    gapless_ = true;
    prevPos_ = 0;                    // mpv holds the one entry its last replace-load gave it
    appendedThrough_ = trackIndex_;  // …which is the playing entry: its successor is the frontier to feed
}

void PlaybackSession::setDeferAppend(bool on)
{
    // Armed by the host alongside setGapless, for a queue where crossfade is enabled. Off is the default and
    // the no-regression guarantee: unarmed, playIndex and onPlaylistPos feed the one-ahead entry immediately,
    // which is exactly what they did before crossfade existed.
    deferAppend_ = on;
}

void PlaybackSession::feedNextTrack()
{
    // The host's half of the deferral. maybeAppendNext's one-ahead invariant makes this idempotent, so the
    // host may call it from more than one place at the same boundary (the file-loaded and the duration
    // callback both reach the decision, whichever arrives second) without appending the same entry twice.
    if (gapless_) maybeAppendNext();
}

void PlaybackSession::maybeAppendNext()
{
    // Keep mpv's own playlist exactly one entry ahead of what it is playing: append the single queue index past
    // the frontier, if any. Fed incrementally (one append per boundary) so a long album never front-loads the
    // whole list, and carrying the per-track headers for symmetry with playRequested (empty for the local audio
    // queue gapless applies to). Emits nothing at the end of the queue — the last track has no successor.
    // The one-ahead INVARIANT, stated rather than assumed (#141 crossfade): mpv is fed the single entry after
    // the one playing, and only when it does not already have it. Both original callers satisfy this by
    // construction (playIndex seeds appendedThrough_ = index, onPlaylistPos bumps trackIndex_ first), so this
    // changes nothing for them - it is what makes the host-driven feedNextTrack() safe to call more than once
    // at the same boundary, which is the only way the deferred feed can be written without a second latch.
    if (appendedThrough_ != trackIndex_) return;
    const int next = appendedThrough_ + 1;
    if (next >= 0 && next < tracks_.size())
    {
        // Named with 'header' and read with at() (not value()), exactly as playIndex does: the log-discipline
        // gate needs the container to carry 'header' so its value-read check can see it, and a bounded at()
        // rather than value() so it is not read as pulling one header's value into a local. Same shape, same
        // reason — see the long note in playIndex.
        const StreamHeaders::Headers trackHeaders =
            next < trackHeaders_.size() ? trackHeaders_.at(next) : StreamHeaders::Headers();
        emit appendRequested(tracks_[next], trackHeaders);
        appendedThrough_ = next;
    }
}

void PlaybackSession::onPlaylistPos(int mpvPos)
{
    if (!gapless_) return;                       // off path never routes here; guard so a stray call is inert
    const int done = tracksCompleted(prevPos_, mpvPos);
    prevPos_ = mpvPos;                           // advance the cursor even for a dup/backward (which report 0)
    for (int k = 0; k < done; ++k)
    {
        // The SAME per-item work handleTrackEnd does for a track that just finished: flush its final accrual,
        // then drop its resume mark because it played to the end. audioPos_ still holds the FINISHING track's
        // last position here — mpv reports playlist-pos as it loads the next entry, before that entry's time-pos
        // resets — so the tail seconds accrue to the right track. (persistResume then finishResume, the exact
        // order handleTrackEnd uses so the last ≤5s window is not dropped.)
        persistResume();
        finishResume();
        // Advance the app's current track WITHOUT a reload — mpv already crossed into the next entry itself,
        // which is the whole point of gapless. beginResume + trackChanged are the same per-track callbacks
        // playIndex fires; the one-ahead append is refed so the FOLLOWING boundary is already decoding.
        if (trackIndex_ + 1 < tracks_.size())
        {
            advanceWithoutReload();
            if (!deferAppend_) maybeAppendNext();  // #141: deferred, the host refeeds once it has decided
        }
        else
        {
            // Defensive only. The normal final-track completion is an EOF the host routes to handleTrackEnd:
            // there is no playlist-pos PAST the last index for mpv to report, so this branch is unreachable in
            // the ordinary flow and exists so a mpv that somehow skips past the end still finishes the queue
            // rather than silently stranding it. (probe_playback documents this as a tripwire, not a path.)
            emit queueFinished();
        }
    }
}

// The per-item work a boundary the PLAYER crossed by itself still owes the app: move current on by one, re-key
// the resume to the new file, and tell the host. Extracted (#141 crossfade) because there are now two ways a
// boundary can happen without a reload - mpv's own gapless playlist advance, and the crossfade handover - and
// they owe the queue exactly the same thing. A second hand-written copy is how one of them ends up not
// re-keying the resume, which is invisible until somebody reopens the album.
void PlaybackSession::advanceWithoutReload()
{
    if (trackIndex_ + 1 >= tracks_.size()) return;
    trackIndex_ += 1;
    beginResume(tracks_[trackIndex_]);
    emit trackChanged(trackIndex_, tracks_.size(), titleAt(trackIndex_));
}

void PlaybackSession::onCrossfadePromoted()
{
    if (trackIndex_ < 0) return;
    // The finishing track's tail seconds accrue to IT, then its resume mark goes because it played out - the
    // same persist-then-finish order handleTrackEnd and the gapless boundary use, for the same reason (the
    // last <=5s window would otherwise be dropped by finishResume forgetting the file first).
    persistResume();
    finishResume();
    if (trackIndex_ + 1 < tracks_.size())
    {
        advanceWithoutReload();
        // RE-SEAT THE GAPLESS BOOKKEEPING. The handover swapped players: the deck now playing is a different
        // mpv instance whose playlist holds exactly ONE entry, the track that just became current. So its
        // playlist-pos starts at 0 again and its append frontier is that entry - carrying the old deck's
        // numbers across would make the next playlist-pos read as a backward jump and the next append land at
        // the wrong index. Then feed the FOLLOWING boundary the same way the gapless path does, unless the
        // host is deferring (it is, whenever crossfade is armed - which is whenever we are here).
        prevPos_ = 0;
        appendedThrough_ = trackIndex_;
        if (!deferAppend_) maybeAppendNext();
    }
    else
    {
        // Defensive, like the gapless boundary's last-track branch: a crossfade is only ever started for a
        // boundary that HAS a successor, so there is no way to promote past the end of the queue. If one ever
        // happens the queue finishes properly rather than stranding on a track nothing owns.
        emit queueFinished();
    }
}

// The OTHER thing a crossfade handover can land on: a file that is not in this queue at all. Channel mode
// (#141) airs one playlist item at a time, each as its own queue, so the boundary out of the LAST entry of
// one item and into the channel's next pick crosses between two queues — and by the time this is called the
// player is already several seconds into the second one, on the deck that just took over. So the queue is
// installed AROUND what is playing instead of starting it.
//
// Every line below is onCrossfadePromoted's, in its order and for its reasons; the only difference is that
// "advance by one" becomes "this list, at this index". That is deliberate — a channel boundary owes the app
// exactly what an in-queue boundary owes it, and the day one of them stops re-keying the resume it should be
// because somebody changed both.
void PlaybackSession::adoptPlayingQueue(const QStringList& files, int index, const QStringList& titles)
{
    if (files.isEmpty() || index < 0 || index >= files.size()) return;
    // The finishing track's tail seconds accrue to IT, then its resume mark goes because it played out — the
    // same persist-then-finish order every boundary in this class uses, so the last <=5 s window survives.
    persistResume();
    finishResume();
    tracks_ = files;
    // Cleared WITH the tracks, exactly as setQueue assigns them: this is a new track list, and the previous
    // queue's per-track headers would be indexed by position into it. A local music queue carries none, which
    // is the only kind of queue that reaches here.
    trackHeaders_ = {};
    titles_ = displayTitlesFor(titles);   // #193: kept for the same reason setQueue keeps it
    emit queueChanged(titles_, index, /*replaced*/ true);   // a channel handover installs a queue wholesale
    trackIndex_ = index;
    beginResume(tracks_[index]);
    emit trackChanged(index, tracks_.size(), titleAt(index));
    // RE-SEAT THE GAPLESS BOOKKEEPING onto the deck that is now the player — a different mpv instance, whose
    // playlist holds exactly the one entry playing. Its playlist-pos starts at 0 again and its append frontier
    // is that entry; carrying the old deck's numbers over would read the next position as a backward jump and
    // land the next append at the wrong index. Identical to onCrossfadePromoted's re-seat, for its reason.
    prevPos_ = 0;
    appendedThrough_ = index;
    if (!deferAppend_) maybeAppendNext();
}

// ================= Editing the queue you are listening to (issue #193) ======================================
//
// The list surgery is three lines each; everything that is actually difficult about editing a LIVE queue is
// either in QueueEdit (which entry mpv is holding, and whether this edit invalidated it) or in commitEdit
// below (what the host is owed as a result). The verbs themselves exist only to keep the three parallel lists
// in step, and they do that BEFORE consulting the plan's cursor, because the plan is computed from the
// pre-edit state on purpose — the arithmetic is stated against the list the caller was looking at.

// The queue's per-track headers are a SHORTER-OR-EQUAL parallel list (a local queue carries none at all). An
// edit that renumbers the tracks has to renumber them too, or an IPTV queue plays entry 5 with entry 4's
// credentials — so pad to the full length first and let the same surgery run over both. Left empty when it
// was empty, which is every local music queue and therefore the common case: padding an empty list would turn
// "this queue has no headers" into "this queue has N empty header sets", which playIndex reads identically
// but which would then be carried, copied and cleared for no reason.
void PlaybackSession::padTrackHeaders()
{
    if (trackHeaders_.isEmpty()) return;
    while (trackHeaders_.size() < tracks_.size()) trackHeaders_.push_back(StreamHeaders::Headers());
}

bool PlaybackSession::insertTracks(int at, const QStringList& files, const QStringList& titles)
{
    const QueueEdit::Plan plan =
        QueueEdit::planInsert({ int(tracks_.size()), trackIndex_, appendedThrough_ }, at, int(files.size()));
    if (!plan.valid) return false;
    padTrackHeaders();
    for (int k = 0; k < files.size(); ++k)
    {
        tracks_.insert(at + k, files.at(k));
        // Each inserted row's own title, falling back to the file's base name exactly as displayTitlesFor
        // does for an install — one rule for what a row is called, whether it arrived with the queue or was
        // added to it an hour later.
        const QString t = k < titles.size() && !titles.at(k).isEmpty()
                              ? titles.at(k) : QFileInfo(files.at(k)).completeBaseName();
        titles_.insert(at + k, t);
        if (!trackHeaders_.isEmpty()) trackHeaders_.insert(at + k, StreamHeaders::Headers());
    }
    return commitEdit(plan);
}

bool PlaybackSession::removeTrack(int at)
{
    const QueueEdit::Plan plan =
        QueueEdit::planRemove({ int(tracks_.size()), trackIndex_, appendedThrough_ }, at);
    if (!plan.valid) return false;
    padTrackHeaders();
    tracks_.removeAt(at);
    titles_.removeAt(at);
    if (!trackHeaders_.isEmpty()) trackHeaders_.removeAt(at);
    return commitEdit(plan);
}

bool PlaybackSession::moveTrack(int from, int to)
{
    const QueueEdit::Plan plan =
        QueueEdit::planMove({ int(tracks_.size()), trackIndex_, appendedThrough_ }, from, to);
    if (!plan.valid) return false;
    padTrackHeaders();
    tracks_.move(from, to);
    titles_.move(from, to);
    if (!trackHeaders_.isEmpty()) trackHeaders_.move(from, to);
    return commitEdit(plan);
}

bool PlaybackSession::playNext(const QStringList& files, const QStringList& titles)
{
    // With nothing playing there is no "next": append instead of inserting at 0, so a play-next fired at an
    // idle queue does not jump the whole list. (An empty queue makes both the same thing.)
    return insertTracks(trackIndex_ < 0 ? int(tracks_.size()) : trackIndex_ + 1, files, titles);
}

bool PlaybackSession::enqueue(const QStringList& files, const QStringList& titles)
{
    return insertTracks(int(tracks_.size()), files, titles);
}

// The single tail every edit runs. Announce the new list, carry the cursor and the append frontier, and say
// what mpv still owes — in that order, because the host rebuilds its row model from queueChanged and both of
// the later steps can move the highlighted row.
bool PlaybackSession::commitEdit(const QueueEdit::Plan& plan)
{
    if (plan.cursorRemoved)
    {
        // THE TRACK PLAYING WAS DELETED. Announce the shortened list first so the host is not briefly holding
        // a row model with a track in it that no longer exists, then either advance onto the entry that took
        // its place — a hard replace-load, which is also what reseeds prevPos_/appendedThrough_ onto mpv's
        // reset playlist, so no separate mpv repair is owed — or stop, when there was nothing after it.
        //
        // EITHER WAY the removed track's position is SAVED first — playIndex's own persistResume on the
        // advance branch, the explicit one on the stop branch — and it matters WHICH file that names:
        // resumePath_ still holds the removed track at this point, so the position lands under its own key
        // and the listener can come back to it. finishResume is deliberately NOT called on either branch:
        // the track did not play out, it was deleted, and dropping its bookmark would be the queue edit
        // quietly forgetting where you were in a track you only meant to take out of tonight's listening.
        trackIndex_ = plan.cursor;
        appendedThrough_ = -1;
        prevPos_ = 0;
        emit queueChanged(titles_, plan.playIndex, /*replaced*/ false);   // an EDIT: present nothing
        if (plan.playIndex >= 0)
            playIndex(plan.playIndex);
        else
        {
            persistResume();
            emit playbackStopped();
        }
        return true;
    }
    trackIndex_ = plan.cursor;
    appendedThrough_ = plan.frontier;
    emit queueChanged(titles_, trackIndex_, /*replaced*/ false);   // an EDIT: present nothing
    // The reseat is the host's, not ours: this class never talks to mpv. It drops the entries mpv holds past
    // the one playing and calls feedNextTrack(), which lands back in maybeAppendNext — whose one-ahead
    // invariant is exactly the state the plan just restored by shrinking the frontier to the cursor.
    if (plan.reseat) emit queueFeedInvalidated();
    return true;
}

void PlaybackSession::next()
{
    if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) playIndex(trackIndex_ + 1);
}

void PlaybackSession::prev()
{
    if (trackIndex_ > 0) playIndex(trackIndex_ - 1);
}

void PlaybackSession::handleTrackEnd()
{
    // Flush the final ≤5s accrual BEFORE clearing the resume mark: the last heartbeat window (up to the throttle
    // interval of un-accrued playback) would otherwise be dropped when finishResume() forgets the file. Mirrors
    // clearQueue's persist-then-clear ordering so a completed track's tail seconds land in ConsumptionStats.
    persistResume();
    finishResume(); // the file played to the end -> drop its resume mark (next open starts fresh)
    // Auto-advance the audio queue when a track finishes (ignored for video / single files).
    if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) { playIndex(trackIndex_ + 1); return; }
    emit queueFinished();
}

void PlaybackSession::beginResume(const QString& path)
{
    resumePath_ = path;
    double pos = store().value(mediaResumeKey(path) + QStringLiteral("pos"), 0.0).toDouble();
    if (pos <= 0.0) pos = store().value(legacyAudiobookKey(path) + QStringLiteral("pos"), 0.0).toDouble();
    resumeSeek_ = pos;       // applied once the duration is known (see onDuration)
    audioPos_ = 0.0;
    lastSavedPos_ = -100.0;
    // Consumption stats: start accrual from the resume point so the resume jump itself never dumps time, and
    // clear any carried remainder from the previous track (per-track reset).
    lastAccruedPos_ = pos;
    statsAccum_ = 0.0;
}

void PlaybackSession::persistResume()
{
    if (resumePath_.isEmpty() || audioPos_ <= 1.0) return; // nothing meaningful to remember yet
    const QString k = mediaResumeKey(resumePath_);
    store().setValue(k + QStringLiteral("pos"), audioPos_);
    store().setValue(k + QStringLiteral("dur"), duration_); // lets the home screen show a progress bar
    store().setValue(k + QStringLiteral("title"), QFileInfo(resumePath_).completeBaseName());
    store().setValue(k + QStringLiteral("ts"), QDateTime::currentSecsSinceEpoch()); // for cross-device merge-by-recency
    store().sync();
    // A position undoes an earlier clear of the same item (issue #150): re-watching something you finished must
    // not be suppressed by the tombstone that finishing it left. Newest-wins would carry all but the same-second
    // case on its own — the merge's `tomb >= item` rule, which favourites share — so this closes that, and says
    // out loud that "cleared" is not permanent. Cheap: a lookup that finds nothing on every ordinary save.
    ResumeStore::noteResumed(resumePath_);
    lastSavedPos_ = audioPos_;

    // Consumption stats: accrue the forward-only playback delta since the last heartbeat, clamped to [0, 30]s so
    // a seek-forward can't dump minutes and a seek-backward accrues nothing. The exact float position drives the
    // diff (seeks handled correctly, no runaway); a sub-second remainder carries in statsAccum_ so whole-second
    // rounding never drifts. Keyed by the resume identity (its own per-profile store — NOT the global resume
    // keys), category from the file's kind, title as the current resume title.
    const double dpos = std::min(std::max(audioPos_ - lastAccruedPos_, 0.0), 30.0);
    lastAccruedPos_ = audioPos_;
    statsAccum_ += dpos;
    const qint64 whole = qint64(statsAccum_);
    if (whole > 0)
    {
        statsAccum_ -= double(whole);
        ConsumptionStats::addMediaSeconds(resumePath_,
            mediaIsVideo_ ? QStringLiteral("video") : QStringLiteral("audio"),
            whole, QFileInfo(resumePath_).completeBaseName());
    }

    emit resumeSaved(); // host schedules the cloud "continue watching" push (debounced)
}

void PlaybackSession::finishResume()
{
    if (resumePath_.isEmpty()) return;
    // Through ResumeStore, which removes the group AND records a dated tombstone (issue #150). A bare remove()
    // made "played to the end" and "this device has never opened that file" the same fact on disk, and the
    // merge — which cannot delete, because an absence carries no timestamp — read the second one: the cloud
    // document still holds THIS device's own pre-finish row, so the next sync put the position back with no
    // second device involved. The tombstone is a clear with a time on it, which the merge can compare.
    ResumeStore::clear(store(), resumePath_);
    store().remove(legacyAudiobookKey(resumePath_)); // also clear any legacy audiobook bookmark
    store().sync();
    resumePath_.clear();
    resumeSeek_ = 0.0; // a finished file has no pending seek; don't let a stale target leak forward
    lastSavedPos_ = -100.0;
}

void PlaybackSession::clearQueue()
{
    persistResume();      // save where we left off before leaving this media (also flushes final accrual)
    resumePath_.clear();
    resumeSeek_ = 0.0;
    lastSavedPos_ = -100.0;
    lastAccruedPos_ = 0.0; // consumption-stats: per-track reset (next media starts a fresh accrual span)
    statsAccum_ = 0.0;
    tracks_.clear();
    titles_.clear();      // #193: the display list is part of the queue and dies with it
    // The queue's headers die with the queue. NOT observable — every read of trackHeaders_ is a playIndex
    // reached through a setQueue, which assigns the list wholesale — so probe_playback pins no assertion on
    // this line and says why. It stays because tracks_ and trackHeaders_ are a parallel pair: clearing one
    // and not the other leaves two members disagreeing about how many tracks there are, which is the state a
    // future change to the read path would be bitten by.
    trackHeaders_.clear();
    trackIndex_ = -1;
    emit queueCleared();
}

void PlaybackSession::setPosition(double s)
{
    audioPos_ = s;
    // Throttle resume writes so we're not hammering the ini every position tick.
    if (!resumePath_.isEmpty() && std::abs(s - lastSavedPos_) >= 5.0)
        persistResume();
}

void PlaybackSession::setDuration(double s)
{
    duration_ = s;
}

double PlaybackSession::takeResumeSeek()
{
    // Keep the old onDuration guard: with no tracked file there is no valid seek target — a stale
    // value must never drive a seek on a late/duplicate durationChanged after finishResume().
    if (resumePath_.isEmpty()) return 0.0;
    const double s = resumeSeek_;
    resumeSeek_ = 0.0;
    return s;
}
