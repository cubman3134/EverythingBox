// Should a pending external-emulator launch be cancelled, and how? Pulled out of EmulatorManager as a pure
// function so the decision can be probed without a window, a network, or a child process (house rule: a new
// pure component gets a probe).
//
// The question has a two-phase answer because the external-launch pipeline has two ownership regimes, and a
// cancel that ignores the difference either wedges the manager or races itself:
//
//   1. INSTALL CHAIN (startInstall -> fetch -> download -> extract -> finishInstall). Every continuation in it
//      is connected to the manager itself, not to the per-launch context, so retiring that context cancels
//      nothing here; and clearing `busy_` from the cancel would let a new play() of the same emulator race the
//      still-running download/extract on the same members and directory. The correct cancel is to DISARM the
//      boot (launchAfterInstall = false) and ABORT the in-flight download, then let the chain's own finished
//      handler do what it always does — delete the partial file and clear busy_. The manager frees itself
//      within an event-loop turn, and nothing boots. If the cancel lands in the short extract/install window
//      instead (no reply in flight, seconds not minutes), there is nothing to abort: that step runs to
//      completion as an install-only one, and finishInstall clears busy_.
//
//   2. LAUNCH PHASE (launch() onward: the BIOS/keys fetch chains, the PS3 update worker's boot continuation).
//      Every continuation there is parented or connect-bound to the per-launch context, so retiring the
//      context orphans all of them at once. But then NOTHING remains that would ever clear `busy_` — the
//      dropped continuation is exactly the code that used to — so the cancel must clear it and emit a terminal
//      signal itself, or the manager stays wedged and the wait page never goes away.
//
// The three cases that are deliberately NOT cancellable:
//   * not busy — there is no pending launch to supersede;
//   * a game process is already running — ending that is closeGame()/terminateGame() territory (destructive,
//     save-affecting), not this primitive's business;
//   * launchAfterInstall false — a Settings-initiated install boots nothing, so there is nothing to supersede,
//     and the user wants that download to finish. (This is also what keeps an in-app launch from silently
//     killing a download the user deliberately started from Settings ▸ Emulators.)
#pragma once

namespace LaunchCancel {

enum class Action
{
    None,          // nothing pending that this primitive may cancel
    CancelInstall, // install chain owns the flow: disarm the boot and abort the download; the chain clears busy_
    CancelNow,     // launch phase: retire the context, clear busy_, emit the terminal signal here
};

// `installing` is what distinguishes the two regimes: true between startInstall() and the top of launch().
inline Action decide(bool busy, bool gameRunning, bool launchAfterInstall, bool installing)
{
    if (!busy || gameRunning || !launchAfterInstall) return Action::None;
    return installing ? Action::CancelInstall : Action::CancelNow;
}

} // namespace LaunchCancel
