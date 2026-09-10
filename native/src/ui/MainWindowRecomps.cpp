// THE SELF-COMPILED TIER'S CARD (issue #248, increments b and c).
//
// A catalogue entry that names a recompiler (`build.generate.engine` — psxrecomp / snesrecomp / gbarecomp) is
// not a download. The port does not exist anywhere as a binary somebody could hand you; it is PRODUCED on this
// machine, by that engine, out of the dump you already own. Increment (b) listed those entries and stopped
// honestly; increment (c) is where the build happens.
//
// WHAT THE CARD SAYS, and each line is here because a person deciding needs it BEFORE anything happens:
//   * which engine builds it, by name. A self-compiled port is that project's program as much as the port
//     author's, and the credit belongs on screen either way;
//   * under what LICENCE that engine is. All three named engines are PolyForm Noncommercial 1.0.0, which is a
//     term with actual consequences for a user, and #248 named psxrecomp's case specifically. Nothing of any
//     engine is bundled in this app: the engine comes from the project's own release, onto this machine, at
//     the moment a build is asked for, and it lands beside a notice saying so;
//   * WHETHER THIS COMPUTER CAN BUILD IT AT ALL, before the button rather than after it. core/Toolchain.h
//     answers that, and when the answer is no the card names exactly what to install and offers the vendor's
//     own installer page. This app does not download compilers and has none hidden away to fall back on;
//   * which dump it would be built from — read where it lies, never copied, never moved.
//
// WHILE IT RUNS the card is LIVE: a timer inside the nav card's own nested loop re-reads RecompBuildJob's
// snapshot and relabels the message with the phase, the percentage when the tools give one, the elapsed time
// and the tail of the build log. That is NavCountdown's proven arrangement (NavOverlay.h: timers fire inside
// ask()'s loop), and it is why there is no separate window and no nested loop of our own beyond the one every
// nav card already runs. Walking away does not stop the build — it lives in RecompBuildJob, not in this card.
//
// In its OWN translation unit rather than appended to MainWindow.cpp: that file is one giant TU that a dozen
// branches edit at once, and a feature that can live beside it should (#186's direction). Only the declaration
// is in MainWindow.h.
#include "MainWindow.h"

#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

#include "../core/ArchiveRom.h"
#include "../core/DownloadsStore.h"
#include "../core/HashVerify.h"
#include "../core/EmulatorManager.h"
#include "../core/RecompBuild.h"
#include "../core/RecompBuildJob.h"
#include "../core/RecompFeed.h"
#include "../core/RecompRows.h"
#include "../core/RecompUpdates.h"
#include "../core/RomLibrary.h"
#include "../core/Toolchain.h"
#include "../launch/GameLauncher.h"
#include "HomeView.h"
#include "nav/NavOverlay.h"

namespace {

// The library, in the shape the ROM-identity gate reads. The same three sources HomeView::populateRecomps
// gathers, kept local because that function builds a whole section out of them and this one needs a single
// path — and because a shared helper would have to live somewhere that can see both RomLibrary and the
// downloads store, which is to say in one of the two files anyway.
QVector<recomps::LibraryRom> gatherLibrary()
{
    QVector<recomps::LibraryRom> library;
    auto add = [&library](const QString& systemId, const QString& title, const QString& path) {
        recomps::LibraryRom r;
        r.systemId = systemId;
        r.title = title;
        r.path = path;
        if (!path.isEmpty())
        {
            const QFileInfo fi(path);
            if (fi.exists()) r.size = fi.size();
            r.archive = ArchiveRom::isArchive(path);
            const HashVerify::Hashes h = HashVerify::cachedHashes(path);
            r.hashes = { h.crc, h.md5, h.sha1, h.sha256 };
        }
        library.push_back(r);
    };
    for (const RomLibrary::SystemGroup& g : RomLibrary::scan())
        for (const RomLibrary::Rom& r : g.roms) add(r.systemId, r.title, r.path);
    for (const DownloadedItem& d : DownloadsStore::list())
        if (d.kind == QStringLiteral("game") && !d.system.isEmpty()) add(d.system, d.title, d.path);
    return library;
}

// The message a live build card shows, rebuilt from the job's snapshot on every tick. One string: the phase
// and its progress, then the last few lines the build tools printed.
QString liveMessage(const RecompBuildJob::Snapshot& s)
{
    QStringList lines;
    if (recompbuild::isTerminal(s.phase) && !s.message.isEmpty())
        lines << s.message;
    else if (s.stopping)
        // The stop is not always instantaneous — see RecompBuildJob::Snapshot::stopping — so it says so
        // rather than leaving a Stop that appears to have done nothing.
        lines << MainWindow::tr("Stopping… (%1)").arg(recompbuild::progressLine(s.progress));
    else
        lines << recompbuild::progressLine(s.progress);
    if (!s.logTail.isEmpty())
    {
        // The LAST few, because that is where a build says what went wrong. Six rather than the tail's full
        // twenty-four: a nav card that grows past the window is a card whose buttons walk off the bottom.
        const QStringList tail = s.logTail.mid(qMax(0, s.logTail.size() - 6));
        lines << tail.join(QStringLiteral("\n"));
    }
    if (!s.logFilePath.isEmpty()) lines << MainWindow::tr("Full log: %1").arg(s.logFilePath);
    return lines.join(QStringLiteral("\n\n"));
}

// NavConfirm::ask, with a heartbeat. Identical loop to the one in NavOverlay.cpp — the card owns the timer,
// so the timer dies with the card — plus a `refresh` that relabels the message each tick and a `done`
// predicate that closes the card by itself when the build ends.
int askLive(const QString& title, const std::function<QString()>& message, const QStringList& buttons,
            int focusIndex, int cancelIndex, const std::function<bool()>& done, int doneResult,
            QWidget* window)
{
    auto* card = new NavConfirm(title, message(), buttons, focusIndex, window);
    int result = cancelIndex;
    QEventLoop loop;
    QObject::connect(card, &NavOverlay::closed, &loop, [&](int r) {
        result = (r < 0) ? cancelIndex : r;
        loop.quit();
    });
    auto* beat = new QTimer(card);
    QObject::connect(beat, &QTimer::timeout, card, [card, message, done, doneResult]() {
        card->setMessage(message());
        if (done && done()) card->dismiss(doneResult);
    });
    beat->start(700);
    loop.exec();
    return result;
}

}  // namespace

// The one-time wiring, done here rather than in MainWindow's constructor so this feature costs that file no
// hunk at all (#186). A build can only ever be started from the card below, so the first call to this
// function always precedes the first build.
void MainWindow::ensureRecompBuildWiring()
{
    if (recompBuildWired_) return;
    recompBuildWired_ = true;

    // THE DOWNLOADS-STYLE SURFACE. A build survives navigating away, so its progress has to be visible from
    // wherever the user went: a sticky note that relabels itself, and the Recomps rows repainting. Both are
    // throttled to once a second — a compile emits hundreds of lines a second and neither the notifier nor a
    // section repaint is free.
    connect(&RecompBuildJob::instance(), &RecompBuildJob::progressed, this, [this]() {
        if (recompBuildPaintDue_.isValid() && recompBuildPaintDue_.elapsed() < 1000) return;
        recompBuildPaintDue_.restart();
        const RecompBuildJob::Snapshot s = RecompBuildJob::instance().snapshot();
        if (!s.running()) return;
        notify(tr("Building %1 — %2").arg(s.title, recompbuild::progressLine(s.progress)), 0);
        if (home_) home_->refreshRecompsIfShown();
    });
    connect(&RecompBuildJob::instance(), &RecompBuildJob::ended, this,
            [this](const QString&, int, const QString& message) {
                // Never a spinner that stopped: every ending says what happened, in a sentence, and the
                // failure ones carry the path to the full log.
                notify(message, 12000);
                if (home_) home_->refreshRecompsIfShown();
            });

    // ---- #248 (d): THE MOMENT A NEW BUILD PROVES ITSELF ------------------------------------------------
    // A rebuild leaves TWO builds on the disk — the new one, and the one that worked before it — and the old
    // one is not removed until the new one has actually run. This is where "has actually run" is decided, and
    // it is the only place the kept copy is ever dropped.
    //
    // ON THE LAUNCHER'S OWN SIGNAL rather than on a timer or on a guess: it reports whether a process existed
    // at all, how long it was up and whether the user closed it themselves, which are exactly the three facts
    // recompupdate::launchProvesBuild needs. A run that does not prove the build changes NOTHING — the kept
    // copy stays and the row offers to go back to it.
    if (launcher_)
        connect(launcher_, &GameLauncher::externalRunEnded, this,
                [this](const QString& emulatorId, bool started, qint64 upMs, bool userClosed) {
                    if (emulatorId.isEmpty()) return;
                    const ExternalEmulator* p = NativePorts::byId(emulatorId);
                    ExternalEmulator feedPort;
                    if (!p && RecompFeed::findById(emulatorId, &feedPort)) p = &feedPort;
                    if (!p) return;
                    const QString installDir = EmulatorManager::installDir(*p);
                    const QString title = p->port.name.isEmpty() ? p->displayName : p->port.name;
                    if (!recompupdate::launchProvesBuild(started, upMs, userClosed))
                    {
                        // Only worth saying when there is something to go back TO. A recomp that has always
                        // been the only build on this machine crashing is increment (c)'s territory.
                        if (recompupdate::hasKept(installDir))
                            notify(recompupdate::keptSurvivedSentence(title), 14000);
                        return;
                    }
                    // It ran. The stamp records that (so a restart does not undo it), and the copy that was
                    // being held in case it did not is removed.
                    recompupdate::markLaunched(installDir);
                    if (recompupdate::hasKept(installDir))
                    {
                        const qint64 freed = recompupdate::keptBytes(installDir);
                        if (recompupdate::dropKept(installDir))
                            notify(recompupdate::keptRemovedSentence(title, freed), 8000);
                    }
                    if (home_) home_->refreshRecompsIfShown();
                });
}

void MainWindow::showSelfCompiledPort(const ExternalEmulator& port)
{
    ensureRecompBuildWiring();

    const RecompFeed::Engine engine = RecompFeed::engineInfo(port.port.buildEngine);
    // The port project's own name where the catalogue gives one, never the recompiler's brand as the title
    // (#233: those developers asked a third-party launcher for exactly that).
    const QString heading = port.displayName.isEmpty() ? port.port.name : port.displayName;

    RecompBuildJob& job = RecompBuildJob::instance();

    // ---- already building this one: the live card ------------------------------------------------------
    if (job.isActive(port.id) && job.snapshot().running())
    {
        const int choice = askLive(
            heading, []() { return liveMessage(RecompBuildJob::instance().snapshot()); },
            { tr("Leave it running"), tr("Stop the build") }, /*focusIndex*/ 0, /*cancelIndex*/ 0,
            []() { return !RecompBuildJob::instance().snapshot().running(); }, /*doneResult*/ 0, this);
        if (choice == 1)
        {
            // A cancel that actually stops it: the flag the runner watches is set, the child is asked to
            // stop and then killed, and the state is already Cancelled before the child's exit arrives.
            job.cancel();
            notify(tr("Stopping the build…"), 4000);
        }
        if (home_) home_->refreshRecompsIfShown();
        return;
    }

    // ---- the standing card -----------------------------------------------------------------------------
    QStringList lines;
    lines << tr("%1 is a recompilation of “%2” that is BUILT ON THIS COMPUTER — there is no download of the "
                "finished program. It is made from your own copy of the game by a separate recompiler, and it "
                "is not made by EverythingBox.")
                 .arg(heading, port.port.name);

    // ---- #248 (d): where this copy stands against the catalogue ----------------------------------------
    // SECOND, ahead of the standing explanation of what a recomp is, and that position is deliberate: these
    // two lines are about the state of this machine RIGHT NOW, and a live drive showed why it matters. The
    // card is long enough on a 1280x760 window that its last paragraph is clipped by the button row — with
    // the disk statement written last, the one sentence saying two builds are taking up space was the one
    // sentence nobody could read.
    //
    // Read off what the BUILD recorded about itself, never off a timestamp and never off the catalogue's own
    // revision — a catalogue is republished whenever anybody's entry is approved, and this one may not have
    // changed at all. RecompUpdates.h holds the comparison and the wording.
    const QString installDir = EmulatorManager::installDir(port);
    const bool installedHere = EmulatorManager::isInstalled(port);
    const recompupdate::BuildStamp stamp = recompupdate::readStamp(installDir);
    const recompupdate::CatalogueBuild wanted = recomps::catalogueBuildOf(port);
    const recompupdate::Update verdict =
        installedHere ? recompupdate::compareBuild(stamp, wanted) : recompupdate::Update::Unknown;
    const bool updateHere = recompupdate::updateAvailable(verdict);
    const bool keptHere = recompupdate::hasKept(installDir);

    if (installedHere && verdict != recompupdate::Update::Unknown)
        lines << recompupdate::updateSentence(verdict, stamp, wanted);
    // THE DISK COST (#248 d item 5). Two builds of one title exist between a rebuild and its first run, and
    // that is said with a size and a path rather than left to be discovered.
    if (keptHere)
        lines << recompupdate::keptCopySentence(heading, recompupdate::keptBytes(installDir),
                                                recompupdate::keptDirFor(installDir));

    if (!port.port.description.isEmpty()) lines << port.port.description;

    // The engine, and its terms. Only what has been checked: an engine this build does not know keeps an
    // empty licence, and the line is left out rather than guessed at.
    if (!engine.id.isEmpty())
    {
        lines << (engine.license.isEmpty()
                      ? tr("It is built with %1.").arg(engine.id)
                      : tr("It is built with %1, which is licensed %2. EverythingBox does not include or "
                           "redistribute %1 — it comes from the project's own release, onto this computer, "
                           "when you ask for a build.")
                            .arg(engine.id, engine.license));
    }
    if (!port.port.authorNotes.isEmpty()) lines << port.port.authorNotes;

    // WHAT THIS COMPUTER HAS. Cached, so this is a hash lookup rather than four process launches, and
    // re-checked on demand by the verb below.
    const toolchain::Decision tc = toolchain::decide(toolchain::detect(), toolchain::hostOs());
    lines << tr("%1 %2").arg(tc.headline, tc.detail);

    // WHICH DUMP, and it is read where it lies. The gate is increment (b)'s, unchanged and not re-implemented.
    const QVector<recomps::LibraryRom> library = gatherLibrary();
    const QString rom = recomps::matchedDumpPath(port, library);
    if (rom.isEmpty())
        lines << tr("No copy of the game that matches this entry was found in your library, so there is "
                    "nothing to build from yet. Nothing is ever downloaded for you — the dump has to be one "
                    "you already own.");
    else
        lines << tr("It would be built from your own copy at %1. That file is read where it is; it is never "
                    "copied, moved or changed.").arg(rom);

    // The result of the LAST build of this entry, if there was one this session. A card that opens after a
    // failure and says nothing about it is how a person ends up pressing the same button again.
    const RecompBuildJob::Snapshot last = job.snapshot();
    if (last.portId == port.id && !last.message.isEmpty()) lines << last.message;

    enum class Verb { Cancel, Build, Install, Recheck, Engine, Homepage, Play, GoBack };
    QStringList buttons{ tr("Cancel") };
    QVector<Verb> verbs{ Verb::Cancel };
    const bool canBuild = tc.canBuild() && !rom.isEmpty();
    // PLAY, and this is where the self-compiled tier finally reaches the launch seam #233 built. It is the
    // SAME verb, the same EmulatorManager install folder and the same process supervision a downloaded port
    // gets — a built recomp is not a second kind of program.
    if (installedHere)
    {
        buttons << tr("Play (native)");
        verbs << Verb::Play;
    }
    if (canBuild)
    {
        // A REBUILD IS EXPLICIT AND IT IS THIS BUTTON. Nothing anywhere else in this feature starts one:
        // a feed refresh moves the label on the row and stops there (#248 d, decision 2).
        buttons << (installedHere ? (updateHere ? tr("Rebuild (newer version)")
                                                : tr("Build it again"))
                                  : tr("Build it here"));
        verbs << Verb::Build;
    }
    // GOING BACK. Offered whenever the previous build is still being held, which is exactly the window in
    // which a rebuild might have produced something that does not work.
    if (keptHere)
    {
        buttons << tr("Go back a build");
        verbs << Verb::GoBack;
    }
    if (!tc.canBuild())
    {
        // The exact thing to install, at the vendor's own page. Never a package manager command pretending to
        // be a link, and never a download this app performs.
        buttons << tr("How to install the build tools");
        verbs << Verb::Install;
        buttons << tr("Check again");
        verbs << Verb::Recheck;
    }
    if (!engine.homepage.isEmpty()) { buttons << tr("Open %1").arg(engine.id); verbs << Verb::Engine; }
    if (!port.homepage.isEmpty())   { buttons << tr("Open homepage");          verbs << Verb::Homepage; }

    // WHAT THE CARD OPENS ON, in the order somebody would want it: the rebuild when there is an update to
    // take, otherwise playing what is already there, otherwise building it for the first time. Resolved
    // through the verb list rather than by index, because the button set is conditional and a hardcoded 1 is
    // how the focus lands on Cancel the first time a verb is added above it.
    const Verb preferred = (canBuild && updateHere) ? Verb::Build
                                                    : (installedHere ? Verb::Play
                                                                     : (canBuild ? Verb::Build : Verb::Cancel));
    const int focusIdx = qMax(0, int(verbs.indexOf(preferred)));

    const int choice = NavConfirm::ask(heading, lines.join(QStringLiteral("\n\n")), buttons,
                                       focusIdx, /*cancelIndex*/ 0, this);
    if (choice < 0 || choice >= verbs.size()) return;

    switch (verbs.at(choice))
    {
        case Verb::Cancel:
            return;

        case Verb::Play:
        {
            // No ROM argument, exactly as the pre-built tier's launch: a recomp IS the game — the dump was
            // consumed at generate time and the program does not take one.
            if (!launcher_) return;
            launcher_->runEmulator(port, QString(), heading, QString(), QString(), port.port.platform);
            return;
        }

        case Verb::GoBack:
        {
            const int sure = NavConfirm::ask(
                tr("Go back to the previous build of %1?").arg(heading),
                tr("This puts back the build that was working before the last rebuild, and removes the one "
                   "that replaced it. Your saved games are the port's own and are not touched."),
                { tr("Cancel"), tr("Go back") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
            if (sure != 1) return;
            QString why;
            if (recompupdate::restoreKept(installDir, &why))
                notify(recompupdate::restoredSentence(heading), 9000);
            else
                notify(tr("Couldn't put the previous build of %1 back — %2.").arg(heading, why), 10000);
            if (home_) home_->refreshRecompsIfShown();
            return;
        }

        case Verb::Engine:
            QDesktopServices::openUrl(QUrl(engine.homepage));
            return;

        case Verb::Homepage:
            QDesktopServices::openUrl(QUrl(port.homepage));
            return;

        case Verb::Install:
        {
            // One page per missing thing, and the user picks which. `missing` is never empty here — the
            // decision table guarantees it for every verdict that is not Ready.
            QStringList items;
            for (const toolchain::Requirement& r : tc.missing) items << r.what;
            const int which = NavMenu::pick(tr("What to install"), items, this);
            if (which < 0 || which >= tc.missing.size()) return;
            QDesktopServices::openUrl(QUrl(tc.missing.at(which).url));
            return;
        }

        case Verb::Recheck:
        {
            // Somebody who has just installed the Build Tools in the other window must not have to restart
            // the app to be believed.
            toolchain::recheck();
            const toolchain::Decision again = toolchain::decide(toolchain::detect(), toolchain::hostOs());
            notify(again.headline, 6000);
            if (home_) home_->refreshRecompsIfShown();
            return;
        }

        case Verb::Build:
            break;
    }

    QString why;
    if (!job.start(port, rom, &why))
    {
        notify(why.isEmpty() ? tr("That build couldn't be started.") : why, 8000);
        return;
    }
    if (home_) home_->refreshRecompsIfShown();

    // Straight into the live card, so the first thing after pressing Build is the build. Closing it leaves
    // the compile running — the note and the row keep reporting it.
    const int during = askLive(
        heading, []() { return liveMessage(RecompBuildJob::instance().snapshot()); },
        { tr("Leave it running"), tr("Stop the build") }, /*focusIndex*/ 0, /*cancelIndex*/ 0,
        []() { return !RecompBuildJob::instance().snapshot().running(); }, /*doneResult*/ 0, this);
    if (during == 1)
    {
        job.cancel();
        notify(tr("Stopping the build…"), 4000);
    }
    if (home_) home_->refreshRecompsIfShown();
}
