#pragma once
#include "../video/PlayerGestures.h"

#include <QPointF>
#include <QtGlobal>
#include <QtMath>

// The READER's touch vocabulary (issue #147), as a pure decision table: a coordinate (or a travel) and a
// Config go in, an action comes out. No QWidget, no QTouchEvent, no Settings — which is what lets
// probe_readergestures drive every zone, every preset and both sides of every threshold headlessly.
//
// This is deliberately NOT a second gesture vocabulary. Issue #162 built one for the video player and the two
// have to agree, so the numbers below are read off PlayerGestures::Config rather than copied into it: the
// travel that stops a press being a tap, the travel that commits a swipe, and the OS edge band are the SAME
// numbers the player uses, and a change there reaches here without anyone remembering. What is genuinely
// different is only what a reader has and a player does not — a page to turn and a menu to open — and that
// is the whole of what this file adds.
//
// Two gates are enforced HERE rather than at each call site:
//
//   * Config::enabled is the FORM-FACTOR gate, resolved by ReaderGestureConfig.h from the same one authority
//     #162 uses (FormFactor, touch only). With it false EVERY entry point falls through to the legacy*
//     functions below — the reader's behaviour exactly as it stood before this file existed — so a desktop
//     or TV build is untouched in both directions: it neither gains the presets nor loses what it had.
//   * A touch that starts within Config::edgeInsetPx of an edge is inert. That band belongs to the OS's own
//     back and notification swipes, and half-recognising one is worse than ignoring it (#162's rule, and the
//     reason it is stated once here instead of twice in two hosts).
namespace ReaderGestures
{

// The tap-zone PRESETS (issue #147). Three, not Moon+'s 24-operation matrix — presets over matrices, which is
// the app's settings philosophy and the issue's own wording. Stored as an int, so the enum's values are part
// of the on-disk format and must not be renumbered.
enum class TapPreset
{
    // The default. Left edge goes back, right edge goes forward, the middle opens the menu — the arrangement
    // every e-reader ships and the one a right-thumb grip reaches.
    RightForward = 0,
    // Its MIRROR, for the left-thumb grip the issue calls out: left goes forward, right goes back. It mirrors
    // the TAP map only. A swipe keeps the page-flip convention in both presets (see swipeAction) because a
    // swipe is a direction of travel, not a place your thumb can reach.
    LeftForward  = 1,
    // No paging zones at all: every tap opens the menu and paging is the swipe alone. For readers who tap the
    // page while thinking and do not want to lose their place for it.
    MenuOnly     = 2,
};

inline TapPreset presetFromInt(int v)
{
    if (v == int(TapPreset::LeftForward)) return TapPreset::LeftForward;
    if (v == int(TapPreset::MenuOnly))    return TapPreset::MenuOnly;
    return TapPreset::RightForward;   // total: an unknown stored value reads as the default, never as nothing
}
inline int presetToInt(TapPreset p) { return int(p); }

// What a gesture asks the reader to do. Prev/Next are the reader's own page turns (which already know about
// chapter boundaries and reading direction); Menu is the chrome toggle each host already had.
enum class Kind { None = 0, Prev, Next, Menu };

// The three numbers shared with the video player (#162), read off ITS Config so the two cannot drift. They
// are functions rather than constants precisely so this file never states a value of its own.
inline int sharedTapSlopPx()    { return PlayerGestures::Config().tapSlopPx; }
inline int sharedSwipeStartPx() { return PlayerGestures::Config().swipeStartPx; }
inline int sharedEdgeInsetPx()  { return PlayerGestures::Config().edgeInsetPx; }

struct Config
{
    // The form-factor gate. False (the desktop/TV default) routes everything to the legacy behaviour.
    bool      enabled = false;
    TapPreset preset  = TapPreset::RightForward;
    bool      swipe   = true;        // swipe paging; off leaves the tap zones as the only way to turn a page

    int tapSlopPx    = sharedTapSlopPx();
    int swipeStartPx = sharedSwipeStartPx();
    int edgeInsetPx  = sharedEdgeInsetPx();

    // The band across the top that opens the menu whatever column it falls in — "tap the top of the page for
    // the menu" is not a thing anyone qualifies by column. The host supplies it (the reader's own declared
    // chrome inset), so the zone is the bar you can see.
    double topBandPx = 56.0;
};

// The reader's thirds. Deliberately the same split PlayerGestures::Recognizer::zoneOf draws: -1 left,
// 0 centre, +1 right.
inline int zoneOf(double x, double viewportW)
{
    const double w = qMax(1.0, viewportW);
    if (x < w / 3.0)       return -1;
    if (x > 2.0 * w / 3.0) return 1;
    return 0;
}

// ---- The behaviour BEFORE #147 -----------------------------------------------------------------------------
// Reached whenever the form-factor gate is off, and kept as its own two functions rather than expressed as a
// preset with the thresholds turned down: making the old behaviour depend on the new rules is exactly the
// coupling a fallback exists to avoid (#162 makes the same argument about handleLegacyPlayerTouch).
inline Kind legacyTapAction(double x, double y, double viewportW, double topBandPx)
{
    if (y <= topBandPx) return Kind::Menu;
    const int z = zoneOf(x, viewportW);
    if (z < 0) return Kind::Prev;
    if (z > 0) return Kind::Next;
    return Kind::Menu;
}

// The 80 px the themed reader has always asked for. NOT the player's 32 px: unifying them is a #147 decision
// and it applies on the touch form factors #147 is about, not retroactively to a desktop touchscreen.
inline Kind legacySwipeAction(double dx, double dy)
{
    if (qAbs(dx) < 80.0 || qAbs(dx) <= qAbs(dy)) return Kind::None;
    return dx < 0 ? Kind::Next : Kind::Prev;     // leftward = next, the page-flip convention
}

// ---- The rules -----------------------------------------------------------------------------------------

// A touch that STARTS inside the OS's reserved edge band is inert for its whole sequence. The host asks this
// once, on the press, and remembers the answer — which is what makes it a property of the sequence rather
// than of wherever the finger happened to be when it was lifted.
inline bool inertStart(const Config& cfg, double x, double y, double vw, double vh)
{
    if (!cfg.enabled) return false;         // the legacy path never reserved a band; it must not start now
    const double i = double(cfg.edgeInsetPx);
    if (i <= 0.0) return false;
    return x < i || y < i || x > qMax(1.0, vw) - i || y > qMax(1.0, vh) - i;
}

// Did this press/release pair travel little enough to be a tap? The same slop #162 uses, so a finger that
// rests on a word means the same thing over a page as it does over a video.
inline bool isTap(const Config& cfg, double dx, double dy)
{
    const double slop = double(cfg.enabled ? cfg.tapSlopPx : sharedTapSlopPx());
    return qAbs(dx) < slop && qAbs(dy) < slop;
}

// Where a tap lands and what it means. The top band wins over every preset (including MenuOnly, where it is
// the same answer anyway), then the preset decides the outer thirds.
inline Kind tapAction(const Config& cfg, double x, double y, double vw, double vh)
{
    if (!cfg.enabled) return legacyTapAction(x, y, vw, cfg.topBandPx);
    if (inertStart(cfg, x, y, vw, vh)) return Kind::None;
    if (y <= cfg.topBandPx) return Kind::Menu;
    if (cfg.preset == TapPreset::MenuOnly) return Kind::Menu;
    const int z = zoneOf(x, vw);
    if (z == 0) return Kind::Menu;
    const bool leftGoesBack = (cfg.preset == TapPreset::RightForward);
    if (z < 0) return leftGoesBack ? Kind::Prev : Kind::Next;
    return leftGoesBack ? Kind::Next : Kind::Prev;
}

// A swipe: a short slide, no curl (issue #147 is explicit that the animation is not the point). The threshold
// and the axis test are #162's, so a swipe means the same thing in the reader and in the player.
//
// The direction is NOT mirrored by the LeftForward preset. A preset mirrors where your THUMB reaches; a swipe
// is the sheet of paper moving, and dragging a page leftward has meant "next" since before either of these
// features existed. Mirroring it would leave the mirror preset's user with a reader whose taps and swipes
// disagree about which way the book runs.
inline Kind swipeAction(const Config& cfg, double dx, double dy)
{
    if (!cfg.enabled) return legacySwipeAction(dx, dy);
    if (!cfg.swipe) return Kind::None;
    if (qAbs(dx) < double(cfg.swipeStartPx) || qAbs(dx) <= qAbs(dy)) return Kind::None;
    return dx < 0 ? Kind::Next : Kind::Prev;
}

} // namespace ReaderGestures
