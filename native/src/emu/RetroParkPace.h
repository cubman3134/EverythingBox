// Pure frame-pacing helper for RetroParkView (no Qt, no runtime — unit-asserted in probe_retropark_content).
//
// RetroParkView paces its present loop off a single-shot QTimer whose interval is an INTEGER number of
// milliseconds. A console's real frame period is fractional (NES/FCEUmm runs at ~60.0988 Hz, i.e. ~16.639 ms),
// so a flat integer 16 ms timer runs ~4 % fast — the game plays too quickly and RetroPark's own XAudio2 device
// (which the driven core feeds at the true rate) drops buffers and crackles. There is no host-side audio hook in
// the RetroPark ABI, so pacing is the only lever; getting it right is what fixes both symptoms.
//
// The fix carries the fractional remainder in an accumulator so the LONG-RUN AVERAGE of the returned integer
// intervals converges to the true period: e.g. 16.639 ms is paid out as 16, 17, 16, 17, 16, … averaging 16.639.
#pragma once

namespace rppace {

// Advance the fractional-ms accumulator by one frame's period and return the next integer timer interval.
//   frameIntervalMs — the core's true frame period in ms (1000.0 / fps).
//   accumMs         — carried remainder; caller keeps it across calls (start at 0.0).
// Add this frame's period, take the whole-ms part as the interval to arm, and keep the sub-ms remainder for
// next time. accumMs is always non-negative here (it stays in [0,1) after each call, plus a positive period),
// so the int cast floors. Clamped to >= 1 ms so a degenerate/zero fps can never busy-spin the timer at 0 ms.
inline int nextFrameIntervalMs(double frameIntervalMs, double& accumMs)
{
    accumMs += frameIntervalMs;
    int ms = (int)accumMs;      // floor (accumMs >= 0)
    accumMs -= (double)ms;      // carry the fractional remainder into the next frame
    return ms < 1 ? 1 : ms;
}

} // namespace rppace
