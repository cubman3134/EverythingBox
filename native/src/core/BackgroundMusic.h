// RetroBat-style menu background music: plays audio files dropped in <data>/music while the user is browsing
// menus, and pauses whenever a game / video / reader is on screen so it never fights the content's own audio.
// Tracks loop in a shuffled order. Volume + on/off come from Settings (bgm/*).
#pragma once
#include <QObject>
#include <QStringList>

class QMediaPlayer;
class QAudioOutput;

class BackgroundMusic : public QObject
{
    Q_OBJECT
public:
    explicit BackgroundMusic(QObject* parent = nullptr);

    static QString musicDir();      // <data>/music, created (with a README) on first use

    void reload();                  // rescan the folder + reshuffle (doesn't interrupt the current track)
    void setActive(bool on);        // true on a menu screen, false on content -> play / pause
    void setEnabled(bool on);       // master on/off (the Settings toggle)
    void setVolume(int pct);        // 0..100
    bool hasTracks() const { return !tracks_.isEmpty(); }

private:
    void applyState();              // play or pause based on enabled_ && active_ && hasTracks()
    void playIndex(int i);

    QMediaPlayer* player_ = nullptr;
    QAudioOutput* out_ = nullptr;
    QStringList tracks_;
    int idx_ = -1;
    bool active_ = false;
    bool enabled_ = true;
};
