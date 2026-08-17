#pragma once
// Fractional frame pacing for the single-player libretro loop — pure math so it can be unit-tested without a
// QTimer or a clock.
//
// A QTimer fires on whole-millisecond intervals, but a console's true frame period is fractional (NES 60.10fps
// => 16.639ms). Any fixed integer interval is wrong: 17ms runs ~2% slow, 16ms ~4% fast. Instead, each tick we
// look at the real elapsed time and choose the integer interval that steers the NEXT tick back onto the ideal
// fractional schedule. Over time the chosen intervals come out 16,17,16,17… and average exactly the period, so
// game speed (and therefore audio pitch) is correct.

#include <cmath>

namespace eb {

// Returns the whole-millisecond delay to arm for the next tick and advances the schedule.
//   period      : exact frame period in ms (1000 / fps)
//   nowMs       : monotonic clock reading for this tick, in ms
//   nextFrameMs : in/out — ideal time of the next tick; advanced by one period each call
// A gap larger than a few frames (pause, load hitch) resyncs the baseline to now instead of firing a catch-up
// burst. The result is always >= 1 (a QTimer can't take 0 here without busy-spinning).
inline int nextPaceIntervalMs(double period, double nowMs, double& nextFrameMs)
{
    if (!(period > 0.0)) period = 16.6667;
    nextFrameMs += period;
    double delay = nextFrameMs - nowMs;
    if (delay < -4.0 * period) { nextFrameMs = nowMs + period; delay = period; }
    const int iv = int(delay + 0.5);
    return iv < 1 ? 1 : iv;
}

} // namespace eb
