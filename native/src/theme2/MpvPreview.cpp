#include "MpvPreview.h"
#include "VideoPreviewBridge.h"

#include <QMetaObject>
#include <QPainter>
#include <QUrl>

#include <mpv/client.h>
#include <mpv/render.h>

MpvPreview::MpvPreview(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    setRenderTarget(QQuickPaintedItem::Image); // software-backend friendly (QPainter into a QImage)

    mpv_ = mpv_create();
    if (!mpv_) return;
    mpv_set_option_string(mpv_, "vo", "libmpv");   // output through the render API, not a window
    mpv_set_option_string(mpv_, "hwdec", "no");    // software decode: correct + easily handles a small preview
    // Audio is DECODED but starts muted (issue #55): the snap is silent by default (volume 0) and only becomes
    // audible when the user raises the snap volume — applyVolume() drives mpv's mute+volume. The old "aid=no"
    // disabled the audio stream outright, so raising the volume then did nothing without reloading the clip.
    mpv_set_option_string(mpv_, "mute", "yes");
    mpv_set_option_string(mpv_, "volume", "0");
    mpv_set_option_string(mpv_, "loop-file", "inf");     // loop the trailer while the item stays selected
    mpv_set_option_string(mpv_, "loop-playlist", "inf");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "cache", "yes");         // smooth a streamed url
    mpv_set_option_string(mpv_, "network-timeout", "30");
    mpv_set_option_string(mpv_, "osc", "no");
    mpv_set_option_string(mpv_, "terminal", "no");
    mpv_set_option_string(mpv_, "config", "no");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    if (mpv_initialize(mpv_) < 0) { mpv_terminate_destroy(mpv_); mpv_ = nullptr; return; }

    mpv_render_param params[]{
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW) },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_create(&rctx_, mpv_, params) < 0) { rctx_ = nullptr; return; }
    mpv_render_context_set_update_callback(rctx_, &MpvPreview::onMpvRedraw, this);
    mpv_set_wakeup_callback(mpv_, &MpvPreview::onMpvWakeup, this);
    applyVolume(); // honour any volume set before init (mute at the default 0)
}

MpvPreview::~MpvPreview()
{
    // A player torn down while an audible snap was on screen must release the BGM duck it was holding, or the
    // music would stay paused after the item is gone.
    if (audible_) { audible_ = false; VideoPreviewBridge::instance().reportAudible(false); }
    if (rctx_) { mpv_render_context_free(rctx_); rctx_ = nullptr; } // free the render context before the handle
    if (mpv_)  { mpv_terminate_destroy(mpv_);   mpv_ = nullptr; }
}

void MpvPreview::setSource(const QString& s)
{
    if (s == source_) return;
    source_ = s;
    emit sourceChanged();
    setPlaying(false);
    setFailed(false);
    frame_ = QImage();
    update();
    if (!mpv_) return;
    if (s.isEmpty())
    {
        const char* cmd[]{ "stop", nullptr };
        mpv_command_async(mpv_, 0, cmd);
        return;
    }
    // mpv wants a native path or a real URL. A file:// url (with %20-escapes, or unescaped spaces) is handed
    // back as a plain local path so filenames with spaces/parentheses (RetroBat media) open correctly.
    QString target = s;
    const QUrl url(s);
    if (url.isLocalFile()) target = url.toLocalFile();
    const QByteArray u = target.toUtf8();
    const char* cmd[]{ "loadfile", u.constData(), nullptr };
    mpv_command_async(mpv_, 0, cmd);
}

void MpvPreview::onMpvRedraw(void* ctx)
{
    QMetaObject::invokeMethod(static_cast<MpvPreview*>(ctx), "renderFrame", Qt::QueuedConnection);
}

void MpvPreview::onMpvWakeup(void* ctx)
{
    QMetaObject::invokeMethod(static_cast<MpvPreview*>(ctx), "drainEvents", Qt::QueuedConnection);
}

void MpvPreview::renderFrame()
{
    if (!rctx_) return;
    const uint64_t flags = mpv_render_context_update(rctx_);
    // A cleared source must STAY blank: the render thread's redraw queued before the async "stop" lands
    // would otherwise re-paint the previous clip's last frame here (and flip `playing` back on), ghosting
    // the old item's video over the next item's artwork. Acknowledge the update, drop the frame.
    if (source_.isEmpty())
    {
        if (!frame_.isNull()) { frame_ = QImage(); update(); }
        return;
    }
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) return;

    const int w = qMax(1, int(width()));
    const int h = qMax(1, int(height()));
    if (frame_.width() != w || frame_.height() != h)
        frame_ = QImage(w, h, QImage::Format_RGB32); // OPAQUE (0xffRRGGBB): memory B,G,R,x == mpv's "bgr0"

    // Format_RGB32 is the opaque battle-tested format; using an *X8888 alpha format here rendered the frame
    // transparent (mpv leaves the pad byte 0), which composited as white over the opaque backing.
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
    if (mpv_render_context_render(rctx_, params) >= 0)
    {
        if (!playing_) setPlaying(true); // first real frame -> the still underneath can fade out
        update();
    }
}

void MpvPreview::drainEvents()
{
    while (mpv_)
    {
        mpv_event* e = mpv_wait_event(mpv_, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        // loop-file keeps a good clip going, so END_FILE here is the failure path: a url that 404s, an
        // unsupported codec, or a dead stream ends with an error before any frame rendered. Surface it so
        // the QML side can drop its "trailer loading" cue instead of showing a play badge forever.
        if (e->event_id == MPV_EVENT_END_FILE && !source_.isEmpty() && !playing_)
        {
            const auto* ef = static_cast<mpv_event_end_file*>(e->data);
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) setFailed(true);
        }
    }
}

void MpvPreview::paint(QPainter* p)
{
    if (frame_.isNull()) return; // no frame yet: the Ken Burns still shows through from behind
    // Draw a copy in the painter's native format. The software QQuickPaintedItem painter silently no-ops a
    // raw RGB32 blit here; converting to ARGB32_Premultiplied (opaque, since mpv's bgr0 -> RGB32 is opaque)
    // is what actually renders. Cheap for a small preview.
    p->drawImage(boundingRect(), frame_.convertToFormat(QImage::Format_ARGB32_Premultiplied));
}

void MpvPreview::setVolume(int v)
{
    v = qBound(0, v, 100);
    if (v == volume_) return;
    volume_ = v;
    applyVolume();
    emit volumeChanged();
    updateAudible(); // crossing 0 changes whether this snap fights the BGM
}

void MpvPreview::applyVolume()
{
    if (!mpv_) return;
    int mute = volume_ > 0 ? 0 : 1;      // 0 volume == muted, and never ducks the BGM
    mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &mute);
    double vol = qBound(0, volume_, 100);
    mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void MpvPreview::updateAudible()
{
    // A snap is "audible" only when it is actually painting frames AND its volume is above 0. Reported to the
    // bridge on transitions only; the bridge reference-counts and asks MainWindow to duck the BGM.
    const bool a = playing_ && volume_ > 0;
    if (a == audible_) return;
    audible_ = a;
    VideoPreviewBridge::instance().reportAudible(a);
}

void MpvPreview::setPlaying(bool v)
{
    if (playing_ == v) return;
    playing_ = v;
    emit playingChanged();
    updateAudible(); // stopping (or starting) a clip changes its audible state -> restore/duck the BGM
}

void MpvPreview::setFailed(bool v)
{
    if (failed_ == v) return;
    failed_ = v;
    emit failedChanged();
}
