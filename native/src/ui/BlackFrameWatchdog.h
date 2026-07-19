#pragma once

#include <QObject>
#include <QImage>
#include <functional>

class QTimer;

// A debug-gated self-heal for the user's intermittent "all-black app" state: once a second it samples a tiny
// downscaled grab of the window and, if the frame is (near-)entirely black in a context where it never should
// be, logs WHERE it happened and — on a SECOND consecutive black frame — asks the host to kick the renderer
// back to life. Created ONLY under the UI-test/debug gate (MMV_UITEST / --uitest / Settings ▸ Debug), so it
// costs nothing in a normal run.
//
// The classifier isBlack() is a pure, side-effect-free static so the probe can pin its judgment exhaustively.
class BlackFrameWatchdog : public QObject
{
    Q_OBJECT
public:
    // sampler: returns a (cheap, downscaled) snapshot of the current window frame — the host hands us
    //          MainWindow::grab().scaled(64,36), which renders even when occluded (software backend).
    // skip:    returns true when a black frame is EXPECTED and must be ignored (in content — a game/video/
    //          reader —, mid launch-handoff, or an esc-menu transition). Consulted on every tick; a skipped
    //          tick also resets the consecutive counter, so a legit black view never primes a false recovery.
    BlackFrameWatchdog(std::function<QImage()> sampler, std::function<bool()> skip, QObject* parent = nullptr);

    void start();   // begin the 1000 ms sampling timer
    void stop();

    int consecutive() const { return consecutive_; }

    // Pure classifier: true iff at least `threshold` (default 0.99) of the pixels have Rec.601 luma < 16.
    // A null/empty image returns false — a FAILED grab must never be classified as a black frame. At exactly
    // the threshold the (inclusive) >= classifies the frame as BLACK.
    static bool isBlack(const QImage& img, double threshold = 0.99);

signals:
    // Emitted on EVERY detected black frame (not just the recovery one); `consecutive` is the run length so
    // the host can act on the 2nd. The host logs on every emission and fires the recovery kick on consec==2.
    void blackFrameDetected(int consecutive);

private:
    void tick();

    std::function<QImage()> sampler_;
    std::function<bool()>   skip_;
    QTimer* timer_ = nullptr;
    int consecutive_ = 0;
};
