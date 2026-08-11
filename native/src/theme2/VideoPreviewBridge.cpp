#include "VideoPreviewBridge.h"

#include <QtGlobal>

VideoPreviewBridge& VideoPreviewBridge::instance()
{
    static VideoPreviewBridge s;
    return s;
}

void VideoPreviewBridge::setEnabled(bool on)
{
    if (enabled_ == on) return;
    enabled_ = on;
    emit changed();
}

void VideoPreviewBridge::setVolume(int pct)
{
    const int v = qBound(0, pct, 100);
    if (volume_ == v) return;
    volume_ = v;
    emit changed();
}

void VideoPreviewBridge::reportAudible(bool on)
{
    if (on)
    {
        if (++audibleCount_ == 1) emit duckRequested(true);   // 0 -> 1: first audible snap, duck the BGM
    }
    else
    {
        if (audibleCount_ <= 0) return;                        // never underflow (a stray false is a no-op)
        if (--audibleCount_ == 0) emit duckRequested(false);   // 1 -> 0: last audible snap gone, restore the BGM
    }
}
