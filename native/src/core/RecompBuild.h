// BUILDING A RECOMP ON THIS MACHINE (issue #248, increment c) — the model of the build, kept away from the
// process that runs it.
//
// WHAT THE OPERATION IS. A self-compiled catalogue entry ships no binary. The port exists only as source plus
// a recompiler, and the finished program is produced HERE: the recompiler is run against the dump the user
// already owns, and the C++ it emits is compiled by the user's own toolchain. RetComM's SCHEMA.md describes
// the recipe in as many words — obtain the source, harvest or download the SDK, run the SDK's `generate`
// against the verified ROM, then `cmake --build`, then stage the launch binary.
//
// ONE DELIBERATE DIVERGENCE FROM THAT RECIPE, AND IT IS THE POINT OF THE INCREMENT. RetComM's step between
// `generate` and `cmake` is "fetch a toolchain pack". This app does not: it detects the toolchain the user
// already has (core/Toolchain.h) and, when there is none, says exactly what to install. There is no bundled
// compiler here and no code path that could acquire one.
//
// WHY THE MODEL IS SEPARATE FROM THE RUNNER. Everything in this header is pure: the state machine, the step
// plan, the log tail, the progress line, the failure sentences and the path safety. Not one function starts a
// process, opens a socket or reads the clock. That is what lets probe_recompbuild drive the whole of it —
// including the four things that decide whether this feature is honest and that a live drive can essentially
// never stage on demand:
//   * a CANCEL landing in the middle of a compile, and the child's exit arriving AFTERWARDS;
//   * a child that dies rather than exits;
//   * a compiler that reports success and leaves no artefact (which on Windows is what Defender quarantining
//     a freshly built unsigned exe looks like from here);
//   * a build log that grows without limit.
// The runner (RecompBuild.cpp + RecompBuildJob) owns QProcess, the worker thread and the files, and it makes
// no decisions of its own: every transition it performs is one of the transitions below.
//
// THE RULE THE MACHINE EXISTS FOR is that a TERMINAL STATE ABSORBS EVERYTHING. When somebody presses Cancel,
// the child is asked to stop and then killed — and it then exits, non-zero, milliseconds later. A machine
// that took that exit at face value would turn "you cancelled it" into "the compiler failed with code 1",
// which is a lie about the user's own action. Cancelled, Succeeded, Failed and Blocked accept no further
// signal, and `apply()` returns false to say so rather than silently doing nothing.
//
// NO ROM MOVES. The dump is passed to the recompiler as a PATH, on its command line, and that is the only
// thing that ever happens to it. There is no step in any plan below whose program copies a file, and the
// workspace root is derived from the catalogue id alone, so no plan can be steered at the ROM's own folder.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include "Toolchain.h"

namespace recompbuild
{
    // ---- the state machine ------------------------------------------------------------------------------
    // The phases are the real steps of the recipe, in order. `Blocked` is not a failure of a build — it is a
    // build that never started because this machine has nothing to build with, and it reads differently on
    // screen for that reason.
    enum class Phase
    {
        Idle,
        FetchingSource,   // the project's own source archive is coming down
        Unpacking,        // ...and being written into the workspace
        Generating,       // the recompiler runs against the user's dump
        Configuring,      // cmake -S … -B …
        Compiling,        // cmake --build …
        Staging,          // the finished binary is put where Play (native) looks for it
        Succeeded,
        Failed,
        Cancelled,
        Blocked,          // no compiler / no CMake; nothing was attempted
    };

    // Progress signals only. Every way a build can END that is not "the next step finished" goes through
    // `fail()` or `cancel()`, so there is exactly one place each terminal state is entered.
    enum class Signal
    {
        Start,
        SourceReady,
        UnpackDone,
        GenerateDone,
        ConfigureDone,
        CompileDone,
        StageDone,
        Cancel,
    };

    enum class Fault
    {
        None,
        ToolchainMissing,   // -> Blocked, not Failed
        SourceUnavailable,  // the download did not arrive
        SourceTooBig,       // ...or arrived over the ceiling and was abandoned
        UnpackFailed,       // not a readable archive, or a member that tried to escape the workspace
        ChildExit,          // a step's program exited non-zero
        ChildCrash,         // ...or did not exit at all
        ArtefactMissing,    // every step said it worked and the program is not there
        Internal,           // a plan that could not be made; always accompanied by its own sentence
    };

    // Phase predicates as free functions, because the UI asks them of a phase it read out of a snapshot and
    // does not have a Machine to ask.
    inline bool isTerminal(Phase p)
    {
        return p == Phase::Succeeded || p == Phase::Failed || p == Phase::Cancelled || p == Phase::Blocked;
    }
    inline bool isRunning(Phase p) { return p != Phase::Idle && !isTerminal(p); }

    struct Machine
    {
        Phase phase = Phase::Idle;
        Fault fault = Fault::None;
        Phase failedIn = Phase::Idle;   // WHICH step it was; the sentence needs it
        int   exitCode = 0;             // only meaningful for Fault::ChildExit

        bool terminal() const { return isTerminal(phase); }
        bool running() const { return isRunning(phase); }

        // Advance. Returns false — and changes NOTHING — for a signal that does not belong to the current
        // phase, which covers both the out-of-order case and the late-child-exit-after-cancel case.
        bool apply(Signal s)
        {
            if (terminal()) return false;
            if (s == Signal::Cancel)
            {
                if (phase == Phase::Idle) return false;   // nothing to cancel
                phase = Phase::Cancelled;
                return true;
            }
            const Phase next = nextFor(phase, s);
            if (next == phase) return false;
            phase = next;
            return true;
        }

        // End it. `ToolchainMissing` lands in Blocked; everything else in Failed, remembering the step it was
        // in so the sentence can name it.
        bool fail(Fault why, int code = 0)
        {
            if (terminal() || why == Fault::None) return false;
            failedIn = phase;
            fault = why;
            exitCode = code;
            phase = (why == Fault::ToolchainMissing) ? Phase::Blocked : Phase::Failed;
            return true;
        }

    private:
        static Phase nextFor(Phase p, Signal s)
        {
            switch (s)
            {
                case Signal::Start:         return p == Phase::Idle           ? Phase::FetchingSource : p;
                case Signal::SourceReady:   return p == Phase::FetchingSource ? Phase::Unpacking      : p;
                case Signal::UnpackDone:    return p == Phase::Unpacking      ? Phase::Generating     : p;
                case Signal::GenerateDone:  return p == Phase::Generating     ? Phase::Configuring    : p;
                case Signal::ConfigureDone: return p == Phase::Configuring    ? Phase::Compiling      : p;
                case Signal::CompileDone:   return p == Phase::Compiling      ? Phase::Staging        : p;
                case Signal::StageDone:     return p == Phase::Staging        ? Phase::Succeeded      : p;
                case Signal::Cancel:        return p;   // handled above
            }
            return p;
        }
    };

    // WHAT A FINISHED CHILD MEANS. Extracted as its own function, and it is the most load-bearing five lines
    // in the feature: the runner, the job and the probe all read a child's outcome THROUGH here, so the rule
    // that a terminal machine absorbs a late result is one rule rather than three copies of one.
    //
    // The order matters. `terminal()` is tested FIRST — before `crashed`, before the exit code — because the
    // sequence that actually happens is: the user presses Cancel, the machine goes to Cancelled, the child is
    // asked to stop, and the child then exits non-zero a few milliseconds later. Testing the exit code first
    // would rewrite the user's own cancellation as "the compiler failed with code 1".
    inline bool applyChildResult(Machine& m, bool crashed, int exitCode, Signal onSuccess)
    {
        if (m.terminal()) return false;
        if (crashed) return m.fail(Fault::ChildCrash);
        if (exitCode != 0) return m.fail(Fault::ChildExit, exitCode);
        return m.apply(onSuccess);
    }

    // ...and what the END of a build means. A compiler that exits 0 has said nothing about whether the file
    // it was supposed to produce is there — which on Windows is exactly the shape of a freshly built unsigned
    // exe being quarantined between the linker writing it and this app looking for it.
    inline bool applyArtefactCheck(Machine& m, bool artefactExists)
    {
        if (m.terminal()) return false;
        if (!artefactExists) return m.fail(Fault::ArtefactMissing);
        return m.apply(Signal::StageDone);
    }

    // What the phase is called on screen. Present tense, because it is shown while it is happening.
    inline QString phaseLabel(Phase p)
    {
        switch (p)
        {
            case Phase::Idle:           return QStringLiteral("not started");
            case Phase::FetchingSource: return QStringLiteral("downloading the source");
            case Phase::Unpacking:      return QStringLiteral("unpacking the source");
            case Phase::Generating:     return QStringLiteral("recompiling your game");
            case Phase::Configuring:    return QStringLiteral("preparing the build");
            case Phase::Compiling:      return QStringLiteral("compiling");
            case Phase::Staging:        return QStringLiteral("finishing up");
            case Phase::Succeeded:      return QStringLiteral("ready");
            case Phase::Failed:         return QStringLiteral("failed");
            case Phase::Cancelled:      return QStringLiteral("cancelled");
            case Phase::Blocked:        return QStringLiteral("needs build tools");
        }
        return QString();
    }

    // The program a phase runs, named as the user saw it go past in the log. Used by the failure sentence so
    // "exited with code 1" says WHICH thing exited.
    inline QString phaseProgram(Phase p, const QString& engine)
    {
        switch (p)
        {
            case Phase::Generating:  return engine.isEmpty() ? QStringLiteral("the recompiler") : engine;
            case Phase::Configuring: return QStringLiteral("cmake");
            case Phase::Compiling:   return QStringLiteral("cmake --build");
            default:                 return QStringLiteral("the build");
        }
    }

    // ---- the sentences ----------------------------------------------------------------------------------
    // Everything a failed build says. Here, beside the machine, for the reason Toolchain.h gives: the exact
    // wording is under test, and a probe and the screen must not be able to disagree about what a person was
    // told. Every one of them is a sentence plus a path — never a code, never a spinner that stopped.
    struct FailureContext
    {
        QString title;         // the GAME's name
        QString engine;        // "psxrecomp"
        QString sourceRepo;    // "owner/repo", for the sentence that has to name where a download failed
        QString logPath;       // the full log on disk
        QString artefactPath;  // where the finished program was expected
        // The refusal a plan produced, when the fault is Internal. On the CONTEXT rather than on the machine
        // because it is a sentence about this entry, not a state — the machine stays a pure state machine
        // with no strings in it. A live drive is what put this here: the generic "this version can't build
        // it" was true and useless next to the log line beside it, which said the recompiler was not in the
        // source that had just been downloaded.
        QString internalReason;
        toolchain::Os os = toolchain::Os::Windows;
    };

    inline QString failureSentence(const Machine& m, const FailureContext& c)
    {
        const QString log = c.logPath.isEmpty() ? QStringLiteral("the build folder")
                                                : c.logPath;
        switch (m.fault)
        {
            case Fault::None:
                return QString();

            case Fault::ToolchainMissing:
                // Deliberately does NOT list what to install: that list is the toolchain decision's, it is
                // OS-specific, and two places writing it is how they come to differ.
                return QStringLiteral("%1 can't be built on this computer yet — the tools it is built with "
                                      "aren't installed. Nothing was downloaded and nothing was changed.")
                    .arg(c.title);

            case Fault::SourceUnavailable:
                return QStringLiteral("Couldn't download %1's source from %2. Nothing on this computer was "
                                      "changed — you can try again later.")
                    .arg(c.title, c.sourceRepo.isEmpty() ? QStringLiteral("its project page") : c.sourceRepo);

            case Fault::SourceTooBig:
                return QStringLiteral("%1's source download was larger than this app will fetch in one piece, "
                                      "so it was stopped. Nothing was installed.")
                    .arg(c.title);

            case Fault::UnpackFailed:
                return QStringLiteral("%1's source archive couldn't be unpacked — it may have been published "
                                      "broken, or the download was cut short. Nothing was installed.")
                    .arg(c.title);

            case Fault::ChildExit:
                return QStringLiteral("The build stopped: %1 exited with code %2. The full log is at %3.")
                    .arg(phaseProgram(m.failedIn, c.engine))
                    .arg(m.exitCode)
                    .arg(log);

            case Fault::ChildCrash:
                return QStringLiteral("The build stopped: %1 ended unexpectedly — it wasn't asked to stop and "
                                      "it didn't finish. The full log is at %2.")
                    .arg(phaseProgram(m.failedIn, c.engine), log);

            case Fault::ArtefactMissing:
                // #248's Defender case, and the sentence is the whole of what this app is allowed to do about
                // it: say what happened, say where the file was, and point at the place a person can look.
                // It does not retry, and it never suggests turning anything off.
                return (c.os == toolchain::Os::Windows)
                           ? QStringLiteral("The build finished, but %1 isn't at %2. A program compiled a "
                                            "moment ago is unsigned and brand new, and Windows Security "
                                            "quarantines those on sight — look under Windows Security → "
                                            "Protection history to see whether that is what happened. The "
                                            "full log is at %3.")
                                 .arg(c.title, c.artefactPath.isEmpty() ? QStringLiteral("the build folder")
                                                                        : c.artefactPath,
                                      log)
                           : QStringLiteral("The build finished, but %1 isn't at %2. Your security software "
                                            "may have removed it. The full log is at %3.")
                                 .arg(c.title, c.artefactPath.isEmpty() ? QStringLiteral("the build folder")
                                                                        : c.artefactPath,
                                      log);

            case Fault::Internal:
                return c.internalReason.trimmed().isEmpty()
                           ? QStringLiteral("%1 can't be built by this version of EverythingBox. The full "
                                            "log is at %2.")
                                 .arg(c.title, log)
                           : QStringLiteral("%1 The full log is at %2.")
                                 .arg(c.internalReason.trimmed(), log);
        }
        return QString();
    }

    // Cancel is not a failure and does not read like one. It also answers the question a person actually has
    // after pressing it, which is what happened to their game file.
    inline QString cancelledSentence(const QString& title)
    {
        return QStringLiteral("Stopped building %1. Nothing was installed, and your copy of the game wasn't "
                              "touched.")
            .arg(title);
    }

    inline QString succeededSentence(const QString& title, const QString& artefactPath)
    {
        return QStringLiteral("%1 is built and ready. It is at %2.").arg(title, artefactPath);
    }

    // ---- progress ---------------------------------------------------------------------------------------
    // The percent a build tool printed, or -1. CMake's own build drivers (make, ninja) prefix every line with
    // `[ 42%]`; MSBuild prints none at all, and that is exactly why -1 has to be a first-class answer here
    // rather than a 0 that looks like a stuck build.
    inline int parsePercent(const QString& line)
    {
        const int open = line.indexOf(QLatin1Char('['));
        if (open < 0) return -1;
        const int close = line.indexOf(QLatin1Char(']'), open + 1);
        if (close < 0 || close - open > 8) return -1;
        QString inner = line.mid(open + 1, close - open - 1).trimmed();
        if (!inner.endsWith(QLatin1Char('%'))) return -1;
        inner.chop(1);
        bool ok = false;
        const int pct = inner.trimmed().toInt(&ok);
        if (!ok || pct < 0 || pct > 100) return -1;
        return pct;
    }

    struct Progress
    {
        Phase   phase = Phase::Idle;
        int     percent = -1;      // -1 = the tools did not say, which is the normal case on MSVC
        qint64  elapsedMs = 0;
        QString lastLine;          // the most recent line of the build's own output
    };

    // THE ONE LINE THE USER SEES WHILE IT RUNS, and the rule it enforces is #248's: never a spinner that
    // never resolves. When there is no percent there is still a phase, an elapsed time and the tools' own
    // last line — three facts that all change while a build is alive and all stop changing when it is not.
    inline QString progressLine(const Progress& p)
    {
        QString head = phaseLabel(p.phase);
        if (p.percent >= 0) head += QStringLiteral(" %1%").arg(p.percent);
        const qint64 secs = p.elapsedMs / 1000;
        head += (secs >= 60) ? QStringLiteral(" · %1m %2s").arg(secs / 60).arg(secs % 60)
                             : QStringLiteral(" · %1s").arg(secs);
        if (!p.lastLine.trimmed().isEmpty()) head += QStringLiteral(" · ") + p.lastLine.trimmed();
        return head;
    }

    // ---- the log tail -----------------------------------------------------------------------------------
    // A compiler emits megabytes. What a person needs on screen is the last twenty lines, and what this app
    // must not do is hold the whole of it in memory to get them — a build that emits a warning per template
    // instantiation would otherwise be a slow leak with a UI in front of it.
    //
    // BOUNDED IN BOTH DIRECTIONS: at most `maxLines` lines, each at most `maxChars` characters. The second
    // bound is not decoration — a linker printing one 40 MB line (a single command line echoed back) is a
    // real shape, and a line cap is the only thing that bounds it.
    class LogTail
    {
    public:
        explicit LogTail(int maxLines = 24, int maxChars = 400)
            : maxLines_(maxLines > 0 ? maxLines : 1), maxChars_(maxChars > 0 ? maxChars : 80) {}

        // Feed it whatever arrived. Splits on both line endings, holds an unterminated tail back until the
        // rest of it turns up, and truncates that held tail too so an endless line with no newline in it
        // cannot grow the buffer either.
        // Returns the lines this chunk COMPLETED, already clipped — so a caller that forwards each new line
        // onward does not have to work out which ones are new by counting.
        QStringList append(const QString& chunk)
        {
            QStringList fresh;
            bytes_ += chunk.size();
            partial_ += chunk;
            int nl;
            while ((nl = partial_.indexOf(QLatin1Char('\n'))) >= 0)
            {
                QString line = partial_.left(nl);
                partial_.remove(0, nl + 1);
                if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
                fresh << clip(line);
                push(line);
            }
            if (partial_.size() > maxChars_) partial_ = clip(partial_);
            return fresh;
        }

        // The unterminated tail, promoted to a line. Called when the child ends: a compiler that dies mid-line
        // usually died saying why.
        QStringList flush()
        {
            if (partial_.trimmed().isEmpty()) { partial_.clear(); return {}; }
            const QStringList fresh{ clip(partial_) };
            push(partial_);
            partial_.clear();
            return fresh;
        }

        QStringList lines() const { return lines_; }
        QString last() const { return lines_.isEmpty() ? partial_ : lines_.constLast(); }
        QString text() const { return lines_.join(QStringLiteral("\n")); }
        qint64 bytesSeen() const { return bytes_; }
        int droppedLines() const { return dropped_; }

    private:
        QString clip(const QString& s) const
        {
            return s.size() <= maxChars_ ? s : s.left(maxChars_ - 1) + QChar(0x2026);
        }
        void push(const QString& line)
        {
            lines_.push_back(clip(line));
            while (lines_.size() > maxLines_) { lines_.removeFirst(); ++dropped_; }
        }

        int         maxLines_;
        int         maxChars_;
        QStringList lines_;
        QString     partial_;
        qint64      bytes_ = 0;
        int         dropped_ = 0;
    };

    // The on-disk log's ceiling. The tail above bounds what is in memory; this bounds what is on the user's
    // disk, because "the full log is at …" must not one day mean 6 GB. Past it the runner writes one final
    // line saying so and stops.
    inline constexpr qint64 kMaxLogBytes = 32 * 1024 * 1024;

    // The ceiling on the source download. Generous for a source tree (the largest recomp sources are tens of
    // megabytes) and low enough that a mistagged release cannot turn a build into a disk-filling download.
    inline constexpr qint64 kMaxSourceBytes = 192 * 1024 * 1024;
    inline constexpr int    kSourceTimeoutMs = 120000;

    // ---- path safety ------------------------------------------------------------------------------------
    // The workspace is named from the catalogue id, and a catalogue is DATA — an id is whatever a feed said
    // it was. Anything that is not a plain slug becomes one, so `../../..` names a folder called
    // "------------" inside the builds root instead of naming the builds root's grandparent.
    inline QString sanitiseId(const QString& id)
    {
        QString out;
        for (const QChar c : id.trimmed().toLower())
            out += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')) ? c
                                                                                           : QLatin1Char('-');
        while (out.startsWith(QLatin1Char('-'))) out.remove(0, 1);
        while (out.endsWith(QLatin1Char('-'))) out.chop(1);
        return out;
    }

    // Where an archive member may be written, or "" for one that may not. Zip slip, stated as a function: a
    // member is refused when it is absolute, when it names a drive, or when ANY of its segments is "..".
    // Refusing the whole archive on one bad member is the runner's job; this only ever answers about one.
    inline QString safeMemberPath(const QString& base, const QString& member)
    {
        QString m = member;
        m.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (m.isEmpty() || m.startsWith(QLatin1Char('/'))) return QString();
        if (m.size() >= 2 && m.at(1) == QLatin1Char(':')) return QString();
        const QStringList parts = m.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) return QString();
        for (const QString& p : parts)
            if (p == QStringLiteral("..")) return QString();
        return base + QLatin1Char('/') + parts.join(QLatin1Char('/'));
    }

    // A source zipball unpacks into one folder named after the repo and the commit. Every path in the recipe
    // (`cmake.build_dir`, `generate.out_dir`) is relative to the tree INSIDE that folder, so the first
    // component comes off. Returns "" when there is nothing left, which is how the top-level folder entry
    // itself is skipped.
    inline QString stripFirstComponent(const QString& memberPath)
    {
        const int slash = memberPath.indexOf(QLatin1Char('/'));
        return slash < 0 ? QString() : memberPath.mid(slash + 1);
    }

    // ---- the step plan ----------------------------------------------------------------------------------
    struct Step
    {
        Phase   phase = Phase::Generating;
        QString label;      // shown while it runs
        QString program;    // absolute path; never resolved through PATH at run time
        QStringList args;
        QString workDir;
    };

    struct PlanInput
    {
        QString engine;          // build.generate.engine
        QString engineExe;       // the recompiler, wherever it was harvested to
        QString sourceDir;       // the unpacked source tree
        QString buildDir;        // absolute; recipe's cmake.build_dir resolved under sourceDir
        QString cmakeExe;        // the CMake the detector found — never the bare word "cmake"
        QString cmakeConfig;     // "Release" unless the recipe says otherwise
        QString cmakeTarget;     // "" = whatever the project builds by default
        QString romPath;         // THE USER'S DUMP, read where it lies
        QString generateConfig;  // build.generate.config; PSX defaults to game.toml
        toolchain::Os os = toolchain::Os::Windows;
    };

    struct Plan
    {
        QVector<Step> steps;
        // Non-empty means no plan could be made, and it IS the sentence — a refusal a person can act on
        // rather than an empty step list somebody downstream reads as "nothing to do".
        QString refusal;
        bool ok() const { return refusal.isEmpty() && !steps.isEmpty(); }
    };

    // PSX FIRST, and only PSX. #248 (c) says so, and the reason it is a refusal rather than a guess is in
    // SCHEMA.md: the SNES and GBA generators take different arguments (`cfg_dir`/`out_dir`/`funcs_h` and
    // `--rom`/`--bios`) whose exact spelling that document does not give. Inventing them would produce a
    // build that fails at its first step with a compiler error nobody could act on; saying which engine is
    // wired up is a fact a person can do something with.
    inline Plan planSteps(const PlanInput& in)
    {
        Plan p;
        const QString engine = in.engine.trimmed().toLower();
        if (engine.isEmpty())
        {
            p.refusal = QStringLiteral("This entry doesn't say which recompiler builds it.");
            return p;
        }
        if (engine != QStringLiteral("psxrecomp"))
        {
            p.refusal = QStringLiteral("Building %1 titles on this computer isn't wired up yet — only "
                                       "psxrecomp is. The project's own page has instructions.")
                            .arg(engine);
            return p;
        }
        if (in.romPath.trimmed().isEmpty())
        {
            p.refusal = QStringLiteral("No matching copy of the game was found on this computer.");
            return p;
        }
        if (in.engineExe.trimmed().isEmpty())
        {
            p.refusal = QStringLiteral("The %1 recompiler wasn't found in the downloaded source.").arg(engine);
            return p;
        }
        if (in.cmakeExe.trimmed().isEmpty() || in.sourceDir.trimmed().isEmpty()
            || in.buildDir.trimmed().isEmpty())
        {
            p.refusal = QStringLiteral("The build couldn't be set up on this computer.");
            return p;
        }

        const QString config = in.cmakeConfig.trimmed().isEmpty() ? QStringLiteral("Release")
                                                                  : in.cmakeConfig.trimmed();
        // SCHEMA.md: "PSX uses `config` (default `game.toml`) and passes the library disc as `--disc`."
        const QString genCfg = in.generateConfig.trimmed().isEmpty() ? QStringLiteral("game.toml")
                                                                     : in.generateConfig.trimmed();

        Step gen;
        gen.phase = Phase::Generating;
        gen.label = QStringLiteral("Recompiling your copy of the game with %1").arg(engine);
        gen.program = in.engineExe;
        // THE ROM, BY PATH, AND ONLY BY PATH. This argument is the entire relationship between this feature
        // and the user's dump: it is read where it lies, it is not copied, and no other step in this plan
        // mentions it at all.
        gen.args = QStringList{ genCfg, QStringLiteral("--disc"), in.romPath };
        gen.workDir = in.sourceDir;
        p.steps.push_back(gen);

        Step cfg;
        cfg.phase = Phase::Configuring;
        cfg.label = QStringLiteral("Preparing the build");
        cfg.program = in.cmakeExe;
        cfg.args = QStringList{ QStringLiteral("-S"), in.sourceDir, QStringLiteral("-B"), in.buildDir,
                                QStringLiteral("-DCMAKE_BUILD_TYPE=") + config };
        cfg.workDir = in.sourceDir;
        p.steps.push_back(cfg);

        Step build;
        build.phase = Phase::Compiling;
        build.label = QStringLiteral("Compiling");
        build.program = in.cmakeExe;
        build.args = QStringList{ QStringLiteral("--build"), in.buildDir, QStringLiteral("--config"), config };
        if (!in.cmakeTarget.trimmed().isEmpty())
            build.args << QStringLiteral("--target") << in.cmakeTarget.trimmed();
        build.args << QStringLiteral("--parallel");
        build.workDir = in.sourceDir;
        p.steps.push_back(build);

        return p;
    }

    // ---- where things live (RecompBuild.cpp; they need AppPaths) ----------------------------------------
    // <data>/recomps/builds/<id>  — the workspace: the downloaded source, the generated C++, the object files
    // and build.log. Removable in one go, and it is the only place a build writes.
    QString buildsRoot();
    QString workspaceDir(const QString& portId);
    QString sourceDir(const QString& portId);
    QString cmakeBuildDir(const QString& portId, const QString& relativeBuildDir);
    QString logPath(const QString& portId);

    // <data>/recomps/engines/<engine> — where a recompiler that had to be downloaded separately is kept.
    //
    // WHY IT IS NOT REDISTRIBUTION, stated here because this is the line the licence sits on. psxrecomp is
    // PolyForm Noncommercial: it may be RUN by the person who obtained it, and it may not be redistributed by
    // us. Nothing of it is in this repository, nothing of it is in the app's shipped artefact, and no build of
    // EverythingBox has ever contained a byte of it. What lands here is a copy the USER's machine fetched from
    // the engine project's own release, at the moment that user asked for a build, and it stays a copy of
    // somebody else's program: `writeEngineNotice` puts its name, its licence and the URL it came from in the
    // folder beside it, so the bytes carry their provenance even to somebody who finds them later.
    QString enginesRoot();
    QString engineDir(const QString& engineId);
    bool    writeEngineNotice(const QString& dir, const QString& engineId, const QString& license,
                              const QString& homepage);

    // The text of that notice. Pure so the probe can assert it names the licence and the origin.
    inline QString engineNoticeText(const QString& engineId, const QString& license, const QString& homepage)
    {
        return QStringLiteral(
                   "This folder holds %1, which is NOT part of EverythingBox.\n"
                   "\n"
                   "It was downloaded to this computer from the project's own release when you asked for a\n"
                   "recomp to be built. EverythingBox does not include it, does not ship it, and does not\n"
                   "redistribute it.\n"
                   "\n"
                   "Licence: %2\n"
                   "Source:  %3\n")
            .arg(engineId,
                 license.isEmpty() ? QStringLiteral("see the project's own LICENSE file") : license,
                 homepage.isEmpty() ? QStringLiteral("the project's own release page") : homepage);
    }
}
