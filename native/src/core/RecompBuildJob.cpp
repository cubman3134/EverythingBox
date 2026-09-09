// The build, run (issue #248, increment c). See RecompBuildJob.h for what it does and what it refuses to do.
#include "RecompBuildJob.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QThreadPool>

#include "BoundedFetch.h"
#include "EmulatorManager.h"
#include "RecompBuildRunner.h"
#include "RecompFeed.h"
#include "Toolchain.h"

using namespace recompbuild;

namespace {

// The project's own source, as GitHub publishes it. The zipball endpoint takes a tag, a branch or a commit
// and — with no ref at all — the repository's default branch, which is what an entry that pins no `ref` is
// asking for. BoundedFetch follows the redirect it answers with.
QString zipballUrl(const QString& repo, const QString& ref)
{
    if (repo.trimmed().isEmpty()) return QString();
    QString url = QStringLiteral("https://api.github.com/repos/") + repo.trimmed()
                  + QStringLiteral("/zipball");
    if (!ref.trimmed().isEmpty()) url += QLatin1Char('/') + ref.trimmed();
    return url;
}

// SCHEMA.md: RetComM "harvests tools from that zip when present". So the recompiler is looked for INSIDE the
// source tree that was just unpacked, by the name the entry gave it. Nothing is downloaded to find it, and
// nothing is downloaded when it is not there — the build stops and says so.
QString findEngineExe(const QString& sourceRoot, const QString& engineId)
{
    if (sourceRoot.isEmpty() || engineId.trimmed().isEmpty()) return QString();
    const QString bare = engineId.trimmed();
    QStringList wanted{ bare };
#if defined(Q_OS_WIN)
    wanted << bare + QStringLiteral(".exe");
#endif
    QDirIterator it(sourceRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        const QString name = QFileInfo(path).fileName();
        for (const QString& w : wanted)
            if (name.compare(w, Qt::CaseInsensitive) == 0) return path;
    }
    return QString();
}

// The finished program, wherever the project's CMake put it. `launch.<os>` is a path relative to the install
// the port publishes, so it is looked for under the build directory first and then anywhere beneath it — a
// multi-config generator (which is what MSVC is) puts it in build/Release/ and the recipe does not say so.
QString findArtefact(const QString& buildDir, const QString& launchRelative)
{
    if (buildDir.isEmpty()) return QString();
    const QString wanted = QFileInfo(launchRelative.trimmed()).fileName();
    if (wanted.isEmpty()) return QString();
    const QString direct = QDir(buildDir).filePath(launchRelative.trimmed());
    if (QFileInfo::exists(direct)) return direct;
    QDirIterator it(buildDir, QStringList{ wanted }, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

QString launchRelativeForThisOs(const NativePortBinding& p)
{
#if defined(Q_OS_WIN)
    return p.launchWindows;
#elif defined(Q_OS_MACOS)
    return p.launchMacos;
#else
    return p.launchLinux;
#endif
}

}  // namespace

RecompBuildJob& RecompBuildJob::instance()
{
    static RecompBuildJob job;
    return job;
}

RecompBuildJob::Snapshot RecompBuildJob::snapshot() const
{
    QMutexLocker lock(&mutex_);
    return snap_;
}

bool RecompBuildJob::busy() const
{
    QMutexLocker lock(&mutex_);
    return busy_;
}

bool RecompBuildJob::isActive(const QString& portId) const
{
    QMutexLocker lock(&mutex_);
    return !portId.isEmpty() && snap_.portId == portId;
}

void RecompBuildJob::cancel()
{
    cancelFlag_.storeRelease(1);
    {
        QMutexLocker lock(&mutex_);
        if (!isRunning(snap_.phase)) return;   // nothing to stop; do not paint "stopping" over a finished one
        snap_.stopping = true;
    }
    emit progressed();
}

void RecompBuildJob::publish(const Machine& m, const Progress& p, const QStringList& tail)
{
    {
        QMutexLocker lock(&mutex_);
        snap_.phase = m.phase;
        snap_.progress = p;
        snap_.logTail = tail;
    }
    // Queued: this runs on the pooled thread and every connected slot repaints.
    QMetaObject::invokeMethod(this, [this]() { emit progressed(); }, Qt::QueuedConnection);
}

bool RecompBuildJob::start(const ExternalEmulator& port, const QString& romPath, QString* why)
{
    {
        QMutexLocker lock(&mutex_);
        if (busy_)
        {
            // Named, because "a build is already running" without saying WHICH is useless on a machine with
            // fifteen catalogue entries.
            if (why)
                *why = QObject::tr("A build is already running (%1). Only one at a time — they would compete "
                                   "for the same processor.")
                           .arg(snap_.title.isEmpty() ? snap_.portId : snap_.title);
            return false;
        }
        busy_ = true;
        cancelFlag_.storeRelease(0);
        snap_ = Snapshot{};
        snap_.portId = port.id;
        snap_.title = port.port.name.isEmpty() ? port.displayName : port.port.name;
        snap_.logFilePath = logPath(port.id);
        snap_.phase = Phase::Idle;
        snap_.stopping = false;
    }
    ExternalEmulator copy = port;
    const QString rom = romPath;
    QThreadPool::globalInstance()->start([this, copy, rom]() { runOnWorker(copy, rom); });
    return true;
}

void RecompBuildJob::runOnWorker(ExternalEmulator port, QString romPath)
{
    Machine m;
    Progress prog;
    Runner runner;

    FailureContext ctx;
    ctx.title = port.port.name.isEmpty() ? port.displayName : port.port.name;
    ctx.engine = port.port.buildEngine.trimmed().toLower();
    ctx.sourceRepo = port.port.buildSourceRepo;
    ctx.logPath = logPath(port.id);
    ctx.os = toolchain::hostOs();

    auto finish = [&]() {
        QString message;
        if (m.phase == Phase::Succeeded) message = succeededSentence(ctx.title, ctx.artefactPath);
        else if (m.phase == Phase::Cancelled) message = cancelledSentence(ctx.title);
        else message = failureSentence(m, ctx);
        {
            QMutexLocker lock(&mutex_);
            snap_.phase = m.phase;
            snap_.message = message;
            snap_.logTail = runner.tail.lines();
            busy_ = false;
        }
        const QString id = port.id;
        const int phase = int(m.phase);
        QMetaObject::invokeMethod(
            this, [this, id, phase, message]() { emit progressed(); emit ended(id, phase, message); },
            Qt::QueuedConnection);
    };

    // ---- 1. what can this machine build with? ----------------------------------------------------------
    // FIRST, before a byte is downloaded and before a directory is created. A person whose computer has no
    // compiler should be told that in a second, not after a 40 MB download.
    const toolchain::Decision decision = toolchain::decide(toolchain::detect(), toolchain::hostOs());
    if (!decision.canBuild())
    {
        m.fail(Fault::ToolchainMissing);
        publish(m, prog, {});
        finish();
        return;
    }
    if (cancelFlag_.loadAcquire() != 0) { m.apply(Signal::Start); m.apply(Signal::Cancel); finish(); return; }

    const QString ws = workspaceDir(port.id);
    const QString src = sourceDir(port.id);
    if (ws.isEmpty() || src.isEmpty())
    {
        m.fail(Fault::Internal);
        publish(m, prog, {});
        finish();
        return;
    }
    QDir(ws).removeRecursively();     // a rebuild starts from the source the catalogue pins TODAY
    QDir().mkpath(src);
    QFile::remove(ctx.logPath);
    runner.logFilePath = ctx.logPath;
    runner.cancelFlag = &cancelFlag_;
    runner.onProgress = [&](const Progress& p) { prog = p; publish(m, prog, runner.tail.lines()); };
    runner.onLine = [&](const QString&) { publish(m, prog, runner.tail.lines()); };

    // ---- 2. the source ---------------------------------------------------------------------------------
    m.apply(Signal::Start);
    prog.phase = m.phase;
    publish(m, prog, {});
    const QString url = zipballUrl(port.port.buildSourceRepo, port.port.buildSourceRef);
    if (url.isEmpty()) { m.fail(Fault::SourceUnavailable); publish(m, prog, {}); finish(); return; }
    const BoundedFetch::Result got = BoundedFetch::get(url, kSourceTimeoutMs, kMaxSourceBytes);
    if (cancelFlag_.loadAcquire() != 0) { m.apply(Signal::Cancel); publish(m, prog, {}); finish(); return; }
    if (got.verdict != BoundedFetch::Result::Ok)
    {
        m.fail(got.verdict == BoundedFetch::Result::TooBig ? Fault::SourceTooBig : Fault::SourceUnavailable);
        publish(m, prog, {});
        finish();
        return;
    }

    // ---- 3. unpack -------------------------------------------------------------------------------------
    m.apply(Signal::SourceReady);
    prog.phase = m.phase;
    publish(m, prog, {});
    QString unpackError;
    // Reuses the feed's zip reader rather than a second one: one implementation of "read a zip" is one place
    // a malformed archive is handled. It unpacks in memory, which bounds this step at kMaxSourceBytes plus
    // the expansion — the reason that ceiling is where it is.
    const QHash<QString, QByteArray> members = RecompFeed::unpack(got.body, &unpackError);
    if (members.isEmpty())
    {
        m.fail(Fault::UnpackFailed);
        publish(m, prog, {});
        finish();
        return;
    }
    bool wroteAnything = false;
    for (auto it = members.constBegin(); it != members.constEnd(); ++it)
    {
        // A GitHub zipball wraps everything in one folder named for the repo and the commit; the recipe's
        // paths are relative to what is inside it.
        const QString rel = stripFirstComponent(it.key());
        if (rel.isEmpty()) continue;
        const QString dest = safeMemberPath(src, rel);
        if (dest.isEmpty()) continue;     // a member that tried to escape the workspace; see safeMemberPath
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) continue;
        f.write(it.value());
        f.close();
        wroteAnything = true;
    }
    if (!wroteAnything)
    {
        m.fail(Fault::UnpackFailed);
        publish(m, prog, {});
        finish();
        return;
    }
    if (cancelFlag_.loadAcquire() != 0) { m.apply(Signal::Cancel); publish(m, prog, {}); finish(); return; }

    // ---- 4. the plan -----------------------------------------------------------------------------------
    m.apply(Signal::UnpackDone);
    prog.phase = m.phase;
    publish(m, prog, runner.tail.lines());

    PlanInput in;
    in.engine = ctx.engine;
    in.engineExe = findEngineExe(src, ctx.engine);
    in.sourceDir = src;
    in.buildDir = cmakeBuildDir(port.id, port.port.buildCmakeDir);
    in.cmakeExe = toolchain::detect().cmakePath;
    in.cmakeConfig = port.port.buildCmakeConfig;
    in.cmakeTarget = port.port.buildCmakeTarget;
    in.romPath = romPath;
    in.generateConfig = port.port.buildGenerateConfig;
    in.os = toolchain::hostOs();
    const Plan plan = planSteps(in);
    if (!plan.ok())
    {
        // The refusal is a sentence in its own right; it goes into the log so "the full log is at …" leads to
        // it, and the terminal state is Internal so the card says which one it was.
        QFile log(ctx.logPath);
        if (log.open(QIODevice::WriteOnly | QIODevice::Append))
            log.write((QStringLiteral("\n[EverythingBox] ") + plan.refusal + QLatin1Char('\n')).toUtf8());
        ctx.internalReason = plan.refusal;
        m.fail(Fault::Internal);
        publish(m, prog, runner.tail.lines());
        finish();
        return;
    }

    // ---- 5. run it -------------------------------------------------------------------------------------
    static const Signal doneSignal[] = { Signal::GenerateDone, Signal::ConfigureDone, Signal::CompileDone };
    for (int i = 0; i < plan.steps.size() && i < 3; ++i)
    {
        prog.phase = m.phase;
        publish(m, prog, runner.tail.lines());
        const StepOutcome o = runner.runStep(plan.steps.at(i));
        if (o.cancelled || cancelFlag_.loadAcquire() != 0) m.apply(Signal::Cancel);
        if (!o.started)
        {
            // The program named in the plan was not there when it came to run it — which for the recompiler
            // means the source tree did not hold what the entry said it did.
            ctx.internalReason = QObject::tr("%1 couldn't be run — it isn't where the recipe said it would be.")
                                     .arg(QFileInfo(plan.steps.at(i).program).fileName());
            m.fail(Fault::Internal);
            break;
        }
        applyChildResult(m, o.crashed, o.exitCode, doneSignal[i]);
        if (m.terminal()) break;
    }
    if (m.terminal()) { publish(m, prog, runner.tail.lines()); finish(); return; }

    // ---- 6. stage it -----------------------------------------------------------------------------------
    // The finished program goes where the standalone tier already looks for it, so Play (native) is the
    // SAME verb on a built port as on a downloaded one — #248 item 5, and no second launch path.
    prog.phase = m.phase;
    publish(m, prog, runner.tail.lines());
    const QString built = findArtefact(in.buildDir, launchRelativeForThisOs(port.port));
    ctx.artefactPath = built.isEmpty()
                           ? QDir(in.buildDir).filePath(QFileInfo(launchRelativeForThisOs(port.port)).fileName())
                           : built;
    if (!built.isEmpty())
    {
        const QString installDir = EmulatorManager::installDir(port);
        const QString dest = QDir(installDir).filePath(launchRelativeForThisOs(port.port));
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QFile::remove(dest);
        if (QFile::copy(built, dest))
        {
            ctx.artefactPath = dest;
            // Everything that sat beside the binary in the build tree comes with it: a recomp ships shaders,
            // fonts and its own data files, and a lone .exe launches to a missing-asset error.
            const QDir from(QFileInfo(built).absolutePath());
            const QDir to(QFileInfo(dest).absolutePath());
            QDirIterator sib(from.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
            while (sib.hasNext())
            {
                const QString one = sib.next();
                if (one == built) continue;
                const QString rel = from.relativeFilePath(one);
                const QString target = to.filePath(rel);
                QDir().mkpath(QFileInfo(target).absolutePath());
                QFile::remove(target);
                QFile::copy(one, target);
            }
        }
    }
    // THE ARTEFACT CHECK, and on Windows this is the Defender case: every step exited 0 and the file the
    // linker wrote a moment ago is not there any more.
    applyArtefactCheck(m, QFileInfo::exists(ctx.artefactPath));
    publish(m, prog, runner.tail.lines());
    finish();
}
