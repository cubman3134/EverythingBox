#include "MpvWidget.h"
#include "MpvHeaderApply.h"
#include "HwDecode.h"
#include "MpvLogThrottle.h"        // #231: the burst counter mpv's own log is written through
#include "MpvLogLevel.h"           // #231: what is asked of libmpv, and which of it is worth keeping
#include "../core/LogSafeText.h"   // #231: StoredUrl's rule applied to whatever mpv put in a message
#include "RefreshSync.h"
#include "AudioOutput.h"
#include "HdrOutput.h"
#include "ReplayGain.h"
#include "Crossfade.h"
#include "../core/AppPaths.h"
#include "../core/DisplayTitle.h"   // #202: what may be put on screen (QtCore-only, header-only)
#include "../core/Settings.h"
#include "../core/LanguageCodes.h"
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

// What mpv is asked to report, and which of it is written down (issue #231). The rule, the measurement
// behind it and why "warn" is the wrong answer are all in MpvLogLevel.h; these two are only the environment
// reads, kept here so the header stays pure and testable.
static QByteArray mpvLogLevel() { return MpvLogLevel::requested(qgetenv("EB_MPV_LOG")); }

// "Show me everything at the level I asked for": the tree's existing diagnostics switch, or an explicit
// EB_MPV_LOG, which is nobody's accident.
static bool mpvVerboseWanted()
{
    return qEnvironmentVariableIntValue("EB_PERF") == 1 || !qgetenv("EB_MPV_LOG").trimmed().isEmpty();
}

MpvWidget::MpvWidget(QWidget* parent) : MpvWidgetBase(parent)
{
    mpv = mpv_create();
    if (!mpv)
        throw std::runtime_error("could not create mpv context");
    // #141 crossfade: this is the PRIMARY deck. It keeps the render context and is the only deck video ever
    // plays on; `mpv` is re-pointed at the second deck for the length of a music crossfade and back again on
    // the next replace-load. Everything below this line configures the primary deck exactly as it always did.
    mpvPrimary_ = mpv;

    // Render video THROUGH the libmpv render API (our QOpenGLWidget) instead of letting mpv open its own
    // window. This must be set before mpv_initialize - without it mpv uses the default 'gpu' output and
    // pops a separate window.
    mpv_set_option_string(mpv, "vo", "libmpv");
    // Hardware decode is a per-machine Setting (issue #67), read once here at player creation. The old
    // hard-coded "no" existed because this machine's D3D11VA corrupts 10-bit HEVC (p010) even in copy mode.
    // Auto asks for copy-back decoders by name (issue #229 measured that "auto-safe", which used to stand
    // here, resolves to nvdec DIRECT on NVIDIA — CUDA<->GL interop into the QOpenGLWidget below, the opposite
    // of what it was chosen for). Off keeps "no", On is full "auto", and the iOS software-render path is
    // forced to "no" regardless (see HwDecode::mpvOption).
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
    // Repair a dropped connection instead of decoding through the hole. ffmpeg's HTTP reader does NOT reconnect
    // by default: when a long transfer is reset, throttled out or expires part-way -- routine for a multi-hour,
    // multi-gigabyte debrid link -- it returns short reads, and the demuxer hands the decoder truncated packets
    // rather than an error. HEVC then propagates that damage from the broken reference frame onward, which
    // looks like torn, smeared picture that never recovers, not like a stall. reconnect_streamed covers the
    // non-seekable case; delay_max caps the backoff so a dead link still fails instead of retrying forever.
    mpv_set_option_string(mpv, "stream-lavf-o",
                          "reconnect=1,reconnect_streamed=1,reconnect_delay_max=30");
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
    // Paused or not, so a transport button can say which. Observed rather than read on demand: mpv reports an
    // observed property's value once at observe time and then on every change, INCLUDING the changes this
    // class did not make — a keybinding, the OS lifecycle pause, #140's sleep timer — which is exactly the
    // set a polling host misses.
    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);

    // #231: ASK MPV WHAT IT THINKS. Everything above this line tells mpv what to do; nothing until now ever
    // listened to what it said back, so a truncated bitstream, a reconnect, a refused range request and a
    // dead link all reached the log as the same silence. Delivered as MPV_EVENT_LOG_MESSAGE on the queue the
    // wakeup below already drains — see handleLogMessage for the scrub and the burst counter.
    {
        const QByteArray lvl = mpvLogLevel();
        const int rc = mpv_request_log_messages(mpv, lvl.constData());
        videoLog(QStringLiteral("mpv: log capture level='") + QString::fromUtf8(lvl)
                 + QStringLiteral("' filter=") + (mpvVerboseWanted() ? QStringLiteral("all")
                                                                     : QStringLiteral("warn+ffmpeg"))
                 + (rc < 0 ? QStringLiteral(" — REFUSED (") + QString::fromUtf8(mpv_error_string(rc))
                                 + QStringLiteral("), falling back to 'v'")
                           : QString()));
        if (rc < 0) mpv_request_log_messages(mpv, "v");   // a mistyped EB_MPV_LOG must not silence the capture
    }
    // The summary half of the burst counter needs a clock the messages themselves do not provide: a stream
    // that emits 1,400 concealment warnings and then goes quiet would otherwise hold its own "and 1,396 more"
    // line until the next message, which may be never. Started only while something is actually being
    // counted, and stopped again the moment nothing is — an idle player pays for no timer at all.
    logFlushTimer_ = new QTimer(this);
    logFlushTimer_->setInterval(2000);
    connect(logFlushTimer_, &QTimer::timeout, this, [this] {
        for (const QString& line : logThrottle_.flush(logClock_.elapsed()))
            videoLog(QStringLiteral("mpvlog ") + line);
        if (!logThrottle_.pending()) logFlushTimer_->stop();
    });
    logClock_.start();

    mpv_set_wakeup_callback(mpv, onMpvWakeup, this);

#ifdef Q_OS_IOS
    // Software render context, created up-front (there is no initializeGL on the QWidget path).
    mpv_render_param rparams[]{
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW) },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_create(&mpv_gl, mpvPrimary_, rparams) < 0)
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

    // #213: the load watchdog. Single-shot; its own handler re-arms it for the second phase.
    loadTimer_ = new QTimer(this);
    loadTimer_->setSingleShot(true);
    connect(loadTimer_, &QTimer::timeout, this, &MpvWidget::onLoadWatchdog);

    // #141 crossfade ramp clock. 25 ms (40 Hz) is not a frame rate, it is a zipper threshold: mpv applies a
    // volume change at the next audio buffer, so a coarse ramp is heard as steps rather than as a fade. It
    // only ever runs inside a window, so it costs nothing the rest of the time.
    xfTimer_ = new QTimer(this);
    xfTimer_->setInterval(25);
    connect(xfTimer_, &QTimer::timeout, this, &MpvWidget::crossfadeTick);
}

MpvWidget::~MpvWidget()
{
    // #231: the last outstanding counts, before the player that was accumulating them goes away. Closing the
    // app during a broken stream is exactly when the summary matters and is the one exit that has no EOF.
    for (const QString& line : logThrottle_.drain(logClock_.elapsed()))
        videoLog(QStringLiteral("mpvlog ") + line);
#ifndef Q_OS_IOS
    makeCurrent();
#endif
    if (mpv_gl)
        mpv_render_context_free(mpv_gl);
    // Both decks (#141). The render context above was created on mpvPrimary_ and has just been freed, so the
    // order here is the same one the single-deck destructor always had — the context first, then the contexts
    // it was bound to. The second deck never owns a render context and is simply torn down beside it.
    if (mpvSecond_)
        mpv_terminate_destroy(mpvSecond_);
    if (mpvPrimary_)
        mpv_terminate_destroy(mpvPrimary_);
    mpv = nullptr;
    mpvPrimary_ = mpvSecond_ = xfIncoming_ = nullptr;
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
    // Bound to mpvPrimary_, NOT to `mpv` (#141): `mpv` is the active deck and may be the audio-only crossfade
    // deck at the moment this first runs. The render context follows the video path, and the video path is
    // the primary deck for the whole life of the widget.
    if (mpv_render_context_create(&mpv_gl, mpvPrimary_, params) < 0)
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
    // BOTH decks are drained (#141), because both have this widget as their wakeup callback and a queue that
    // is never emptied is a queue that grows forever. Which deck an event came off is passed down: the
    // inactive deck's events are almost entirely discarded, and the ones that are not are named in
    // handleEvent. Draining the active deck first keeps the ordinary single-deck case byte-identical.
    mpv_handle* decks[2] = { mpv, (mpv == mpvPrimary_) ? mpvSecond_ : mpvPrimary_ };
    for (mpv_handle* deck : decks)
    {
        if (!deck) continue;
        for (;;)
        {
            mpv_event* event = mpv_wait_event(deck, 0);
            if (event->event_id == MPV_EVENT_NONE)
                break;
            handleEvent(event, deck, deck == mpv);
        }
    }
}

// One MPV_EVENT_LOG_MESSAGE, scrubbed, counted, and written (issue #231).
//
// Handled BEFORE the inactive-deck gate below, and on purpose. That gate exists because letting a second
// decoder's TRANSPORT events through is how the app ends up showing a position, a duration or an EOF that
// belongs to a file nothing on screen is playing. A log line drives nothing — and the crossfade deck's own
// reason for failing to open a track is exactly the kind of thing the log has never had. It is tagged so the
// two decks can be told apart.
//
// The order here is not arbitrary: SCRUB, then throttle. The throttle keys on the shape of the message, and
// an unscrubbed url would put a per-request signature into that key — every message its own bucket, and the
// rate limiting silently does nothing. Scrubbing first also means a credential never reaches the throttle's
// stored `lastSuppressed`, so there is exactly one place in this path that has ever held one.
void MpvWidget::handleLogMessage(mpv_event* event, bool fromActive)
{
    auto* m = static_cast<mpv_event_log_message*>(event->data);
    if (!m) return;
    const QByteArray level  = m->level  ? QByteArray(m->level)  : QByteArrayLiteral("?");
    const QByteArray prefix = m->prefix ? QByteArray(m->prefix) : QByteArrayLiteral("?");
    // libmpv is asked for `v` because the message this exists for is one ffmpeg logs at INFO; the filter is
    // what keeps a healthy open from costing a hundred lines of mpv's own verbose. See MpvLogLevel.h.
    static const bool verbose = mpvVerboseWanted();
    if (!MpvLogLevel::keep(level, prefix, verbose)) return;
    // mpv's own text arrives with a trailing newline (and occasionally several lines at once); stream_debug.log
    // is one record per line, so it is trimmed here rather than embedded raw.
    QString text = QString::fromUtf8(m->text ? m->text : "").trimmed();
    if (text.isEmpty()) return;
    text.replace(QLatin1Char('\n'), QLatin1String(" | "));

    const QString body = QStringLiteral("[%1] %2%3: %4")
                             .arg(QString::fromUtf8(level),
                                  fromActive ? QString() : QStringLiteral("deck2 "),
                                  QString::fromUtf8(prefix),
                                  LogSafeText::scrub(text));

    for (const QString& line : logThrottle_.admit(body, logClock_.elapsed()))
        videoLog(QStringLiteral("mpvlog ") + line);
    if (logThrottle_.pending() && logFlushTimer_ && !logFlushTimer_->isActive())
        logFlushTimer_->start();
}

void MpvWidget::handleEvent(mpv_event* event, mpv_handle* from, bool fromActive)
{
    if (event->event_id == MPV_EVENT_LOG_MESSAGE)
    {
        handleLogMessage(event, fromActive);
        return;
    }
    if (!fromActive)
    {
        // The deck fading IN during a crossfade window, or a deck that has been stopped and is waiting to be
        // the incoming one next time. Exactly ONE event matters here and it is the file-loaded: it is the
        // first and only moment mpv has parsed this file's tags, which is when #141's own ReplayGain has to
        // be on it — the deck is about to become audible at a level the levelling decides, and applying it
        // after the promotion would mean the first seconds of every crossfaded track played unlevelled.
        // Everything else — its time-pos, its duration, its EOF — belongs to a file nothing above this class
        // is showing, and forwarding any of it is how a second decoder starts driving the app's transport.
        if (event->event_id == MPV_EVENT_FILE_LOADED && from == xfIncoming_)
            applyReplayGainTo(from, /*isMusic*/ true); // a crossfade only ever happens between music tracks
        // ...and one failure. If the INCOMING file cannot be opened - it was deleted, the drive went away, it
        // is not really audio - the window must be abandoned, not ridden to its end: promoting a deck that is
        // playing nothing would fade the music out into silence and leave the transport sitting on a track
        // that never started. Cancelling instead brings the outgoing track back to full volume and lets its
        // own end-of-file drive the ordinary advance, which is also the path that surfaces the failure.
        if (event->event_id == MPV_EVENT_END_FILE && from == xfIncoming_)
        {
            auto* ef = static_cast<mpv_event_end_file*>(event->data);
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR)
            {
                videoLog(QStringLiteral("mpv: crossfade - incoming file failed to load (")
                         + QString::fromUtf8(mpv_error_string(ef->error))
                         + QStringLiteral("), abandoning the overlap"));
                cancelCrossfade();
            }
        }
        return;
    }
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
        else if (prop->format == MPV_FORMAT_FLAG)
        {
            // MPV_FORMAT_FLAG's payload is an int, 0 or 1 — not a bool and not an int64.
            if (std::strcmp(prop->name, "pause") == 0)
                emit pausedChanged(*static_cast<int*>(prop->data) != 0);
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
        // #213: mpv has begun the file. From here it will say FILE_LOADED, END_FILE, or — the case this guards
        // — nothing at all. Only the ACTIVE deck is watched: the crossfade deck's failure has its own path in
        // the !fromActive branch, and a gapless advance is a fresh START_FILE here, so it re-arms by itself.
        fileLoaded_ = false;
        if (loadWatched_ && loadTimer_) armLoadWatchdog(LoadWatchdog::Phase::First);
        break;
    case MPV_EVENT_FILE_LOADED:
    {
        fileLoaded_ = true;                       // #213: the watchdog's question is answered
        if (loadTimer_) loadTimer_->stop();
        // The track list is now populated: report whether an embedded subtitle in the preferred language is
        // present, so the app can decide to auto-download one. Also report whether this is a video track (an
        // audio-only file never wants subtitles). mpv lang codes may be 2- or 3-letter; canonicalize both
        // sides to ISO-639-1 so e.g. a "spa" track matches the "es" preference.
        const QString want = LanguageCodes::toCanonical(Settings::preferredLanguage());
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
                if (!want.isEmpty() && LanguageCodes::toCanonical(l) == want) wantSub = true;
                mpv_free(lg);
            }
        }
        const bool usable = want.isEmpty() ? anySub : wantSub;
        emit fileLoaded(usable, video || hasVideo_);
        break;
    }
    case MPV_EVENT_END_FILE:
    {
        // #231: whatever this file was still being counted for, say so now — a burst that ran out the last
        // seconds of a stream is the most interesting one in the log, and its window has not closed.
        for (const QString& line : logThrottle_.drain(logClock_.elapsed()))
            videoLog(QStringLiteral("mpvlog ") + line);
        if (logFlushTimer_) logFlushTimer_->stop();
        if (nowPlaying_) nowPlaying_->hide();
        if (loadTimer_) loadTimer_->stop();       // #213: the file ended, however it ended; nothing to watch
        // Only a natural end-of-file should advance a playlist; stop/seek/redirect must not.
        auto* ef = static_cast<mpv_event_end_file*>(event->data);
        if (ef && ef->reason == MPV_END_FILE_REASON_EOF && xfIncoming_)
        {
            // #141: the outgoing deck ran out before the ramp did. This is the ORDINARY way a window ends,
            // not a rare one — `time-pos` is where the decoder is and the speaker is an audio buffer behind
            // it, so the sound stops slightly before the position says it will (measured at 5.91 s of a
            // 6.00 s ramp, the outgoing track already 32 dB down). What it is NOT is the queue's
            // end-of-track: the next track is already playing on the other deck. Hand over now
            // (promoteIncomingDeck takes the incoming deck to full volume) rather than emitting an EOF that
            // would advance the queue a second time and stop-start the track already in the air.
            videoLog(QStringLiteral("mpv: crossfade — outgoing deck hit EOF, promoting early"));
            promoteIncomingDeck();
        }
        else if (ef && ef->reason == MPV_END_FILE_REASON_EOF)
        {
            // #217: the queue's ONE advance signal when gapless is off, and it was unlogged — so "the book
            // stopped at the end of part one" could not be told apart from "mpv never said the part ended".
            // One line per file end is the cheapest way to make that distinguishable, and it is the fact
            // every other line about a boundary is downstream of.
            videoLog(QStringLiteral("mpv: end of file"));
            emit endReached();
        }
        // A file that could not be opened ends here too, and used to end here silently — which is how a
        // stored link that had since expired produced a player showing nothing, saying nothing, forever.
        else if (ef && ef->reason == MPV_END_FILE_REASON_ERROR)
        {
            const QString why = QString::fromUtf8(mpv_error_string(ef->error));
            videoLog(QStringLiteral("mpv: load failed: ") + why);
            emit loadFailed(why);
        }
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

// #202: the same title, pushed in by the host for the two advances that never reload (a gapless boundary
// inside mpv's own playlist, and a crossfade promotion). Refreshes, because a crossfade promotion sets
// mpv's media-title and repaints the overlay BEFORE the host has advanced its queue — without the repaint
// here the overlay would sit on the previous track's name until something else happened to redraw it.
void MpvWidget::setNowPlayingTitle(const QString& displayTitle)
{
    if (hostTitle_ == displayTitle) return;
    hostTitle_ = displayTitle;
    refreshNowPlaying();
}

// THE AUDIO-ONLY OVERLAY'S LABEL (issue #202).
//
// This used to be `mediaTitle_` verbatim, and mediaTitle_ is mpv's `media-title` — which is the file's real
// title tag when it has one, the live ICY song name for a radio stream, and THE URL ITSELF when the stream
// carries no metadata at all. A Subsonic stream url carries the user's salted token in its query, so the
// last of those three put a live credential on the screen, in every screenshot and every screen share. That
// was the finding.
//
// The order below is deliberate and is the SMALLEST change that closes it: mpv's title still wins whenever
// it is a title, so an ICY radio stream goes on naming the song that is playing right now and a tagged file
// goes on showing its tag. Only when mpv hands back a url does the host's queue title — the value the themed
// page has been showing all along, which is why the themed page was never affected — take over. The url is
// the LAST resort and never appears verbatim: DisplayTitle::fromLocation reduces it to its host.
//
// The VISIBILITY test is deliberately unchanged (mediaTitle_, not the label): "mpv has told us about a
// file" is what it has always meant, and widening it here would make an unrelated timing change ride along
// with a security fix.
void MpvWidget::refreshNowPlaying()
{
    if (!nowPlaying_) return;
    if (!hasVideo_ && !mediaTitle_.isEmpty())
    {
        nowPlaying_->setText(QStringLiteral("♪\n\n")
                             + DisplayTitle::choose(mediaTitle_, hostTitle_, playedUrl_)); // ♪ + title
        nowPlaying_->setGeometry(rect());
        nowPlaying_->show();
        nowPlaying_->raise();
    }
    else
    {
        nowPlaying_->hide();
    }
}

void MpvWidget::play(const QString& url, const StreamHeaders::Headers& headers, const QString& displayTitle)
{
    // #202: both remembered BEFORE the load, so the first refreshNowPlaying this load provokes already has
    // them. Assigned unconditionally — a replace-load is a new track, and a title left over from the last one
    // would name the wrong song rather than merely be missing.
    hostTitle_ = displayTitle;
    playedUrl_ = url;
    // #141: a replace-load is the end of any crossfade and the end of the second deck's turn. Dropping the
    // window first means a Next/new-open during one can never leave a decoder running behind the thing that
    // replaced it; returning to the primary deck means VIDEO always lands on the deck that owns the render
    // context, so the audio-only crossfade deck can never end up being asked to show a picture. Both are
    // no-ops on the ordinary path, which is every play that is not the tail of a crossfade.
    cancelCrossfade();
    if (mpv != mpvPrimary_ && mpvPrimary_)
    {
        const char* stopCmd[] = { "stop", nullptr };
        mpv_command_async(mpv, 0, stopCmd);   // idle the crossfade deck; the handle stays for the next window
        mpv = mpvPrimary_;
        hasVideo_ = false;
        applyDeckVolumes();
        videoLog(QStringLiteral("mpv: crossfade — replace-load, active deck back to primary"));
    }

    // Per-stream HTTP headers (behaviorHints.proxyHeaders.request). UNCONDITIONAL, before the load:
    // MpvHeaderApply::apply writes all three properties every time, so a stream that needs none actively
    // clears whatever the previous one set. Nothing here logs a value — only how many and which names.
    MpvHeaderApply::apply(mpv, headers);
    if (!headers.isEmpty())
        videoLog(QStringLiteral("mpv: applying stream ") + StreamHeaders::logSummary(headers));

    // Apply the user's subtitle defaults before loading, so they take effect for this video (and changing
    // them in Settings applies to the next one). "subs-fallback=yes" makes mpv select a sub track even when
    // none is marked default; "slang" sets the preferred language so the right track is picked.
    // Set slang/alang UNCONDITIONALLY every play (empty string when there is no preference): the mpv
    // handle outlives a play, so clearing the preference mid-session must actively reset both — otherwise
    // a prior "es,spa" keeps steering track selection and "no preference" no longer means mpv's default.
    const QByteArray langList = LanguageCodes::toMpvLangList(Settings::preferredLanguage().trimmed()).toUtf8();
    mpv_set_option_string(mpv, "slang", langList.constData());  // preferred subtitle language ("" = default)
    mpv_set_option_string(mpv, "alang", langList.constData());  // preferred audio language ("" = default)
    const bool subsOn = Settings::subtitlesOnByDefault();
    mpv_set_option_string(mpv, "subs-fallback", subsOn ? "yes" : "no");
    double normalSpeed = 1.0;
    mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &normalSpeed); // each new video starts at normal speed

    // #213: decide up front whether this load is one the watchdog stands over (a live/HLS link is not). It is
    // ARMED on START_FILE rather than here, because that is the moment mpv has actually begun the file; any
    // deadline still running from the previous load is over, whatever it was going to say.
    loadWatched_ = LoadWatchdog::watches(url.toStdString());
    fileLoaded_ = false;
    if (loadTimer_) loadTimer_->stop();

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

void MpvWidget::dropQueuedAfterCurrent()
{
    // #193: un-hand the entries mpv has been given but has not started. Under gapless the app feeds mpv's own
    // playlist one entry ahead, so a queue edit at or before that entry leaves mpv holding a file the app no
    // longer believes comes next — and the symptom is the wrong song, silently. This is the repair: everything
    // strictly AFTER the playing entry goes, and the app re-feeds afterwards.
    //
    // INAUDIBLE BY CONSTRUCTION, which is the whole reason a re-seat is preferable to forbidding the edit: the
    // entries removed here are ones no sample has been taken from (mpv is still decoding the one at
    // playlist-pos), so the file being played, its position, and its decoder are all untouched. Nothing
    // stops, nothing reloads.
    //
    // Removed from the END DOWNWARDS because playlist-remove renumbers: taking pos+1 first would shift the
    // entry that was at pos+2 down into the index just freed, and a forward loop would skip it. Synchronous
    // (mpv_command, not _async) so the whole repair is complete before the caller's re-feed appends — an
    // append that overtook a pending remove would be removed itself.
    if (!mpv) return;
    int64_t pos = -1, count = 0;
    if (mpv_get_property(mpv, "playlist-pos", MPV_FORMAT_INT64, &pos) < 0) return;
    if (mpv_get_property(mpv, "playlist-count", MPV_FORMAT_INT64, &count) < 0) return;
    if (pos < 0) return;   // nothing playing: there is no "after current" to drop
    int dropped = 0;
    for (int64_t i = count - 1; i > pos; --i)
    {
        const QByteArray idx = QByteArray::number(qlonglong(i));
        const char* cmd[] = { "playlist-remove", idx.constData(), nullptr };
        if (mpv_command(mpv, cmd) >= 0) ++dropped;
    }
    if (dropped > 0)
        videoLog(QStringLiteral("mpv: queue edit - dropped %1 un-started playlist entr%2 after pos %3")
                     .arg(dropped).arg(dropped == 1 ? QStringLiteral("y") : QStringLiteral("ies")).arg(pos));
}

void MpvWidget::setGaplessAudio(bool on)
{
    if (!mpv) return;
    // "weak" is the honest value the issue asks for: mpv keeps the audio output continuous only when adjacent
    // tracks share a format, and reinitialises (a correct, expected gap) when they truly differ — so a mixed
    // queue is never forced through a mismatched device config. Only ever called with `on` = true (the host
    // arms it when a gapless audio queue starts); the off path builds no mpv playlist and never calls this.
    gaplessArmed_ = on;   // #141: remembered so a crossfade deck created later inherits it (ensureSecondDeck)
    mpv_set_option_string(mpv, "gapless-audio", on ? "weak" : "no");
    if (mpvSecond_) mpv_set_option_string(mpvSecond_, "gapless-audio", on ? "weak" : "no");
    videoLog(QStringLiteral("mpv: gapless-audio='") + (on ? QStringLiteral("weak") : QStringLiteral("no"))
             + QStringLiteral("'"));
}

void MpvWidget::stop()
{
    // #141: stop means BOTH decks. This is the path leaving the now-playing page takes on the themed surface
    // (leaveThemedAudioPage) and the path Back takes on the classic one, and a crossfade deck that survived it
    // would keep a track audible over a screen that has already left the media. cancelCrossfade() ends any
    // window; the explicit stop of each handle covers the deck that is merely idle as well as the active one.
    cancelCrossfade();
    const char* cmd[] = { "stop", nullptr };
    if (mpvPrimary_) mpv_command_async(mpvPrimary_, 0, cmd);
    if (mpvSecond_)  mpv_command_async(mpvSecond_, 0, cmd);
    if (mpvPrimary_) mpv = mpvPrimary_;   // next play starts from the deck that owns the render context
    if (npTimer_) npTimer_->stop();
    if (nowPlaying_) nowPlaying_->hide();
}

// ---- #213: the load watchdog -----------------------------------------------------------------------------
// LoadWatchdog.h holds the rules and says why they are two phases; this is the host half — the timer, the
// question put to mpv at each deadline, and the signal. Nothing here decides what the screen is owed.
void MpvWidget::armLoadWatchdog(LoadWatchdog::Phase phase)
{
    loadPhase_ = phase;
    loadTimer_->start(LoadWatchdog::deadlineMs(phase));
}

// What can mpv say about the current file's bytes? THREE-valued, and the middle value is the one found live
// (2026-09-01): a server that sent headers and 4 KiB then hung read IDENTICALLY to one that sent nothing,
// because until enough bytes arrive to identify the format there is no demuxer, and every property asked
// below is a demuxer property. So "the property is unavailable" (mpv_get_property < 0) is Unknown, NOT None —
// reading it as None is exactly the slow-link kill this watchdog exists to avoid. None is a demuxer that
// answered and reports nothing; Some is a positive cache time or stream position. Which it was is logged,
// because it is the one fact about this rule a headless probe cannot check.
LoadWatchdog::Progress MpvWidget::loadProgress() const
{
    if (!mpv) return LoadWatchdog::Progress::Unknown;
    bool answered = false;
    double cacheTime = 0.0;
    if (mpv_get_property(mpv, "demuxer-cache-time", MPV_FORMAT_DOUBLE, &cacheTime) >= 0)
    {
        answered = true;
        if (cacheTime > 0.0)
        {
            videoLog(QStringLiteral("mpv: load watchdog — progress via demuxer-cache-time=%1").arg(cacheTime));
            return LoadWatchdog::Progress::Some;
        }
    }
    int64_t pos = 0;
    if (mpv_get_property(mpv, "stream-pos", MPV_FORMAT_INT64, &pos) >= 0)
    {
        answered = true;
        if (pos > 0)
        {
            videoLog(QStringLiteral("mpv: load watchdog — progress via stream-pos=%1").arg(static_cast<qint64>(pos)));
            return LoadWatchdog::Progress::Some;
        }
    }
    videoLog(answered ? QStringLiteral("mpv: load watchdog — a demuxer answered, and reports nothing")
                      : QStringLiteral("mpv: load watchdog — no demuxer property available yet (format not identified)"));
    return answered ? LoadWatchdog::Progress::None : LoadWatchdog::Progress::Unknown;
}

void MpvWidget::onLoadWatchdog()
{
    const LoadWatchdog::Progress progress = loadProgress();
    switch (LoadWatchdog::judge({ loadPhase_, fileLoaded_, progress }))
    {
    case LoadWatchdog::Verdict::Loaded:
        return;   // a queued timeout that landed after FILE_LOADED: nothing to do
    case LoadWatchdog::Verdict::Regrace:
        videoLog(QStringLiteral("mpv: load slow — no file-loaded after %1 s, waiting %2 s more")
                     .arg(LoadWatchdog::deadlineMs(LoadWatchdog::Phase::First) / 1000)
                     .arg(LoadWatchdog::deadlineMs(LoadWatchdog::Phase::Second) / 1000));
        armLoadWatchdog(LoadWatchdog::Phase::Second);
        return;
    case LoadWatchdog::Verdict::Stall:
    {
        const int waited = LoadWatchdog::waitedSeconds(loadPhase_);
        const char* what = progress == LoadWatchdog::Progress::Some ? "some bytes, never parsed"
                         : progress == LoadWatchdog::Progress::None ? "a demuxer with nothing in it"
                                                                    : "no demuxer ever";
        videoLog(QStringLiteral("mpv: load stalled: no file-loaded after %1 s (%2)").arg(waited).arg(QLatin1String(what)));
        loadWatched_ = false;
        emit loadStalled(waited);
        return;
    }
    }
}

void MpvWidget::setPaused(bool paused)
{
    int flag = paused ? 1 : 0;
    mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &flag);
    // #141: inside a window BOTH tracks are audible, so a pause that only reached one deck would leave the
    // other one playing on alone. The ramp clock reads the active deck's pause state each tick and freezes
    // with it, so the window resumes where it was rather than completing silently while paused.
    if (xfIncoming_) mpv_set_property(xfIncoming_, "pause", MPV_FORMAT_FLAG, &flag);
}

bool MpvWidget::isPaused() const
{
    if (!mpv) return false;
    int flag = 0;
    mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &flag);
    return flag != 0;
}

bool MpvWidget::hasMedia() const
{
    if (!mpv) return false;
    int idle = 0;
    // A read that FAILS is answered "no media" rather than "yes": the caller uses this to decide whether a
    // position and a title mean anything, and guessing yes there is how a stopped player goes on reporting
    // the last thing it played. idle-active is 0 from the moment a load starts, so a still-loading file is
    // correctly media.
    if (mpv_get_property(mpv, "idle-active", MPV_FORMAT_FLAG, &idle) < 0) return false;
    return idle == 0;
}

void MpvWidget::togglePause()
{
    // #141: inside a window this becomes an explicit set of BOTH decks rather than a per-deck "cycle" — two
    // independent cycles can only stay in step if they started in step, and a deck that was paused a moment
    // earlier by anything else would invert instead of following. Outside a window it is the same single
    // async cycle it has always been, so the ordinary video/audio path is untouched.
    if (xfIncoming_) { setPaused(!isPaused()); return; }
    const char* cmd[] = { "cycle", "pause", nullptr };
    mpv_command_async(mpv, 0, cmd);
}

void MpvWidget::setVolume(int percent)
{
    // #141: the host's volume is REMEMBERED rather than written straight through, because the crossfade ramp
    // needs to scale it. Without that, the two would fight: a volume change mid-window (the slider, the
    // sleep-timer fade in #140) would overwrite the ramp's gain on one deck and the next tick would overwrite
    // the user's volume back — the audible result being a stuttering fade at the wrong level. Keeping the
    // requested percent here makes "what the user asked for" and "where we are in the fade" two separate
    // numbers that are multiplied, so either can change at any time without disturbing the other.
    volumePercent_ = percent < 0 ? 0 : (percent > 200 ? 200 : percent);
    applyDeckVolumes();
}

void MpvWidget::setMuted(bool muted)
{
    int flag = muted ? 1 : 0;
    mpv_set_property(mpv, "mute", MPV_FORMAT_FLAG, &flag);
    if (xfIncoming_) mpv_set_property(xfIncoming_, "mute", MPV_FORMAT_FLAG, &flag); // both, for the same reason as pause
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
    // #141 asks that the #69 device/passthrough settings apply to BOTH instances, and this is where that is
    // true: the live re-apply reaches every deck that exists, and ensureSecondDeck() runs the same function
    // on a deck the moment it is created, so a deck born after the last settings change is not stale.
    applyAudioOutputTo(mpv);
    if (mpvSecond_ && mpvSecond_ != mpv) applyAudioOutputTo(mpvSecond_);
}

void MpvWidget::applyAudioOutputTo(mpv_handle* deck)
{
    if (!deck) return;
    // The pure map owns every decision (Auto -> "auto", the passthrough codec list, the empty-when-off reset).
    // Here we only push each (name, value) onto that deck. UNCONDITIONAL, like the subtitle apply: every
    // option is written every time, so turning passthrough or exclusive mode off actively resets it rather than
    // leaving the previous value set on the context. The device change is live; passthrough/exclusive take full
    // effect on the next AO (re)init.
    const QVector<QPair<QString, QString>> opts = AudioOutput::toMpvOptions(Settings::audioOutput());
    for (const auto& o : opts)
        mpv_set_option_string(deck, o.first.toUtf8().constData(), o.second.toUtf8().constData());
    videoLog(QStringLiteral("mpv: audio-device='") + Settings::audioDevice()
             + QStringLiteral("' passthrough=") + (Settings::audioPassthrough() ? QStringLiteral("on") : QStringLiteral("off"))
             + QStringLiteral(" exclusive=") + (Settings::audioExclusive() ? QStringLiteral("on") : QStringLiteral("off")));
}

// Presence + value of one REPLAYGAIN_* tag on the file mpv currently has LOADED (issue #141). mpv's own
// demuxer already parsed the container's tag block to decide whether to apply a gain at all, so asking it what
// it found costs one property read and needs no second open of the file — which is the whole reason this does
// not call AudioTags::read() here: that reader exists for the library SCAN (thousands of files, off-thread,
// cover bytes and all) and re-running it on the GUI thread at every track start would be a file read to learn
// something the loaded demuxer is already holding. The value type is #74's GainValue precisely so PRESENCE
// stays a separate bit from the number: "0.00 dB" is a real tag on an already-normalised track, and a caller
// that inferred absence from a zero would classify exactly those tracks as untagged.
//
// mpv's metadata lookup is case-insensitive, which matters: Vorbis comments store REPLAYGAIN_TRACK_GAIN
// upper-case while ffmpeg hands ID3/APE tags over lower-case. A tag that is present but not a number (a
// truncated tagger, "N/A") reads as ABSENT — mpv could not use it either, so pretending we have a gain would
// only mislead the log.
static AudioTags::GainValue mpvGainTag(mpv_handle* mpv, const char* tagName)
{
    AudioTags::GainValue g;
    if (!mpv) return g;
    const QByteArray prop = QByteArray("metadata/by-key/") + tagName;
    char* s = mpv_get_property_string(mpv, prop.constData());
    if (!s) return g;                                  // no such tag on this file
    QString raw = QString::fromUtf8(s).trimmed();
    mpv_free(s);
    if (raw.endsWith(QLatin1String("dB"), Qt::CaseInsensitive)) raw.chop(2); // "-7.89 dB" -> "-7.89"
    bool ok = false;
    const double v = raw.trimmed().toDouble(&ok);
    if (!ok) return g;
    g.present = true;
    g.value   = v;
    return g;
}

void MpvWidget::applyReplayGain(bool isMusic)
{
    // The ACTIVE deck only. The crossfade deck is levelled at its own file-loaded (see handleEvent's inactive
    // branch), against the tags of the file IT has open — asking for it here would read the active deck's
    // tags and put one track's gain on the other's audio.
    applyReplayGainTo(mpv, isMusic);
}

void MpvWidget::applyReplayGainTo(mpv_handle* deck, bool isMusic)
{
    if (!deck) return;
    // What this file is actually tagged with. Only the two GAIN tags are consulted: the PEAKs matter only to
    // mpv's own clipping prevention (which reads them itself), never to the decision of which mode to ask for.
    const AudioTags::GainValue trackGain = mpvGainTag(deck, "REPLAYGAIN_TRACK_GAIN");
    const AudioTags::GainValue albumGain = mpvGainTag(deck, "REPLAYGAIN_ALBUM_GAIN");
    // The pure map owns every decision — the music-only carve-out, the album<->track fallback when only one of
    // the tags is present, the untagged->Off answer, the preamp clamp and the reset-to-mpv-defaults when the
    // answer is Off. Here we only push each (name, value) onto that deck. UNCONDITIONAL, like the
    // subtitle/audio/HDR applies: all four options are written every time, so an audiobook opened after an
    // album-gained record actively clears the gain instead of inheriting it.
    const ReplayGain::Mode setting = Settings::replayGainMode();
    const QVector<QPair<QString, QString>> opts =
        ReplayGain::toMpvOptions(setting, isMusic, Settings::replayGainPreamp(), trackGain, albumGain);
    // The return code is CHECKED here, unlike the applies above, and the reason is that this feature is
    // nothing BUT these four option names: if a build of libmpv did not know one of them, mpv would quietly
    // ignore it and the log would still read like a success while no levelling happened. A rejected option is
    // therefore named in the same line, so "we asked" and "mpv took it" are not the same claim.
    QString applied;
    for (const auto& o : opts)
    {
        const int rc = mpv_set_option_string(deck, o.first.toUtf8().constData(), o.second.toUtf8().constData());
        applied += (applied.isEmpty() ? QString() : QStringLiteral(" ")) + o.first + QStringLiteral("=") + o.second;
        if (rc < 0)
            applied += QStringLiteral("[REJECTED:") + QString::fromUtf8(mpv_error_string(rc)) + QStringLiteral("]");
    }
    videoLog(QStringLiteral("mpv: ") + applied
             + QStringLiteral(" (setting=") + ReplayGain::idForMode(setting)
             + QStringLiteral(" music=") + (isMusic ? QStringLiteral("yes") : QStringLiteral("no"))
             + QStringLiteral(" tags: track=") + (trackGain.present ? QString::number(trackGain.value) : QStringLiteral("-"))
             + QStringLiteral(" album=") + (albumGain.present ? QString::number(albumGain.value) : QStringLiteral("-"))
             + QStringLiteral(")"));
}

// ============================ Crossfade: the second decode path (issue #141) ============================
//
// WHY A SECOND INSTANCE AND NOT A FILTER. #141 settled this and it is worth restating where the code is:
// libmpv gives an embedder ONE playback pipeline per context. There is no supported way to feed a second file
// into a running instance's audio graph and mix the two — lavfi-complex operates on the tracks of the file
// mpv already has open, and it cannot open another. So overlapping two tracks means two decoders, and the
// ceiling for that was already known in this codebase: MediaPane runs two MpvWidgets side by side for split
// screen. This is the same fact, used for one file instead of two panes.
//
// THE DECK MODEL. Two mpv contexts, called decks. `mpvPrimary_` is the one the constructor makes: it owns the
// render context and is the only deck video ever plays on. `mpvSecond_` is created the first time a crossfade
// actually happens (an install that never turns the setting on never pays for it) and is audio-only. `mpv` —
// the member every other method in this class already uses — points at whichever deck is currently THE
// player, so the swap costs nothing at the ~150 call sites above this widget: they all keep talking to one
// MpvWidget. The decks ping-pong, because after a handover the deck that just finished is the idle one and is
// therefore the next incoming.
//
// WHY THE PRIMARY IS ALWAYS THE ONE VIDEO USES. play() is a replace-load, and it returns the active deck to
// the primary before doing anything else. So the render context never has to be rebuilt on a different
// handle, video never lands on the audio-only deck, and the only time the active deck is the second one is
// between a music crossfade and the next replace-load.

// Create the crossfade deck, once. AUDIO ONLY and deliberately so: it has no render context (the primary owns
// the only one), so a video track would be decoded into nothing but CPU time, and `vid=no` also stops mpv
// trying to bring up a VO it cannot have. Everything the primary was configured with that matters to AUDIO is
// mirrored — volume-max so the boost band is the same on both, the cache options so a network track behaves
// the same, and #69's device/passthrough, which #141 explicitly says must apply to both instances.
mpv_handle* MpvWidget::ensureSecondDeck()
{
    if (mpvSecond_) return mpvSecond_;
    mpv_handle* h = mpv_create();
    if (!h) { videoLog(QStringLiteral("mpv: crossfade - second deck could not be created")); return nullptr; }
    mpv_set_option_string(h, "vo", "null");
    mpv_set_option_string(h, "vid", "no");     // audio only: no video decode, no VO, no cover-art surface
    mpv_set_option_string(h, "sub", "no");     // and no subtitle renderer for a deck that draws nothing
    mpv_set_option_string(h, "volume-max", "200");
    mpv_set_option_string(h, "cache", "yes");
    mpv_set_option_string(h, "cache-secs", "120");
    mpv_set_option_string(h, "network-timeout", "60");
    if (mpv_initialize(h) < 0)
    {
        mpv_terminate_destroy(h);
        videoLog(QStringLiteral("mpv: crossfade - second deck could not be initialized"));
        return nullptr;
    }
    mpvSecond_ = h;
    // #69 on the second instance, as #141 requires. Done here rather than only in applyAudioOutput() so a deck
    // created long after the user last touched an Audio setting still comes up on the device they chose.
    applyAudioOutputTo(h);
    if (gaplessArmed_) mpv_set_option_string(h, "gapless-audio", "weak");
    // The SAME observers the primary carries. They have to exist from creation, not from the promotion: the
    // duration and the position of the incoming file are reported while this deck is still the inactive one
    // (and are discarded then), and re-observing at handover would mean the first values arrive whenever mpv
    // next happens to change them rather than at once.
    mpv_observe_property(h, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(h, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(h, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(h, 0, "chapters", MPV_FORMAT_INT64);
    mpv_observe_property(h, 0, "playlist-pos", MPV_FORMAT_INT64);
    // `pause` belongs to that same set for a reason of its own: this deck's pause reports are discarded while
    // it is the inactive one (handleEvent drops everything but two events from it), but once it is PROMOTED
    // it is the deck the transport button is speaking for. A deck that never observed `pause` would report no
    // change for the rest of its life, and the button would freeze on whatever it last said.
    mpv_observe_property(h, 0, "pause", MPV_FORMAT_FLAG);
    // #231 on this deck too. A crossfade deck is the one that opens the NEXT track, so "the file the fade was
    // going to hand over to could not be read" is a fault only it can report; requested here for the same
    // reason the observers above are, at creation rather than at promotion.
    mpv_request_log_messages(h, mpvLogLevel().constData());
    mpv_set_wakeup_callback(h, onMpvWakeup, this);
    videoLog(QStringLiteral("mpv: crossfade - second deck created (audio-only)"));
    return h;
}

// Push `volumePercent_` scaled by each deck's place in the ramp. Outside a window that is simply the host's
// volume on the active deck; inside one it is the equal-power pair. The two numbers are multiplied rather
// than one overwriting the other — see setVolume for why that matters.
void MpvWidget::applyDeckVolumes()
{
    double base = double(volumePercent_);
    if (!xfIncoming_)
    {
        if (mpv) mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &base);
        return;
    }
    const double t = xfSeconds_ > 0.0 ? xfElapsed_ / xfSeconds_ : 1.0;
    double out = base * Crossfade::outgoingGain(t);
    double in  = base * Crossfade::incomingGain(t);
    if (mpv) mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &out);
    mpv_set_property(xfIncoming_, "volume", MPV_FORMAT_DOUBLE, &in);
}

void MpvWidget::beginCrossfade(const QString& url, double seconds, const StreamHeaders::Headers& headers)
{
    if (xfIncoming_ || url.isEmpty() || seconds <= 0.0 || !mpv) return;
    // The incoming deck is whichever one is NOT playing. First time through that means creating the second
    // deck; after a handover the primary is the idle one and takes its turn back.
    mpv_handle* incoming = (mpv == mpvPrimary_) ? ensureSecondDeck() : mpvPrimary_;
    if (!incoming || incoming == mpv) return;   // no second deck available: the boundary is simply not faded

    MpvHeaderApply::apply(incoming, headers);   // symmetry with play(); a local music queue carries none
    // A replace-load, so the incoming deck cannot inherit a playlist from its previous turn — otherwise mpv
    // would run on into whatever that turn had appended once this track finished.
    double normalSpeed = 1.0;
    mpv_set_property(incoming, "speed", MPV_FORMAT_DOUBLE, &normalSpeed);
    int unpause = 0;
    mpv_set_property(incoming, "pause", MPV_FORMAT_FLAG, &unpause);
    // Silent BEFORE the load, not after: the load is asynchronous, so a deck left at the last window's volume
    // would be audible at full level for however long it takes the first tick to arrive.
    double zero = 0.0;
    mpv_set_property(incoming, "volume", MPV_FORMAT_DOUBLE, &zero);
    const QByteArray u = url.toUtf8();
    const char* cmd[] = { "loadfile", u.constData(), nullptr };
    mpv_command_async(incoming, 0, cmd);

    xfIncoming_ = incoming;
    xfSeconds_  = seconds;
    xfElapsed_  = 0.0;
    xfClock_.start();
    applyDeckVolumes();
    if (xfTimer_) xfTimer_->start();
    videoLog(QStringLiteral("mpv: crossfade - begin, ") + QString::number(seconds, 'f', 1)
             + QStringLiteral("s, incoming deck=") + (incoming == mpvPrimary_ ? QStringLiteral("primary")
                                                                              : QStringLiteral("second")));
}

void MpvWidget::crossfadeTick()
{
    if (!xfIncoming_) { if (xfTimer_) xfTimer_->stop(); return; }
    const double slice = double(xfClock_.restart()) / 1000.0;   // real time since the last tick, always consumed
    // Frozen while paused. The window is a stretch of MUSIC, not of wall-clock time: letting it run on while
    // the user is paused would complete the handover in silence and drop the outgoing track's last seconds.
    // The clock is restarted above rather than here, so the paused interval is discarded instead of arriving
    // as one huge slice the moment playback resumes.
    if (isPaused()) return;
    xfElapsed_ += slice;
    if (xfElapsed_ >= xfSeconds_) { xfElapsed_ = xfSeconds_; applyDeckVolumes(); promoteIncomingDeck(); return; }
    applyDeckVolumes();
}

void MpvWidget::endCrossfadeNow()
{
    if (!xfIncoming_) return;
    // A Next press inside the window. #141 says a skip during a crossfade resolves to the INCOMING track, and
    // that is what this is: the track already fading in is the next track, so finishing the window at once
    // lands on it — one deck playing, at full volume, with the queue advanced by exactly one.
    xfElapsed_ = xfSeconds_;
    videoLog(QStringLiteral("mpv: crossfade - skipped to the incoming track"));
    promoteIncomingDeck();
}

void MpvWidget::cancelCrossfade()
{
    if (!xfIncoming_) return;
    mpv_handle* dropped = xfIncoming_;
    xfIncoming_ = nullptr;                      // cleared FIRST: applyDeckVolumes must see a finished window
    if (xfTimer_) xfTimer_->stop();
    const char* cmd[] = { "stop", nullptr };
    mpv_command_async(dropped, 0, cmd);         // the handle stays; only the file it was decoding goes
    applyDeckVolumes();                         // the outgoing deck comes back to the volume the host asked for
    videoLog(QStringLiteral("mpv: crossfade - cancelled, incoming deck released"));
}

// The handover. After this the incoming deck IS the player and the outgoing one is idle.
void MpvWidget::promoteIncomingDeck()
{
    if (!xfIncoming_) return;
    mpv_handle* incoming = xfIncoming_;
    mpv_handle* outgoing = mpv;
    xfIncoming_ = nullptr;
    if (xfTimer_) xfTimer_->stop();

    mpv = incoming;
    applyDeckVolumes();                         // the new active deck goes to the host's full volume
    const char* stopCmd[] = { "stop", nullptr };
    if (outgoing) mpv_command_async(outgoing, 0, stopCmd); // release the finished track's decoder + AO

    // The overlay follows the deck: the file now playing is on a context whose media-title arrived while it
    // was inactive (and was discarded, like everything else from an inactive deck).
    hasVideo_ = false;
    char* t = mpv_get_property_string(mpv, "media-title");
    mediaTitle_ = t ? QString::fromUtf8(t) : QString();
    if (t) mpv_free(t);
    refreshNowPlaying();

    // How much of the ramp actually ran is logged, not just that a handover happened: a window that keeps
    // ending well short of its length means the outgoing track is running out first, which is a symptom of the
    // trigger being late rather than of anything wrong here, and it is invisible from a bare "promoted".
    videoLog(QStringLiteral("mpv: crossfade - promoted, active deck=")
             + (mpv == mpvPrimary_ ? QStringLiteral("primary") : QStringLiteral("second"))
             + QStringLiteral(" after ") + QString::number(xfElapsed_, 'f', 2)
             + QStringLiteral("s of a ") + QString::number(xfSeconds_, 'f', 2) + QStringLiteral("s ramp"));
    // ORDER MATTERS. The host advances its queue on this signal, which re-keys the resume, the per-track
    // segment state and the now-playing card to the incoming track. announceActiveDeck() then replays the
    // file facts for that track — and those handlers read the very keys the advance just set, so the advance
    // has to have happened first. This is the same ordering the gapless playlist-pos boundary has.
    emit crossfadePromoted();
    announceActiveDeck();
}

// Re-announce the newly active deck's file, because the host never saw any of it: duration, position, chapter
// count and file-loaded all fired while this deck was the inactive one and were dropped on the floor there.
// Without this the transport keeps the finished track's length, the progress bar runs against the wrong
// total, and the per-file choke point (sync offsets, remembered speed, ReplayGain) never runs for the track
// that is now playing.
void MpvWidget::announceActiveDeck()
{
    if (!mpv) return;
    double dur = 0.0, pos = 0.0;
    mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
    mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    int64_t chapters = 0;
    mpv_get_property(mpv, "chapters", MPV_FORMAT_INT64, &chapters);
    if (dur > 0.0) emit durationChanged(dur);
    emit chapterCountChanged(static_cast<int>(chapters));
    if (pos > 0.0) emit positionChanged(pos);
    // A crossfade only ever runs between MUSIC tracks, so both of these are settled facts rather than guesses:
    // there is no video track to subtitle and no subtitle to go looking for.
    emit fileLoaded(/*hasUsableSubtitle*/ false, /*isVideo*/ false);
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
