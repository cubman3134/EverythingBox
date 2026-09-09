// The child process, run (issue #248, increment c). See RecompBuildRunner.h for why this is blocking.
#include "RecompBuildRunner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace recompbuild {

void Runner::writeLog(const QString& text)
{
    if (logFilePath.isEmpty() || logCapped_) return;
    QFile f(logFilePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const QByteArray bytes = text.toUtf8();
    if (logBytes_ + bytes.size() > kMaxLogBytes)
    {
        // ONE final line and then nothing. The alternative — letting it grow — turns a build that emits a
        // warning per template instantiation into a disk-filling operation the user never agreed to, and the
        // alternative to THAT (silently dropping the rest) leaves a truncated log that reads as a build which
        // stopped for no reason.
        f.write(QStringLiteral("\n[EverythingBox] this log reached its size limit (%1 MB) and stops here. The "
                               "build itself carried on.\n")
                    .arg(kMaxLogBytes / (1024 * 1024))
                    .toUtf8());
        logCapped_ = true;
        return;
    }
    logBytes_ += f.write(bytes);
}

StepOutcome Runner::runStep(const Step& step)
{
    StepOutcome out;
    QElapsedTimer clock;
    clock.start();

    // A step's program is always an ABSOLUTE path that the plan resolved — never a bare name. Resolving
    // "cmake" here would mean a build that quietly used a different CMake from the one the detector reported
    // to the user, and on Windows it would mean whatever a hostile directory on PATH called cmake.exe.
    if (step.program.trimmed().isEmpty() || !QFileInfo::exists(step.program))
    {
        writeLog(QStringLiteral("\n[EverythingBox] cannot run: %1\n").arg(step.program));
        out.elapsedMs = clock.elapsed();
        return out;   // started == false
    }
    if (!step.workDir.isEmpty()) QDir().mkpath(step.workDir);

    QProcess p;
    p.setProgram(step.program);
    p.setArguments(step.args);
    if (!step.workDir.isEmpty()) p.setWorkingDirectory(step.workDir);
    // Merged, because a compiler's errors and its progress are one narrative and interleaving them the way
    // the terminal would is what makes the tail readable.
    p.setProcessChannelMode(QProcess::MergedChannels);
    // Nothing to type at. A build tool that decides to prompt would otherwise hold the whole build open with
    // nobody able to see the question.
    p.setStandardInputFile(QProcess::nullDevice());

    // The command line goes into the log and NOT into the tail: it is the first thing anybody opening the log
    // wants and the last thing worth one of twenty-four lines on screen.
    writeLog(QStringLiteral("\n[EverythingBox] %1\n[EverythingBox] > %2 %3\n")
                 .arg(step.label, step.program, step.args.join(QLatin1Char(' '))));

    p.start();
    if (!p.waitForStarted(15000))
    {
        p.kill();
        p.waitForFinished(1000);
        writeLog(QStringLiteral("[EverythingBox] it would not start\n"));
        out.elapsedMs = clock.elapsed();
        return out;
    }
    out.started = true;

    auto drain = [&]() {
        const QByteArray chunk = p.readAll();
        if (chunk.isEmpty()) return;
        const QString text = QString::fromLocal8Bit(chunk);
        writeLog(text);
        const QStringList fresh = tail.append(text);
        if (onLine) for (const QString& l : fresh) onLine(l);
    };

    int lastPercent = -1;
    auto report = [&]() {
        if (!onProgress) return;
        Progress pr;
        pr.phase = step.phase;
        pr.elapsedMs = clock.elapsed();
        pr.lastLine = tail.last();
        const int pct = parsePercent(pr.lastLine);
        // A percent that has been seen once is kept: build drivers interleave percent-prefixed lines with
        // plain ones, and a bar that fell back to "unknown" every other line would read as broken.
        if (pct >= 0) lastPercent = pct;
        pr.percent = lastPercent;
        onProgress(pr);
    };

    bool killed = false;
    while (p.state() != QProcess::NotRunning)
    {
        // waitForReadyRead's timeout is the poll interval too: a silent compile still comes round here five
        // times a second, which is what keeps the elapsed clock on screen moving and stops a long link from
        // looking like a dead build.
        if (p.waitForReadyRead(200)) drain();
        if (!killed && cancelFlag && cancelFlag->loadAcquire() != 0)
        {
            killed = true;
            out.cancelled = true;
            writeLog(QStringLiteral("\n[EverythingBox] cancelled — stopping %1\n").arg(step.program));
            p.terminate();
            if (!p.waitForFinished(graceMs))
            {
                p.kill();
                p.waitForFinished(2000);
            }
            break;
        }
        report();
    }
    if (!killed) p.waitForFinished(2000);
    drain();
    {
        const QStringList fresh = tail.flush();
        if (onLine) for (const QString& l : fresh) onLine(l);
    }
    report();

    out.crashed = (p.exitStatus() != QProcess::NormalExit) || killed;
    out.exitCode = p.exitCode();
    out.elapsedMs = clock.elapsed();
    writeLog(QStringLiteral("\n[EverythingBox] %1 after %2 ms (exit %3%4)\n")
                 .arg(out.cancelled ? QStringLiteral("cancelled") : QStringLiteral("finished"))
                 .arg(out.elapsedMs)
                 .arg(out.exitCode)
                 .arg(out.crashed && !out.cancelled ? QStringLiteral(", did not exit normally") : QString()));
    return out;
}

}  // namespace recompbuild
