#include "MpvWidget.h"
#include "MpvHeaderApply.h"
#include "HwDecode.h"
#include "RefreshSync.h"
#include "AudioOutput.h"
#include "HdrOutput.h"
#include "../core/AppPaths.h"
#include "../core/Settings.h"
#ifndef Q_OS_IOS
#include <QOpenGLContext>
#else
#include <QPainter>
#include <QPaintEvent>
#endif
#include <QMetaObject>
#include <QLabel>
#include <QTimer>
#include <QResizeEvent>
#include <QFile>
#include <QCoreApplication>
#include <QDateTime>
#include <stdexcept>
#include <cstring>
#include <cstdio>

// One-line append to <app>/stream_debug.log, shared with the addon stream/manga tracing.
static void videoLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
}

MpvWidget::MpvWidget(QWidget* parent) : MpvWidgetBase(parent)
{
    mpv = mpv_create();
    if (!mpv)
        throw std::runtime_error("could not create mpv context");

    // Render video THROUGH the libmpv render API (our QOpenGLWidget) instead of letting mpv open its own
    // window. This must be set before mpv_initialize - without it mpv uses the default 'gpu' output and
    // pops a separate window.
    mpv_set_option_string(mpv, "vo", "libmpv");
    // Hardware decode is now a per-machine Setting (issue #67), read once here at player creation. The old
    // hard-coded "no" existed because this machine's D3D11VA corrupts 10-bit HEVC (p010) even in copy mode;
    // the default (Auto -> "auto-safe") sidesteps that class by preferring copy-back and falling back to
    // software on an unsupported profile, rather than re-opening it. Off keeps "no", On is full "auto", and
    // the iOS software-render path is forced to "no" regardless (see HwDecode::mpvOption).
    const QByteArray hwdecOpt = HwDecode::mpvOption(Settings::hwDecode(), HwDecode::currentPlatform()).toUtf8();
    mpv_set_option_string(mpv, "hwdec", hwdecOpt.constData());
    videoLog(QStringLiteral("mpv: hwdec requested='") + QString::fromUtf8(hwdecOpt)
             + QStringLiteral("' (setting=") + Settings::hwDecode() + QStringLiteral(")"));
    // Network/debrid streams arrive in bursts and at high bitrate; buffer generously so playback doesn't
    // stutter. A big forward demuxer cache + reading well ahead smooths over the source's pacing, and on an
    // underrun we wait until a couple of seconds are buffered before resuming (rather than stutter-resuming).
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "demuxer-max-bytes", "512MiB");
    mpv_set_option_string(mpv, "demuxer-max-back-bytes", "128MiB");
    mpv_set_option_string(mpv, "cache-secs", "120");
    mpv_set_option_string(mpv, "cache-pause-wait", "2");
    mpv_set_option_string(mpv, "network-timeout", "60");
    // Allow software amplification above 100% (VLC-style "boost"). mpv defaults volume-max to 130; raise it
    // to 200 so the volume slider can push a quiet source louder than its original level.
    mpv_set_option_string(mpv, "volume-max", "200");
    // Subtitles: embedded tracks are auto-selected by mpv and rendered into our FBO (subs + OSD composite
    // through the render API). "sub-auto=fuzzy" also pulls in sidecar files (movie.srt, movie.eng.srt, …)
    // sitting next to the video, not just exact-name matches.
    mpv_set_option_string(mpv, "sub-auto", "fuzzy");
    // mpv parses numbers with the C locale; main() must also setlocale(LC_NUMERIC, "C").

    if (mpv_initialize(mpv) < 0)
        throw std::runtime_error("could not initialize mpv");

    // Apply the user's subtitle-appearance preferences (issue #71) once the context is up. These are all
    // runtime-settable options, so the same call re-applies live from Settings whenever a Subtitles setting
    // changes (MainWindow calls applySubtitleStyle() on change).
    applySubtitleStyle();
    // Apply the user's audio-output preferences (issue #69): device, passthrough, exclusive mode. Set here
    // before the first file loads (and thus before mpv creates its AO), and re-applied live from Settings when
    // an Audio setting changes (MainWindow calls applyAudioOutput() on change).
    applyAudioOutput();
    // Apply refresh-rate matching Tier 1 (issue #70): video-sync=display-resync when the toggle is on, so video
    // locks to the display clock (mpv resamples audio) and 24fps-on-60Hz judder is smoothed. Set here after init
    // and re-applied live from Settings when the toggle changes (MainWindow calls applyRefreshSyncLive()). Inert
    // for audio-only playback (mpv keeps the audio clock when there is no video track), so it needs no gating.
    applyRefreshSync();
    // Apply HDR output handling (issue #68): tone-map HDR to SDR (default) so it stops washing out on an SDR
    // panel, or signal HDR10 to the swapchain when the display supports it (passthrough). Set here after init and
    // re-applied live from Settings when the setting changes (MainWindow calls applyHdrOutputLive()). Inert for
    // SDR content (mpv's tone-mapping only engages on an HDR transfer), so like refresh-sync it needs no gating.
    applyHdrOutput();

    // Observe playback state for the seek bar / end-of-file, plus title + video presence for the overlay.
    mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 0, "width", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "chapters", MPV_FORMAT_INT64); // chapter count -> show/hide chapter nav
    // Gapless (issue #141): mpv's index within its OWN playlist. Under gapless the app feeds the next track via
    // `loadfile append`, and mpv crosses into it without stopping — so the per-track boundary shows up here as a
    // playlist-pos change, not as an EOF. Observed unconditionally (a passive, cheap read): with gapless off the
    // app never builds a multi-entry mpv playlist, so this only ever reports 0 and the host ignores it.
    mpv_observe_property(mpv, 0, "playlist-pos", MPV_FORMAT_INT64);

    mpv_set_wakeup_callback(mpv, onMpvWakeup, this);

#ifdef Q_OS_IOS
    // Software render context, created up-front (there is no initializeGL on the QWidget path).
    mpv_render_param rparams[]{
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW) },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_create(&mpv_gl, mpv, rparams) < 0)
        throw std::runtime_error("failed to initialize mpv software render context");
    mpv_render_context_set_update_callback(mpv_gl, onMpvRedraw, this);
#endif

    // Audio-only "now playing" overlay (a child widget over the GL surface).
    nowPlaying_ = new QLabel(this);
    nowPlaying_->setAlignment(Qt::AlignCenter);
    nowPlaying_->setWordWrap(true);
    nowPlaying_->setAttribute(Qt::WA_TransparentForMouseEvents);
    nowPlaying_->setStyleSheet(QStringLiteral("color:#e8e8e8; font-size:22px; background:transparent;"));
    nowPlaying_->hide();

    npTimer_ = new QTimer(this);
    npTimer_->setSingleShot(true);
    connect(npTimer_, &QTimer::timeout, this, &MpvWidget::refreshNowPlaying);
}

MpvWidget::~MpvWidget()
{
#ifndef Q_OS_IOS
    makeCurrent();
#endif
    if (mpv_gl)
        mpv_render_context_free(mpv_gl);
    if (mpv)
        mpv_terminate_destroy(mpv);
}

#ifdef Q_OS_IOS

void MpvWidget::renderSoftwareFrame()
{
    if (!mpv_gl) return;
    const uint64_t flags = mpv_render_context_update(mpv_gl);
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) return;
    const int w = qMax(1, int(width() * devicePixelRatioF()));
    const int h = qMax(1, int(height() * devicePixelRatioF()));
    if (frame_.width() != w || frame_.height() != h)
        frame_ = QImage(w, h, QImage::Format_RGB32); // opaque; memory order matches mpv "bgr0"
    int size[2]{ w, h };
    size_t stride = size_t(frame_.bytesPerLine());
    void* ptr = frame_.bits();
    mpv_render_param params[]{
        { MPV_RENDER_PARAM_SW_SIZE, size },
        { MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>("bgr0") },
        { MPV_RENDER_PARAM_SW_STRIDE, &stride },
        { MPV_RENDER_PARAM_SW_POINTER, ptr },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_render(mpv_gl, params) >= 0)
        update();
}

void MpvWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (frame_.isNull()) return;
    frame_.setDevicePixelRatio(devicePixelRatioF());
    p.drawImage(rect(), frame_);
}

#else

void* MpvWidget::getProcAddress(void* ctx, const char* name)
{
    Q_UNUSED(ctx);
    QOpenGLContext* glctx = QOpenGLContext::currentContext();
    if (!glctx)
        return nullptr;
    return reinterpret_cast<void*>(glctx->getProcAddress(QByteArray(name)));
}

void MpvWidget::initializeGL()
{
    mpv_opengl_init_params gl_init_params{ getProcAddress, this };
    mpv_render_param params[]{
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0)
        throw std::runtime_error("failed to initialize mpv GL render context");
    mpv_render_context_set_update_callback(mpv_gl, onMpvRedraw, this);
}

void MpvWidget::paintGL()
{
    if (!mpv_gl)
        return;
    mpv_opengl_fbo mpfbo{ static_cast<int>(defaultFramebufferObject()),
                          static_cast<int>(width() * devicePixelRatioF()),
                          static_cast<int>(height() * devicePixelRatioF()), 0 };
    int flip_y{ 1 };
    mpv_render_param params[]{
        { MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo },
        { MPV_RENDER_PARAM_FLIP_Y, &flip_y },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    mpv_render_context_render(mpv_gl, params);
}

#endif // Q_OS_IOS

void MpvWidget::onMpvRedraw(void* ctx)
{
    QMetaObject::invokeMethod(static_cast<MpvWidget*>(ctx), "maybeUpdate", Qt::QueuedConnection);
}

void MpvWidget::onMpvWakeup(void* ctx)
{
    QMetaObject::invokeMethod(static_cast<MpvWidget*>(ctx), "onMpvEvents", Qt::QueuedConnection);
}

void MpvWidget::maybeUpdate()
{
#ifdef Q_OS_IOS
    // Software path: drain the ready frame into frame_ (which schedules the repaint itself). This also
    // keeps mpv's frame queue draining while occluded — no GL context to juggle.
    renderSoftwareFrame();
#else
    // If the window is minimized, render off-screen so mpv's frame queue keeps draining.
    if (window()->isMinimized())
    {
        makeCurrent();
        paintGL();
        context()->swapBuffers(context()->surface());
        doneCurrent();
    }
    else
    {
        update();
    }
#endif
}

void MpvWidget::onMpvEvents()
{
    while (mpv)
    {
        mpv_event* event = mpv_wait_event(mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        handleEvent(event);
    }
}

void MpvWidget::handleEvent(mpv_event* event)
{
    switch (event->event_id)
    {
    case MPV_EVENT_PROPERTY_CHANGE:
    {
        auto* prop = static_cast<mpv_event_property*>(event->data);
        if (prop->data == nullptr)
            break;
        if (prop->format == MPV_FORMAT_DOUBLE)
        {
            double v = *static_cast<double*>(prop->data);
            if (std::strcmp(prop->name, "time-pos") == 0)
                emit positionChanged(v);
            else if (std::strcmp(prop->name, "duration") == 0)
                emit durationChanged(v);
        }
        // (eof is signalled via MPV_EVENT_END_FILE below, which carries the reason - more reliable than
        //  the eof-reached flag, which also trips on manual stop/seek.)
        else if (prop->format == MPV_FORMAT_INT64)
        {
            if (std::strcmp(prop->name, "width") == 0 && *static_cast<int64_t*>(prop->data) > 0)
            {
                const bool firstFrame = !hasVideo_;
                hasVideo_ = true;       // a real video track -> no overlay
                if (nowPlaying_) nowPlaying_->hide();
                if (firstFrame) QTimer::singleShot(700, this, [this] { logVideoInfo(); }); // let hwdec settle
            }
            else if (std::strcmp(prop->name, "chapters") == 0)
            {
                emit chapterCountChanged(static_cast<int>(*static_cast<int64_t*>(prop->data)));
            }
            else if (std::strcmp(prop->name, "playlist-pos") == 0)
            {
                // #141: mpv moved within its own playlist. Reported for any value (including -1 at the end of
                // the list); the host acts on it only while gapless is armed. This is the boundary signal that
                // replaces the per-track EOF when the decoder runs continuously across a gapless transition.
                emit playlistPositionChanged(static_cast<int>(*static_cast<int64_t*>(prop->data)));
            }
        }
        else if (prop->format == MPV_FORMAT_STRING)
        {
            if (std::strcmp(prop->name, "media-title") == 0)
            {
                mediaTitle_ = QString::fromUtf8(*static_cast<char**>(prop->data));
                if (nowPlaying_ && nowPlaying_->isVisible()) refreshNowPlaying();
            }
        }
        break;
    }
    case MPV_EVENT_START_FILE:
        // New file: assume audio until a video track shows up; decide a beat later to avoid a flash.
        hasVideo_ = false;
        mediaTitle_.clear();
        if (nowPlaying_) nowPlaying_->hide();
        if (npTimer_) npTimer_->start(400);
        emit chapterCountChanged(0); // hide chapter nav until the new file reports its own count
        break;
    case MPV_EVENT_FILE_LOADED:
    {
        // The track list is now populated: report whether an embedded subtitle in the preferred language is
        // present, so the app can decide to auto-download one. Also report whether this is a video track (an
        // audio-only file never wants subtitles). mpv lang codes may be 2- or 3-letter; match on the first two.
        const QString want = Settings::subtitleLanguage().trimmed().toLower();
        int64_t count = 0;
        mpv_get_property(mpv, "track-list/count", MPV_FORMAT_INT64, &count);
        bool anySub = false, wantSub = false, video = false;
        for (int64_t i = 0; i < count; ++i)
        {
            char key[64];
            std::snprintf(key, sizeof key, "track-list/%lld/type", static_cast<long long>(i));
            char* ty = mpv_get_property_string(mpv, key);
            const QString type = ty ? QString::fromUtf8(ty) : QString();
            if (ty) mpv_free(ty);
            if (type == QStringLiteral("video")) { video = true; continue; }
            if (type != QStringLiteral("sub")) continue;
            anySub = true;
            std::snprintf(key, sizeof key, "track-list/%lld/lang", static_cast<long long>(i));
            char* lg = mpv_get_property_string(mpv, key);
            if (lg)
            {
                const QString l = QString::fromUtf8(lg).toLower();
                if (!want.isEmpty() && (l.left(2) == want.left(2))) wantSub = true;
                mpv_free(lg);
            }
        }
        const bool usable = want.isEmpty() ? anySub : wantSub;
        emit fileLoaded(usable, video || hasVideo_);
        break;
    }
    case MPV_EVENT_END_FILE:
    {
        if (nowPlaying_) nowPlaying_->hide();
        // Only a natural end-of-file should advance a playlist; stop/seek/redirect must not.
        auto* ef = static_cast<mpv_event_end_file*>(event->data);
        if (ef && ef->reason == MPV_END_FILE_REASON_EOF)
            emit endReached();
        break;
    }
    default:
        break;
    }
}

void MpvWidget::logVideoInfo()
{
    if (!mpv) return;
    auto getS = [this](const char* prop) {
        char* s = mpv_get_property_string(mpv, prop);
        QString r = s ? QString::fromUtf8(s) : QStringLiteral("?");
        if (s) mpv_free(s);
        return r;
    };
    // codec + container format, decoded geometry/pixel format, the hwdec that actually engaged, frame rate,
    // and the colour transfer/primaries (to spot HDR content being shown on an SDR path).
    videoLog(QStringLiteral("video: codec='") + getS("video-codec")
             + QStringLiteral("' fmt=") + getS("video-format")
             + QStringLiteral(" ") + getS("video-params/w") + QStringLiteral("x") + getS("video-params/h")
             + QStringLiteral(" pixfmt=") + getS("video-params/pixelformat")
             + QStringLiteral(" hwdec-current=") + getS("hwdec-current")
             + QStringLiteral(" fps=") + getS("container-fps")
             + QStringLiteral(" transfer=") + getS("video-params/gamma")
             + QStringLiteral(" primaries=") + getS("video-params/primaries")
             + QStringLiteral(" bitrate=") + getS("video-bitrate"));
}

void MpvWidget::resizeEvent(QResizeEvent* e)
{
    MpvWidgetBase::resizeEvent(e);
    if (nowPlaying_) nowPlaying_->setGeometry(rect());
}

void MpvWidget::refreshNowPlaying()
{
    if (!nowPlaying_) return;
    if (!hasVideo_ && !mediaTitle_.isEmpty())
    {
        nowPlaying_->setText(QStringLiteral("♪\n\n") + mediaTitle_); // ♪ + title
        nowPlaying_->setGeometry(rect());
        nowPlaying_->show();
        nowPlaying_->raise();
    }
    else
    {
        nowPlaying_->hide();
    }
}

void MpvWidget::play(const QString& url, const StreamHeaders::Headers& headers)
{
    // Per-stream HTTP headers (behaviorHints.proxyHeaders.request). UNCONDITIONAL, before the load:
    // MpvHeaderApply::apply writes all three properties every time, so a stream that needs none actively
    // clears whatever the previous one set. Nothing here logs a value — only how many and which names.
    MpvHeaderApply::apply(mpv, headers);
    if (!headers.isEmpty())
        videoLog(QStringLiteral("mpv: applying stream ") + StreamHeaders::logSummary(headers));

    // Apply the user's subtitle defaults before loading, so they take effect for this video (and changing
    // them in Settings applies to the next one). "subs-fallback=yes" makes mpv select a sub track even when
    // none is marked default; "slang" sets the preferred language so the right track is picked.
    const QString lang = Settings::subtitleLanguage().trimmed();
    if (!lang.isEmpty()) mpv_set_option_string(mpv, "slang", lang.toUtf8().constData());
    const bool subsOn = Settings::subtitlesOnByDefault();
    mpv_set_option_string(mpv, "subs-fallback", subsOn ? "yes" : "no");
    double normalSpeed = 1.0;
    mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &normalSpeed); // each new video starts at normal speed

    QByteArray u = url.toUtf8();
    const char* cmd[] = { "loadfile", u.constData(), nullptr };
    mpv_command_async(mpv, 0, cmd); // mpv copies the args
    setPaused(false);
}

void MpvWidget::appendFile(const QString& url, const StreamHeaders::Headers& headers)
{
    // #141: `loadfile <url> append` — add to mpv's playlist WITHOUT touching the current file, so mpv can flow
    // into it gaplessly when the current one ends. Unlike play(), this sets no per-file options and does not
    // unpause: it must not disturb what is already playing. Headers are applied for symmetry with play() but the
    // audio queue this is used for carries none (local files), so the apply clears to empty, matching the
    // currently-playing track's (also empty) — no cross-track header bleed.
    MpvHeaderApply::apply(mpv, headers);
    QByteArray u = url.toUtf8();
    const char* cmd[] = { "loadfile", u.constData(), "append", nullptr };
    mpv_command_async(mpv, 0, cmd); // mpv copies the args
}

void MpvWidget::setGaplessAudio(bool on)
{
    if (!mpv) return;
    // "weak" is the honest value the issue asks for: mpv keeps the audio output continuous only when adjacent
    // tracks share a format, and reinitialises (a correct, expected gap) when they truly differ — so a mixed
    // queue is never forced through a mismatched device config. Only ever called with `on` = true (the host
    // arms it when a gapless audio queue starts); the off path builds no mpv playlist and never calls this.
    mpv_set_option_string(mpv, "gapless-audio", on ? "weak" : "no");
    videoLog(QStringLiteral("mpv: gapless-audio='") + (on ? QStringLiteral("weak") : QStringLiteral("no"))
             + QStringLiteral("'"));
}

void MpvWidget::stop()
{
    const char* cmd[] = { "stop", nullptr };
    mpv_command_async(mpv, 0, cmd);
    if (npTimer_) npTimer_->stop();
    if (nowPlaying_) nowPlaying_->hide();
}

void MpvWidget::setPaused(bool paused)
{
    int flag = paused ? 1 : 0;
    mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

bool MpvWidget::isPaused() const
{
    if (!mpv) return false;
    int flag = 0;
    mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &flag);
    return flag != 0;
}

void MpvWidget::togglePause()
{
    const char* cmd[] = { "cycle", "pause", nullptr };
    mpv_command_async(mpv, 0, cmd);
}

void MpvWidget::setVolume(int percent)
{
    double v = percent < 0 ? 0.0 : (percent > 200 ? 200.0 : double(percent));
    mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

void MpvWidget::setMuted(bool muted)
{
    int flag = muted ? 1 : 0;
    mpv_set_property(mpv, "mute", MPV_FORMAT_FLAG, &flag);
}

void MpvWidget::setSpeed(double factor)
{
    mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &factor);
}

double MpvWidget::speed() const
{
    double s = 1.0;
    if (mpv) mpv_get_property(mpv, "speed", MPV_FORMAT_DOUBLE, &s);
    return s;
}

void MpvWidget::seekRelative(double seconds)
{
    QByteArray s = QByteArray::number(seconds);
    const char* cmd[] = { "seek", s.constData(), "relative", nullptr };
    mpv_command_async(mpv, 0, cmd);
}

void MpvWidget::setPosition(double seconds)
{
    mpv_set_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &seconds);
}

void MpvWidget::nextChapter()
{
    const char* cmd[] = { "add", "chapter", "1", nullptr };
    mpv_command_async(mpv, 0, cmd);
}

void MpvWidget::prevChapter()
{
    // "add chapter -1" from just-past a boundary snaps to the chapter start (the usual "skip back" behaviour).
    const char* cmd[] = { "add", "chapter", "-1", nullptr };
    mpv_command_async(mpv, 0, cmd);
}

void MpvWidget::cycleSubtitle()
{
    // Cycles sid through each subtitle track and "no" (off). mpv shows an OSD label of the new track.
    const char* cmd[] = { "cycle", "sid", nullptr };
    mpv_command_async(mpv, 0, cmd);
}

void MpvWidget::addSubtitle(const QString& path)
{
    QByteArray p = path.toUtf8();
    const char* cmd[] = { "sub-add", p.constData(), "select", nullptr }; // load and switch to it
    mpv_command_async(mpv, 0, cmd); // mpv copies the args
}

void MpvWidget::takeScreenshot(const QString& path)
{
    QByteArray p = path.toUtf8();
    // "subtitles" = the video frame with rendered subtitles, but without the player's OSD/controls.
    const char* cmd[] = { "screenshot-to-file", p.constData(), "subtitles", nullptr };
    mpv_command_async(mpv, 0, cmd); // mpv copies the args
}

// Enumerate the current file's tracks of one type ("sub" or "audio") from mpv's track-list.
static QVector<MpvWidget::Track> tracksOfType(mpv_handle* mpv, const char* wantType)
{
    QVector<MpvWidget::Track> out;
    if (!mpv) return out;
    int64_t count = 0;
    mpv_get_property(mpv, "track-list/count", MPV_FORMAT_INT64, &count);
    for (int64_t i = 0; i < count; ++i)
    {
        char key[80];
        auto field = [&](const char* name) {
            std::snprintf(key, sizeof key, "track-list/%lld/%s", static_cast<long long>(i), name);
            return key;
        };
        char* ty = mpv_get_property_string(mpv, field("type"));
        const bool match = ty && std::strcmp(ty, wantType) == 0;
        if (ty) mpv_free(ty);
        if (!match) continue;

        MpvWidget::Track t;
        int64_t id = 0;
        mpv_get_property(mpv, field("id"), MPV_FORMAT_INT64, &id);
        t.id = static_cast<int>(id);
        char* lg = mpv_get_property_string(mpv, field("lang"));
        if (lg) { t.lang = QString::fromUtf8(lg); mpv_free(lg); }
        char* ti = mpv_get_property_string(mpv, field("title"));
        if (ti) { t.title = QString::fromUtf8(ti); mpv_free(ti); }
        int sel = 0;
        mpv_get_property(mpv, field("selected"), MPV_FORMAT_FLAG, &sel);
        t.selected = sel != 0;
        out.push_back(t);
    }
    return out;
}

QVector<MpvWidget::Track> MpvWidget::subtitleTracks() const { return tracksOfType(mpv, "sub"); }
QVector<MpvWidget::Track> MpvWidget::audioTracks() const { return tracksOfType(mpv, "audio"); }

// The current file's chapters WITH their titles — chapterCountChanged only ever carried the count, which
// cannot tell a caller that chapter 2 is called "Opening Credits". Indexed-property reads, exactly like
// tracksOfType above.
QVector<MediaSegments::Chapter> MpvWidget::chapters() const
{
    QVector<MediaSegments::Chapter> out;
    if (!mpv) return out;
    int64_t count = 0;
    mpv_get_property(mpv, "chapter-list/count", MPV_FORMAT_INT64, &count);
    for (int64_t i = 0; i < count; ++i)
    {
        char key[80];
        auto field = [&](const char* name) {
            std::snprintf(key, sizeof key, "chapter-list/%lld/%s", static_cast<long long>(i), name);
            return key;
        };
        MediaSegments::Chapter c;
        mpv_get_property(mpv, field("time"), MPV_FORMAT_DOUBLE, &c.time);
        char* ti = mpv_get_property_string(mpv, field("title"));
        if (ti) { c.title = QString::fromUtf8(ti); mpv_free(ti); }
        out.push_back(c);
    }
    return out;
}

double MpvWidget::fps() const
{
    if (!mpv) return 0.0;
    double f = 0.0;
    mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &f);
    return f > 0.0 ? f : 0.0;
}

void MpvWidget::setSubtitleTrack(int id)
{
    if (!mpv) return;
    if (id < 0) { mpv_set_property_string(mpv, "sid", "no"); return; }
    const QByteArray v = QByteArray::number(id);
    mpv_set_property_string(mpv, "sid", v.constData());
}

void MpvWidget::setAudioTrack(int id)
{
    if (!mpv) return;
    if (id < 0) { mpv_set_property_string(mpv, "aid", "no"); return; }
    const QByteArray v = QByteArray::number(id);
    mpv_set_property_string(mpv, "aid", v.constData());
}

double MpvWidget::subtitleDelay() const
{
    double d = 0.0;
    if (mpv) mpv_get_property(mpv, "sub-delay", MPV_FORMAT_DOUBLE, &d);
    return d;
}

void MpvWidget::setSubtitleDelay(double seconds)
{
    if (mpv) mpv_set_property(mpv, "sub-delay", MPV_FORMAT_DOUBLE, &seconds);
}

double MpvWidget::audioDelay() const
{
    double d = 0.0;
    if (mpv) mpv_get_property(mpv, "audio-delay", MPV_FORMAT_DOUBLE, &d);
    return d;
}

void MpvWidget::setAudioDelay(double seconds)
{
    if (mpv) mpv_set_property(mpv, "audio-delay", MPV_FORMAT_DOUBLE, &seconds);
}

double MpvWidget::subtitleScale() const
{
    double f = 1.0;
    if (mpv) mpv_get_property(mpv, "sub-scale", MPV_FORMAT_DOUBLE, &f);
    return f;
}

void MpvWidget::setSubtitleScale(double factor)
{
    if (mpv) mpv_set_property(mpv, "sub-scale", MPV_FORMAT_DOUBLE, &factor);
}

void MpvWidget::applySubtitleStyle()
{
    if (!mpv) return;
    // The pure map owns every decision (the box-alpha encoding, the ASS-override gate, the mpv-default
    // fallbacks); here we only push each (name, value) onto the mpv instance. UNCONDITIONAL, like the
    // per-stream header apply: every option is written every time, so turning the box off or clearing a font
    // actively resets it rather than leaving the previous value set on the context.
    const QVector<QPair<QString, QString>> opts = SubtitleStyle::toMpvOptions(Settings::subtitleStyle());
    for (const auto& o : opts)
        mpv_set_option_string(mpv, o.first.toUtf8().constData(), o.second.toUtf8().constData());
}

void MpvWidget::applyRefreshSync()
{
    if (!mpv) return;
    // The pure map (RefreshSync::videoSyncFor) owns the decision — the iOS force-off and the off->default reset.
    // Here we only push the one option. UNCONDITIONAL like the subtitle/audio applies: written every time so
    // turning the toggle off actively clears video-sync back to mpv's "audio" default rather than leaving
    // "display-resync" set on the context. Settings::videoRefreshSync() already resolves the form-factor default.
    const QByteArray vs = RefreshSync::videoSyncFor(Settings::videoRefreshSync(),
                                                    RefreshSync::currentPlatform()).toUtf8();
    mpv_set_option_string(mpv, "video-sync", vs.constData());
    videoLog(QStringLiteral("mpv: video-sync='") + QString::fromUtf8(vs)
             + QStringLiteral("' (refreshSync=") + (Settings::videoRefreshSync() ? QStringLiteral("on") : QStringLiteral("off"))
             + QStringLiteral(")"));
}

void MpvWidget::applyAudioOutput()
{
    if (!mpv) return;
    // The pure map owns every decision (Auto -> "auto", the passthrough codec list, the empty-when-off reset).
    // Here we only push each (name, value) onto the mpv instance. UNCONDITIONAL, like the subtitle apply: every
    // option is written every time, so turning passthrough or exclusive mode off actively resets it rather than
    // leaving the previous value set on the context. The device change is live; passthrough/exclusive take full
    // effect on the next AO (re)init.
    const QVector<QPair<QString, QString>> opts = AudioOutput::toMpvOptions(Settings::audioOutput());
    for (const auto& o : opts)
        mpv_set_option_string(mpv, o.first.toUtf8().constData(), o.second.toUtf8().constData());
    videoLog(QStringLiteral("mpv: audio-device='") + Settings::audioDevice()
             + QStringLiteral("' passthrough=") + (Settings::audioPassthrough() ? QStringLiteral("on") : QStringLiteral("off"))
             + QStringLiteral(" exclusive=") + (Settings::audioExclusive() ? QStringLiteral("on") : QStringLiteral("off")));
}

void MpvWidget::applyHdrOutput()
{
    if (!mpv) return;
    // The pure map (HdrOutput::optionsFor) owns every decision — the iOS force-to-tone-map and the reset of the
    // other mode's key to mpv's default. Here we only push each (name, value) onto the mpv instance. UNCONDITIONAL
    // like the subtitle/audio/refresh applies: every option is written every time, so flipping the mode actively
    // resets the previous mode's key (target-colorspace-hint / the SDR curve) rather than leaving it set.
    const HdrOutput::Mode mode = Settings::hdrOutput();
    const QVector<QPair<QString, QString>> opts = HdrOutput::optionsFor(mode, HdrOutput::currentPlatform());
    QString applied;
    for (const auto& o : opts)
    {
        mpv_set_option_string(mpv, o.first.toUtf8().constData(), o.second.toUtf8().constData());
        applied += (applied.isEmpty() ? QString() : QStringLiteral(" ")) + o.first + QStringLiteral("=") + o.second;
    }
    // Surface what happened for supportability (the issue asks for this alongside the existing video-info log):
    // the stored mode and the exact option set requested. logVideoInfo() already logs the source transfer/primaries.
    videoLog(QStringLiteral("mpv: hdr mode=") + HdrOutput::idForMode(mode)
             + QStringLiteral(" (") + applied + QStringLiteral(")"));
}

QVector<MpvWidget::AudioDevice> MpvWidget::availableAudioDevices() const
{
    QVector<AudioDevice> out;
    if (!mpv) return out;
    // `audio-device-list` is a NODE_ARRAY of NODE_MAPs, each carrying string "name" (the id we store and set as
    // `audio-device`) and "description" (the human label). It is a system property, so this player's context
    // answers it without a file loaded. mpv owns the returned node until mpv_free_node_contents.
    mpv_node root;
    if (mpv_get_property(mpv, "audio-device-list", MPV_FORMAT_NODE, &root) < 0)
        return out;
    if (root.format == MPV_FORMAT_NODE_ARRAY && root.u.list)
    {
        for (int i = 0; i < root.u.list->num; ++i)
        {
            const mpv_node& entry = root.u.list->values[i];
            if (entry.format != MPV_FORMAT_NODE_MAP || !entry.u.list) continue;
            AudioDevice dev;
            for (int j = 0; j < entry.u.list->num; ++j)
            {
                const char* key = entry.u.list->keys[j];
                const mpv_node& val = entry.u.list->values[j];
                if (val.format != MPV_FORMAT_STRING || !key) continue;
                if (std::strcmp(key, "name") == 0)             dev.name = QString::fromUtf8(val.u.string);
                else if (std::strcmp(key, "description") == 0) dev.description = QString::fromUtf8(val.u.string);
            }
            // mpv's own list already includes an "auto" entry; the settings picker builds its own labelled Auto
            // row and skips this one so there is exactly one. A nameless entry is unusable — drop it.
            if (dev.name.isEmpty() || dev.name == QStringLiteral("auto")) continue;
            out.push_back(dev);
        }
    }
    mpv_free_node_contents(&root);
    return out;
}
