// MainWindow, the retro-computer half (issue #190).
//
// One member lives here, and it is here rather than in MainWindow.cpp for two reasons that are both about
// the file rather than the feature: MainWindow.cpp is already over MSVC's 65,536-COMDAT-sections-per-object
// limit (it is built with /bigobj to get past C1128), and ten concurrent branches editing one 25,000-line
// file is exactly how a merge ends up silently double-defining a member — a fault that no conflict marker
// reports and only C2084 catches.
//
// WHAT IT DOES. A folder game (an MS-DOS game is a directory of files, not one ROM) can hold several
// plausible programs. LaunchRecipes::chooseExecutable resolves the ordinary case with no question asked; when
// it genuinely cannot, GameLauncher stops the launch and emits chooseBootProgram, and this is the answer:
// ask once with a nav-kit NavMenu, record the answer in the game's launch override, re-open.
//
// TWO THINGS ARE LOAD-BEARING HERE.
//
//  1. The re-open goes through openGamePath, NOT GameLauncher::open. Increment 1 called open() directly,
//     which is right for a full-screen launch and wrong for a split pane: the pane is still the focused
//     target, so the answer to "which program?" has to land back IN the pane, not full-screen over it.
//     openGamePath's own tail IS launcher_->open() when no pane is focused, so the full-screen route is
//     unchanged; when a pane is focused it routes into the pane, which is what makes the picker exist on the
//     split-pane path at all (increment 1 documented it as missing).
//
//  2. The identity the answer is filed under is "the stable item id, else the source path". A catalog row has
//     an id; a game opened straight off disk has only its path — and it MUST still get an identity, because
//     re-opening with nothing remembered would ask the same question again, forever. GameLauncher recomputes
//     that key from the same two values, so both sides agree without a new field on the plan.
//
// The NavMenu opens a nested event loop, so this is only ever reached through a QUEUED connection (the
// launch that emitted the signal has fully unwound first) — the #28 / #211 family. Nothing here opens a
// dialog: NavMenu is the nav kit, which renders in-window over both the themed and the classic surface.
#include "MainWindow.h"

#include "../launch/GameLauncher.h"
#include "../core/LaunchOptionsStore.h"   // per-game launch override: this is where the answer is stored
#include "nav/NavOverlay.h"          // NavMenu::pick — the nav kit, never a QDialog

void MainWindow::askBootProgramThenReopen(const QString& title, const QStringList& choices, const QString& rom,
                                          const QString& thumb, const QString& key, const QString& systemHint)
{
    if (choices.isEmpty()) return;
    const int row = NavMenu::pick(tr("Which program runs “%1”?").arg(title), choices, this);
    if (row < 0 || row >= choices.size()) return;   // backed out: no launch, nothing remembered

    const QString bootKey = key.isEmpty() ? rom : key;
    LaunchOpts::Override ov = LaunchOpts::get(bootKey);
    ov.bootFile = choices.at(row);
    LaunchOpts::set(bootKey, ov);

    // Re-open: resolution now finds the stored answer and has nothing left to ask. Through openGamePath so a
    // split pane keeps the launch (see the note at the top of this file).
    openGamePath(rom, title, thumb, key, systemHint);
}
