// ReadAloudSleep — the sleep timer, applied to narration (issue #145). PURE: QtCore only, no engine, no QTimer,
// no wall clock. Every input is passed in, every output is a yes/no or a number the controller acts on, so
// probe_readaloud drives it with a fake clock on a runner that has no speech module at all.
//
// It is NOT a second timer. The decision is SleepTimer's (src/media/SleepTimer.h, the #140 player's): the same
// Timer, the same presets, the same fired() over the same expiryTime arithmetic, the same fadeGain. What
// narration adds is only what its time base needs:
//
//   * THE CLOCK. A minute timer counts minutes OF NARRATION: a SleepTimer::RunClock that runs while narration
//     is speaking and stops while it is paused. Pause for ten minutes and the timer has not moved.
//   * THE CHAPTER. Narration has no chapter list in seconds; it has spine indices. End of chapter is the
//     predicate "the next utterance belongs to a different spine item than the one the timer was armed in".
//   * THE BOUNDARY. Nothing here ever cuts a word. A minute timer that runs out mid-utterance only makes a stop
//     DUE; the stop itself happens at the next utterance boundary — the utterance that was playing finishes,
//     and the one after it is never started. End of chapter is only ever true AT a boundary (the one into the
//     next chapter), so it stops at the start of the next chapter and not mid-sentence.
//   * THE FADE. SleepTimer::fadeGain over the last kSleepFadeWindowSec seconds, down to kSleepFadeFloor rather
//     than to silence: the utterance still being spoken at expiry is finished, and finishing it inaudibly would
//     be cutting it by another name. For End of chapter the seconds left are an ESTIMATE from the characters
//     still to be spoken (the engine reports no durations); the stop itself is exact either way, because it is
//     the boundary, not the estimate, that stops narration.
//
// LIFETIME is the narration session: armed from the menu while narrating, disarmed by ANY stop — the user's,
// the timer's own, leaving the book (which stops narration), the end of the book. It shares nothing with the
// player's timer: that one lives on MainWindow and times the playback position; this one lives on the
// narration controller.
#pragma once
#include "ReadAloud.h"
#include "../media/SleepTimer.h"

namespace ReadAloud
{
    // The fade: over the last 10 s of narration, if the engine exposes a volume.
    inline constexpr double kSleepFadeWindowSec = 10.0;
    // Where the fade bottoms out. Not zero — see THE FADE above.
    inline constexpr double kSleepFadeFloor = 0.25;
    // An estimate of speaking rate at 1x, for End of chapter's fade only: ~150 words a minute of ~6 characters
    // (spaces included) is 15 characters a second. Too fast or too slow only moves where the fade starts; it
    // never moves the stop.
    inline constexpr double kSpokenCharsPerSecondAt1x = 15.0;

    // The seconds an engine will take to say `chars` characters at `speed` (a non-positive speed reads as 1x).
    inline double estimateSpeakingSeconds(int chars, double speed)
    {
        if (chars <= 0) return 0.0;
        const double s = speed > 0.0 ? speed : 1.0;
        return double(chars) / (kSpokenCharsPerSecondAt1x * s);
    }

    // The estimated seconds left in the chapter being narrated: every utterance from `current` to the end of
    // `plan`, less the seconds already spent on `current`. Never negative. An out-of-range `current` counts
    // from the start of the plan (nothing has been spoken yet).
    inline double chapterSecondsLeft(const QVector<Utterance>& plan, int current, double secondsIntoCurrent,
                                     double speed)
    {
        int chars = 0;
        for (int i = (current < 0 || current >= plan.size()) ? 0 : current; i < plan.size(); ++i)
            chars += int(plan[i].text.size());
        const double left = estimateSpeakingSeconds(chars, speed) - (secondsIntoCurrent > 0.0 ? secondsIntoCurrent : 0.0);
        return left > 0.0 ? left : 0.0;
    }

    class SleepSession
    {
    public:
        // Arm (or re-arm, replacing whatever was armed) while narrating. `now` is the caller's monotonic seconds;
        // `chapter` is the spine index narration is in; `paused` says whether narration is paused right now, in
        // which case the clock is armed stopped and starts on resume. Off, or a non-positive minute count,
        // disarms and returns false: nothing to time.
        bool arm(const SleepTimer::Timer& t, double now, int chapter, bool paused)
        {
            disarm();
            if (t.mode == SleepTimer::Mode::Off) return false;
            if (t.mode == SleepTimer::Mode::Minutes && !(t.minutes > 0.0)) return false;
            timer_   = t;
            chapter_ = chapter;
            armed_   = true;
            if (!paused) clock_.start(now);
            return true;
        }

        // Any stop ends the session's timer: the user's own Stop, the timer firing, leaving the book, the end of
        // the book, Off from the menu.
        void disarm() { *this = SleepSession(); }
        // The two lifetime edges, named so the controller says WHICH one it is at (and a probe can pin both).
        // Narration stopped for any reason: the timer belonged to that session and goes with it; starting
        // narration again does not bring it back. The book was left (another opened, the reader torn down):
        // the same, and for the same reason.
        void narrationStopped() { disarm(); }
        void bookLeft()         { disarm(); }

        bool             armed() const   { return armed_; }
        SleepTimer::Mode mode() const    { return armed_ ? timer_.mode : SleepTimer::Mode::Off; }
        double           minutes() const { return armed_ ? timer_.minutes : 0.0; }
        int              armedChapter() const { return armed_ ? chapter_ : -1; }

        // Narration paused / resumed. Idempotent, like the clock under them, so the controller may re-assert
        // the state it is in after every change rather than track transitions.
        void pause(double now)  { clock_.stop(now); }
        void resume(double now) { if (armed_) clock_.start(now); }

        // Seconds of narration since arming, paused time excluded.
        double narrated(double now) const { return armed_ ? clock_.elapsed(now) : 0.0; }

        // A minute timer whose minutes have run out: a stop is PENDING, to happen at the next boundary. Never
        // true for End of chapter, which is decided at the boundary alone.
        bool due(double now) const
        {
            return armed_ && timer_.mode == SleepTimer::Mode::Minutes
                && SleepTimer::fired(timer_, 0.0, narrated(now), false);
        }

        // THE BOUNDARY RULE. Asked when the utterance being spoken has finished and the next one — which belongs
        // to spine item `nextChapter` — is about to begin. True means stop HERE, before it: the minutes have run
        // out, or narration is crossing out of the chapter the timer was armed in.
        bool stopAtBoundary(double now, int nextChapter) const
        {
            return armed_ && SleepTimer::fired(timer_, 0.0, narrated(now), nextChapter != chapter_);
        }

        // Seconds of narration left before the timer fires, or <0 when unknown / not armed. Minutes: exact, from
        // the clock (0 once due). End of chapter: the caller's estimate, passed straight through.
        double secondsLeft(double now, double estimatedChapterSecondsLeft) const
        {
            if (!armed_) return -1.0;
            if (timer_.mode == SleepTimer::Mode::Minutes)
            {
                const double left = SleepTimer::expiryTime(timer_, 0.0, {}, 0.0) - narrated(now);
                return left > 0.0 ? left : 0.0;
            }
            return estimatedChapterSecondsLeft;
        }

        // The volume multiplier to apply now: 1 outside the fade window (or when not armed / the time left is
        // unknown), a linear ramp from 1 down to kSleepFadeFloor across the last kSleepFadeWindowSec seconds,
        // and the floor from expiry until the boundary stops narration.
        double gain(double now, double estimatedChapterSecondsLeft) const
        {
            const double left = secondsLeft(now, estimatedChapterSecondsLeft);
            if (left < 0.0) return 1.0;
            return kSleepFadeFloor + (1.0 - kSleepFadeFloor) * SleepTimer::fadeGain(left, kSleepFadeWindowSec);
        }

    private:
        SleepTimer::Timer    timer_;
        SleepTimer::RunClock clock_;
        int                  chapter_ = -1;
        bool                 armed_   = false;
    };
}
