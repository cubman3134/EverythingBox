#pragma once
#include "../core/StreamHeaders.h"
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QVector>

// The audio-queue + resume state machine: owns the current track list/position, drives next/prev/
// track-end advance, and persists "where you left off" for any timed media (video/audio/audiobook),
// keyed by a stable path/id. The host (MainWindow) owns the actual player and playlist widget; this
// class only decides WHAT plays next and WHERE to resume, telling the host via signals.
class PlaybackSession : public QObject
{
    Q_OBJECT
public:
    // Empty settingsFile = the app's own store (AppPaths::dataDir()/everythingbox.ini); probes pass a
    // scratch path so tests never touch the real user store.
    explicit PlaybackSession(const QString& settingsFile = QString(), QObject* parent = nullptr);

    // A track's HTTP request headers, indexed exactly like `files`. Empty (the default) for every local
    // queue; see setQueue.
    using TrackHeaders = QVector<StreamHeaders::Headers>;

    // resumeKey empty (default) = the starting track resumes keyed by its own file path (current behavior).
    // Non-empty = the first-played track is resume-keyed by `resumeKey` instead (a stable catalog/audiobook
    // id), folded in atomically so callers no longer re-key with a separate beginResume() after setQueue.
    //
    // `trackHeaders` is the PER-TRACK header channel (#59). Before it, this class carried urls only, so every
    // queue-driven load — a gated audio stream, an IPTV channel list, and every advance within one — reached
    // the player bare and 403'd. Per TRACK and not per queue because an IPTV playlist's entries are separate
    // URLs on whatever hosts the list names, and the playlist's own headers belong to the playlist's origin
    // alone: the caller scopes each entry through StreamHeaders::forPlayUrl and hands the answers here.
    //
    // Shorter than `files` (or empty) is normal and means "no headers for the tracks past the end" rather
    // than being an error — playHeaders() reads it with value(), so a local queue passes nothing at all.
    void setQueue(const QStringList& files, int startIndex, const QStringList& titles = {},
                  const QString& resumeKey = QString(), const TrackHeaders& trackHeaders = {});
    void playIndex(int index);
    void next();
    void prev();
    void handleTrackEnd(); // advances the queue, or emits queueFinished() at the last track
    void clearQueue();     // persists then resets (was clearAudioQueue minus the widget lines)

    void beginResume(const QString& pathOrKey); // start tracking this file/key (and queue its saved spot)
    void persistResume();                       // save the current position (throttled / on leave / on exit)
    void finishResume();                        // played to the end -> drop the saved position

    // Consumption stats: the host reports the loaded file's media kind (mpv's fileLoaded isVideo flag) so the
    // persistResume heartbeat accrues watch (video) vs listen (audio) seconds into the right category.
    void setMediaVideo(bool isVideo) { mediaIsVideo_ = isVideo; }
    // The same app-stamped kind, readable. Every open site stamps it SYNCHRONOUSLY (mpv loads async), so it is
    // already settled when the host's duration callback runs — unlike mpv's own fileLoaded isVideo flag, which
    // is an async event with no ordering guarantee against `duration` and mis-reports cover-art audio as video.
    bool mediaIsVideo() const { return mediaIsVideo_; }

    void setPosition(double s); // fed from the host's mpv position callback
    void setDuration(double s); // fed from the host's mpv duration callback

    double takeResumeSeek(); // returns the pending resume target once, then 0 (consumed by onDuration)

    int currentIndex() const { return trackIndex_; }
    int count() const { return tracks_.size(); }
    QString trackAt(int i) const { return tracks_.value(i); }
    double position() const { return audioPos_; }

signals:
    // The host hands both to mpv. The headers ride the SIGNAL rather than being read back off this object
    // afterwards, for the reason #43 gave for the resolve callback: a getter returning "the current track's
    // headers" can be called at a moment when "current" has moved on, and the value that outlives its track
    // is the leak. Emitted on every track — empty for the ones that need none, which is what clears the
    // previous track's headers at the player (MpvHeaderApply writes all three properties unconditionally).
    void playRequested(const QString& path, const StreamHeaders::Headers& trackHeaders);
    void trackChanged(int index, int count, const QString& displayTitle);
    void queueChanged(const QStringList& titles, int current);        // host rebuilds playlist_
    void queueCleared();
    void queueFinished();                                             // host runs scrobble-stop / next-episode
    void resumeSaved();                                                // host schedules the cloud progress push

private:
    QSettings& store();
    QString titleAt(int index) const;

    QStringList tracks_;           // current audio queue (absolute paths)
    TrackHeaders trackHeaders_;    // per-track request headers, parallel to tracks_ (usually empty)
    int trackIndex_ = -1;          // index into tracks_, or -1 when not playing a queue
    QString resumePath_;           // the timed-media file (video/audio/audiobook) whose position we track, or empty
    double resumeSeek_ = 0.0;      // pending resume target applied once the file's duration is known
    double audioPos_ = 0.0;        // last reported playback position
    double lastSavedPos_ = -100.0; // throttle resume writes
    double lastAccruedPos_ = 0.0;  // consumption-stats: position through which watch/listen seconds were accrued
    double statsAccum_ = 0.0;      // consumption-stats: sub-second remainder carried between heartbeats (no drift)
    bool   mediaIsVideo_ = true;   // consumption-stats: the loaded file's kind (set by the host from mpv fileLoaded)
    double duration_ = 0.0;        // last reported duration (for the "dur" progress hint)
    QString settingsFile_;
    QSettings* settings_ = nullptr;
};
