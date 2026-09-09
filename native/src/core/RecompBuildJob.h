// THE BUILD, AS THE APP RUNS IT (issue #248, increment c).
//
// ONE OBJECT, OUTSIDE THE UI, AND THAT IS THE FEATURE. #248 asks for a build that "survives navigating away
// and shows Downloads-style progress". So the build does not belong to the card that started it: it belongs
// here, to a single long-lived object the window can ask about at any time. Close the card, walk back out to
// the home screen, open something else — the compile carries on, the Recomps row keeps saying `building…`
// with a live percentage, and coming back to the row shows the same build with its log tail.
//
// OFF THE GUI THREAD, AND ONLY ONE AT A TIME. The whole sequence runs on a pooled thread; the GUI thread only
// ever reads a mutex-guarded snapshot and receives a queued `progressed()`. Two builds at once would compete
// for every core on the machine and make both take longer than doing them in turn, so a second Start while
// one is running is refused with a sentence rather than queued behind an invisible wait.
//
// WHAT IT ACTUALLY DOES, in order, and every one of these is a state in recompbuild::Machine:
//   1. asks core/Toolchain.h what this machine has. No compiler or no CMake = Blocked, before anything is
//      downloaded and before anything is written;
//   2. downloads the port's own SOURCE from the project's GitHub, bounded and deadlined (BoundedFetch);
//   3. unpacks it into <data>/recomps/builds/<id>/source, refusing any member that tries to escape;
//   4. finds the recompiler the entry names inside that tree — SCHEMA.md's "harvest emitters from the game
//      release zip" — and runs it against the user's dump, IN PLACE, by path;
//   5. runs the user's own cmake to configure and then to compile;
//   6. copies the finished binary into emulators/<id>/, which is what makes EmulatorManager::isInstalled true
//      and lights up the existing Play (native) verb — #248 item 5, with no second launch path.
//
// WHAT IT NEVER DOES: download or install a compiler; fall back to a bundled toolchain; copy, move or modify
// the user's ROM; write anything outside <data>/recomps/builds/<id> and the port's own emulators/<id> folder;
// or leave a failure as a spinner. Every terminal state carries a sentence (recompbuild::failureSentence).
#pragma once
#include <QAtomicInt>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

#include "EmulatorRegistry.h"
#include "RecompBuild.h"

class RecompBuildJob : public QObject
{
    Q_OBJECT
public:
    static RecompBuildJob& instance();

    // A snapshot of a running (or just-finished) build. Copied out under the mutex in one go, so the UI can
    // never read a phase from one moment and a percentage from another.
    struct Snapshot
    {
        QString            portId;
        QString            title;
        recompbuild::Phase phase = recompbuild::Phase::Idle;
        recompbuild::Progress progress;
        QStringList        logTail;
        QString            logFilePath;
        QString            message;      // the sentence for a terminal phase; empty while running
        // A cancel has been ASKED FOR and the build has not stopped yet. It exists because the stop is not
        // instantaneous in one phase: a child process is stopped within a poll (200 ms), but the source
        // download is one blocking bounded fetch with no cancellation point inside it, so a cancel during
        // that phase lands when the fetch returns. Saying "stopping" is what keeps that honest — the
        // alternative is a Stop that visibly does nothing for a few seconds.
        bool               stopping = false;
        bool running() const { return recompbuild::isRunning(phase); }
    };

    Snapshot snapshot() const;
    bool busy() const;
    // Is THIS entry the one building (or just built)? The Recomps row asks per row.
    bool isActive(const QString& portId) const;

    // Start a build of `port` against `romPath`, which is the user's own dump and is read where it lies.
    // Returns false and fills `why` when it cannot start — a build already running, or an entry this build
    // has no plan for.
    bool start(const ExternalEmulator& port, const QString& romPath, QString* why);

    // Ask the current build to stop. Safe from the GUI thread and safe to call twice: the flag is what the
    // runner watches, and the machine is already Cancelled by the time the child dies.
    void cancel();

signals:
    // Something in the snapshot changed. Deliberately carries nothing: a signal with a payload invites a UI
    // to render the payload instead of the snapshot, and then the two disagree.
    void progressed();
    // A build reached a terminal phase. `message` is the sentence — a failure's, or the success one.
    void ended(const QString& portId, int phase, const QString& message);

private:
    explicit RecompBuildJob(QObject* parent = nullptr) : QObject(parent) {}
    void runOnWorker(ExternalEmulator port, QString romPath);
    void publish(const recompbuild::Machine& m, const recompbuild::Progress& p, const QStringList& tail);

    mutable QMutex mutex_;
    Snapshot snap_;
    QAtomicInt cancelFlag_{ 0 };
    bool busy_ = false;
};
