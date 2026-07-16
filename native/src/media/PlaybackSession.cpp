#include "PlaybackSession.h"
#include "../core/AppPaths.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDateTime>
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

void PlaybackSession::setQueue(const QStringList& files, int startIndex, const QStringList& titles)
{
    tracks_ = files;
    QStringList displayTitles;
    for (int i = 0; i < tracks_.size(); ++i)
        displayTitles << (i < titles.size() && !titles[i].isEmpty()
                               ? titles[i] : QFileInfo(tracks_[i]).completeBaseName());
    emit queueChanged(displayTitles, startIndex);
    playIndex(startIndex);
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
    emit resumeSaved(); // host schedules the cloud "continue watching" push (debounced)
}

void PlaybackSession::finishResume()
{
    if (resumePath_.isEmpty()) return;
    store().remove(mediaResumeKey(resumePath_));
    store().remove(legacyAudiobookKey(resumePath_)); // also clear any legacy audiobook bookmark
    store().sync();
    resumePath_.clear();
    lastSavedPos_ = -100.0;
}

void PlaybackSession::clearQueue()
{
    persistResume();      // save where we left off before leaving this media
    resumePath_.clear();
    resumeSeek_ = 0.0;
    lastSavedPos_ = -100.0;
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
    const double s = resumeSeek_;
    resumeSeek_ = 0.0;
    return s;
}
