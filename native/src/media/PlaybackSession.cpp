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

void PlaybackSession::setQueue(const QStringList& files, int startIndex, const QStringList& titles,
                               const QString& resumeKey, const TrackHeaders& trackHeaders)
{
    tracks_ = files;
    // Assigned WITH the tracks, never appended to: a queue replaces the previous one wholesale, and headers
    // left over from the last queue would be indexed by position into a completely different track list.
    trackHeaders_ = trackHeaders;
    QStringList displayTitles;
    for (int i = 0; i < tracks_.size(); ++i)
        displayTitles << (i < titles.size() && !titles[i].isEmpty()
                               ? titles[i] : QFileInfo(tracks_[i]).completeBaseName());
    emit queueChanged(displayTitles, startIndex);
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
        maybeAppendNext();        // hand mpv the next track so its decoder crosses into it with no gap
    }
}

void PlaybackSession::setGapless(bool on)
{
    // Armed by the host BEFORE setQueue, and only for an audio queue with the setting on. Off is the default
    // and the no-regression guarantee: with it off, playIndex takes no gapless branch, onPlaylistPos()
    // early-returns, and appendRequested() is never emitted — the EOF -> handleTrackEnd advance is unchanged.
    gapless_ = on;
}

void PlaybackSession::maybeAppendNext()
{
    // Keep mpv's own playlist exactly one entry ahead of what it is playing: append the single queue index past
    // the frontier, if any. Fed incrementally (one append per boundary) so a long album never front-loads the
    // whole list, and carrying the per-track headers for symmetry with playRequested (empty for the local audio
    // queue gapless applies to). Emits nothing at the end of the queue — the last track has no successor.
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
            trackIndex_ += 1;
            beginResume(tracks_[trackIndex_]);
            emit trackChanged(trackIndex_, tracks_.size(), titleAt(trackIndex_));
            maybeAppendNext();
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
