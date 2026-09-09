// Headless check of THE SELF-COMPILE TIER (issue #248, increment c) — detecting what this machine can build
// with, and building a recomp on it.
//
// WHY A NEW PROBE RATHER THAN MORE OF probe_ports. probe_ports is the CATALOGUE's probe: it links Qt6::Core
// and miniz, it makes no request and it starts no process, and every one of its 24 sections is about parsing
// or matching a document. This increment's subject is a child process — how long it talks for, whether it
// exits or dies, whether it can be stopped halfway, and what is left on disk afterwards. Bolting that onto a
// pure-parse probe would give that target a process dependency and a stub-engine dependency it has no other
// use for, and would make one failure message stand for two unrelated features. So: a second target, with
// the same rails and its own token.
//
// WHAT IT DRIVES, and each of these is a thing no live drive on this machine could stage on demand:
//   * the toolchain decision table over ALL SIXTEEN combinations of (MSVC, clang, gcc, CMake) on all three
//     operating systems — including "nothing at all", which cannot be staged on a computer that has a
//     compiler, and "a compiler but no CMake", which cannot be staged on one that has CMake;
//   * the build state machine's whole path, plus the one rule the feature's honesty rests on: a terminal
//     state absorbs everything, so a child's non-zero exit arriving AFTER a cancel does not rewrite the
//     user's own cancellation as a compiler failure;
//   * the runner against a real child process — the in-tree stub engine (tools/stub_recomp_engine.cpp) —
//     exiting cleanly, exiting non-zero, DYING rather than exiting, being cancelled mid-run, and finishing
//     successfully while leaving no artefact behind (which is what Windows Defender quarantining a freshly
//     built unsigned exe looks like from in here);
//   * a log that grows without limit;
//   * that no ROM byte and no engine binary can reach this repository or the app's own tree.
//
// Expected values are hand-authored, never read back out of the code under test. Prints RECOMPBUILD-OK on
// success; on any failure prints RECOMPBUILD-FAIL <cond> and exits non-zero.
#include "RecompBuild.h"
#include "RecompBuildRunner.h"
#include "RecompUpdates.h"
#include "Toolchain.h"

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

#include "AppPaths.h"

static int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) { std::printf("RECOMPBUILD-FAIL %s\n", #cond); ++failures; } \
    } while (0)

using namespace recompbuild;

// ---- helpers ---------------------------------------------------------------------------------------------
static toolchain::Found found(bool msvc, bool clang, bool gcc, bool cmake)
{
    toolchain::Found f;
    if (msvc)  { f.msvcPath  = QStringLiteral("C:/VS");        f.msvcVersion  = QStringLiteral("17.9.1"); }
    if (clang) { f.clangPath = QStringLiteral("/usr/bin/clang"); f.clangVersion = QStringLiteral("18.1.3"); }
    if (gcc)   { f.gccPath   = QStringLiteral("/usr/bin/g++");   f.gccVersion   = QStringLiteral("13.2.0"); }
    if (cmake) { f.cmakePath = QStringLiteral("/usr/bin/cmake"); f.cmakeVersion = QStringLiteral("3.29.2"); }
    return f;
}

static QString stubPath() { return QStringLiteral(EB_STUB_ENGINE); }

static Step stubStep(const QStringList& args, const QString& workDir, Phase phase = Phase::Compiling)
{
    Step s;
    s.phase = phase;
    s.label = QStringLiteral("stub");
    s.program = stubPath();
    s.args = args;
    s.workDir = workDir;
    return s;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString scratch = AppPaths::dataDir() + QStringLiteral("/probe_recompbuild");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    // ---- 1. the toolchain decision table, exhaustively --------------------------------------------------
    // Sixteen combinations, three operating systems. The verdict is asserted from the two booleans that
    // actually decide it, hand-written here rather than re-derived from the code under test.
    for (int bits = 0; bits < 16; ++bits)
    {
        const bool msvc = bits & 1, clang = bits & 2, gcc = bits & 4, cmake = bits & 8;
        const toolchain::Found f = found(msvc, clang, gcc, cmake);
        const bool haveC = msvc || clang || gcc;
        for (const toolchain::Os os : { toolchain::Os::Windows, toolchain::Os::Linux, toolchain::Os::Macos })
        {
            const toolchain::Decision d = toolchain::decide(f, os);
            const toolchain::Verdict want = haveC && cmake  ? toolchain::Verdict::Ready
                                            : haveC         ? toolchain::Verdict::NoCMake
                                            : cmake         ? toolchain::Verdict::NoCompiler
                                                            : toolchain::Verdict::NoToolchain;
            CHECK(d.verdict == want);
            CHECK(d.canBuild() == (want == toolchain::Verdict::Ready));
            // Every verdict carries BOTH sentences. A state with a verdict and no explanation is the
            // disabled-button-with-no-tooltip failure this table exists to make unreachable.
            CHECK(!d.headline.isEmpty());
            CHECK(!d.detail.isEmpty());
            CHECK(d.missing.isEmpty() == (want == toolchain::Verdict::Ready));
            for (const toolchain::Requirement& r : d.missing)
            {
                CHECK(!r.what.isEmpty());
                CHECK(r.url.startsWith(QStringLiteral("https://")));
            }
            CHECK(!toolchain::shortStatus(d).isEmpty());
        }
    }

    // The three all-present / none-present corners, named, because they are the two the brief calls out and
    // the third is the one a Windows machine with the .NET workload only actually lands in.
    {
        const toolchain::Decision all = toolchain::decide(found(true, true, true, true), toolchain::Os::Windows);
        CHECK(all.verdict == toolchain::Verdict::Ready);
        CHECK(all.compiler == QStringLiteral("MSVC 17.9.1"));       // Windows prefers MSVC over a bare clang
        CHECK(all.detail.contains(QStringLiteral("3.29.2")));

        const toolchain::Decision none = toolchain::decide(found(false, false, false, false),
                                                           toolchain::Os::Windows);
        CHECK(none.verdict == toolchain::Verdict::NoToolchain);
        CHECK(none.compiler.isEmpty());
        CHECK(none.missing.size() == 2);
        // It says what to install, by the name the vendor uses, and it says the app will not do it.
        CHECK(none.detail.contains(QStringLiteral("Visual Studio Build Tools")));
        CHECK(none.detail.contains(QStringLiteral("CMake")));
        CHECK(none.detail.contains(QStringLiteral("will not install")));

        const toolchain::Decision noCMake = toolchain::decide(found(true, false, false, false),
                                                              toolchain::Os::Windows);
        CHECK(noCMake.verdict == toolchain::Verdict::NoCMake);
        CHECK(noCMake.missing.size() == 1);
        CHECK(noCMake.missing.at(0).url == QStringLiteral("https://cmake.org/download/"));
        CHECK(noCMake.detail.contains(QStringLiteral("MSVC 17.9.1")));
    }

    // The advice is per-OS and is not the Windows advice everywhere. A Linux machine told to install Visual
    // Studio has been told something useless.
    {
        const toolchain::Decision lin = toolchain::decide(found(false, false, false, true), toolchain::Os::Linux);
        CHECK(lin.detail.contains(QStringLiteral("build-essential")));
        CHECK(!lin.detail.contains(QStringLiteral("Visual Studio")));
        const toolchain::Decision mac = toolchain::decide(found(false, false, false, true), toolchain::Os::Macos);
        CHECK(mac.detail.contains(QStringLiteral("xcode-select")));
        CHECK(!mac.detail.contains(QStringLiteral("Visual Studio")));
    }

    // The per-OS preference order, which is about where the system headers come from and not about taste.
    {
        const toolchain::Found both = found(true, true, true, true);
        CHECK(toolchain::preferredCompiler(both, toolchain::Os::Windows).startsWith(QStringLiteral("MSVC")));
        CHECK(toolchain::preferredCompiler(both, toolchain::Os::Macos).startsWith(QStringLiteral("clang")));
        CHECK(toolchain::preferredCompiler(both, toolchain::Os::Linux).startsWith(QStringLiteral("gcc")));
        CHECK(toolchain::preferredCompiler(toolchain::Found{}, toolchain::Os::Linux).isEmpty());
        // A compiler that is present but did not answer --version is still a compiler.
        toolchain::Found mute;
        mute.gccPath = QStringLiteral("/usr/bin/g++");
        CHECK(mute.haveCompiler());
        CHECK(toolchain::preferredCompiler(mute, toolchain::Os::Linux) == QStringLiteral("gcc"));
        CHECK(toolchain::decide(mute, toolchain::Os::Linux).verdict == toolchain::Verdict::NoCMake);
    }

    // ---- 2. version banners -----------------------------------------------------------------------------
    CHECK(toolchain::firstVersionToken(QStringLiteral("cmake version 3.29.2")) == QStringLiteral("3.29.2"));
    CHECK(toolchain::firstVersionToken(QStringLiteral("clang version 18.1.3 (…)")) == QStringLiteral("18.1.3"));
    CHECK(toolchain::firstVersionToken(QStringLiteral("g++ (Ubuntu 13.2.0-4ubuntu3) 13.2.0"))
          == QStringLiteral("13.2.0"));
    CHECK(toolchain::firstVersionToken(QStringLiteral("17.9.34728.123")) == QStringLiteral("17.9.34728.123"));
    // Only the FIRST LINE is read: a banner's second line is a copyright notice with a year in it.
    CHECK(toolchain::firstVersionToken(QStringLiteral("cmake version 3.29.2\nCopyright 2024"))
          == QStringLiteral("3.29.2"));
    // A bare integer is not a version — otherwise "gcc (Ubuntu 13" yields "13" out of a package string.
    CHECK(toolchain::firstVersionToken(QStringLiteral("some tool 2024")).isEmpty());
    CHECK(toolchain::firstVersionToken(QString()).isEmpty());

    // ---- 3. the cached answer -----------------------------------------------------------------------------
    {
        const toolchain::Found f = found(true, false, true, true);
        const toolchain::Found back = toolchain::fromJson(toolchain::toJson(f));
        CHECK(back.msvcPath == f.msvcPath);
        CHECK(back.gccVersion == f.gccVersion);
        CHECK(back.cmakePath == f.cmakePath);
        CHECK(back.haveCompiler() && back.haveCMake());
        // A CORRUPT cache must never be able to assert that this machine has no compiler: it yields an empty
        // Found with no stamp, which detect() reads as "no usable cache" and re-probes. The failure this
        // prevents is a truncated file permanently telling somebody to install a compiler they already have.
        const toolchain::Found junk = toolchain::fromJson(QStringLiteral("{\"msvc_path\": "));
        CHECK(junk.probedAt.isEmpty());
        CHECK(!junk.haveCompiler());
    }

    // ---- 4. the state machine -----------------------------------------------------------------------------
    {
        Machine m;
        CHECK(m.phase == Phase::Idle && !m.running() && !m.terminal());
        // Nothing to cancel before anything started.
        CHECK(!m.apply(Signal::Cancel));
        CHECK(m.phase == Phase::Idle);
        // Out of order: a signal that does not belong to this phase changes nothing and says so.
        CHECK(!m.apply(Signal::CompileDone));
        CHECK(m.phase == Phase::Idle);

        CHECK(m.apply(Signal::Start) && m.phase == Phase::FetchingSource);
        CHECK(m.running());
        CHECK(m.apply(Signal::SourceReady) && m.phase == Phase::Unpacking);
        CHECK(m.apply(Signal::UnpackDone) && m.phase == Phase::Generating);
        CHECK(!m.apply(Signal::Start));                       // no restarting mid-flight
        CHECK(m.apply(Signal::GenerateDone) && m.phase == Phase::Configuring);
        CHECK(m.apply(Signal::ConfigureDone) && m.phase == Phase::Compiling);
        CHECK(m.apply(Signal::CompileDone) && m.phase == Phase::Staging);
        CHECK(m.apply(Signal::StageDone) && m.phase == Phase::Succeeded);
        CHECK(m.terminal() && !m.running());
        // TERMINAL ABSORBS. Nothing reopens a finished build.
        CHECK(!m.apply(Signal::Cancel));
        CHECK(!m.fail(Fault::ChildExit, 1));
        CHECK(m.phase == Phase::Succeeded);
    }
    {
        // Cancel mid-compile, and THEN the child exits non-zero — the actual sequence, because terminating a
        // compiler is how it comes to exit non-zero. The exit must not rewrite the cancellation.
        Machine m;
        m.apply(Signal::Start); m.apply(Signal::SourceReady); m.apply(Signal::UnpackDone);
        m.apply(Signal::GenerateDone); m.apply(Signal::ConfigureDone);
        CHECK(m.phase == Phase::Compiling);
        CHECK(m.apply(Signal::Cancel) && m.phase == Phase::Cancelled);
        CHECK(!applyChildResult(m, /*crashed*/ true, /*exit*/ 1, Signal::CompileDone));
        CHECK(m.phase == Phase::Cancelled);
        CHECK(m.fault == Fault::None);          // a cancel is not a fault and must not acquire one
        CHECK(!applyArtefactCheck(m, false));
        CHECK(m.phase == Phase::Cancelled);
    }
    {
        // fail() records WHICH step it was, because the sentence names the program.
        Machine m;
        m.apply(Signal::Start); m.apply(Signal::SourceReady); m.apply(Signal::UnpackDone);
        CHECK(m.phase == Phase::Generating);
        CHECK(m.fail(Fault::ChildExit, 2));
        CHECK(m.phase == Phase::Failed && m.failedIn == Phase::Generating && m.exitCode == 2);
        CHECK(!m.fail(Fault::ChildCrash));     // the first reason is the reason
        CHECK(m.fault == Fault::ChildExit);
    }
    {
        // A toolchain that is not there is BLOCKED, not FAILED: nothing was attempted, and the row and the
        // sentence both read differently for it.
        Machine m;
        CHECK(m.fail(Fault::ToolchainMissing));
        CHECK(m.phase == Phase::Blocked && m.terminal());
    }
    {
        // applyChildResult's three arms, from Generating.
        Machine ok, bad, dead;
        for (Machine* m : { &ok, &bad, &dead })
        { m->apply(Signal::Start); m->apply(Signal::SourceReady); m->apply(Signal::UnpackDone); }
        CHECK(applyChildResult(ok, false, 0, Signal::GenerateDone) && ok.phase == Phase::Configuring);
        CHECK(applyChildResult(bad, false, 7, Signal::GenerateDone) && bad.phase == Phase::Failed
              && bad.fault == Fault::ChildExit && bad.exitCode == 7);
        CHECK(applyChildResult(dead, true, 0, Signal::GenerateDone) && dead.phase == Phase::Failed
              && dead.fault == Fault::ChildCrash);
    }

    // ---- 5. the sentences -------------------------------------------------------------------------------
    {
        FailureContext c;
        c.title = QStringLiteral("Twisted Metal 4");
        c.engine = QStringLiteral("psxrecomp");
        c.sourceRepo = QStringLiteral("example/tm4");
        c.logPath = QStringLiteral("D:/eb/recomps/builds/tm4/build.log");
        c.artefactPath = QStringLiteral("D:/eb/emulators/tm4/tm4.exe");

        Machine m;
        m.apply(Signal::Start); m.apply(Signal::SourceReady); m.apply(Signal::UnpackDone);
        m.apply(Signal::GenerateDone); m.apply(Signal::ConfigureDone);
        m.fail(Fault::ChildExit, 1);
        const QString exited = failureSentence(m, c);
        // A sentence plus a path. It names WHICH program, WHAT it did, and WHERE the log is — the three
        // facts "the build failed" leaves somebody without.
        CHECK(exited.contains(QStringLiteral("cmake --build")));
        CHECK(exited.contains(QStringLiteral("exited with code 1")));
        CHECK(exited.contains(c.logPath));

        Machine crashed;
        crashed.apply(Signal::Start); crashed.apply(Signal::SourceReady); crashed.apply(Signal::UnpackDone);
        crashed.fail(Fault::ChildCrash);
        const QString died = failureSentence(crashed, c);
        CHECK(died.contains(QStringLiteral("psxrecomp")));       // the generate step, by the engine's name
        CHECK(died.contains(QStringLiteral("ended unexpectedly")));
        CHECK(died.contains(c.logPath));
        CHECK(!died.contains(QStringLiteral("exited with code")));

        // THE DEFENDER CASE (#248 decision 5). It says what happened, where the file was, and where to look —
        // and it must never tell anybody to turn their antivirus off.
        Machine gone;
        gone.apply(Signal::Start); gone.apply(Signal::SourceReady); gone.apply(Signal::UnpackDone);
        gone.apply(Signal::GenerateDone); gone.apply(Signal::ConfigureDone); gone.apply(Signal::CompileDone);
        gone.fail(Fault::ArtefactMissing);
        c.os = toolchain::Os::Windows;
        const QString win = failureSentence(gone, c);
        CHECK(win.contains(c.artefactPath));
        CHECK(win.contains(QStringLiteral("unsigned")));
        CHECK(win.contains(QStringLiteral("Protection history")));
        CHECK(win.contains(c.logPath));
        CHECK(!win.contains(QStringLiteral("disable")));
        CHECK(!win.contains(QStringLiteral("turn off")));
        CHECK(!win.contains(QStringLiteral("exclusion")));
        c.os = toolchain::Os::Linux;
        const QString nix = failureSentence(gone, c);
        CHECK(nix.contains(QStringLiteral("security software")));
        CHECK(!nix.contains(QStringLiteral("Windows Security")));

        // The three that happen before anything is compiled all say that nothing was changed, because the
        // question a person has after a failed install is what it left behind.
        for (const Fault f : { Fault::SourceUnavailable, Fault::SourceTooBig, Fault::UnpackFailed,
                               Fault::ToolchainMissing })
        {
            Machine e;
            e.apply(Signal::Start);
            e.fail(f);
            const QString s = failureSentence(e, c);
            CHECK(s.contains(QStringLiteral("Twisted Metal 4")));
            CHECK(s.contains(QStringLiteral("Nothing")) || s.contains(QStringLiteral("nothing")));
        }
        // A PLAN THAT COULD NOT BE MADE says WHICH thing it could not do. The generic sentence is the
        // fallback and not the answer: a live drive of this feature ended on "can't be built by this version
        // of EverythingBox" while the log line beside it said the recompiler was not in the source that had
        // just been downloaded, which is the sentence a person can act on.
        Machine noPlan;
        noPlan.apply(Signal::Start); noPlan.apply(Signal::SourceReady); noPlan.apply(Signal::UnpackDone);
        noPlan.fail(Fault::Internal);
        c.internalReason = QStringLiteral("The psxrecomp recompiler wasn't found in the downloaded source.");
        const QString refused = failureSentence(noPlan, c);
        CHECK(refused.startsWith(c.internalReason));
        CHECK(refused.contains(c.logPath));
        c.internalReason.clear();
        const QString generic = failureSentence(noPlan, c);
        CHECK(generic.contains(QStringLiteral("Twisted Metal 4")));
        CHECK(generic.contains(c.logPath));

        // A cancel does not read as a failure, and it answers the question a person actually has.
        const QString cancelled = cancelledSentence(c.title);
        CHECK(cancelled.contains(QStringLiteral("Stopped building")));
        CHECK(cancelled.contains(QStringLiteral("wasn't touched")));
        CHECK(!cancelled.contains(QStringLiteral("failed")));
        CHECK(succeededSentence(c.title, c.artefactPath).contains(c.artefactPath));
    }

    // ---- 6. progress ------------------------------------------------------------------------------------
    CHECK(parsePercent(QStringLiteral("[ 42%] Building CXX object x.o")) == 42);
    CHECK(parsePercent(QStringLiteral("[100%] Linking")) == 100);
    CHECK(parsePercent(QStringLiteral("[  0%] Starting")) == 0);
    CHECK(parsePercent(QStringLiteral("cl : warning C4100")) == -1);
    CHECK(parsePercent(QStringLiteral("[warning] 42% done")) == -1);   // not a percent field
    CHECK(parsePercent(QStringLiteral("[420%] nonsense")) == -1);
    CHECK(parsePercent(QString()) == -1);
    {
        // THE HONEST SURFACE: with no percent at all there is still a phase, an elapsed time and the tools'
        // own last line — three facts that keep changing while the build is alive. #248: never a spinner that
        // never resolves.
        Progress p;
        p.phase = Phase::Compiling;
        p.percent = -1;
        p.elapsedMs = 95000;
        p.lastLine = QStringLiteral("cl : Command line warning D9002");
        const QString line = progressLine(p);
        CHECK(line.contains(QStringLiteral("compiling")));
        CHECK(line.contains(QStringLiteral("1m 35s")));
        CHECK(line.contains(QStringLiteral("D9002")));
        CHECK(!line.contains(QStringLiteral("%")));
        p.percent = 42;
        CHECK(progressLine(p).contains(QStringLiteral("42%")));
    }

    // ---- 7. a log that grows without limit --------------------------------------------------------------
    {
        LogTail t(24, 400);
        for (int i = 0; i < 100000; ++i)
            t.append(QStringLiteral("line %1 of a compiler that will not stop talking\n").arg(i));
        CHECK(t.lines().size() == 24);
        CHECK(t.droppedLines() == 100000 - 24);
        CHECK(t.lines().constLast().contains(QStringLiteral("line 99999")));
        CHECK(t.bytesSeen() > 4000000);
        // ONE line of 40 000 characters is the other shape — a linker echoing its own command line — and a
        // line count alone bounds nothing against it.
        LogTail one(4, 80);
        one.append(QString(40000, QLatin1Char('x')) + QStringLiteral("\n"));
        CHECK(one.lines().size() == 1);
        CHECK(one.lines().at(0).size() == 80);
        // A chunk boundary in the middle of a line is not two lines.
        LogTail split(8, 200);
        split.append(QStringLiteral("hello, "));
        CHECK(split.lines().isEmpty());
        const QStringList fresh = split.append(QStringLiteral("world\r\nand more\n"));
        CHECK(fresh.size() == 2);
        CHECK(split.lines().at(0) == QStringLiteral("hello, world"));   // the CR does not survive
        // ...and an unterminated tail is promoted when the child ends, because a compiler that dies mid-line
        // usually died saying why.
        LogTail dying(8, 200);
        dying.append(QStringLiteral("internal compiler error"));
        CHECK(dying.lines().isEmpty());
        CHECK(dying.flush().size() == 1);
        CHECK(dying.lines().at(0) == QStringLiteral("internal compiler error"));
    }

    // ---- 8. paths that cannot escape --------------------------------------------------------------------
    {
        CHECK(sanitiseId(QStringLiteral("twisted-metal4-psx")) == QStringLiteral("twisted-metal4-psx"));
        CHECK(sanitiseId(QStringLiteral("../../Windows/System32")) == QStringLiteral("windows-system32"));
        CHECK(sanitiseId(QStringLiteral("C:/evil")) == QStringLiteral("c--evil"));
        CHECK(sanitiseId(QStringLiteral("...")).isEmpty());
        // An id that sanitises to nothing gets NO workspace, rather than the builds root itself — which,
        // handed to a recursive delete, would be every build on the machine.
        CHECK(workspaceDir(QStringLiteral("...")).isEmpty());
        CHECK(logPath(QStringLiteral("...")).isEmpty());

        const QString root = QDir(buildsRoot()).absolutePath();
        for (const QString& id : { QStringLiteral("ok"), QStringLiteral("../../etc"),
                                   QStringLiteral("C:/Windows"), QStringLiteral("a/b/c") })
        {
            const QString ws = workspaceDir(id);
            CHECK(!ws.isEmpty());
            CHECK(QDir(ws).absolutePath().startsWith(root + QLatin1Char('/')));
        }
        // The recipe's own cmake.build_dir is catalogue data, so it is checked the same way.
        const QString src = QDir(sourceDir(QStringLiteral("ok"))).absolutePath();
        CHECK(QDir(cmakeBuildDir(QStringLiteral("ok"), QStringLiteral("build"))).absolutePath()
              == src + QStringLiteral("/build"));
        CHECK(QDir(cmakeBuildDir(QStringLiteral("ok"), QStringLiteral("../../../escape"))).absolutePath()
              == src + QStringLiteral("/build"));
        CHECK(QDir(cmakeBuildDir(QStringLiteral("ok"), QStringLiteral("/etc"))).absolutePath()
              == src + QStringLiteral("/build"));
        CHECK(QDir(cmakeBuildDir(QStringLiteral("ok"), QString())).absolutePath()
              == src + QStringLiteral("/build"));

        // Zip slip, as a function.
        const QString base = QStringLiteral("/ws/source");
        CHECK(safeMemberPath(base, QStringLiteral("src/main.cpp"))
              == QStringLiteral("/ws/source/src/main.cpp"));
        CHECK(safeMemberPath(base, QStringLiteral("a/../b")).isEmpty());
        CHECK(safeMemberPath(base, QStringLiteral("../out")).isEmpty());
        CHECK(safeMemberPath(base, QStringLiteral("/etc/passwd")).isEmpty());
        CHECK(safeMemberPath(base, QStringLiteral("C:/Windows/x")).isEmpty());
        CHECK(safeMemberPath(base, QStringLiteral("..\\out")).isEmpty());   // the other slash, too
        CHECK(safeMemberPath(base, QString()).isEmpty());
        CHECK(stripFirstComponent(QStringLiteral("repo-abc123/src/main.cpp"))
              == QStringLiteral("src/main.cpp"));
        CHECK(stripFirstComponent(QStringLiteral("repo-abc123/")).isEmpty());
        CHECK(stripFirstComponent(QStringLiteral("repo-abc123")).isEmpty());
    }

    // ---- 9. the engine's provenance ---------------------------------------------------------------------
    {
        // WHERE A FETCHED ENGINE LANDS, and why that is not redistribution. Under the user's own data
        // directory, never inside this repository, with a notice beside it naming what it is, whose it is and
        // where it came from.
        const QString dir = engineDir(QStringLiteral("psxrecomp"));
        CHECK(!dir.isEmpty());
        CHECK(QDir(dir).absolutePath().startsWith(QDir(enginesRoot()).absolutePath() + QLatin1Char('/')));
        // A garbage id gets NO engine folder rather than the engines root itself, for the same reason a
        // garbage port id gets no workspace.
        CHECK(engineDir(QStringLiteral("../../..")).isEmpty());
        CHECK(!writeEngineNotice(QString(), QStringLiteral("x"), QString(), QString()));
        const QString notice = engineNoticeText(QStringLiteral("psxrecomp"),
                                                QStringLiteral("PolyForm Noncommercial 1.0.0"),
                                                QStringLiteral("https://example.invalid/psxrecomp"));
        CHECK(notice.contains(QStringLiteral("psxrecomp")));
        CHECK(notice.contains(QStringLiteral("PolyForm Noncommercial 1.0.0")));
        CHECK(notice.contains(QStringLiteral("https://example.invalid/psxrecomp")));
        CHECK(notice.contains(QStringLiteral("does not")));
        CHECK(writeEngineNotice(dir, QStringLiteral("psxrecomp"), QStringLiteral("PolyForm Noncommercial 1.0.0"),
                                QStringLiteral("https://example.invalid/psxrecomp")));
        CHECK(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("WHERE-THIS-CAME-FROM.txt"))));
    }

    // ---- 10. the step plan ------------------------------------------------------------------------------
    const QString romDir = scratch + QStringLiteral("/library");
    const QString romPath = romDir + QStringLiteral("/Some Game (USA).bin");
    const QByteArray romBytes = QByteArray("PROBE-ROM-MARKER-") + QByteArray(512, 'Z');
    {
        QDir().mkpath(romDir);
        QFile f(romPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(romBytes);
    }
    {
        PlanInput in;
        in.engine = QStringLiteral("psxrecomp");
        in.engineExe = stubPath();
        in.sourceDir = scratch + QStringLiteral("/src");
        in.buildDir = scratch + QStringLiteral("/src/build");
        in.cmakeExe = stubPath();      // stands in for cmake; the plan does not care which program it is
        in.cmakeConfig = QStringLiteral("Release");
        in.cmakeTarget = QStringLiteral("TM4");
        in.romPath = romPath;
        const Plan p = planSteps(in);
        CHECK(p.ok());
        CHECK(p.steps.size() == 3);
        CHECK(p.steps.at(0).phase == Phase::Generating);
        CHECK(p.steps.at(1).phase == Phase::Configuring);
        CHECK(p.steps.at(2).phase == Phase::Compiling);
        // SCHEMA.md: PSX passes the library disc as `--disc`, with `config` defaulting to game.toml.
        CHECK(p.steps.at(0).args == QStringList({ QStringLiteral("game.toml"), QStringLiteral("--disc"),
                                                  romPath }));
        CHECK(p.steps.at(2).args.contains(QStringLiteral("--target")));
        CHECK(p.steps.at(2).args.contains(QStringLiteral("TM4")));
        CHECK(p.steps.at(1).args.contains(QStringLiteral("-DCMAKE_BUILD_TYPE=Release")));

        // NO ROM MOVES. The dump appears exactly once in the whole plan, as ONE argument of ONE step, and no
        // step's working directory is the folder it lives in. That is the entire relationship between this
        // feature and somebody's game file, asserted rather than described.
        int mentions = 0;
        for (const Step& s : p.steps)
        {
            for (const QString& a : s.args) if (a == romPath) ++mentions;
            CHECK(QDir(s.workDir).absolutePath() != QDir(romDir).absolutePath());
            CHECK(!s.program.contains(QStringLiteral("copy")));
            CHECK(!s.program.contains(QStringLiteral("xcopy")));
        }
        CHECK(mentions == 1);

        // The refusals, each of which is a sentence a person can act on rather than an empty step list.
        PlanInput other = in;
        other.engine = QStringLiteral("snesrecomp");
        CHECK(!planSteps(other).ok());
        CHECK(planSteps(other).refusal.contains(QStringLiteral("snesrecomp")));
        CHECK(planSteps(other).steps.isEmpty());
        other = in; other.engine.clear();
        CHECK(!planSteps(other).ok());
        other = in; other.romPath.clear();
        CHECK(planSteps(other).refusal.contains(QStringLiteral("copy of the game")));
        other = in; other.engineExe.clear();
        CHECK(planSteps(other).refusal.contains(QStringLiteral("psxrecomp")));
        other = in; other.cmakeExe.clear();
        CHECK(!planSteps(other).ok());
        // A default config and a default generate config, so an entry that says neither still plans.
        other = in; other.cmakeConfig.clear(); other.cmakeTarget.clear();
        const Plan defaults = planSteps(other);
        CHECK(defaults.ok());
        CHECK(defaults.steps.at(1).args.contains(QStringLiteral("-DCMAKE_BUILD_TYPE=Release")));
        CHECK(!defaults.steps.at(2).args.contains(QStringLiteral("--target")));
    }

    // ---- 11. the runner, against a real child process ---------------------------------------------------
    const QString ws = scratch + QStringLiteral("/run");
    QDir().mkpath(ws);
    const QString runLog = ws + QStringLiteral("/build.log");
    const QString artefact = ws + QStringLiteral("/game.exe");

    {
        // A clean run: it talks, it exits 0, it leaves the artefact. The machine walks Generating -> …
        // -> Succeeded and the tail holds what the child said.
        Machine m;
        m.apply(Signal::Start); m.apply(Signal::SourceReady); m.apply(Signal::UnpackDone);
        Runner r;
        r.logFilePath = runLog;
        int lines = 0;
        r.onLine = [&lines](const QString&) { ++lines; };
        int progressCalls = 0;
        Progress lastProgress;
        r.onProgress = [&](const Progress& p) { ++progressCalls; lastProgress = p; };
        const StepOutcome o = r.runStep(stubStep({ QStringLiteral("--lines"), QStringLiteral("8"),
                                                   QStringLiteral("--make"), artefact },
                                                 ws, Phase::Generating));
        CHECK(o.started && !o.crashed && !o.cancelled && o.exitCode == 0);
        CHECK(lines >= 8);
        CHECK(progressCalls > 0);
        CHECK(lastProgress.percent == 100);          // the stub's last line is [100%]
        CHECK(r.tail.lines().constLast().contains(QStringLiteral("rec_7")));
        CHECK(applyChildResult(m, o.crashed, o.exitCode, Signal::GenerateDone));
        CHECK(m.phase == Phase::Configuring);
        CHECK(QFileInfo::exists(artefact));
        CHECK(QFileInfo(runLog).size() > 0);
    }
    {
        // A non-zero exit. The machine fails with the code, and the sentence carries it.
        Machine m;
        m.apply(Signal::Start); m.apply(Signal::SourceReady); m.apply(Signal::UnpackDone);
        m.apply(Signal::GenerateDone); m.apply(Signal::ConfigureDone);
        Runner r;
        r.logFilePath = runLog;
        const StepOutcome o = r.runStep(stubStep({ QStringLiteral("--lines"), QStringLiteral("3"),
                                                   QStringLiteral("--exit"), QStringLiteral("9") }, ws));
        CHECK(o.started && !o.crashed && o.exitCode == 9);
        CHECK(applyChildResult(m, o.crashed, o.exitCode, Signal::CompileDone));
        CHECK(m.phase == Phase::Failed && m.fault == Fault::ChildExit && m.exitCode == 9);
        FailureContext c;
        c.title = QStringLiteral("A Game");
        c.engine = QStringLiteral("psxrecomp");
        c.logPath = runLog;
        CHECK(failureSentence(m, c).contains(QStringLiteral("exited with code 9")));
    }
    {
        // A child that DIES rather than exits. Reported as a crash, not as an exit code, because "ended
        // unexpectedly" and "told you it failed" are different things to be told.
        Machine m;
        m.apply(Signal::Start); m.apply(Signal::SourceReady); m.apply(Signal::UnpackDone);
        Runner r;
        r.logFilePath = runLog;
        const StepOutcome o = r.runStep(stubStep({ QStringLiteral("--lines"), QStringLiteral("2"),
                                                   QStringLiteral("--crash") }, ws, Phase::Generating));
        CHECK(o.started);
        CHECK(o.crashed);
        CHECK(!o.cancelled);
        CHECK(applyChildResult(m, o.crashed, o.exitCode, Signal::GenerateDone));
        CHECK(m.phase == Phase::Failed && m.fault == Fault::ChildCrash);
    }
    {
        // CANCEL, LANDING INSIDE A RUNNING CHILD, deterministically: the flag is set from the line callback
        // on the third line of a child that would otherwise never stop. Then the child's own outcome — which
        // is a kill, and therefore a crash with a non-zero code — is fed to the machine, and must NOT rewrite
        // the cancellation.
        Machine m;
        m.apply(Signal::Start); m.apply(Signal::SourceReady); m.apply(Signal::UnpackDone);
        m.apply(Signal::GenerateDone); m.apply(Signal::ConfigureDone);
        QAtomicInt cancel(0);
        Runner r;
        r.logFilePath = runLog;
        r.cancelFlag = &cancel;
        r.graceMs = 300;   // see RecompBuildRunner.h: terminate() cannot reach a console child on Windows
        int seen = 0;
        r.onLine = [&](const QString&) {
            if (++seen == 3) { cancel.storeRelease(1); m.apply(Signal::Cancel); }
        };
        const StepOutcome o = r.runStep(stubStep({ QStringLiteral("--forever"),
                                                   QStringLiteral("--sleep-ms"), QStringLiteral("10") }, ws));
        CHECK(o.started);
        CHECK(o.cancelled);
        CHECK(seen >= 3);
        CHECK(m.phase == Phase::Cancelled);
        CHECK(!applyChildResult(m, o.crashed, o.exitCode, Signal::CompileDone));
        CHECK(m.phase == Phase::Cancelled);
        CHECK(m.fault == Fault::None);
        CHECK(failureSentence(m, FailureContext{}).isEmpty());   // a cancel has no failure sentence at all
    }
    {
        // THE DEFENDER CASE, END TO END: every step says it worked and the file is not there.
        QFile::remove(artefact);
        Machine m;
        m.apply(Signal::Start); m.apply(Signal::SourceReady); m.apply(Signal::UnpackDone);
        m.apply(Signal::GenerateDone); m.apply(Signal::ConfigureDone);
        Runner r;
        r.logFilePath = runLog;
        const StepOutcome o = r.runStep(stubStep({ QStringLiteral("--lines"), QStringLiteral("2") }, ws));
        CHECK(o.exitCode == 0 && !o.crashed);
        CHECK(applyChildResult(m, o.crashed, o.exitCode, Signal::CompileDone));
        CHECK(m.phase == Phase::Staging);
        CHECK(applyArtefactCheck(m, QFileInfo::exists(artefact)));
        CHECK(m.phase == Phase::Failed && m.fault == Fault::ArtefactMissing);
    }
    {
        // A program that is not there at all does not start, and that is not a crash and not an exit code.
        Runner r;
        r.logFilePath = runLog;
        Step missing = stubStep({}, ws);
        missing.program = ws + QStringLiteral("/no-such-program");
        const StepOutcome o = r.runStep(missing);
        CHECK(!o.started && !o.cancelled);
    }
    {
        // A child that will not stop talking does not grow the on-screen tail, and the log on disk stays a
        // file somebody could open.
        Runner r;
        r.logFilePath = ws + QStringLiteral("/spew.log");
        const StepOutcome o = r.runStep(stubStep({ QStringLiteral("--long"), QStringLiteral("400") }, ws));
        CHECK(o.started && o.exitCode == 0);
        CHECK(r.tail.lines().size() <= 24);
        for (const QString& l : r.tail.lines()) CHECK(l.size() <= 400);
        CHECK(r.tail.bytesSeen() > 1000000);
        CHECK(QFileInfo(r.logFilePath).size() < kMaxLogBytes);
    }

    // ---- 12. nothing of the user's game, and nothing of an engine, reaches this repository --------------
    {
        // The dump the plan was pointed at is byte-for-byte what it was, in the place it was.
        QFile f(romPath);
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll() == romBytes);
        // ...and no copy of it exists anywhere the build wrote.
        int copies = 0;
        QDirIterator it(ws, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            QFile c(it.next());
            if (c.open(QIODevice::ReadOnly) && c.readAll().contains(QByteArray("PROBE-ROM-MARKER-"))) ++copies;
        }
        CHECK(copies == 0);

        // Every path this feature can write to is under the app's own data directory, and that is not inside
        // the source tree. An engine fetched at build time therefore cannot land in the repository, whatever
        // a catalogue says its id is.
        const QString data = QDir(AppPaths::dataDir()).absolutePath();
        const QString repo = QDir(QStringLiteral(EB_RECOMP_SOURCE_DIR)).absolutePath();
        CHECK(!data.startsWith(repo + QLatin1Char('/')));
        for (const QString& p : { buildsRoot(), enginesRoot(), workspaceDir(QStringLiteral("x")),
                                  engineDir(QStringLiteral("psxrecomp")), logPath(QStringLiteral("x")) })
        {
            CHECK(!p.isEmpty());
            CHECK(QDir(p).absolutePath().startsWith(data));
            CHECK(!QDir(p).absolutePath().startsWith(repo + QLatin1Char('/')));
        }
        // And this repository holds no engine binary — nothing in the tree is named like one. psxrecomp is
        // PolyForm Noncommercial: it may be run on a user's machine and may never be redistributed by us.
        int enginesInRepo = 0;
        QDirIterator repoIt(repo, QStringList{ QStringLiteral("*recomp*.exe"), QStringLiteral("*recomp*.dll"),
                                               QStringLiteral("psxrecomp"), QStringLiteral("snesrecomp"),
                                               QStringLiteral("gbarecomp") },
                            QDir::Files, QDirIterator::Subdirectories);
        while (repoIt.hasNext()) { repoIt.next(); ++enginesInRepo; }
        CHECK(enginesInRepo == 0);
    }

    // ---- 13. UPDATES (issue #248, increment d) ---------------------------------------------------------
    // The comparison that puts `update available` on a row, and the rule that keeps the build that WORKED
    // until the one that replaced it has actually run. The second half is the whole point of the increment,
    // and it is here rather than in a live drive because a live drive cannot stage the case that matters: a
    // rebuild that compiles cleanly and produces something that does not start.
    {
        // ORDERING VERSIONS. Three different projects publish these and none of them promised semver, so the
        // rules are stated rather than assumed — and "not orderable" is a first-class answer.
        auto cmp = [](const char* a, const char* b) {
            return recompupdate::compareVersions(QString::fromLatin1(a), QString::fromLatin1(b));
        };
        CHECK(cmp("1.4.0", "1.4.1") < 0);
        CHECK(cmp("1.4.1", "1.4.0") > 0);
        CHECK(cmp("1.4.0", "1.4.0") == 0);
        CHECK(cmp("1.4", "1.4.0") == 0);            // a missing component is a zero
        CHECK(cmp("v1.4.0", "1.4.0") == 0);         // the leading v is decoration
        CHECK(cmp("1.9.0", "1.10.0") < 0);          // NUMERIC, not lexicographic: 10 comes after 9
        CHECK(cmp("2024.01.15", "2024.2.1") < 0);   // date-shaped versions order too
        CHECK(cmp("1.4.0-rc1", "1.4.0") < 0);       // a pre-release precedes its release
        CHECK(cmp("1.4.0", "1.4.0-rc1") > 0);
        CHECK(cmp("1.4.0-rc1", "1.4.0-rc2") < 0);
        CHECK(recompupdate::versionsOrderable(QStringLiteral("1.4.0"), QStringLiteral("1.5.0")));
        CHECK(!recompupdate::versionsOrderable(QStringLiteral("nightly"), QStringLiteral("1.5.0")));
        CHECK(!recompupdate::versionsOrderable(QString(), QStringLiteral("1.5.0")));
        // A digit run nobody could mean as a number does not silently become zero.
        CHECK(recompupdate::parseVersion(QStringLiteral("123456789012345678901234567890")).ok);
    }
    {
        // THE STAMP a build writes about itself: every field survives the round trip, and a document from a
        // later version of this app is refused rather than half-read.
        recompupdate::BuildStamp s;
        s.portId = QStringLiteral("tm4");
        s.engineVersion = QStringLiteral("1.4.0");
        s.recipe.engine = QStringLiteral("psxrecomp");
        s.recipe.sourceRepo = QStringLiteral("owner/repo");
        s.recipe.sourceRef = QStringLiteral("v1");
        s.recipe.sdkId = QStringLiteral("psx-sdk-1");
        s.recipe.generateConfig = QStringLiteral("game.toml");
        s.recipe.generateOutDir = QStringLiteral("gen");
        s.recipe.cmakeDir = QStringLiteral("build");
        s.recipe.cmakeTarget = QStringLiteral("game");
        s.recipe.cmakeConfig = QStringLiteral("Release");
        s.builtAtMs = 1700000000000LL;
        s.valid = true;

        const recompupdate::BuildStamp back = recompupdate::decodeStamp(recompupdate::encodeStamp(s));
        CHECK(back.valid);
        CHECK(back.portId == s.portId);
        CHECK(back.engineVersion == s.engineVersion);
        CHECK(back.builtAtMs == s.builtAtMs);
        CHECK(!back.launched);
        CHECK(!recompupdate::recipeChanged(back.recipe, s.recipe));
        // ...and every one of those fields really did survive: move any of them and it is seen.
        for (int field = 0; field < 9; ++field)
        {
            recompupdate::BuildRecipe moved = s.recipe;
            QString* target[] = { &moved.engine, &moved.sourceRepo, &moved.sourceRef, &moved.sdkId,
                                  &moved.generateConfig, &moved.generateOutDir, &moved.cmakeDir,
                                  &moved.cmakeTarget, &moved.cmakeConfig };
            *target[field] = QStringLiteral("moved");
            CHECK(recompupdate::recipeChanged(back.recipe, moved));
        }
        CHECK(!recompupdate::decodeStamp(QByteArray()).valid);
        CHECK(!recompupdate::decodeStamp(QByteArray("not json at all")).valid);
        QJsonObject future = QJsonDocument::fromJson(recompupdate::encodeStamp(s)).object();
        future[QStringLiteral("schema")] = 99;
        CHECK(!recompupdate::decodeStamp(QJsonDocument(future).toJson()).valid);
    }
    {
        // THE COMPARISON, case by case. Every expected value is written out here rather than derived from the
        // code under test.
        auto stampOf = [](const char* ver, const char* ref) {
            recompupdate::BuildStamp s;
            s.portId = QStringLiteral("tm4");
            s.engineVersion = QString::fromLatin1(ver);
            s.recipe.engine = QStringLiteral("psxrecomp");
            s.recipe.sourceRepo = QStringLiteral("owner/repo");
            s.recipe.sourceRef = QString::fromLatin1(ref);
            s.recipe.cmakeConfig = QStringLiteral("Release");
            s.builtAtMs = 1000;
            s.valid = true;
            return s;
        };
        auto catOf = [](const char* ver, const char* ref) {
            recompupdate::CatalogueBuild c;
            c.engineVersion = QString::fromLatin1(ver);
            c.recipe.engine = QStringLiteral("psxrecomp");
            c.recipe.sourceRepo = QStringLiteral("owner/repo");
            c.recipe.sourceRef = QString::fromLatin1(ref);
            c.recipe.cmakeConfig = QStringLiteral("Release");
            return c;
        };

        // A NEWER ENGINE.
        CHECK(recompupdate::compareBuild(stampOf("1.4.0", "v1"), catOf("1.5.0", "v1"))
              == recompupdate::Update::EngineNewer);
        CHECK(recompupdate::updateAvailable(recompupdate::Update::EngineNewer));

        // A NEWER RECIPE, same engine.
        CHECK(recompupdate::compareBuild(stampOf("1.4.0", "v1"), catOf("1.4.0", "v2"))
              == recompupdate::Update::RecipeChanged);
        CHECK(recompupdate::updateAvailable(recompupdate::Update::RecipeChanged));

        // THE SAME ENTRY, REPUBLISHED, and this is the case the whole design turns on: the catalogue is
        // rebuilt and redated whenever anybody's submission is approved, and this row did not change.
        CHECK(recompupdate::compareBuild(stampOf("1.4.0", "v1"), catOf("1.4.0", "v1"))
              == recompupdate::Update::UpToDate);
        CHECK(!recompupdate::updateAvailable(recompupdate::Update::UpToDate));

        // ...and it stays UpToDate however far apart the two build times are. `builtAtMs` is written for a
        // person reading the file and is read by nothing.
        {
            recompupdate::BuildStamp old = stampOf("1.4.0", "v1");
            old.builtAtMs = 1;
            recompupdate::BuildStamp recent = stampOf("1.4.0", "v1");
            recent.builtAtMs = 4102444800000LL;
            CHECK(recompupdate::compareBuild(old, catOf("1.4.0", "v1"))
                  == recompupdate::Update::UpToDate);
            CHECK(recompupdate::compareBuild(recent, catOf("1.4.0", "v1"))
                  == recompupdate::Update::UpToDate);
        }

        // A CATALOGUE THAT WENT BACKWARDS. It happens — a release is yanked and the pin reverts — and it must
        // never present as an update, because pressing that button spends twenty minutes going downhill. Note
        // the recipe changed in that same publish and it still is not one.
        CHECK(recompupdate::compareBuild(stampOf("1.5.0", "v2"), catOf("1.4.0", "v1"))
              == recompupdate::Update::CatalogueBehind);
        CHECK(!recompupdate::updateAvailable(recompupdate::Update::CatalogueBehind));

        // NO STAMP AT ALL is "nobody knows", never "out of date".
        CHECK(recompupdate::compareBuild(recompupdate::BuildStamp{}, catOf("9.9.9", "v9"))
              == recompupdate::Update::Unknown);
        CHECK(!recompupdate::updateAvailable(recompupdate::Update::Unknown));

        // AN UNKNOWN ON EITHER SIDE of the engine version falls back to the recipe rather than guessing.
        CHECK(recompupdate::compareBuild(stampOf("", "v1"), catOf("2.0.0", "v1"))
              == recompupdate::Update::UpToDate);
        CHECK(recompupdate::compareBuild(stampOf("1.4.0", "v1"), catOf("", "v2"))
              == recompupdate::Update::RecipeChanged);

        // A FIELD THE CATALOGUE STOPPED PINNING is unknown, not a difference. This is the upgrade-day rule:
        // it is also what stops a stamp written by an older build of this app from reading as out of date.
        CHECK(recompupdate::compareBuild(stampOf("1.4.0", "v1"), catOf("1.4.0", ""))
              == recompupdate::Update::UpToDate);

        // Two engine versions that cannot be ordered are `changed`, never `newer`.
        CHECK(recompupdate::compareBuild(stampOf("nightly-a", "v1"), catOf("nightly-b", "v1"))
              == recompupdate::Update::EngineChanged);
        CHECK(recompupdate::compareBuild(stampOf("nightly-a", "v1"), catOf("nightly-a", "v1"))
              == recompupdate::Update::UpToDate);

        // The sentences name both versions, and the backwards one says so out loud rather than going quiet.
        CHECK(recompupdate::updateSentence(recompupdate::Update::EngineNewer, stampOf("1.4.0", "v1"),
                                           catOf("1.5.0", "v1"))
                  .contains(QStringLiteral("1.5.0")));
        CHECK(recompupdate::updateSentence(recompupdate::Update::CatalogueBehind, stampOf("1.5.0", "v1"),
                                           catOf("1.4.0", "v1"))
                  .contains(QStringLiteral("OLDER")));

        // WHAT CHANGED, in words — one line per field, and it is the same walk the predicate makes.
        const QStringList ch = recompupdate::recipeChanges(stampOf("1.4.0", "v1").recipe,
                                                           catOf("1.4.0", "v2").recipe);
        CHECK(ch.size() == 1);
        CHECK(ch.value(0).contains(QStringLiteral("source version")));
        CHECK(ch.value(0).contains(QStringLiteral("v2")));
    }
    {
        // WHAT PROVES A BUILD. Conservative on purpose: keeping a dead copy costs disk, dropping a live one
        // costs somebody the program that worked.
        CHECK(!recompupdate::launchProvesBuild(false, 0, false));           // never became a process
        CHECK(!recompupdate::launchProvesBuild(false, 600000, true));       // ...however it is dressed up
        CHECK(recompupdate::launchProvesBuild(true, 10, true));             // the user closed it: it ran
        CHECK(!recompupdate::launchProvesBuild(true, 300, false));          // up for 300 ms is a failed boot
        CHECK(!recompupdate::launchProvesBuild(true, recompupdate::kLaunchProvesMs - 1, false));
        CHECK(recompupdate::launchProvesBuild(true, recompupdate::kLaunchProvesMs, false));
        CHECK(recompupdate::kLaunchProvesMs == 4000);   // GameLauncher's own failed-boot threshold
    }
    {
        // ---- THE KEEP-UNTIL-LAUNCHED RULE, on real directories -----------------------------------------
        // Both directions, which is what #248 (d) asks for: the new build running (the old one goes) and the
        // new build failing to start (the old one is still there AND still launchable).
        auto putFile = [](const QString& path, const QByteArray& bytes) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            const bool ok = f.write(bytes) == bytes.size();
            f.close();
            return ok;
        };
        auto slurp = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };

        const QString emuRoot = scratch + QStringLiteral("/emulators");
        const QString install = emuRoot + QStringLiteral("/tm4");
        const QString exe = install + QStringLiteral("/tm4.exe");
        QString why;

        recompupdate::BuildStamp s1;
        s1.portId = QStringLiteral("tm4");
        s1.engineVersion = QStringLiteral("1.4.0");
        s1.recipe.engine = QStringLiteral("psxrecomp");
        s1.recipe.sourceRepo = QStringLiteral("owner/repo");
        s1.recipe.sourceRef = QStringLiteral("v1");
        s1.builtAtMs = 1;
        s1.valid = true;

        // ---- the FIRST build. There is nothing to keep, and that is a success with no kept copy.
        CHECK(recompupdate::keepAside(install, &why));
        CHECK(!recompupdate::hasKept(install));
        CHECK(putFile(exe, QByteArray("BUILD-ONE")));
        CHECK(putFile(install + QStringLiteral("/data/shader.glsl"), QByteArray("SHADER-ONE")));
        CHECK(recompupdate::writeStamp(install, s1));
        CHECK(recompupdate::readStamp(install).valid);
        CHECK(recompupdate::readStamp(install).engineVersion == QStringLiteral("1.4.0"));
        CHECK(!recompupdate::readStamp(install).launched);
        CHECK(recompupdate::markLaunched(install));
        CHECK(recompupdate::readStamp(install).launched);
        CHECK(recompupdate::markLaunched(install));   // idempotent: a second run is not a second promotion

        // ---- the catalogue moves, and the row would now say `update available`.
        recompupdate::CatalogueBuild moved;
        moved.engineVersion = QStringLiteral("1.5.0");
        moved.recipe = s1.recipe;
        CHECK(recompupdate::compareBuild(recompupdate::readStamp(install), moved)
              == recompupdate::Update::EngineNewer);

        // ---- REBUILD ONE: it works.
        CHECK(recompupdate::keepAside(install, &why));
        CHECK(recompupdate::hasKept(install));
        CHECK(!QFileInfo::exists(exe));                                     // the place is empty for the new one
        CHECK(slurp(recompupdate::keptDirFor(install) + QStringLiteral("/tm4.exe")) == QByteArray("BUILD-ONE"));
        CHECK(slurp(recompupdate::keptDirFor(install) + QStringLiteral("/data/shader.glsl"))
              == QByteArray("SHADER-ONE"));                                 // everything came with it
        CHECK(putFile(exe, QByteArray("BUILD-TWO")));
        recompupdate::BuildStamp s2 = s1;
        s2.engineVersion = QStringLiteral("1.5.0");
        s2.launched = false;
        s2.builtAtMs = 2;
        CHECK(recompupdate::writeStamp(install, s2));
        // TWO BUILDS ON THE DISK, and the size of the kept one is knowable — which is what the sentence on
        // the card and the qualifier on the row are made of.
        CHECK(recompupdate::hasKept(install));
        CHECK(recompupdate::keptBytes(install) > 0);
        CHECK(recompupdate::dirBytes(install) > 0);

        // ...the new one runs, so the old one goes, and EXACTLY ONE build is left.
        CHECK(recompupdate::launchProvesBuild(true, 9000, false));
        CHECK(recompupdate::markLaunched(install));
        CHECK(recompupdate::dropKept(install));
        CHECK(!recompupdate::hasKept(install));
        CHECK(recompupdate::keptBytes(install) == -1);
        CHECK(slurp(exe) == QByteArray("BUILD-TWO"));
        CHECK(!QDir(recompupdate::keptDirFor(install)).exists());

        // ---- REBUILD TWO: it compiles and the program does not start.
        CHECK(recompupdate::keepAside(install, &why));
        CHECK(putFile(exe, QByteArray("BUILD-THREE-BROKEN")));
        recompupdate::BuildStamp s3 = s2;
        s3.engineVersion = QStringLiteral("1.6.0");
        s3.launched = false;
        CHECK(recompupdate::writeStamp(install, s3));
        CHECK(!recompupdate::launchProvesBuild(false, 0, false));           // it never became a process
        // NOTHING IS DROPPED. The build that worked is still there, whole, and still a program.
        CHECK(recompupdate::hasKept(install));
        CHECK(slurp(recompupdate::keptDirFor(install) + QStringLiteral("/tm4.exe")) == QByteArray("BUILD-TWO"));
        CHECK(QFileInfo(recompupdate::keptDirFor(install) + QStringLiteral("/tm4.exe")).isFile());
        // ...and going back puts it where the launcher looks for it, with its own stamp intact — including
        // the fact that it had already run, so it is not offered as `ready` all over again.
        CHECK(recompupdate::restoreKept(install, &why));
        CHECK(slurp(exe) == QByteArray("BUILD-TWO"));
        CHECK(!recompupdate::hasKept(install));
        CHECK(recompupdate::readStamp(install).engineVersion == QStringLiteral("1.5.0"));
        CHECK(recompupdate::readStamp(install).launched);

        // ---- REBUILD THREE: it starts and dies instantly, which is not a launch either.
        CHECK(recompupdate::keepAside(install, &why));
        CHECK(putFile(exe, QByteArray("BUILD-FOUR-BROKEN")));
        CHECK(!recompupdate::launchProvesBuild(true, 300, false));
        CHECK(recompupdate::hasKept(install));
        CHECK(recompupdate::restoreKept(install, &why));
        CHECK(slurp(exe) == QByteArray("BUILD-TWO"));

        // ---- A FAILED REBUILD CHANGES NOTHING, in the one window where it could: the compile worked, the
        // staging did not, and the previous build was already moved aside. RecompBuildJob restores it there;
        // this is that restore, and what it has to leave behind is the byte-for-byte install.
        const QByteArray liveExe = slurp(exe);
        const QByteArray liveStamp = slurp(recompupdate::stampPath(install));
        CHECK(!liveExe.isEmpty());
        CHECK(!liveStamp.isEmpty());
        CHECK(recompupdate::keepAside(install, &why));
        CHECK(recompupdate::restoreKept(install, &why));
        CHECK(slurp(exe) == liveExe);
        CHECK(slurp(recompupdate::stampPath(install)) == liveStamp);
        CHECK(!recompupdate::hasKept(install));

        // ---- RESTORING WHEN THERE IS NOTHING KEPT must not touch the install. This is the ordering guard:
        // a restore that deleted first and looked afterwards would delete the only build on the machine.
        QString whyNone;
        CHECK(!recompupdate::restoreKept(install, &whyNone));
        CHECK(!whyNone.isEmpty());
        CHECK(slurp(exe) == liveExe);

        // ---- NO ACCUMULATION. Rebuild three more times; ONE kept copy exists, and it is the one from the
        // last rebuild rather than a museum of every build this machine ever made.
        for (int i = 0; i < 3; ++i)
        {
            CHECK(recompupdate::keepAside(install, &why));
            CHECK(putFile(exe, QByteArray("REBUILD-") + QByteArray::number(i)));
            CHECK(recompupdate::writeStamp(install, s2));
        }
        const QStringList kept = QDir(emuRoot + QStringLiteral("/.eb-previous"))
                                     .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        CHECK(kept.size() == 1);
        CHECK(slurp(recompupdate::keptDirFor(install) + QStringLiteral("/tm4.exe"))
              == QByteArray("REBUILD-1"));
        CHECK(slurp(exe) == QByteArray("REBUILD-2"));

        // ---- A FEED REFRESH THAT CHANGES EVERYTHING STARTS NOTHING AND WRITES NOTHING. The comparison is
        // asked two hundred times about catalogues that share no field with the one this was built from; the
        // stamp on disk is byte-for-byte what it was and the kept copy is exactly where it was.
        const QByteArray stampBefore = slurp(recompupdate::stampPath(install));
        const QByteArray keptBefore = slurp(recompupdate::keptDirFor(install) + QStringLiteral("/tm4.exe"));
        CHECK(!stampBefore.isEmpty());
        for (int i = 0; i < 200; ++i)
        {
            recompupdate::CatalogueBuild wild;
            wild.engineVersion = QStringLiteral("%1.%2.0").arg(i).arg(i);
            wild.recipe.engine = QStringLiteral("engine-%1").arg(i);
            wild.recipe.sourceRepo = QStringLiteral("owner/repo-%1").arg(i);
            wild.recipe.sourceRef = QStringLiteral("ref-%1").arg(i);
            wild.recipe.sdkId = QStringLiteral("sdk-%1").arg(i);
            wild.recipe.cmakeTarget = QStringLiteral("target-%1").arg(i);
            (void)recompupdate::compareBuild(recompupdate::readStamp(install), wild);
        }
        CHECK(slurp(recompupdate::stampPath(install)) == stampBefore);
        CHECK(slurp(recompupdate::keptDirFor(install) + QStringLiteral("/tm4.exe")) == keptBefore);
        CHECK(slurp(exe) == QByteArray("REBUILD-2"));
    }
    {
        // PATH SAFETY. Every one of these functions can end in a recursive delete, and the directory they act
        // on is named after a catalogue id. Nothing without a name and a parent is acted on at all.
        CHECK(recompupdate::stampPath(QString()).isEmpty());
        CHECK(recompupdate::keptDirFor(QString()).isEmpty());
        CHECK(recompupdate::keptDirFor(QStringLiteral("/")).isEmpty());
        CHECK(recompupdate::keptDirFor(QStringLiteral("   ")).isEmpty());
        // A kept copy is not itself something to keep: the dot folder they live in is refused by name.
        CHECK(recompupdate::keptDirFor(scratch + QStringLiteral("/emulators/.eb-previous")).isEmpty());
        QString whyBad;
        CHECK(!recompupdate::keepAside(QString(), &whyBad));
        CHECK(!whyBad.isEmpty());
        CHECK(!recompupdate::restoreKept(QString(), &whyBad));
        CHECK(!recompupdate::writeStamp(QString(), recompupdate::BuildStamp{}));
        CHECK(!recompupdate::readStamp(QString()).valid);
        CHECK(!recompupdate::markLaunched(scratch + QStringLiteral("/emulators/never-built")));
        // Keeping nothing is a success with no kept copy — the first build of an entry.
        CHECK(recompupdate::keepAside(scratch + QStringLiteral("/emulators/never-built"), &whyBad));
        CHECK(!recompupdate::hasKept(scratch + QStringLiteral("/emulators/never-built")));
        CHECK(recompupdate::dirBytes(scratch + QStringLiteral("/emulators/never-built")) == -1);
    }
    {
        // THE DISK COST, SAID OUT LOUD (#248 d item 5). A number and a path, or an honest sentence with
        // neither — never "0 MB".
        CHECK(recompupdate::megabytes(-1).isEmpty());
        CHECK(recompupdate::megabytes(52428800) == QStringLiteral("50 MB"));
        CHECK(recompupdate::megabytes(1572864) == QStringLiteral("1.5 MB"));
        const QString kept = recompupdate::keptCopySentence(QStringLiteral("Klonoa"), 52428800,
                                                            QStringLiteral("D:/emulators/.eb-previous/klonoa"));
        CHECK(kept.contains(QStringLiteral("50 MB")));
        CHECK(kept.contains(QStringLiteral("D:/emulators/.eb-previous/klonoa")));
        CHECK(kept.contains(QStringLiteral("first time the new one runs")));
        CHECK(recompupdate::keptCopySentence(QStringLiteral("Klonoa"), -1, QString())
                  .contains(QStringLiteral("still on this computer")));
        CHECK(recompupdate::keptRemovedSentence(QStringLiteral("Klonoa"), 52428800)
                  .contains(QStringLiteral("50 MB")));
        CHECK(recompupdate::keptSurvivedSentence(QStringLiteral("Klonoa"))
                  .contains(QStringLiteral("Go back to the previous build")));
        CHECK(recompupdate::cannotKeepSentence(QStringLiteral("Klonoa"))
                  .contains(QStringLiteral("will not build over one it cannot put back")));
        CHECK(recompupdate::restoredSentence(QStringLiteral("Klonoa"))
                  .contains(QStringLiteral("nothing else was changed")));
    }
    {
        // ---- A REBUILD IS EXPLICIT, AND NOTHING ON THE REFRESH PATH CAN START ONE ----------------------
        // #248 (d) decision 2, asserted where it can actually be broken. The behavioural half is above (the
        // comparison writes nothing, however wildly the catalogue moves); this is the structural half: the
        // feed's own translation units do not so much as NAME the build job, and the single call that starts
        // a build lives in the card that has a button on it.
        const QString repo = QStringLiteral(EB_RECOMP_SOURCE_DIR);
        auto text = [](const QString& p) {
            QFile f(p);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        for (const QString& rel : { QStringLiteral("/src/core/RecompFeed.cpp"),
                                    QStringLiteral("/src/core/RecompFeedFetch.cpp"),
                                    QStringLiteral("/src/core/RecompUpdates.cpp") })
        {
            const QString body = text(repo + rel);
            CHECK(!body.isEmpty());
            CHECK(!body.contains(QStringLiteral("RecompBuildJob")));
        }
        int startersInCard = 0, startersElsewhere = 0;
        QDirIterator srcIt(repo + QStringLiteral("/src"), QStringList{ QStringLiteral("*.cpp") },
                           QDir::Files, QDirIterator::Subdirectories);
        while (srcIt.hasNext())
        {
            const QString path = srcIt.next();
            const QString body = text(path);
            const int calls = body.count(QStringLiteral("job.start("))
                              + body.count(QStringLiteral("RecompBuildJob::instance().start("));
            if (calls == 0) continue;
            if (QFileInfo(path).fileName() == QStringLiteral("MainWindowRecomps.cpp")) startersInCard += calls;
            else startersElsewhere += calls;
        }
        CHECK(startersInCard == 1);
        CHECK(startersElsewhere == 0);
    }

    QDir(scratch).removeRecursively();
    if (failures) { std::printf("RECOMPBUILD-FAIL %d checks\n", failures); return 1; }
    std::printf("RECOMPBUILD-OK\n");
    return 0;
}
