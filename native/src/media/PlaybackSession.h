#pragma once
#include <QObject>
#include <QSettings>
#include <QStringList>

// The audio-queue + resume state machine: owns the current track list/position, drives next/prev/
// track-end advance, and persists "where you left off" for any timed media (video/audio/audiobook),
// keyed by a stable path/id. The host (MainWindow) owns the actual player and playlist widget; this
// class only decides WHAT plays next and WHERE to resume, telling the host via signals.
class PlaybackSession : public QObject
{
    Q_OBJECT
public:
    // Empty settingsFile = the app's own store (AppPaths::dataDir()/mymediavault.ini); probes pass a
    // scratch path so tests never touch the real user store.
    explicit PlaybackSession(const QString& settingsFile = QString(), QObject* parent = nullptr);

    // resumeKey empty (default) = the starting track resumes keyed by its own file path (current behavior).
    // Non-empty = the first-played track is resume-keyed by `resumeKey` instead (a stable catalog/audiobook
    // id), folded in atomically so callers no longer re-key with a separate beginResume() after setQueue.
    void setQueue(const QStringList& files, int startIndex, const QStringList& titles = {},
                  const QString& resumeKey = QString());
    void playIndex(int index);
    void next();
    void prev();
    void handleTrackEnd(); // advances the queue, or emits queueFinished() at the last track
    void clearQueue();     // persists then resets (was clearAudioQueue minus the widget lines)

    void beginResume(const QString& pathOrKey); // start tracking this file/key (and queue its saved spot)
    void persistResume();                       // save the current position (throttled / on leave / on exit)
    void finishResume();                        // played to the end -> drop the saved position

    void setPosition(double s); // fed from the host's mpv position callback
    void setDuration(double s); // fed from the host's mpv duration callback

    double takeResumeSeek(); // returns the pending resume target once, then 0 (consumed by onDuration)

    int currentIndex() const { return trackIndex_; }
    int count() const { return tracks_.size(); }
    QString trackAt(int i) const { return tracks_.value(i); }
    double position() const { return audioPos_; }

signals:
    void playRequested(const QString& path);                          // host hands it to mpv
    void trackChanged(int index, int count, const QString& displayTitle);
    void queueChanged(const QStringList& titles, int current);        // host rebuilds playlist_
    void queueCleared();
    void queueFinished();                                             // host runs scrobble-stop / next-episode
    void resumeSaved();                                                // host schedules the cloud progress push

private:
    QSettings& store();
    QString titleAt(int index) const;

    QStringList tracks_;           // current audio queue (absolute paths)
    int trackIndex_ = -1;          // index into tracks_, or -1 when not playing a queue
    QString resumePath_;           // the timed-media file (video/audio/audiobook) whose position we track, or empty
    double resumeSeek_ = 0.0;      // pending resume target applied once the file's duration is known
    double audioPos_ = 0.0;        // last reported playback position
    double lastSavedPos_ = -100.0; // throttle resume writes
    double duration_ = 0.0;        // last reported duration (for the "dur" progress hint)
    QString settingsFile_;
    QSettings* settings_ = nullptr;
};
