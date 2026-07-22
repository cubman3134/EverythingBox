#include "PlaybackSession.h"
#include "../core/AppPaths.h"
#include "../core/ConsumptionStats.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDateTime>
#include <algorithm>
#include <cmath>

// Stable, path-derived key prefix for one file's resume state (shared by video / audio / audiobooks).
static QString mediaResumeKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("resume/") + QString::fromLatin1(h) + QStringLiteral("/");
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
            ? AppPaths::dataDir() + QStringLiteral("/mymediavault.ini")
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
                               const QString& resumeKey)
{
    tracks_ = files;
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
    emit playRequested(tracks_[index]);
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
    store().remove(mediaResumeKey(resumePath_));
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
