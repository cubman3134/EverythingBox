// The settings area's exit gate, run one event-loop turn late for the classic doors (issue #471). This file
// holds MainWindow::leaveSettingsAreaLater and nothing else. It is a separate translation unit so this member
// does not add to MainWindow.cpp.
//
// WHAT WENT WRONG. Leaving the settings area goes through leaveSettingsArea(), and when a setting changed during
// the visit that gate asks "Save changes?" with NavConfirm::ask, which is a nested QEventLoop. The classic hub's
// Back, its Split Screen row and classic Stats' Back called the gate directly, so the loop ran INSIDE the press:
//
//     sendNavKey -> panelBack_->click() -> QAbstractButton::clicked -> panelOnBack_ -> leaveSettingsArea
//                -> NavConfirm::ask -> QEventLoop::exec
//
// (that is the GUI thread's stack, sampled while the prompt was up). The press therefore did not return until
// the prompt was answered. The app stayed responsive, because the nested loop serves every other event, but
// anything waiting for the press to finish waited for the user. A uitest `key back` holds its reply until the
// key's handler returns, so it waited as long as the prompt stood, and uitest.py reported that as "the GUI
// thread is blocked". That is issue #471: change the theme in classic Appearance, press Back twice, and the
// harness saw 30 s and more. The theme had nothing to do with it; any changed setting brings up the prompt.
//
// The themed hub never did this. ThemedPanelHost defers its root Back by a turn (#165), so the themed Save
// prompt opens after the press has unwound. This is the classic half of the same rule: a nested loop does not
// run inside the signal delivery that asked for it (see MainWindow::deferPastQmlEmission, and #28 and #211 for
// what it costs when one does). Only the ordering changes. The prompt, its three answers and the page each
// answer leads to are exactly what leaveSettingsArea already does, because it is still leaveSettingsArea that
// does it.
//
// WHY THE CALLERS CAN WAIT A TURN. None of them needs the gate's answer. "Keep editing" leaves the classic panel
// standing (showPanel's Back only calls panelOnBack_, so nothing was torn down). The themed hub's root Back is
// the one caller that does need the bool, and it stays on leaveSettingsArea, already a turn late (#165).
//
// Pinned by the "classic settings exit off the Back dispatch (#471)" gate in native/tools/run-headless-probes.sh.
#include "MainWindow.h"

#include <QMetaObject>

void MainWindow::leaveSettingsAreaLater(std::function<void(SettingsReturn)> proceed)
{
    // ONE exit per request. Two presses can both land before the queued gate runs: a key auto-repeat, or a
    // Back that arrives in the same batch as the first. Each would queue a gate, and the second would open a
    // second prompt from inside the first prompt's loop. The flag also stays set while the prompt is up, so a
    // click that reaches the header Back behind the prompt cannot stack another one. A second press here
    // means what the first already asked for.
    if (settingsExitQueued_) return;
    settingsExitQueued_ = true;
    QMetaObject::invokeMethod(this, [this, proceed = std::move(proceed)] {
        // Left some other way during the turn (a navigation the user did not start here). That route either
        // went through the gate itself or was handled by enterSettingsArea's stale-transaction commit, so
        // there is nothing left for this one to close.
        if (inSettingsArea()) leaveSettingsArea(proceed);
        settingsExitQueued_ = false;
    }, Qt::QueuedConnection);
}
