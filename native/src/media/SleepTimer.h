// The sleep-timer DECISION, pulled out of the player so it can be pinned without a QTimer, a volume knob or a
// running file (issue #140). Deliberately pure: QtCore only, no player, no wall clock — every input is passed
// in, every output is a number the wiring acts on. probe_listening exercises it directly; the live wiring in
// MainWindow (arm from the transport menu, drive the volume by fadeGain on each position tick, pause + store a
// nudged resume on expiry) is a thin shell over these three functions.
//
// The three shapes the menu offers:
//   * a minute preset (15/30/45/60) or a Custom minute count  -> Mode::Minutes, `minutes` carries the count;
//   * "End of chapter" -> Mode::EndOfChapter, which needs the file's chapter list (we have REAL chapter data —
//     MediaSegments::Chapter — so this is the running chapter's genuine end, not the faked "≈X minutes" most
//     apps ship);
//   * Off -> Mode::Off, i.e. not armed.
//
// expiryTime() reports the ABSOLUTE playback-seconds at which the timer fires. Minute modes are defined against
// the position at arm time (nowSec + minutes*60): the pure layer does not know the playback rate, and this
// treats "stop in 30 minutes" as 30 minutes of the position clock, which is 30 wall-minutes at 1x. A negative
// return means "never / not armed" — Off, a non-positive minute count, or an end-of-chapter whose computed end
// is already at or behind the current position (nothing left to fire on).
#pragma once
#include "../core/MediaSegments.h"
#include <QVector>

namespace SleepTimer
{
    enum class Mode { Off, Minutes, EndOfChapter };

    struct Timer
    {
        Mode   mode    = Mode::Off;
        double minutes = 0.0;   // used by Mode::Minutes only (a preset or the custom count)
    };

    // The absolute playback-second at which the timer fires, or a negative sentinel for "never / not armed".
    // `chapters` and `duration` are consulted only by Mode::EndOfChapter; a minute timer ignores them.
    inline double expiryTime(const Timer& t, double nowSec,
                             const QVector<MediaSegments::Chapter>& chapters, double duration)
    {
        switch (t.mode)
        {
            case Mode::Off:
                return -1.0;
            case Mode::Minutes:
                // A non-positive count is not a timer (a 0-minute "sleep" would fire instantly); report never.
                return t.minutes > 0.0 ? nowSec + t.minutes * 60.0 : -1.0;
            case Mode::EndOfChapter:
            {
                const int n = chapters.size();
                // The RUNNING chapter is the last one that starts at or before nowSec. Not short-circuited on
                // the first later chapter so a mis-ordered list still resolves to the greatest qualifying start.
                int i = -1;
                for (int k = 0; k < n; ++k)
                    if (chapters[k].time <= nowSec) i = k;
                // Its end is the next chapter's start, or the file duration for the last chapter. Sitting before
                // the first chapter (i < 0), the running region is the pre-chapter head, which ends at chapter 0;
                // with no chapters at all the whole file is one region ending at duration.
                double end;
                if (i < 0) end = (n > 0) ? chapters[0].time : duration;
                else       end = (i + 1 < n) ? chapters[i + 1].time : duration;
                // An end at or behind us cannot fire (a last chapter with an unknown duration<=0, or a position
                // already past the computed end): report never rather than a spurious immediate expiry.
                return end > nowSec ? end : -1.0;
            }
        }
        return -1.0;
    }

    // A 1 -> 0 gain to ramp the volume down over the last `fadeWindowSec` seconds instead of cutting hard.
    // 1.0 outside the window (more than the window's worth of time left), a linear ramp inside it, 0.0 at or
    // past expiry. A non-positive window disables the fade (returns 1.0 until the hard stop). The result is in
    // [0,1] by construction.
    inline double fadeGain(double secondsRemaining, double fadeWindowSec)
    {
        if (fadeWindowSec <= 0.0)          return 1.0; // no fade window configured -> no ramp
        if (secondsRemaining >= fadeWindowSec) return 1.0; // outside the window
        if (secondsRemaining <= 0.0)       return 0.0; // expired
        return secondsRemaining / fadeWindowSec;       // linear inside the window
    }

    // Where to drop the stored resume position when the timer fires, so a listener who drifted off does not have
    // to hunt back for the last thing they heard. Nudged back `nudgeSec` from the expiry position, clamped at 0
    // (never a negative seek).
    inline double resumeNudgeBack(double posOnExpiry, double nudgeSec = 30.0)
    {
        const double p = posOnExpiry - nudgeSec;
        return p > 0.0 ? p : 0.0;
    }
}
