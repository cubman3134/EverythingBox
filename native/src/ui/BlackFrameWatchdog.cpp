#include "BlackFrameWatchdog.h"

#include <QTimer>

BlackFrameWatchdog::BlackFrameWatchdog(std::function<QImage()> sampler, std::function<bool()> skip, QObject* parent)
    : QObject(parent), sampler_(std::move(sampler)), skip_(std::move(skip))
{
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &BlackFrameWatchdog::tick);
}

void BlackFrameWatchdog::start() { if (timer_) timer_->start(); }
void BlackFrameWatchdog::stop()  { if (timer_) timer_->stop(); }

void BlackFrameWatchdog::tick()
{
    // A context where a black frame is expected (in a game/video/reader, mid launch-handoff, esc transition):
    // ignore it AND reset the run, so a legitimately black view never primes a false recovery on exit.
    if (skip_ && skip_()) { consecutive_ = 0; return; }

    const QImage frame = sampler_ ? sampler_() : QImage();
    // A failed grab (null image) is NOT a black frame — isBlack() returns false for it, so this both avoids a
    // false positive and resets the run.
    if (isBlack(frame))
    {
        ++consecutive_;
        emit blackFrameDetected(consecutive_);
    }
    else
    {
        consecutive_ = 0;
    }
}

bool BlackFrameWatchdog::isBlack(const QImage& img, double threshold)
{
    // A failed grab yields a null/empty image — that is NOT a black frame. Classifying it black would fire a
    // phantom recovery on the very failure mode (an unrenderable frame) least likely to be a real blackout.
    if (img.isNull() || img.width() <= 0 || img.height() <= 0)
        return false;

    // Read as 32-bit so scanlines are plain QRgb; convert once if the sampler handed us another format.
    const QImage im = (img.format() == QImage::Format_ARGB32 || img.format() == QImage::Format_RGB32)
                          ? img
                          : img.convertToFormat(QImage::Format_ARGB32);

    const qint64 total = qint64(im.width()) * qint64(im.height());
    qint64 dark = 0;
    for (int y = 0; y < im.height(); ++y)
    {
        const QRgb* row = reinterpret_cast<const QRgb*>(im.constScanLine(y));
        for (int x = 0; x < im.width(); ++x)
        {
            const QRgb p = row[x];
            // Rec.601 luma with integer weights summing to 256 (>>8): a solid gray g maps to luma g exactly.
            const int luma = (77 * qRed(p) + 150 * qGreen(p) + 29 * qBlue(p)) >> 8;
            if (luma < 16) ++dark;
        }
    }
    // Inclusive: exactly `threshold` of the pixels dark still reads as black (documented in the probe's edge case).
    return double(dark) >= threshold * double(total);
}
