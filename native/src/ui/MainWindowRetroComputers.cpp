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
#include "../core/DosConf.h"         // #191: gameMidiChoices — what the per-game MIDI chooser offers
#include "../core/LaunchRecipe.h"    // #191: the device list is the system recipe's `midi` block (data, not C++)
#include "../core/EmulationTarget.h" // #191: the core a game resolves to, the way prepareCore resolves it
#include "../core/Settings.h"
#include "../core/SystemCatalog.h"
#include "HomeView.h"                // #191: the game menu's "MIDI device…" door

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

// ---- The per-game MIDI device (issue #191) -----------------------------------------------------------------
// #191 asked for a per-system MIDI device "overridable per game (#51)". The per-system half is the MS-DOS
// setting on both settings builders; this is the per-game half, and it is a #51 lever like any other: stored
// on the game's LaunchOptionsStore record (global, cloud-merged, husk-on-clear), offered on the SAME two
// per-game surfaces as #189's update/DLC levers — the themed detail's "Launch options…" editor and the
// Start-menu emulation panel at "This game" scope — through this one handler, so there is one write path.
//
// The choices are the recipe's DATA (DosConf::gameMidiChoices over LaunchRecipes::midiSpecFor): "Default (use
// MS-DOS setting)" first, which stores the EMPTY value — not an override — then every declared device. They
// come from the recipe entry of the core the game RESOLVES to, the entry the launch reads, so a game gets no
// entry at all unless that core has a MIDI option: every system but MS-DOS, and an MS-DOS game moved onto
// dosbox_core or a standalone DOSBox. A row the launch would ignore is a lever with nothing behind it (#189).
//
// Picking a device fetches nothing. The launch (GameLauncher::dosMidiSeed -> DosConf::midiLaunch) checks the
// system folder for the RESOLVED device's files and, when one is missing, says which file and which folder —
// the same message the MS-DOS setting gives — and the game still plays, on its default audio.
static QList<QPair<QString, QString>> gameMidiChoicesFor(const QString& systemId, const QString& core)
{
    const LaunchRecipe& recipe = LaunchRecipes::forSystem(systemId);
    if (recipe.isNull()) return {};
    return DosConf::gameMidiChoices(LaunchRecipes::midiSpecFor(recipe, core));
}

bool MainWindow::systemOffersGameMidi(const QString& systemId, const QString& core)
{
    return !gameMidiChoicesFor(systemId, core).isEmpty();
}

QString MainWindow::gameMidiLeverValue(const LaunchOpts::Override& ov, const QString& systemId, const QString& core)
{
    const QString cur = ov.midiDevice.trimmed().toLower();
    if (cur.isEmpty()) return tr("Default (use MS-DOS setting)");
    for (const auto& c : gameMidiChoicesFor(systemId, core))
        if (c.second == cur) return c.first;
    // A stored device this recipe no longer declares is ignored at launch (DosConf::resolveMidi falls through),
    // so say that rather than name a device that will not be used.
    return tr("%1 (not offered — using the MS-DOS setting)").arg(cur);
}

#ifdef EB_HAVE_RETROPARK
static constexpr bool kMidiRetroParkBuildAvailable = true;    // MainWindow.cpp's kRetroParkBuildAvailable, which is
#else                                                         // file-static there
static constexpr bool kMidiRetroParkBuildAvailable = false;
#endif

// The libretro core this game launches on, folded exactly as editLaunchOptions and prepareCore fold it (the
// game's #51 override over the per-system defaults). "" for a standalone or RetroPark target.
static QString resolvedMidiCore(const QString& key, const QString& systemId)
{
    const GameSystem* sys = SystemCatalog::byId(systemId);
    if (!sys) return QString();
    const EmulationTarget t = resolveEmulationTarget(
        sys, key.isEmpty() ? LaunchOpts::Override{} : LaunchOpts::get(key), Settings::coreFor(sys->id),
        Settings::emulatorFor(sys->id), Settings::backendFor(sys->id), kMidiRetroParkBuildAvailable,
        kStandaloneBuildAvailable);
    return t.engine == EmuEngine::Libretro ? t.ref : QString();
}

// The classic layout's door. HomeView asks before offering the row, and the pick arrives QUEUED: the menu that
// emitted it has closed by then, and editGameMidiDevice runs a nested NavMenu loop, which must never run inside
// a signal delivery (the #28 / #211 family).
void MainWindow::wireGameMidiDoor()
{
    if (!home_) return;
    home_->setGameMidiOffered([](const QString& key, const QString& systemId) {
        return systemOffersGameMidi(systemId, resolvedMidiCore(key, systemId));
    });
    connect(home_, &HomeView::gameMidiRequested, this, [this](const QString& key, const QString& systemId) {
        editGameMidiDevice(key, systemId, resolvedMidiCore(key, systemId));
    }, Qt::QueuedConnection);
}

void MainWindow::editGameMidiDevice(QString key, QString systemId, QString core)
{
    if (key.isEmpty()) return;
    const QList<QPair<QString, QString>> choices = gameMidiChoicesFor(systemId, core);
    if (choices.isEmpty()) return;
    const QString cur = LaunchOpts::get(key).midiDevice.trimmed().toLower();
    QStringList rows;
    for (const auto& c : choices)
    {
        // The Default row carries the translated label; the devices' labels are the recipe's own.
        const QString label = c.second.isEmpty() ? tr("Default (use MS-DOS setting)") : c.first;
        rows << ((c.second == cur ? QStringLiteral("✓  ") : QStringLiteral("     ")) + label);
    }
    const int pick = NavMenu::pick(tr("MIDI device"), rows, this);
    if (pick < 0 || pick >= choices.size()) return;   // Back: no write
    LaunchOpts::Override next = LaunchOpts::get(key);
    next.midiDevice = choices.at(pick).second;         // "" = Default: the override is cleared, not set to a value
    LaunchOpts::set(key, next);
}
