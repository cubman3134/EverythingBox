// Headless check of the video player's touch gesture recogniser (src/video/PlayerGestures.h) and of the one
// place its Config is built from stored preferences (src/video/PlayerGestureConfig.h) — issue #162.
//
// The recogniser is pure: touch points and an INJECTED millisecond clock in, gesture events out. That is the
// whole reason it exists as a separate unit, because every rule worth getting wrong here is a timing or a
// geometry rule, and neither can be driven reliably through a live QTouchEvent stream:
//
//   * the half-screen split — a vertical swipe on the RIGHT half is volume, on the LEFT half brightness (the
//     near-universal convention issue #162 is explicit about matching);
//   * the thirds — a double-tap left/right skips by #140's SHARED jump interval, centre is play/pause;
//   * cancel-by-returning — a horizontal scrub whose finger comes back to where it started commits nothing;
//   * the OS edge band — a touch that STARTS within edgeInsetPx of any edge is inert for its whole sequence,
//     and is not even claimed, so the OS's back swipe is never fought;
//   * the lock — every family suppressed, and the only thing still reported is a bare LockedTap;
//   * an open menu/overlay — the same total suppression, without the LockedTap;
//   * the form-factor gate — Config::enabled false (desktop/TV) makes begin() claim nothing at all, which is
//     what makes "the D-pad path is untouched" a property of the code rather than of a reviewer's attention;
//   * the double-tap window and the long-press threshold, both at and either side of their boundary;
//   * the pinch direction -> the fit/fill/stretch cycle.
//
// Prints PLAYERGESTURES-OK on success; any failure prints PLAYERGESTURES-FAIL <cond> and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// Settings opens starts empty — which is what lets section 10 assert the DEFAULTS of the new gesture keys.
#include "PlayerGestures.h"
#include "PlayerGestureConfig.h"

#include <QCoreApplication>
#include <QPointF>
#include <QSizeF>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PLAYERGESTURES-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace PlayerGestures;

static const QSizeF kView(1200.0, 800.0);

// A recogniser wired the way the touch host wires it: gestures on (a touch form factor), the viewport known.
static Config touchConfig()
{
    Config c;
    c.enabled = true;
    c.jumpSeconds = 10;
    return c;
}

static QVector<QPointF> one(double x, double y) { return QVector<QPointF>{ QPointF(x, y) }; }
static QVector<QPointF> two(double cx, double cy, double half)
{
    return QVector<QPointF>{ QPointF(cx - half, cy), QPointF(cx + half, cy) };
}

// Drive a one-finger drag as a real sequence would arrive: press, N moves, release. Returns everything the
// recogniser emitted, in order.
static QVector<Event> drag(Recognizer& r, QPointF from, QPointF to, qint64 t0, qint64 durMs, int steps = 8)
{
    QVector<Event> all;
    all << r.begin(one(from.x(), from.y()), t0);
    for (int i = 1; i <= steps; ++i)
    {
        const double f = double(i) / steps;
        const QPointF p(from.x() + (to.x() - from.x()) * f, from.y() + (to.y() - from.y()) * f);
        all << r.update(one(p.x(), p.y()), t0 + durMs * i / steps);
    }
    all << r.end(one(to.x(), to.y()), t0 + durMs);
    return all;
}

static bool has(const QVector<Event>& evs, Kind k)
{
    for (const Event& e : evs) if (e.kind == k) return true;
    return false;
}
static Event last(const QVector<Event>& evs, Kind k)
{
    Event out;
    for (const Event& e : evs) if (e.kind == k) out = e;
    return out;
}
static int count(const QVector<Event>& evs, Kind k)
{
    int n = 0;
    for (const Event& e : evs) if (e.kind == k) ++n;
    return n;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. The form-factor gate. A default Config has enabled == false, which is the desktop/TV state: no
    //         press is claimed and no rule can fire. This is the "TV path untouched by construction" assert.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(Config{});                       // enabled defaults to false
        CHECK(r.begin(one(600, 400), 0).isEmpty());
        CHECK(!r.claimed());
        CHECK(drag(r, QPointF(900, 600), QPointF(900, 200), 0, 200).isEmpty());
        // and a tap, the one thing the player already did on touch, produces nothing either
        r.begin(one(600, 400), 1000);
        CHECK(r.end(one(600, 400), 1050).isEmpty());
    }

    // ---- 2. Vertical swipe, RIGHT half = volume. Up is more.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());
        r.setVolume(50);
        const QVector<Event> evs = drag(r, QPointF(900, 600), QPointF(900, 300), 0, 200);
        CHECK(r.claimed());
        CHECK(has(evs, Kind::VolumeDelta));
        CHECK(!has(evs, Kind::BrightnessDelta));
        // 300 px up over a 800 px viewport with a 0.6 full-swipe fraction == 62.5 points, so 50 -> 100 (clamped)
        CHECK(last(evs, Kind::VolumeDelta).target == 100.0);
        CHECK(r.volume() == 100);
        // and downward reduces it
        r.setVolume(80);
        const QVector<Event> down = drag(r, QPointF(900, 300), QPointF(900, 420), 0, 200);
        CHECK(last(down, Kind::VolumeDelta).target < 80.0);
    }

    // ---- 3. Vertical swipe, LEFT half = brightness. Same arithmetic, the other level.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());
        r.setBrightness(100);
        const QVector<Event> evs = drag(r, QPointF(200, 300), QPointF(200, 540), 0, 200);
        CHECK(has(evs, Kind::BrightnessDelta));
        CHECK(!has(evs, Kind::VolumeDelta));
        // 240 px down / (800 * 0.6) == 50 points: 100 -> 50
        CHECK(last(evs, Kind::BrightnessDelta).target == 50.0);
        CHECK(r.brightness() == 50);
        CHECK(r.volume() == 100);                    // untouched by a brightness swipe
    }

    // ---- 4. The half-screen split is a HALF, not a third: x just past the midpoint is already volume.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());
        CHECK(r.rightHalf(601.0));
        CHECK(!r.rightHalf(599.0));
        const QVector<Event> lo = drag(r, QPointF(599, 500), QPointF(599, 300), 0, 200);
        CHECK(has(lo, Kind::BrightnessDelta));
        const QVector<Event> hi = drag(r, QPointF(601, 500), QPointF(601, 300), 0, 200);
        CHECK(has(hi, Kind::VolumeDelta));
    }

    // ---- 5. Horizontal swipe = scrub, with a preview per move and a commit on release.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());
        r.setPosition(300.0);
        const QVector<Event> evs = drag(r, QPointF(400, 400), QPointF(1000, 400), 0, 300);
        CHECK(count(evs, Kind::SeekPreview) >= 4);   // a live readout, not one jump at the end
        CHECK(has(evs, Kind::SeekCommit));
        CHECK(!has(evs, Kind::SeekCancel));
        // 600 px of a 1200 px viewport == half a full-width swipe == +60 s of the 120 s span
        CHECK(qFuzzyCompare(last(evs, Kind::SeekCommit).target, 360.0));
        CHECK(qFuzzyCompare(last(evs, Kind::SeekCommit).value, 60.0));
        // the target is clamped into the media when the duration is known
        Config c = touchConfig();
        c.durationSec = 320.0;
        r.setConfig(c);
        r.setPosition(300.0);
        const QVector<Event> clamped = drag(r, QPointF(400, 400), QPointF(1000, 400), 0, 300);
        CHECK(qFuzzyCompare(last(clamped, Kind::SeekCommit).target, 320.0));
        // ...and at 0 on the way back
        r.setPosition(10.0);
        const QVector<Event> back = drag(r, QPointF(1000, 400), QPointF(400, 400), 0, 300);
        CHECK(last(back, Kind::SeekCommit).target == 0.0);
    }

    // ---- 6. Cancel-by-returning: out and back to the start commits nothing.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());
        r.setPosition(300.0);
        QVector<Event> all;
        all << r.begin(one(600, 400), 0);
        all << r.update(one(750, 400), 40);          // claim the horizontal axis
        all << r.update(one(900, 400), 80);
        all << r.update(one(700, 400), 120);         // ...and come back
        all << r.update(one(605, 400), 160);
        all << r.end(one(605, 400), 200);
        CHECK(has(all, Kind::SeekPreview));          // the readout still tracked the finger
        CHECK(has(all, Kind::SeekCancel));
        CHECK(!has(all, Kind::SeekCommit));
        CHECK(qFuzzyCompare(last(all, Kind::SeekCancel).target, 300.0));   // exactly where it started
    }

    // ---- 7. Double-tap: thirds, #140's interval, and the timing window at its boundary.
    {
        Recognizer r;
        r.setViewport(kView);
        Config c = touchConfig();
        c.jumpSeconds = 45;                          // whatever #140's setting says, this is what the skip uses
        r.setConfig(c);

        auto tapAt = [&r](double x, qint64 t) {
            r.begin(one(x, 400), t);
            return r.end(one(x, 400), t + 20);
        };
        CHECK(has(tapAt(200, 0), Kind::TapPending));         // the first tap is only ever DEFERRED
        const QVector<Event> dl = tapAt(200, 300);
        CHECK(has(dl, Kind::DoubleTapLeft));
        CHECK(last(dl, Kind::DoubleTapLeft).value == -45.0);
        CHECK(has(tapAt(1000, 1000), Kind::TapPending));
        const QVector<Event> dr = tapAt(1000, 1200);
        CHECK(has(dr, Kind::DoubleTapRight));
        CHECK(last(dr, Kind::DoubleTapRight).value == 45.0);
        CHECK(has(tapAt(600, 2000), Kind::TapPending));
        const QVector<Event> dc = tapAt(600, 2200);          // centre = play/pause, no skip
        CHECK(has(dc, Kind::DoubleTapCentre));
        CHECK(last(dc, Kind::DoubleTapCentre).value == 0.0);

        // The window: 350 ms apart (inclusive) is a double-tap, 400 ms apart is two singles.
        CHECK(has(tapAt(200, 5000), Kind::TapPending));
        CHECK(has(tapAt(200, 5350), Kind::DoubleTapLeft));
        CHECK(has(tapAt(200, 6000), Kind::TapPending));
        CHECK(has(tapAt(200, 6400), Kind::TapPending));      // too late: a second lone tap, not a skip

        // Two taps far APART on screen are two taps, however fast — a two-thumbed user is not scrubbing.
        CHECK(has(tapAt(200, 7000), Kind::TapPending));
        CHECK(has(tapAt(1000, 7100), Kind::TapPending));

        // The family switched off: a double-tap is just two ordinary taps, and the chrome toggle still works.
        c.doubleTap = false;
        r.setConfig(c);
        CHECK(has(tapAt(200, 8000), Kind::TapPending));
        CHECK(has(tapAt(200, 8100), Kind::TapPending));
    }

    // ---- 8. Long-press: the threshold, and the release that restores.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());

        r.begin(one(600, 400), 0);
        CHECK(r.tick(400).isEmpty());                        // 400 ms is not yet a hold
        const QVector<Event> begun = r.tick(500);
        CHECK(has(begun, Kind::LongPressBegin));
        CHECK(last(begun, Kind::LongPressBegin).value == 2.0);                   // 2x while held
        CHECK(r.tick(900).isEmpty());                        // and only once
        CHECK(has(r.end(one(600, 400), 2000), Kind::LongPressEnd));

        // A press that TRAVELS is a swipe, never a hold.
        r.begin(one(600, 400), 3000);
        r.update(one(700, 400), 3100);
        CHECK(r.tick(3600).isEmpty());

        // A short press stays a tap: no hold, and no stray LongPressEnd on release.
        r.begin(one(600, 400), 5000);
        const QVector<Event> quick = r.end(one(600, 400), 5100);
        CHECK(!has(quick, Kind::LongPressEnd));
        CHECK(has(quick, Kind::TapPending));

        // Family off: holding forever does nothing.
        Config c = touchConfig();
        c.longPress = false;
        r.setConfig(c);
        r.begin(one(600, 400), 9000);
        CHECK(r.tick(9999).isEmpty());
    }

    // ---- 9. Pinch -> the fit cycle, and the cycle's own arithmetic.
    {
        CHECK(cycleFit(VideoFit::Fit, true) == VideoFit::Fill);
        CHECK(cycleFit(VideoFit::Fill, true) == VideoFit::Stretch);
        CHECK(cycleFit(VideoFit::Stretch, true) == VideoFit::Fit);       // wraps
        CHECK(cycleFit(VideoFit::Fit, false) == VideoFit::Stretch);      // and backwards
        CHECK(cycleFit(VideoFit::Stretch, false) == VideoFit::Fill);

        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());
        r.begin(two(600, 400, 60), 0);
        CHECK(r.claimed());
        const QVector<Event> outw = r.update(two(600, 400, 120), 100);
        CHECK(has(outw, Kind::PinchOut));
        CHECK(r.update(two(600, 400, 180), 150).isEmpty());              // one cycle step per pinch, not per frame
        r.end(two(600, 400, 180), 200);

        r.begin(two(600, 400, 120), 1000);
        CHECK(has(r.update(two(600, 400, 60), 1100), Kind::PinchIn));
        r.end(two(600, 400, 60), 1200);

        // A pinch that barely moves is not a pinch (a two-finger rest must not cycle the aspect).
        r.begin(two(600, 400, 100), 2000);
        CHECK(r.update(two(600, 400, 105), 2100).isEmpty());
        r.end(two(600, 400, 105), 2200);

        Config c = touchConfig();
        c.pinch = false;
        r.setConfig(c);
        r.begin(two(600, 400, 60), 3000);
        CHECK(!r.claimed());                                             // not ours: it falls through untouched
        CHECK(r.update(two(600, 400, 140), 3100).isEmpty());
    }

    // ---- 10. The OS edge band. A touch STARTING inside it is inert for its whole sequence and is not
    //          claimed, so the platform's own back / notification swipe is never fought.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());                                      // default inset 24 px
        for (const QPointF& p : { QPointF(10, 400), QPointF(1190, 400), QPointF(600, 8), QPointF(600, 795) })
        {
            const QVector<Event> evs = drag(r, p, QPointF(600, 400), 0, 200);
            CHECK(evs.isEmpty());
            CHECK(!r.claimed());
        }
        // A touch that starts INSIDE the band's edge is ours as normal.
        const QVector<Event> ok = drag(r, QPointF(30, 500), QPointF(30, 300), 0, 200);
        CHECK(has(ok, Kind::BrightnessDelta));

        // The inset is configurable, including all the way off.
        Config c = touchConfig();
        c.edgeInsetPx = 0;
        r.setConfig(c);
        const QVector<Event> edge = drag(r, QPointF(2, 500), QPointF(2, 300), 0, 200);
        CHECK(has(edge, Kind::BrightnessDelta));
        // ...and wider, which then swallows a touch the 24 px band allowed.
        c.edgeInsetPx = 64;
        r.setConfig(c);
        CHECK(drag(r, QPointF(30, 500), QPointF(30, 300), 0, 200).isEmpty());
    }

    // ---- 11. The lock suppresses EVERY family, and reports only that a tap happened.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());
        r.setVolume(50);
        r.setPosition(300.0);
        r.setLocked(true);
        CHECK(r.locked());
        CHECK(drag(r, QPointF(900, 600), QPointF(900, 300), 0, 200).isEmpty());   // no volume
        CHECK(r.volume() == 50);
        CHECK(drag(r, QPointF(400, 400), QPointF(1000, 400), 0, 300).isEmpty());  // no scrub
        CHECK(r.update(two(600, 400, 140), 500).isEmpty());                       // no pinch
        r.begin(one(600, 400), 1000);
        CHECK(r.tick(1600).isEmpty());                                            // no hold
        // ...and the ONE thing it still says, so the unlock control can be surfaced:
        r.begin(one(600, 400), 2000);
        const QVector<Event> tap = r.end(one(600, 400), 2050);
        CHECK(has(tap, Kind::LockedTap));
        CHECK(!has(tap, Kind::TapPending));
        // Unlocking restores everything, with no residue from the suppressed sequences.
        r.setLocked(false);
        CHECK(has(drag(r, QPointF(900, 600), QPointF(900, 300), 3000, 200), Kind::VolumeDelta));
    }

    // ---- 12. An open menu / overlay suppresses everything too — and unlike the lock, silently.
    {
        Recognizer r;
        r.setViewport(kView);
        r.setConfig(touchConfig());
        r.setOverlayOpen(true);
        CHECK(drag(r, QPointF(900, 600), QPointF(900, 300), 0, 200).isEmpty());
        CHECK(!r.claimed());                          // NOT claimed: the menu's own touch handling still works
        r.begin(one(600, 400), 1000);
        const QVector<Event> tap = r.end(one(600, 400), 1050);
        CHECK(tap.isEmpty());
        CHECK(!has(tap, Kind::LockedTap));
        r.setOverlayOpen(false);
        CHECK(has(drag(r, QPointF(900, 600), QPointF(900, 300), 2000, 200), Kind::VolumeDelta));
    }

    // ---- 13. Per-family switches on the swipe axes: an off family is INERT and unclaimed, and never
    //          silently becomes the other one.
    {
        Recognizer r;
        r.setViewport(kView);
        Config c = touchConfig();
        c.volume = false;
        r.setConfig(c);
        const QVector<Event> v = drag(r, QPointF(900, 600), QPointF(900, 300), 0, 200);
        CHECK(v.isEmpty());
        CHECK(!r.claimed());
        CHECK(has(drag(r, QPointF(200, 600), QPointF(200, 300), 1000, 200), Kind::BrightnessDelta));

        c.volume = true; c.brightness = false;
        r.setConfig(c);
        CHECK(drag(r, QPointF(200, 600), QPointF(200, 300), 2000, 200).isEmpty());
        CHECK(has(drag(r, QPointF(900, 600), QPointF(900, 300), 3000, 200), Kind::VolumeDelta));

        c.brightness = true; c.seek = false;
        r.setConfig(c);
        const QVector<Event> s = drag(r, QPointF(400, 400), QPointF(1000, 400), 4000, 300);
        CHECK(s.isEmpty());
        CHECK(!r.claimed());
    }

    // ---- 14. Config from the stored settings: the defaults, the form-factor gate, and #140's SHARED
    //          interval (issue #162 asks explicitly for no second knob).
    {
        Settings::setDisplayMode(QStringLiteral("desktop"));
        FormFactor::instance().refresh();
        Config c = configFromSettings();
        CHECK(!c.enabled);                              // desktop: the recogniser is inert
        CHECK(c.volume && c.brightness && c.seek && c.doubleTap && c.longPress && c.pinch);
        CHECK(c.edgeInsetPx == 24);

        Settings::setDisplayMode(QStringLiteral("tv"));
        FormFactor::instance().refresh();
        CHECK(!configFromSettings().enabled);           // TV: inert too, which is the nav contract's guarantee

        Settings::setDisplayMode(QStringLiteral("mobile"));
        FormFactor::instance().refresh();
        CHECK(configFromSettings().enabled);            // touch: the one mode that recognises anything

        Settings::setAudioJumpSeconds(45);
        CHECK(configFromSettings().jumpSeconds == 45);  // the video skip reads #140's key, not one of its own
        Settings::setAudioJumpSeconds(15);
        CHECK(configFromSettings().jumpSeconds == 15);

        Settings::setGestureVolume(false);
        Settings::setGestureSeek(false);
        c = configFromSettings();
        CHECK(!c.volume);
        CHECK(!c.seek);
        CHECK(c.brightness);                            // one family off does not disturb its neighbours
        Settings::setGestureVolume(true);
        Settings::setGestureSeek(true);

        Settings::setGestureEdgeInset(48);
        CHECK(configFromSettings().edgeInsetPx == 48);
        Settings::setGestureEdgeInset(400);             // clamped on write, like every other bounded setting
        CHECK(configFromSettings().edgeInsetPx == 96);
        Settings::setGestureEdgeInset(-5);
        CHECK(configFromSettings().edgeInsetPx == 0);
        Settings::setGestureEdgeInset(24);
        Settings::setDisplayMode(QStringLiteral("auto"));
        FormFactor::instance().refresh();
    }

    if (failures) { std::fprintf(stderr, "PLAYERGESTURES-FAIL %d check(s)\n", failures); return 1; }
    std::printf("PLAYERGESTURES-OK\n");
    return 0;
}
