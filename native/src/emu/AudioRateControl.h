#pragma once
// Dynamic rate control (DRC) for the libretro audio path — pure, device-free math so it can be unit-tested.
//
// Why it exists: the frame loop is driven by a QTimer whose interval is an integer number of milliseconds
// (qRound(1000/fps)), which never equals the core's true frame period. NES runs at 60.10 fps but the timer
// fires every 17 ms => 58.8 fps, so the core is stepped ~2.1% too slowly and emits ~2.1% fewer audio samples
// per second than the output device consumes. Uncompensated, the sink drains and underruns — the clicks and
// crackle you hear. This holds EVEN WHEN the core's sample rate equals the device rate (fceumm's 48000 on a
// 48 kHz device): matching rates does not mean matching *timers*. The fix is to always rate-control, nudging
// the resample step so the buffered audio parks near half-full regardless of the timer drift.
//
// The controller is a PI loop on buffer fill: err = fill_fraction - 0.5. A positive err (too full) grows the
// step so we advance faster through the input and emit fewer output samples (drains); a negative err (draining)
// shrinks the step so we emit slightly more (refills). The integral term cancels the *steady* drift — a
// proportional-only loop would have to sit permanently off-target to keep producing the surplus.

#include <algorithm>
#include <cstdint>

namespace eb {

// Returns the resample step (input samples advanced per output sample) for this frame. stepBase is
// srcRate/outRate (== 1.0 when the core rate equals the device rate). queued/bufSize describe the sink fill
// AFTER counting still-pending bytes. integral is the carried-across-frames accumulator (cancels the steady
// drift); it is updated in place. bufSize <= 0 (sink not ready) => no correction.
inline double drcStep(double stepBase, std::int64_t queued, std::int64_t bufSize, double& integral)
{
    if (bufSize <= 0) return stepBase;
    const double err = double(queued) / double(bufSize) - 0.5;      // + = too full, - = draining
    integral = std::clamp(integral + err * 0.0004, -0.03, 0.03);    // ±3% steady-state authority
    const double corr = std::clamp(0.05 * err + integral, -0.05, 0.05);
    return stepBase * (1.0 + corr); // too full -> larger step -> fewer out samples -> drains
}

} // namespace eb
