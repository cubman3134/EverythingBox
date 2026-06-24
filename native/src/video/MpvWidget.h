// libmpv embedded in a Qt OpenGL surface (mpv "render API"). Plays everything mpv/ffmpeg can decode
// (MKV/HEVC/AV1/AC3/DTS/...), streams large files, hardware-decoded - in a native window, no engine bridge.
// Structure follows the canonical libmpv `qt_opengl` example.
#pragma once
#include <QOpenGLWidget>
#include <QString>
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
    void togglePause();
    void seekRelative(double seconds);
    void setPosition(double seconds);
    void cycleSubtitle();                     // step through subtitle tracks (… -> off -> 1 -> 2 -> …)
    void addSubtitle(const QString& path);    // load + select an external subtitle file (.srt/.ass/…)
    void nextChapter();                       // jump to the next chapter (M4B audiobooks, chaptered videos)
    void prevChapter();                       // jump to the previous chapter

signals:
    void durationChanged(double seconds);
    void positionChanged(double seconds);
    void endReached();
    void chapterCountChanged(int count);      // how many chapters the current file has (0 = none)

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

    mpv_handle* mpv = nullptr;
    mpv_render_context* mpv_gl = nullptr;

    // "Now playing" overlay shown for audio-only files (no video track) so they aren't a black screen.
    QLabel* nowPlaying_ = nullptr;
    QTimer* npTimer_ = nullptr;   // brief delay after load before deciding audio-vs-video (avoids a flash)
    QString mediaTitle_;
    bool hasVideo_ = false;
};
