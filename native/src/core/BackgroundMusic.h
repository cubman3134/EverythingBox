// RetroBat-style menu background music: plays audio files dropped in <data>/music while the user is browsing
// menus, and pauses whenever a game / video / reader is on screen so it never fights the content's own audio.
// Tracks loop in a shuffled order. Volume + on/off come from Settings (bgm/*).
//
// Decoding goes through libmpv in audio-only mode (vid=no), not QMediaPlayer: mpv/ffmpeg robustly plays the
// odd files a menu-music folder collects — e.g. YouTube-ripped MP3s with an embedded cover-art image, which
// QMediaPlayer's backend would play silently.
#pragma once
#include <QObject>
#include <QStringList>

struct mpv_handle;

class BackgroundMusic : public QObject
{
    Q_OBJECT
public:
    explicit BackgroundMusic(QObject* parent = nullptr);
    ~BackgroundMusic() override;

    static QString musicDir();      // <data>/music, created (with a README) on first use

    void reload();                  // rescan the folder + reshuffle (doesn't interrupt the current track)
    void setActive(bool on);        // true on a menu screen, false on content -> play / pause
    void setEnabled(bool on);       // master on/off (the Settings toggle)
    void setVolume(int pct);        // 0..100
    bool hasTracks() const { return !tracks_.isEmpty(); }
    QString currentTitle() const { return title_; } // the playing track's name (empty if none)

signals:
    void nowPlayingChanged(const QString& title); // the track changed (or cleared)

private:
    void applyState();              // play or pause based on enabled_ && active_ && hasTracks()
    void playIndex(int i);
    void setPaused(bool paused);
    Q_INVOKABLE void onMpvEvents(); // drain mpv events on the GUI thread (queued from the wakeup callback)
    static void onWakeup(void* ctx);

    mpv_handle* mpv_ = nullptr;
    QStringList tracks_;
    QString title_;                 // display name of the current track (for the "now playing" readout)
    int idx_ = -1;
    bool active_ = false;
    bool enabled_ = true;
    bool loaded_ = false;           // a track has been handed to mpv (so applyState resumes vs. starts fresh)
};
