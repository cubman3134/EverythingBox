// hwdecframe — the MEASUREMENT tool behind issue #229 ("streams play as dense coloured speckle").
//
// It answers one question no headless probe can: for a given `hwdec` string, what does the decoder actually
// hand the OpenGL render path, and does the resulting picture match a software decode of the same frame? The
// suspicion in #229 is that mpv's "auto-safe" resolves to nvdec DIRECT (CUDA<->GL interop) on an NVIDIA
// machine rather than to a copy-back decoder, and that the interop is what corrupts the picture. "It looks
// wrong on screen" is not a measurement, so this tool renders one exact frame per hwdec mode and prints a
// pixel difference against a reference PNG (the software decode of that same frame).
//
// FIDELITY. The render path here is copied line-for-line from src/video/MpvWidget.cpp: the same QOpenGLWidget
// subclass, `vo=libmpv`, MPV_RENDER_API_TYPE_OPENGL with mpv_opengl_init_params{getProcAddress}, and a
// paintGL that renders into defaultFramebufferObject() with flip_y=1. What it deliberately does NOT copy is
// everything unrelated to decoding (subtitles, audio output, caches, the deck model), so a difference between
// two runs is attributable to the one option that changed. It is NOT part of the headless suite: it needs a
// GPU, a window and a media file, none of which CI has.
//
// USAGE
//   hwdecframe --file <path-or-url> --hwdec <mpv hwdec string> [--at <sec>] [--out <png>] [--ref <png>]
//              [--play <sec>] [--size WxH]
//
//   --at    seek here before grabbing (exact, via mpv's `start` + hr-seek), default 30
//   --play  after grabbing, un-pause for this many seconds and report the throughput counters
//           (frame-drop-count / vo-delayed-frame-count) plus this process's CPU time. Default 0 = skip.
//   --ref   compare the grabbed frame against this PNG: mean absolute error per channel, and the share of
//           pixels off by more than 24/255 on any channel ("bad"). A clean decode of the same frame lands
//           near zero on both; speckle corruption does not.
//
// Prints HWDECFRAME: <fields> (the same fields MpvWidget::logVideoInfo() logs, plus the comparison) and
// exits 0. A hard failure prints HWDECFRAME-FAIL <what> and exits 1.
#include <QApplication>
#include <QOpenGLWidget>
#include <QOpenGLContext>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Seconds of CPU this process has burned (user+kernel, all threads). #229's fix trades one frame copy per
// frame for correctness; the honest unit for that trade is CPU time, not an assertion.
static double processCpuSeconds()
{
#ifdef Q_OS_WIN
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return -1.0;
    auto to100ns = [](const FILETIME& f) {
        return (static_cast<quint64>(f.dwHighDateTime) << 32) | quint64(f.dwLowDateTime);
    };
    return double(to100ns(k) + to100ns(u)) / 1e7;
#else
    return -1.0;
#endif
}

class FrameWidget : public QOpenGLWidget
{
public:
    // `extra` is applied verbatim, in order, straight after the hwdec string: it is how a caller reproduces
    // the REST of MpvWidget's option set (--appopts) or bisects one option at a time (--opt name=value).
    FrameWidget(const QString& hwdec, const QStringList& extra)
    {
        mpv_ = mpv_create();
        if (!mpv_) { std::fprintf(stderr, "HWDECFRAME-FAIL mpv_create\n"); std::exit(1); }
        // The two options that make this the app's path: render through the libmpv render API, and the hwdec
        // string under test. Everything else stays at mpv's defaults on purpose.
        mpv_set_option_string(mpv_, "vo", "libmpv");
        mpv_set_option_string(mpv_, "hwdec", hwdec.toUtf8().constData());
        mpv_set_option_string(mpv_, "audio", "no");     // no AO: this measures pictures
        mpv_set_option_string(mpv_, "sub", "no");       // and only the decoded picture, never an overlay
        mpv_set_option_string(mpv_, "keep-open", "yes");
        mpv_set_option_string(mpv_, "pause", "yes");    // hold on the seeked-to frame until we say otherwise
        mpv_set_option_string(mpv_, "hr-seek", "yes");  // land on the EXACT frame, so two modes are comparable
        for (const QString& kv : extra)
        {
            const int eq = kv.indexOf(QLatin1Char('='));
            if (eq <= 0) continue;
            const QByteArray k = kv.left(eq).toUtf8(), v = kv.mid(eq + 1).toUtf8();
            if (mpv_set_option_string(mpv_, k.constData(), v.constData()) < 0)
                std::fprintf(stderr, "HWDECFRAME: option refused: %s\n", kv.toUtf8().constData());
        }
        // EB_HWDECFRAME_VERBOSE=1 turns on libmpv's own terminal log at -v. When a mode produces NO frame at
        // all, mpv's decoder-init lines are the only thing that says whether the decoder was refused, fell
        // back, or died - and a tool that can only report "no frame" cannot tell those apart.
        if (qEnvironmentVariableIsSet("EB_HWDECFRAME_VERBOSE"))
        {
            mpv_set_option_string(mpv_, "terminal", "yes");
            mpv_set_option_string(mpv_, "msg-level", "all=v");
        }
        if (mpv_initialize(mpv_) < 0) { std::fprintf(stderr, "HWDECFRAME-FAIL mpv_initialize\n"); std::exit(1); }
        mpv_set_wakeup_callback(mpv_, wakeup, this);
    }

    ~FrameWidget() override
    {
        makeCurrent();
        if (rctx_) mpv_render_context_free(rctx_);
        rctx_ = nullptr;
        doneCurrent();
        if (mpv_) mpv_terminate_destroy(mpv_);
    }

    QString prop(const char* name) const
    {
        char* s = mpv_get_property_string(mpv_, name);
        QString r = s ? QString::fromUtf8(s) : QStringLiteral("?");
        if (s) mpv_free(s);
        return r;
    }
    void setProp(const char* name, const char* value) { mpv_set_property_string(mpv_, name, value); }

    void load(const QString& url, double at)
    {
        if (at > 0.0)
            mpv_set_option_string(mpv_, "start", QByteArray::number(at, 'f', 3).constData());
        const QByteArray u = url.toUtf8();
        const char* cmd[] = { "loadfile", u.constData(), nullptr };
        mpv_command_async(mpv_, 0, cmd);
    }

    bool fileLoaded() const { return loaded_; }
    bool failed() const { return failed_; }
    int framesRendered() const { return frames_; }

    // Pump mpv's event queue. Called from the wakeup callback (queued onto this thread) and from the poll.
    void drainEvents()
    {
        if (!mpv_) return;
        while (mpv_event* ev = mpv_wait_event(mpv_, 0))
        {
            if (ev->event_id == MPV_EVENT_NONE) break;
            if (ev->event_id == MPV_EVENT_FILE_LOADED) loaded_ = true;
            if (ev->event_id == MPV_EVENT_END_FILE)
            {
                auto* ef = static_cast<mpv_event_end_file*>(ev->data);
                if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) failed_ = true;
            }
        }
    }

protected:
    // Identical to MpvWidget::getProcAddress.
    static void* getProcAddress(void* ctx, const char* name)
    {
        Q_UNUSED(ctx);
        QOpenGLContext* glctx = QOpenGLContext::currentContext();
        if (!glctx) return nullptr;
        return reinterpret_cast<void*>(glctx->getProcAddress(QByteArray(name)));
    }

    void initializeGL() override
    {
        mpv_opengl_init_params gl_init_params{ getProcAddress, this };
        mpv_render_param params[]{
            { MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
            { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params },
            { MPV_RENDER_PARAM_INVALID, nullptr }
        };
        if (mpv_render_context_create(&rctx_, mpv_, params) < 0)
        { std::fprintf(stderr, "HWDECFRAME-FAIL render_context_create\n"); std::exit(1); }
        mpv_render_context_set_update_callback(rctx_, redraw, this);
    }

    void paintGL() override
    {
        if (!rctx_) return;
        mpv_opengl_fbo mpfbo{ static_cast<int>(defaultFramebufferObject()),
                              static_cast<int>(width() * devicePixelRatioF()),
                              static_cast<int>(height() * devicePixelRatioF()), 0 };
        int flip_y{ 1 };
        mpv_render_param params[]{
            { MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo },
            { MPV_RENDER_PARAM_FLIP_Y, &flip_y },
            { MPV_RENDER_PARAM_INVALID, nullptr }
        };
        mpv_render_context_render(rctx_, params);
        ++frames_;
    }

private:
    // No Q_OBJECT/moc: Qt6's functor overload of invokeMethod marshals onto this object's thread just as a
    // queued slot call would, and this tool has no signals of its own.
    static void wakeup(void* ctx)
    {
        auto* self = static_cast<FrameWidget*>(ctx);
        QMetaObject::invokeMethod(self, [self] { self->drainEvents(); }, Qt::QueuedConnection);
    }
    static void redraw(void* ctx)
    {
        auto* self = static_cast<FrameWidget*>(ctx);
        QMetaObject::invokeMethod(self, [self] { self->update(); }, Qt::QueuedConnection);
    }

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* rctx_ = nullptr;
    bool loaded_ = false;
    bool failed_ = false;
    int frames_ = 0;
};

// Spin the Qt event loop until `pred` holds or `ms` elapse. Returns whether it held.
static bool waitFor(FrameWidget* w, int ms, const std::function<bool()>& pred)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms)
    {
        w->drainEvents();
        if (pred()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    w->drainEvents();
    return pred();
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCommandLineParser p;
    p.addOption({ "file",  "media path or url", "path" });
    p.addOption({ "hwdec", "mpv hwdec string",  "s", "no" });
    p.addOption({ "at",    "seek seconds",      "sec", "30" });
    p.addOption({ "out",   "write frame PNG",   "png" });
    p.addOption({ "ref",   "reference PNG",     "png" });
    p.addOption({ "play",  "seconds to play after the grab", "sec", "0" });
    p.addOption({ "size",  "window size WxH",   "WxH", "1280x720" });
    p.addOption({ "opt",  "extra mpv option, name=value (repeatable)", "name=value" });
    p.addOption({ "appopts", "also apply the option set MpvWidget applies at its defaults" });
    p.addHelpOption();
    p.process(app);

    const QString file = p.value("file");
    if (file.isEmpty()) { std::fprintf(stderr, "HWDECFRAME-FAIL no --file\n"); return 1; }
    const QString hwdec = p.value("hwdec");
    const double at = p.value("at").toDouble();
    const double playSec = p.value("play").toDouble();
    int W = 1280, H = 720;
    { const QStringList wh = p.value("size").split('x'); if (wh.size() == 2) { W = wh[0].toInt(); H = wh[1].toInt(); } }

    // The rest of what src/video/MpvWidget.cpp sets on the primary deck at shipped defaults (refresh-sync
    // on, HDR mode tone-map), so a run can ask whether the corruption needs one of THOSE and not just the
    // hwdec string. Kept as a literal list rather than by linking MpvWidget: the widget drags the whole
    // app in, and a list that has to be read next to the widget is a list somebody will check.
    QStringList extra = p.values("opt");
    if (p.isSet("appopts"))
        extra << QStringLiteral("cache=yes") << QStringLiteral("demuxer-max-bytes=512MiB")
              << QStringLiteral("demuxer-max-back-bytes=128MiB") << QStringLiteral("cache-secs=120")
              << QStringLiteral("cache-pause-wait=2") << QStringLiteral("network-timeout=60")
              << QStringLiteral("stream-lavf-o=reconnect=1,reconnect_streamed=1,reconnect_delay_max=30")
              << QStringLiteral("volume-max=200") << QStringLiteral("sub-auto=fuzzy")
              << QStringLiteral("video-sync=display-resync")
              << QStringLiteral("tone-mapping=bt.2446a") << QStringLiteral("hdr-compute-peak=yes")
              << QStringLiteral("target-colorspace-hint=no");

    FrameWidget w(hwdec, extra);
    w.resize(W, H);
    w.show();
    // The GL context (and therefore the mpv render context) only exists once the widget has been painted.
    if (!waitFor(&w, 10000, [&] { return w.framesRendered() > 0 || w.isVisible(); }))
    { std::fprintf(stderr, "HWDECFRAME-FAIL window never came up\n"); return 1; }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);

    w.load(file, at);
    if (!waitFor(&w, 30000, [&] { return w.fileLoaded() || w.failed(); }) || w.failed())
    { std::fprintf(stderr, "HWDECFRAME-FAIL load (%s)\n", file.toUtf8().constData()); return 1; }

    // Let the decoder settle on the seeked-to frame and the render path draw it several times. Paused, so
    // every draw is the SAME frame — which is what makes the cross-mode pixel comparison meaningful.
    // Wait for the decoder to have PRODUCED a frame (video-params only exist once one has) and for the
    // render path to have drawn at least once. Counting several draws does not work: paused on one frame,
    // mpv signals an update only when it has something new, and how many times that is differs per hwdec
    // mode - an earlier version of this tool waited for three draws and reported "no frame rendered" for
    // every hardware mode, each of which had in fact decoded and drawn.
    const int framesBefore = w.framesRendered();
    if (!waitFor(&w, 20000, [&] { return w.framesRendered() > framesBefore && w.prop("video-params/w") != QStringLiteral("?"); }))
    { std::fprintf(stderr, "HWDECFRAME-FAIL no frame rendered\n"); return 1; }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 400);
    w.repaint();   // one more draw of the SAME held frame, immediately before the read-back
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);

    const QImage shot = w.grabFramebuffer().convertToFormat(QImage::Format_RGB888);

    QString cmp = QStringLiteral(" ref=none");
    if (!p.value("ref").isEmpty())
    {
        QImage ref(p.value("ref"));
        ref = ref.convertToFormat(QImage::Format_RGB888);
        if (ref.isNull() || ref.size() != shot.size())
            cmp = QStringLiteral(" ref=UNUSABLE");
        else
        {
            double sum = 0.0; qint64 bad = 0; const qint64 n = qint64(shot.width()) * shot.height();
            for (int y = 0; y < shot.height(); ++y)
            {
                const uchar* a = shot.constScanLine(y);
                const uchar* b = ref.constScanLine(y);
                for (int x = 0; x < shot.width(); ++x)
                {
                    int d0 = std::abs(int(a[x * 3 + 0]) - int(b[x * 3 + 0]));
                    int d1 = std::abs(int(a[x * 3 + 1]) - int(b[x * 3 + 1]));
                    int d2 = std::abs(int(a[x * 3 + 2]) - int(b[x * 3 + 2]));
                    sum += (d0 + d1 + d2) / 3.0;
                    if (d0 > 24 || d1 > 24 || d2 > 24) ++bad;
                }
            }
            cmp = QStringLiteral(" mae=%1 badpix=%2%")
                      .arg(sum / double(n), 0, 'f', 3)
                      .arg(100.0 * double(bad) / double(n), 0, 'f', 2);
        }
    }

    // Throughput sample: un-pause and play, then read the counters mpv keeps for dropped and late frames.
    QString perf;
    if (playSec > 0.0)
    {
        const double cpu0 = processCpuSeconds();
        const int f0 = w.framesRendered();
        w.setProp("pause", "no");
        QElapsedTimer t; t.start();
        waitFor(&w, int(playSec * 1000), [&] { return false; });
        const double wall = t.elapsed() / 1000.0;
        const double cpu = processCpuSeconds() - cpu0;
        w.setProp("pause", "yes");
        perf = QStringLiteral(" played=%1s renderedFrames=%2 drops=%3 delayed=%4 cpu=%5s cpuPerSec=%6")
                   .arg(wall, 0, 'f', 1).arg(w.framesRendered() - f0)
                   .arg(w.prop("frame-drop-count")).arg(w.prop("vo-delayed-frame-count"))
                   .arg(cpu, 0, 'f', 2).arg(cpu / (wall > 0 ? wall : 1), 0, 'f', 3);
    }

    if (!p.value("out").isEmpty()) shot.save(p.value("out"), "PNG");

    // The same fields MpvWidget::logVideoInfo() writes to stream_debug.log, so a run here is directly
    // comparable with a line from a real session's log.
    std::printf("HWDECFRAME: requested=%s codec='%s' %sx%s pixfmt=%s hwdec-current=%s fps=%s transfer=%s%s%s\n",
                hwdec.toUtf8().constData(),
                w.prop("video-codec").toUtf8().constData(),
                w.prop("video-params/w").toUtf8().constData(),
                w.prop("video-params/h").toUtf8().constData(),
                w.prop("video-params/pixelformat").toUtf8().constData(),
                w.prop("hwdec-current").toUtf8().constData(),
                w.prop("container-fps").toUtf8().constData(),
                w.prop("video-params/gamma").toUtf8().constData(),
                cmp.toUtf8().constData(), perf.toUtf8().constData());
    std::fflush(stdout);
    return 0;
}
