// libmpv embedded in a Qt OpenGL surface (mpv "render API"). Plays everything mpv/ffmpeg can decode
// (MKV/HEVC/AV1/AC3/DTS/...), streams large files, hardware-decoded - in a native window, no engine bridge.
// Structure follows the canonical libmpv `qt_opengl` example.
#pragma once
#include <QString>
#include <QVector>
#include "../core/MediaSegments.h"   // MediaSegments::Chapter — declared in core so it needs no libmpv
#include "../core/StreamHeaders.h"   // per-stream proxyHeaders (QtCore-only, so it costs the probes nothing)
#include <mpv/client.h>
#ifdef Q_OS_IOS
// iOS: OpenGL ES context creation fails in the simulator (and EAGL is deprecated on device), so render
// through mpv's SOFTWARE render API into a QImage instead — same approach as theme2/MpvPreview. The
// public API is identical; only the frame path differs.
#include <QWidget>
#include <QImage>
#include <mpv/render.h>
using MpvWidgetBase = QWidget;
#else
#include <QOpenGLWidget>
#include <mpv/render_gl.h>
using MpvWidgetBase = QOpenGLWidget;
#endif

class QLabel;
class QTimer;

class MpvWidget : public MpvWidgetBase
{
    Q_OBJECT
public:
    explicit MpvWidget(QWidget* parent = nullptr);
    ~MpvWidget() override;

    // local path or http(s)/stream URL. `headers` is this stream's behaviorHints.proxyHeaders.request; it is
    // applied per-load and, being applied unconditionally, CLEARS the previous stream's headers when empty —
    // callers never have to remember to reset anything (and a caller that passes nothing gets a clean load).
    void play(const QString& url, const StreamHeaders::Headers& headers = {});
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
    // The current file's chapters, with their titles — the raw material for chapter-derived skip segments.
    // (nextChapter()/prevChapter() below are relative jumps and cannot answer "is chapter 2 called Intro?".)
    QVector<MediaSegments::Chapter> chapters() const;
    // Container frame rate, 0 when unknown. Only needed to convert a Kodi .edl's "#<frame>" time form.
    double fps() const;
    QVector<Track> subtitleTracks() const;    // sub tracks in the current file (empty if none)
    QVector<Track> audioTracks() const;       // audio tracks in the current file (empty if none)
    void setSubtitleTrack(int id);            // select subtitle track id; id < 0 turns subtitles off
    void setAudioTrack(int id);               // select audio track id; id < 0 disables audio
    double subtitleDelay() const;             // current subtitle timing offset, seconds
    void setSubtitleDelay(double seconds);
    double audioDelay() const;                // current audio timing offset, seconds (mpv "audio-delay"); 0.0 when no mpv
    void setAudioDelay(double seconds);
    double subtitleScale() const;             // current subtitle size multiplier (1.0 = default)
    void setSubtitleScale(double factor);
    // Apply the user's subtitle-appearance preferences (font/size/colour/outline/box/position/bold + the
    // ASS-override gate) from Settings to mpv (issue #71). Called once at player creation and again, live,
    // whenever a Subtitles setting changes — every option is runtime-settable, so a change is visible on the
    // currently-playing sub at once. See SubtitleStyle::toMpvOptions for the pure mapping.
    void applySubtitleStyle();
    // Apply the user's audio-output preferences (device / passthrough / exclusive mode) from Settings to mpv
    // (issue #69). Called once at player creation and again, live, whenever an Audio setting changes. The device
    // change takes effect at once; passthrough and exclusive mode reconfigure the AO, so they take full effect on
    // the next audio (re)init. See AudioOutput::toMpvOptions for the pure mapping.
    void applyAudioOutput();
    // Apply refresh-rate matching Tier 1 (issue #70): set mpv's `video-sync` from the "Reduce judder" toggle so
    // video locks to the display clock (display-resync) or falls back to mpv's audio-clock default. Called once
    // at player creation and again, live, whenever the toggle changes. Inert for audio-only playback (mpv keeps
    // the audio clock when there is no video track). See RefreshSync::videoSyncFor for the pure mapping.
    void applyRefreshSync();
    // One selectable audio output, from mpv's `audio-device-list` property. `name` is the id stored in Settings
    // and set as `audio-device`; `description` is the human label shown in the picker.
    struct AudioDevice { QString name; QString description; };
    // The audio outputs mpv can see on this machine, for the Settings device picker. Read live from this
    // player's initialised mpv context (the list is a system property, so any context answers it); empty on any
    // failure — the picker always prepends its own "Auto" entry regardless.
    QVector<AudioDevice> availableAudioDevices() const;
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
#ifdef Q_OS_IOS
    void paintEvent(QPaintEvent*) override;
#else
    void initializeGL() override;
    void paintGL() override;
#endif
    void resizeEvent(QResizeEvent*) override;

private slots:
    void maybeUpdate();   // a frame is ready -> repaint
    void onMpvEvents();   // drain mpv's event queue on the GUI thread
    void refreshNowPlaying(); // show/hide the audio-only "now playing" overlay

private:
    static void onMpvRedraw(void* ctx);                       // render-update callback (any thread)
    static void onMpvWakeup(void* ctx);                       // event wakeup callback (any thread)
#ifndef Q_OS_IOS
    static void* getProcAddress(void* ctx, const char* name); // GL loader for mpv
#endif
    void handleEvent(mpv_event* event);
    void logVideoInfo(); // append the loaded video's codec/resolution/pixfmt/hwdec to the debug log

    mpv_handle* mpv = nullptr;
    mpv_render_context* mpv_gl = nullptr; // GL render context (desktop) / SW render context (iOS)
#ifdef Q_OS_IOS
    void renderSoftwareFrame(); // drain a ready frame into frame_ and schedule a repaint
    QImage frame_;
#endif

    // "Now playing" overlay shown for audio-only files (no video track) so they aren't a black screen.
    QLabel* nowPlaying_ = nullptr;
    QTimer* npTimer_ = nullptr;   // brief delay after load before deciding audio-vs-video (avoids a flash)
    QString mediaTitle_;
    bool hasVideo_ = false;
};
