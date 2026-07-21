// libmpv embedded in a Qt OpenGL surface (mpv "render API"). Plays everything mpv/ffmpeg can decode
// (MKV/HEVC/AV1/AC3/DTS/...), streams large files, hardware-decoded - in a native window, no engine bridge.
// Structure follows the canonical libmpv `qt_opengl` example.
#pragma once
#include <QOpenGLWidget>
#include <QString>
#include <QVector>
#include <mpv/client.h>
#include <mpv/render_gl.h>

class QLabel;
class QTimer;

class MpvWidget : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit MpvWidget(QWidget* parent = nullptr);
    ~MpvWidget() override;

    void play(const QString& url);   // local path or http(s)/stream URL
    void stop();
    void setPaused(bool paused);
    bool isPaused() const;   // current mpv "pause" flag (OS-lifecycle pause query)
    void togglePause();
    void seekRelative(double seconds);
    void setPosition(double seconds);
    void cycleSubtitle();                     // step through subtitle tracks (… -> off -> 1 -> 2 -> …)
    void addSubtitle(const QString& path);    // load + select an external subtitle file (.srt/.ass/…)
    void takeScreenshot(const QString& path); // save the current frame (with subtitles) to a PNG file

    // One audio or subtitle track in the current file, for building a picker menu.
    struct Track { int id = 0; QString title; QString lang; bool selected = false; };
    QVector<Track> subtitleTracks() const;    // sub tracks in the current file (empty if none)
    QVector<Track> audioTracks() const;       // audio tracks in the current file (empty if none)
    void setSubtitleTrack(int id);            // select subtitle track id; id < 0 turns subtitles off
    void setAudioTrack(int id);               // select audio track id; id < 0 disables audio
    double subtitleDelay() const;             // current subtitle timing offset, seconds
    void setSubtitleDelay(double seconds);
    double subtitleScale() const;             // current subtitle size multiplier (1.0 = default)
    void setSubtitleScale(double factor);
    void nextChapter();                       // jump to the next chapter (M4B audiobooks, chaptered videos)
    void prevChapter();                       // jump to the previous chapter
    void setVolume(int percent);              // 0..200 (boost above 100%); 100 = original level
    void setMuted(bool muted);
    void setSpeed(double factor);             // playback rate (1.0 = normal); pitch-corrected by mpv
    double speed() const;                     // current playback rate

signals:
    void durationChanged(double seconds);
    void positionChanged(double seconds);
    void endReached();
    void chapterCountChanged(int count);      // how many chapters the current file has (0 = none)
    // Fired once the file's tracks are known. hasUsableSubtitle is true when it already carries a subtitle
    // track in the preferred language (or any, if no preference) — so a listener can auto-fetch one only when
    // it's false. isVideo distinguishes a real video (worth subtitling) from an audio-only file.
    void fileLoaded(bool hasUsableSubtitle, bool isVideo);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent*) override;

private slots:
    void maybeUpdate();   // a frame is ready -> repaint
    void onMpvEvents();   // drain mpv's event queue on the GUI thread
    void refreshNowPlaying(); // show/hide the audio-only "now playing" overlay

private:
    static void onMpvRedraw(void* ctx);                       // render-update callback (any thread)
    static void onMpvWakeup(void* ctx);                       // event wakeup callback (any thread)
    static void* getProcAddress(void* ctx, const char* name); // GL loader for mpv
    void handleEvent(mpv_event* event);
    void logVideoInfo(); // append the loaded video's codec/resolution/pixfmt/hwdec to the debug log

    mpv_handle* mpv = nullptr;
    mpv_render_context* mpv_gl = nullptr;

    // "Now playing" overlay shown for audio-only files (no video track) so they aren't a black screen.
    QLabel* nowPlaying_ = nullptr;
    QTimer* npTimer_ = nullptr;   // brief delay after load before deciding audio-vs-video (avoids a flash)
    QString mediaTitle_;
    bool hasVideo_ = false;
};
