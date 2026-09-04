#pragma once
#include <QPointF>
#include <QSizeF>
#include <QVector>
#include <QtGlobal>
#include <QtMath>
#include <cmath>

// The video player's touch gesture vocabulary (issue #162), as a PURE recogniser: touch points and a clock
// go in, gesture events come out. No QWidget, no QTouchEvent, no Settings, no mpv — QtCore geometry only, so
// probe_playergestures drives every rule headlessly with synthetic sequences and an injected millisecond
// clock. The host (MainWindowGestures.cpp) owns the translation: it feeds real QTouchEvent points in and
// turns the events back out into volume/brightness/seek/speed calls.
//
// Two gates are enforced HERE rather than at each call site, because "the TV path is untouched by
// construction" has to be a property of the recogniser and not of the discipline of its caller:
//
//   * Config::enabled is the FORM-FACTOR gate. PlayerGestureConfig.h sets it from FormFactor (touch only);
//     with it false, begin() claims nothing and no rule can ever produce an event. A desktop or TV build
//     therefore behaves exactly as it did before this file existed.
//   * setOverlayOpen(true) is the "never fight a menu" gate, and setLocked(true) is the overlay's lock
//     toggle (pocket protection). Both suppress EVERY family; locked additionally reports a bare
//     Kind::LockedTap so the host can surface the unlock affordance without reviving any other gesture.
//
// A gesture that starts inside Config::edgeInsetPx of any window edge is inert for its whole sequence: that
// band belongs to the OS's own back/notification swipes, and half-recognising one is worse than ignoring it.
namespace PlayerGestures
{

// How the video is fitted to the viewport; the pinch cycles through it (issue #162's "fit / fill / stretch").
enum class VideoFit { Fit = 0, Fill = 1, Stretch = 2 };

// Pinch OUT (fingers apart) walks the cycle forward, pinch IN walks it back. Pure, and total: every input
// maps to a defined output, so a stored value from a future build cannot wedge the cycle.
inline VideoFit cycleFit(VideoFit cur, bool out)
{
    const int n = 3;
    int i = int(cur);
    if (i < 0 || i >= n) i = 0;
    i = out ? (i + 1) % n : (i + n - 1) % n;
    return VideoFit(i);
}

enum class Kind
{
    None = 0,
    TapPending,        // a lone tap: the host must DEFER it (a second tap inside doubleTapMs makes it a skip)
    DoubleTapLeft,     // value = -jumpSeconds
    DoubleTapCentre,   // value = 0 (play/pause)
    DoubleTapRight,    // value = +jumpSeconds
    VolumeDelta,       // value = change in points, target = the new 0..100 level
    BrightnessDelta,   // value = change in points, target = the new 0..100 level
    SeekPreview,       // value = signed seconds from where the swipe started, target = absolute position
    SeekCommit,        // the release: value/target as above, and the host performs the seek
    SeekCancel,        // the finger came back to where it started: nothing moves
    LongPressBegin,    // value = the held speed multiplier (2.0)
    LongPressEnd,      // restore the speed the host had before LongPressBegin
    PinchIn,           // target = the VideoFit the cycle lands on, as an int
    PinchOut,
    LockedTap          // a tap while the lock is engaged: every gesture stayed suppressed
};

struct Event
{
    Kind   kind   = Kind::None;
    double value  = 0.0;
    double target = 0.0;
};

struct Config
{
    // The form-factor gate. False (the desktop/TV default) makes the whole recogniser inert.
    bool enabled = false;

    // Per-family switches — issue #162's "configurable and disable-able", one Settings key each.
    bool volume     = true;
    bool brightness = true;
    bool seek       = true;
    bool doubleTap  = true;
    bool longPress  = true;
    bool pinch      = true;

    int  edgeInsetPx   = 24;   // the OS's reserved edge band; a touch starting inside it is ignored entirely
    int  doubleTapMs   = 350;  // the second tap must land within this of the first
    int  longPressMs   = 500;  // hold this long without travelling to trigger the temporary speed-up
    int  tapSlopPx     = 24;   // travel that stops a press being a tap / a long-press
    int  swipeStartPx  = 32;   // travel before an undecided press commits to an axis
    int  doubleTapRadiusPx = 96; // how far apart two taps may land and still be one double-tap

    int    jumpSeconds       = 10;    // #140's shared interval; the host injects it, this file never reads it
    double seekFullSwipeSec  = 120.0; // a swipe across the FULL width scrubs this many seconds
    double volumeFullSwipeFrac = 0.6; // a swipe up this fraction of the height spans the whole 0..100 range
    double holdSpeed         = 2.0;   // the long-press multiplier
    double durationSec       = 0.0;   // media length; 0 = unknown, and then the seek target is not clamped
};

class Recognizer
{
public:
    void setConfig(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }

    void setViewport(const QSizeF& s) { view_ = s; }
    QSizeF viewport() const { return view_; }

    void setLocked(bool l) { locked_ = l; }
    bool locked() const { return locked_; }
    void setOverlayOpen(bool o) { overlayOpen_ = o; }
    bool overlayOpen() const { return overlayOpen_; }

    // The levels the recogniser accumulates against. The host pushes the real value in before a gesture and
    // reads the target back out of each event, so nothing here has to know how volume or brightness is applied.
    void setVolume(int pct) { vol_ = qBound(0, pct, 100); }
    int  volume() const { return vol_; }
    void setBrightness(int pct) { bright_ = qBound(0, pct, 100); }
    int  brightness() const { return bright_; }
    void setPosition(double sec) { pos_ = sec; }
    double position() const { return pos_; }

    // True while the host must CONSUME the touch event it just fed in (so it cannot also arrive as a
    // synthesized mouse press). False for an inert sequence — one that started in the edge band, or whose
    // family is switched off — which is exactly what lets the untouched paths keep working.
    bool claimed() const { return claimed_; }

    QVector<Event> begin(const QVector<QPointF>& pts, qint64 tMs)
    {
        reset();
        if (!cfg_.enabled || overlayOpen_ || pts.isEmpty()) { phase_ = Phase::Idle; return {}; }
        startMs_ = tMs;
        start_ = pts.first();
        last_  = start_;
        if (locked_) { phase_ = Phase::Locked; claimed_ = true; return {}; }
        if (pts.size() >= 2)
        {
            if (!cfg_.pinch) { phase_ = Phase::Inert; return {}; }
            phase_ = Phase::Pinch;
            pinchStart_ = separation(pts);
            claimed_ = true;
            return {};
        }
        if (inEdgeBand(start_)) { phase_ = Phase::Inert; return {}; }
        phase_ = Phase::Undecided;
        claimed_ = true;
        return {};
    }

    QVector<Event> update(const QVector<QPointF>& pts, qint64 tMs)
    {
        QVector<Event> out;
        if (phase_ == Phase::Idle || phase_ == Phase::Inert || phase_ == Phase::Locked) return out;
        if (phase_ == Phase::Pinch)
        {
            if (pts.size() < 2 || pinchFired_ || pinchStart_ <= 0.0) return out;
            const double ratio = separation(pts) / pinchStart_;
            if (ratio >= 1.15)      { pinchFired_ = true; out << fitEvent(true); }
            else if (ratio <= 0.87) { pinchFired_ = true; out << fitEvent(false); }
            return out;
        }
        if (pts.isEmpty()) return out;
        last_ = pts.first();
        const double dx = last_.x() - start_.x();
        const double dy = last_.y() - start_.y();

        if (phase_ == Phase::Undecided)
        {
            if (std::hypot(dx, dy) < cfg_.swipeStartPx) { out << tick(tMs); return out; }
            if (qAbs(dx) >= qAbs(dy)) return beginSeek(dx);
            return beginVertical(dy);
        }
        if (phase_ == Phase::Seek)       { out << seekEvent(Kind::SeekPreview, dx); return out; }
        if (phase_ == Phase::Volume)     { out << levelEvent(Kind::VolumeDelta, dy); return out; }
        if (phase_ == Phase::Brightness) { out << levelEvent(Kind::BrightnessDelta, dy); return out; }
        return out;
    }

    QVector<Event> end(const QVector<QPointF>& pts, qint64 tMs)
    {
        QVector<Event> out;
        const Phase p = phase_;
        if (!pts.isEmpty()) last_ = pts.first();
        phase_ = Phase::Idle;
        switch (p)
        {
        case Phase::Locked:
            // The lock swallowed the gesture. Report only that a tap happened, so the host can reveal the
            // unlock control — no seek, no volume, no chrome.
            if (isTap(tMs)) out << Event{ Kind::LockedTap, 0.0, 0.0 };
            return out;
        case Phase::Seek:
        {
            const double dx = last_.x() - start_.x();
            // Cancel-by-returning: the finger came back to (near) where the scrub started, so the user
            // changed their mind. Nothing is committed and the position is untouched.
            if (qAbs(dx) <= cfg_.tapSlopPx) { out << Event{ Kind::SeekCancel, 0.0, pos_ }; return out; }
            out << seekEvent(Kind::SeekCommit, dx);
            return out;
        }
        case Phase::LongPress:
            out << Event{ Kind::LongPressEnd, 1.0, 0.0 };
            return out;
        case Phase::Undecided:
            if (!isTap(tMs)) return out;
            return tapEvent(tMs);
        case Phase::Volume:
        case Phase::Brightness:
        case Phase::Pinch:
        case Phase::Inert:
        case Phase::Idle:
        default:
            return out;
        }
    }

    // The long-press deadline. The host arms a single-shot timer for Config::longPressMs on begin() and calls
    // this from it; update() calls it too, so a stationary finger that keeps producing move frames still
    // crosses the threshold without the timer.
    QVector<Event> tick(qint64 tMs)
    {
        QVector<Event> out;
        if (phase_ != Phase::Undecided || !cfg_.longPress) return out;
        if (tMs - startMs_ < cfg_.longPressMs) return out;
        if ((last_ - start_).manhattanLength() > cfg_.tapSlopPx) return out;
        phase_ = Phase::LongPress;
        out << Event{ Kind::LongPressBegin, cfg_.holdSpeed, 0.0 };
        return out;
    }

    // Which third of the viewport an x falls in: -1 left, 0 centre, +1 right. Public because the host's
    // ripple indicator wants the same answer the double-tap did.
    int zoneOf(double x) const
    {
        const double w = qMax(1.0, view_.width());
        if (x < w / 3.0)       return -1;
        if (x > 2.0 * w / 3.0) return 1;
        return 0;
    }

    // Right half = volume, left half = brightness. The near-universal convention; see issue #162.
    bool rightHalf(double x) const { return x >= qMax(1.0, view_.width()) / 2.0; }

private:
    enum class Phase { Idle, Undecided, Seek, Volume, Brightness, LongPress, Pinch, Locked, Inert };

    void reset() { claimed_ = false; pinchFired_ = false; pinchStart_ = 0.0; }

    bool inEdgeBand(const QPointF& p) const
    {
        const double i = cfg_.edgeInsetPx;
        if (i <= 0.0) return false;
        return p.x() < i || p.y() < i
            || p.x() > view_.width() - i || p.y() > view_.height() - i;
    }

    static double separation(const QVector<QPointF>& pts)
    {
        if (pts.size() < 2) return 0.0;
        return std::hypot(pts[1].x() - pts[0].x(), pts[1].y() - pts[0].y());
    }

    bool isTap(qint64 tMs) const
    {
        return (last_ - start_).manhattanLength() <= cfg_.tapSlopPx
            && (tMs - startMs_) < cfg_.longPressMs;
    }

    Event fitEvent(bool out) const { return Event{ out ? Kind::PinchOut : Kind::PinchIn, out ? 1.0 : -1.0, 0.0 }; }

    QVector<Event> beginSeek(double dx)
    {
        if (!cfg_.seek) { phase_ = Phase::Inert; claimed_ = false; return {}; }
        phase_ = Phase::Seek;
        seekBase_ = pos_;
        return QVector<Event>{ seekEvent(Kind::SeekPreview, dx) };
    }

    QVector<Event> beginVertical(double dy)
    {
        const bool right = rightHalf(start_.x());
        if (right && !cfg_.volume)      { phase_ = Phase::Inert; claimed_ = false; return {}; }
        if (!right && !cfg_.brightness) { phase_ = Phase::Inert; claimed_ = false; return {}; }
        phase_ = right ? Phase::Volume : Phase::Brightness;
        levelBase_ = right ? vol_ : bright_;
        return QVector<Event>{ levelEvent(right ? Kind::VolumeDelta : Kind::BrightnessDelta, dy) };
    }

    Event seekEvent(Kind k, double dx)
    {
        const double w = qMax(1.0, view_.width());
        double delta = (dx / w) * cfg_.seekFullSwipeSec;
        double target = seekBase_ + delta;
        if (target < 0.0) target = 0.0;
        if (cfg_.durationSec > 0.0 && target > cfg_.durationSec) target = cfg_.durationSec;
        return Event{ k, target - seekBase_, target };
    }

    Event levelEvent(Kind k, double dy)
    {
        const double h = qMax(1.0, view_.height()) * qMax(0.05, cfg_.volumeFullSwipeFrac);
        // Up is MORE: screen y grows downward, so the travel is negated.
        const int lvl = qBound(0, int(qRound(levelBase_ + (-dy / h) * 100.0)), 100);
        const int prev = (k == Kind::VolumeDelta) ? vol_ : bright_;
        if (k == Kind::VolumeDelta) vol_ = lvl; else bright_ = lvl;
        return Event{ k, double(lvl - prev), double(lvl) };
    }

    QVector<Event> tapEvent(qint64 tMs)
    {
        if (cfg_.doubleTap && lastTapMs_ > 0 && (tMs - lastTapMs_) <= cfg_.doubleTapMs
            && (last_ - lastTapPos_).manhattanLength() <= cfg_.doubleTapRadiusPx)
        {
            lastTapMs_ = 0;
            const int z = zoneOf(last_.x());
            if (z < 0) return QVector<Event>{ Event{ Kind::DoubleTapLeft,  -double(cfg_.jumpSeconds), 0.0 } };
            if (z > 0) return QVector<Event>{ Event{ Kind::DoubleTapRight,  double(cfg_.jumpSeconds), 0.0 } };
            return QVector<Event>{ Event{ Kind::DoubleTapCentre, 0.0, 0.0 } };
        }
        lastTapMs_  = tMs;
        lastTapPos_ = last_;
        return QVector<Event>{ Event{ Kind::TapPending, 0.0, 0.0 } };
    }

    Config  cfg_;
    QSizeF  view_ = QSizeF(1280, 720);
    Phase   phase_ = Phase::Idle;
    bool    claimed_ = false;
    bool    locked_ = false;
    bool    overlayOpen_ = false;
    QPointF start_, last_, lastTapPos_;
    qint64  startMs_ = 0;
    qint64  lastTapMs_ = 0;
    double  pinchStart_ = 0.0;
    bool    pinchFired_ = false;
    double  seekBase_ = 0.0;
    double  levelBase_ = 0.0;
    double  pos_ = 0.0;
    int     vol_ = 100;
    int     bright_ = 100;
};

} // namespace PlayerGestures
