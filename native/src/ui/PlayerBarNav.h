// PlayerBarNav — what a key means to one of the player's transport BARS (the seek slider and the volume
// slider), and how far one arrow press moves it.
//
// The bars sit in the same Left/Right ring as the transport buttons, but a slider is not a button: it has a
// value as well as a position in the row, so one set of arrows has to serve both. Hence two states.
//
//   SELECTED   focused, and inert. Left/Right walk the ring past the bar, exactly as they do across the
//              buttons, so a bar never obstructs getting to the other end of the row. Enter goes in.
//   ADJUSTING  Left/Right move the bar's VALUE. Enter and Back come back out to Selected.
//
// Two rules in the table below are not obvious and are the ones worth keeping:
//
//   * Up and Down belong to the bar in BOTH states. A QSlider handles Up/Down as value steps and ACCEPTS
//     them, so any key this table declines is a key the slider will silently act on — "step off the bar
//     upward" would have nudged the volume on the way out. The bar therefore claims them and performs the
//     player's own Up/Down (to the ‹ Back overlay, or back down onto the row), leaving Adjusting on the way.
//     PageUp/PageDown/Home/End are swallowed outright for the same reason, having no meaning here.
//
//   * Back is NOT ours while merely Selected. There it has to stay the player's unified Back (stop the media
//     and return home), which is what it does on every other control in the row; claiming it would leave a
//     user who has arrowed onto a bar with no way off the player at all.
//
// Deliberately free of Qt widgets, signals and slots — it needs only the Qt::Key_* enum — so it is
// header-only (no moc) and adds no translation unit that the app's own source list would have to repeat.
// Pinned headlessly by native/tools/probe_playerbar.cpp.
#pragma once
#include <Qt>

namespace eb
{

// What the bar does with a key. Anything but NotOurs is consumed by the bar; NotOurs falls through to the
// player's own handling (Space, F12, the queue menu, the speed keys, and — while Selected — Back).
enum class BarAct
{
    NotOurs,      // not the bar's key: let it through untouched
    Consume,      // a key the slider WOULD act on but the bar does not use: swallow it and do nothing
    FocusPrev,    // Selected + Left: step the transport ring backwards
    FocusNext,    // Selected + Right: step the transport ring forwards
    Enter,        // Selected + Enter: go to Adjusting
    Leave,        // Adjusting + Enter/Back: return to Selected
    LeaveToBack,  // Up: leave Adjusting (if in it) and focus the ‹ Back overlay
    LeaveToRow,   // Down: leave Adjusting (if in it) and re-land on the transport row
    StepDown,     // Adjusting + Left: one step down
    StepUp        // Adjusting + Right: one step up
};

inline BarAct barKey(int key, bool adjusting)
{
    switch (key)
    {
    case Qt::Key_Left:      return adjusting ? BarAct::StepDown : BarAct::FocusPrev;
    case Qt::Key_Right:     return adjusting ? BarAct::StepUp   : BarAct::FocusNext;

    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Select:    return adjusting ? BarAct::Leave : BarAct::Enter;

    // Out of Adjusting; the player's own Back while merely Selected (see the header comment).
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
    case Qt::Key_Back:      return adjusting ? BarAct::Leave : BarAct::NotOurs;

    // Always ours, in both states — a declined Up/Down is a value the slider moves behind the user's back.
    case Qt::Key_Up:        return BarAct::LeaveToBack;
    case Qt::Key_Down:      return BarAct::LeaveToRow;

    // Same reasoning, no meaning here: swallowed rather than declined.
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:       return BarAct::Consume;

    default:                return BarAct::NotOurs;
    }
}

// One arrow press on a bar, clamped to the bar's range. `delta` is -1 or +1. Clamping (rather than wrapping)
// is the point: holding a direction has to come to rest at an end, not roll a full-volume bar around to zero.
inline int barStep(int cur, int delta, int step, int lo, int hi)
{
    const int v = cur + delta * step;
    return v < lo ? lo : (v > hi ? hi : v);
}

// The volume bar is 0..200 (above 100 is software boost), so 5 is 40 presses end to end — 20 across the
// ordinary 0..100 range, with the boost half of the scale costing another 20.
inline constexpr int kVolumeStep = 5;
// The seek bar is 0..1000 permille of the media's duration, so 10 is 1% of duration per press — 100 presses
// end to end whatever the length. Proportional on purpose: the ⏪/⏩ buttons already own the precise 10-second
// jump, so the bar is the "get me roughly there" control and should cost the same effort on a 90-minute film
// and a six-hour audiobook.
inline constexpr int kSeekStep = 10;

} // namespace eb
