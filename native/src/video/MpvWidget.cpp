#include "MpvWidget.h"
#include "../core/AppPaths.h"
#include "../core/Settings.h"
#include <QOpenGLContext>
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

MpvWidget::MpvWidget(QWidget* parent) : QOpenGLWidget(parent)
{
    mpv = mpv_create();
    if (!mpv)
        throw std::runtime_error("could not create mpv context");

    // Render video THROUGH the libmpv render API (our QOpenGLWidget) instead of letting mpv open its own
    // window. This must be set before mpv_initialize - without it mpv uses the default 'gpu' output and
    // pops a separate window.
    mpv_set_option_string(mpv, "vo", "libmpv");
    // Decode in software. Diagnosis showed this machine's D3D11VA hardware decoder corrupts 10-bit HEVC
    // (p010) frames - "colored pixels everywhere" - even in copy mode, so hardware decode isn't trustworthy
    // here. Software decode is guaranteed-correct and easily handles 1080p; 4K HEVC is heavier but still
    // playable on a modern CPU. (If performance ever bites, this becomes a per-machine Setting.)
    mpv_set_option_string(mpv, "hwdec", "no");
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

    // Observe playback state for the seek bar / end-of-file, plus title + video presence for the overlay.
    mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 0, "width", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 0, "chapters", MPV_FORMAT_INT64); // chapter count -> show/hide chapter nav

    mpv_set_wakeup_callback(mpv, onMpvWakeup, this);

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
    makeCurrent();
    if (mpv_gl)
        mpv_render_context_free(mpv_gl);
    if (mpv)
        mpv_terminate_destroy(mpv);
}

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
    QOpenGLWidget::resizeEvent(e);
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

void MpvWidget::play(const QString& url)
{
    // Apply the user's subtitle defaults before loading, so they take effect for this video (and changing
    // them in Settings applies to the next one). "subs-fallback=yes" makes mpv select a sub track even when
    // none is marked default; "slang" sets the preferred language so the right track is picked.
    const QString lang = Settings::subtitleLanguage().trimmed();
    if (!lang.isEmpty()) mpv_set_option_string(mpv, "slang", lang.toUtf8().constData());
    const bool subsOn = Settings::subtitlesOnByDefault();
    mpv_set_option_string(mpv, "subs-fallback", subsOn ? "yes" : "no");

    QByteArray u = url.toUtf8();
    const char* cmd[] = { "loadfile", u.constData(), nullptr };
    mpv_command_async(mpv, 0, cmd); // mpv copies the args
    setPaused(false);
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

QVector<MpvWidget::SubtitleTrack> MpvWidget::subtitleTracks() const
{
    QVector<SubtitleTrack> out;
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
        const bool isSub = ty && std::strcmp(ty, "sub") == 0;
        if (ty) mpv_free(ty);
        if (!isSub) continue;

        SubtitleTrack t;
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

void MpvWidget::setSubtitleTrack(int id)
{
    if (!mpv) return;
    if (id < 0) { mpv_set_property_string(mpv, "sid", "no"); return; }
    const QByteArray v = QByteArray::number(id);
    mpv_set_property_string(mpv, "sid", v.constData());
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
