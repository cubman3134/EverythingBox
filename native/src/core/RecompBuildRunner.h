// RUNNING ONE STEP OF A BUILD (issue #248, increment c) — the only part of the feature that starts a child
// process.
//
// NOT A QObject, AND BLOCKING ON PURPOSE. A build is a sequence of long child processes with no UI of their
// own; expressing that as a chain of asynchronous slots buys nothing and costs the ability to read it. So
// this is a plain object with a blocking `runStep`, and the CALLER's job is to be on a worker thread — which
// RecompBuildJob is, and which probe_recompbuild is by being a console program with nothing else to do.
//
// THAT SHAPE IS ALSO WHAT MAKES CANCEL TESTABLE. Cancellation is a QAtomicInt the caller owns and any thread
// may set. In the app it is set from the GUI thread when somebody presses Cancel; in the probe it is set from
// INSIDE the line callback, on the fifth line of the stub engine's output — which is a cancel that lands in
// the middle of a running child, deterministically, on every run. No sleeps, no races, no flake.
//
// WHAT IT DOES NOT DECIDE. It reports what happened to the child (started / crashed / exit code / cancelled)
// and nothing else. Whether that is a failure, and what it makes the build's state, is
// recompbuild::applyChildResult — one rule, read by the job and by the probe through the same function.
#pragma once
#include <QAtomicInt>
#include <QString>
#include <functional>

#include "RecompBuild.h"

namespace recompbuild
{
    struct StepOutcome
    {
        bool started   = false;  // false = the program could not be launched at all (missing, not executable)
        bool crashed   = false;  // it ended without exiting: a signal, an access violation, or our own kill
        bool cancelled = false;  // ...and the reason it ended that way was us
        int  exitCode  = -1;
        qint64 elapsedMs = 0;
    };

    class Runner
    {
    public:
        // Every line the child printed, in order, already split and already clipped by the tail. Called on the
        // runner's own thread.
        std::function<void(const QString&)> onLine;
        // Called as often as there is something new to say — a line, or a second of silence. The caller
        // decides how often to repaint from it.
        std::function<void(const Progress&)> onProgress;

        // Set from any thread to stop the current child. Owned by the caller: a build that is cancelled while
        // between two steps must stay cancelled, and that is the caller's state, not this object's.
        QAtomicInt* cancelFlag = nullptr;

        // Appended to, never truncated between steps, and capped at kMaxLogBytes — past which one line is
        // written saying so and the file stops growing. "The full log is at …" has to stay a promise about a
        // file somebody can open.
        QString logFilePath;

        // The tail the caller shows on screen. Held here so it spans every step of a build rather than
        // restarting at each one.
        LogTail tail;

        // How long to give a child after asking it politely to stop, before killing it. A compiler asked to
        // terminate is usually gone in well under a second; a build driver with children of its own can take
        // longer, and a cancel that leaves cl.exe running would leave the workspace locked.
        //
        // A field rather than a constant because on Windows QProcess::terminate() posts WM_CLOSE, which a
        // console program has no window to receive — so every cancel of a command-line build tool waits out
        // the whole grace period before the kill. The app wants that patience; the probe, which cancels
        // several children per run, does not.
        int graceMs = 4000;

        StepOutcome runStep(const Step& step);

    private:
        void writeLog(const QString& text);
        qint64 logBytes_ = 0;
        bool   logCapped_ = false;
    };
}
