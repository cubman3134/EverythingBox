// Pure decision core for OS-lifecycle pause/resume (Android app backgrounding). It holds only two "we paused
// it" latches and has NO Qt/UI dependencies, so it is unit-probeable in isolation (probe_formfactor pins it)
// while MainWindow::onApplicationStateChanged routes the real RetroView / MpvWidget state through it.
//
// The rule it encodes:
//   * On backgrounding, pause ONLY what is currently ACTIVE and NOT ALREADY PAUSED — a core/video the user
//     paused before leaving is left exactly as they left it.
//   * On foregrounding, resume ONLY what THIS policy paused. Something the user paused (before or after) is
//     never silently un-paused.
//   * The pause latches are STICKY across repeated suspend callbacks (Android can deliver Active -> Inactive
//     -> Suspended, and the second call sees the core already paused-by-us): a later suspend never clears an
//     earlier "we paused it" — only a resume does.
#pragma once

namespace mmv {

class LifecyclePolicy
{
public:
    // What to act on this transition. `core`/`video` true = pause (onSuspend) or resume (onResume) that item.
    struct Actions { bool core = false; bool video = false; };

    // Backgrounding (ApplicationSuspended / ApplicationInactive / ApplicationHidden). Inputs describe the
    // CURRENT runtime state. Returns what to newly pause; latches it (sticky OR) so the matching onResume
    // un-pauses exactly what we froze, even across multiple suspend callbacks.
    Actions onSuspend(bool coreRunning, bool corePaused, bool videoActive, bool videoPaused)
    {
        Actions a;
        a.core  = coreRunning && !corePaused;
        a.video = videoActive && !videoPaused;
        pausedCore_  = pausedCore_  || a.core;
        pausedVideo_ = pausedVideo_ || a.video;
        return a;
    }

    // Foregrounding (ApplicationActive). Returns ONLY what we paused, then clears the latches so a later
    // resume (or a user-driven pause in between) can't double-fire a resume.
    Actions onResume()
    {
        Actions a{ pausedCore_, pausedVideo_ };
        pausedCore_ = pausedVideo_ = false;
        return a;
    }

    bool corePausedByUs()  const { return pausedCore_; }
    bool videoPausedByUs() const { return pausedVideo_; }

private:
    bool pausedCore_  = false;
    bool pausedVideo_ = false;
};

} // namespace mmv
