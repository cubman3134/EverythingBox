#include "DiscCompose.h"
#include "DiscOverlay.h"
#include "RiivolutionPatch.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>

namespace
{
    // The tool is given a generous ceiling rather than none: composing a disc is minutes of work, but a
    // process that has stopped making progress must not hold the install open forever.
    constexpr int kToolTimeoutMs = 45 * 60 * 1000;

    // How long the tool wait blocks before looking at the interruption flag again. Small enough that a
    // cancel is acted on promptly, large enough that the poll costs nothing over a multi-minute convert.
    constexpr int kPollSliceMs = 200;

    // Has the thread running this compose been asked to stop? The flag is per-thread, so on the GUI thread
    // or a probe's main thread -- where nothing ever calls requestInterruption() -- this is always false.
    bool aborted()
    {
        return QThread::currentThread()->isInterruptionRequested();
    }

    bool runTool(const QString& tool, const QStringList& args, QString* error)
    {
        if (aborted()) { *error = DiscCompose::cancelledMessage(); return false; }

        QProcess p;
        p.start(tool, args);
        if (!p.waitForStarted(30000))
        {
            *error = QStringLiteral("the disc tool could not be started");
            return false;
        }

        // Sliced, rather than one waitForFinished(kToolTimeoutMs). A single 45-minute wait cannot notice an
        // interruption request, so a cancel during the convert would be a cancel the caller had to wait out
        // -- and abandoning the wait without ending the child would leave a disc tool running against a
        // staging tree we were about to delete underneath it. So each slice ends the wait, checks the flag,
        // and on a cancel KILLS the process and joins it before returning; only then does the caller's
        // cleanup remove the tree and the ".part" the tool was writing into.
        //
        // The elapsed clock is kept separately because the ceiling has to span the whole wait, not one
        // slice. terminate() is deliberately not tried first: the tool is a batch converter with no message
        // loop and nothing to flush -- its output is the ".part" file, which is removed either way.
        //
        // The loop condition covers the case where waitForFinished() returns false for a process that has
        // ALREADY exited (Qt reports "not running" as a failed wait), which would otherwise spin here.
        QElapsedTimer clock;
        clock.start();
        while (p.state() != QProcess::NotRunning)
        {
            if (p.waitForFinished(kPollSliceMs)) break;
            if (aborted())
            {
                p.kill();
                p.waitForFinished(5000);
                *error = DiscCompose::cancelledMessage();
                return false;
            }
            if (clock.elapsed() >= kToolTimeoutMs)
            {
                p.kill();
                p.waitForFinished(5000);
                *error = QStringLiteral("the disc tool stopped responding");
                return false;
            }
        }
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        {
            // The tool reports its own reason on stderr; carrying it through beats inventing one.
            const QString said = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
            *error = said.isEmpty() ? QStringLiteral("the disc tool failed") : said;
            return false;
        }
        return true;
    }
}

QString DiscCompose::cancelledMessage()
{
    return QStringLiteral("the build was cancelled");
}

qint64 DiscCompose::requiredFreeBytes(qint64 discBytes)
{
    // Extracted tree (about one disc) + composed image (at most one disc) + headroom. Deliberately a
    // multiple: a Wii disc is roughly an order of magnitude larger than a GameCube one, so a fixed figure
    // would be far too small for one and absurd for the other.
    return discBytes * 2 + (512LL * 1024 * 1024);
}

bool DiscCompose::hasFreeSpace(const QString& dir, qint64 needed)
{
    const QStorageInfo info(dir);
    if (!info.isValid() || !info.isReady()) return false;
    return info.bytesAvailable() >= needed;
}

DiscCompose::Outcome DiscCompose::composePatchedDisc(const QString& toolPath, const QString& discPath,
                                                     const QString& modRoot, const QByteArray& riivolutionXml,
                                                     const QString& outputPath, const QString& stagingParent)
{
    Outcome out;

    // The document is read FIRST, before any disc is extracted. A refusal must cost nothing: extracting a
    // 4 GB disc and then discovering the mod cannot be composed would be minutes of work thrown away.
    const auto parsed = RiivolutionPatch::parse(riivolutionXml);
    if (!parsed.ok)
    {
        out.error = parsed.refusal;
        return out;
    }

    // Already cancelled before any of this began -- the worker was interrupted between being started and
    // getting here. Checked BEFORE the staging directory is made, so this path has nothing to clean up: the
    // cleanup guarantee below covers cancels that arrive once there IS something on disk.
    if (aborted())
    {
        out.error = cancelledMessage();
        return out;
    }

    const qint64 discBytes = QFileInfo(discPath).size();
    const qint64 needed = requiredFreeBytes(discBytes);
    if (!hasFreeSpace(stagingParent, needed))
    {
        out.error = QStringLiteral("there is not enough free space to build this hack — it needs about "
                                   "%1 GB free").arg(needed / (1024.0 * 1024 * 1024), 0, 'f', 1);
        return out;
    }

    const QString staging = QDir(stagingParent).absoluteFilePath(
        QStringLiteral("disc-compose-") + QUuid::createUuid().toString(QUuid::Id128));
    if (!QDir().mkpath(staging))
    {
        out.error = QStringLiteral("could not create a working folder to build this hack in");
        return out;
    }

    const QString tree = staging + QStringLiteral("/tree");
    bool ok = runTool(toolPath, {QStringLiteral("extract"), QStringLiteral("-i"), discPath,
                                 QStringLiteral("-o"), tree, QStringLiteral("-g"), QStringLiteral("-q")},
                      &out.error);

    if (ok)
    {
        const auto overlaid = DiscOverlay::apply(tree, modRoot, parsed);
        ok = overlaid.ok;
        if (!ok) out.error = overlaid.error;

        // AN OVERLAY THAT CHANGED NOTHING IS A FAILURE, not a success with nothing to show. apply() treats a
        // mapping whose source is absent as "not an error" -- a document may map a folder the distribution
        // does not ship -- and that is right per op and catastrophic for ALL of them: every op falling through
        // that skip returns ok with filesWritten == 0, and the convert below would then compose the base disc,
        // unmodified, and install it under the hack's name. A vanilla game presented as the mod is the worst
        // outcome available here; it is the reason <memory> is refused rather than ignored, and refusing it
        // has to be the same kind of decision.
        //
        // MEASURED as reachable, not hypothetical: an archive with a single top-level wrapper folder puts the
        // whole tree one level below where an un-anchored modRoot points, so every source path misses. That
        // anchoring is fixed too (DiscOverlay::modRootForXml) -- this guard is the backstop for the layouts
        // neither of us has seen, and it is what the probe's wrapper case pins.
        else if (overlaid.filesWritten == 0)
        {
            ok = false;
            out.error = QStringLiteral("none of this mod's files were found in its download");
        }
    }

    // Composed under a ".part" name and renamed only on success. The tool writes its output incrementally
    // over MINUTES on a disc-sized input, and outputPath is inside the ROMs folder, so handing it the final
    // name puts a growing, truncated image where the library scan can find it and offer it as a playable
    // game -- the one outcome DiscCompose.h says must never happen. Staging already honours that guarantee;
    // the output path did not.
    //
    // ".part" specifically, rather than any invented suffix: RomRouting.h's isLibraryJunkExtension() lists
    // "part" among the junk extensions, so the scan already ignores it -- that entry exists because
    // DownloadManager streams into "<dest>.part" and renames on success, which is the same shape as this.
    // A different suffix would be scanned.
    //
    // Composing onto the staging volume and copying would also work and is NOT done: the image is
    // disc-sized and staging is deliberately on a different volume, so that is a gigabyte-scale copy. A
    // rename within one directory is atomic and free.
    //
    // PARTLY MEASURED now, and the split matters. probe_riivolution cases 16 and 17 point toolPath at the
    // probe's own binary running as a stub tool, so the convert really is started as a child process and
    // the ".part" really is written; case 16 waits for that file to appear, cancels, and asserts the ".part"
    // is gone, the staging tree is gone, and an already-installed outputPath is byte-unchanged. Deleting
    // the ".part" removal below fails exactly one of those assertions, and deleting the staging removal
    // exactly one other -- both measured by running the mutant, not reasoned.
    //
    // The SUCCESS path is measured now too, which it was not: case 20 runs the stub in its finishing mode, so
    // the convert exits 0 and the promotion below really executes over an already-installed image. Both of its
    // halves are pinned separately -- skipping the rename fails three of that case's assertions, skipping the
    // pre-remove fails three others, and the two sets share exactly one, which is what says they are pinned
    // and not merely covered. Worth naming: skipping the rename leaves the Outcome saying ok, so it is caught
    // only by reading the filesystem.
    //
    // Case 18 pins the other side of "ok": an overlay that matched nothing must not reach here at all,
    // asserted on the stub's tool log rather than on the outcome, because a log line is the only thing that
    // can distinguish "the convert never started" from "the convert started and was refused afterwards".
    //
    // What is STILL unmeasured: no probe reads or writes a real disc image, and the stub knows nothing about
    // disc formats. Nothing here says a composed disc BOOTS -- only that the file bookkeeping around it is
    // right, and that the stages ran in the order and the number claimed.
    const QString partPath = outputPath + QStringLiteral(".part");

    if (ok)
    {
        // An earlier run killed mid-convert leaves its .part behind -- the scan ignores it, but the tool
        // would be writing over a file it did not create. Cleared here so a retry starts from nothing.
        QFile::remove(partPath);

        ok = runTool(toolPath, {QStringLiteral("convert"), QStringLiteral("-i"), tree,
                                QStringLiteral("-o"), partPath, QStringLiteral("-f"), QStringLiteral("rvz"),
                                QStringLiteral("-c"), QStringLiteral("zstd"),
                                // -l is REQUIRED whenever -c is not "none": without it the tool exits
                                // with "Compression level must be set when compression type is not
                                // 'none'". Measured in Task 1; the stock tool never reached this check
                                // because it refused the directory first, so the omission was invisible.
                                QStringLiteral("-l"), QStringLiteral("5"),
                                QStringLiteral("-b"), QStringLiteral("131072")},
                     &out.error);
    }

    // Cleaned on every path, success or failure. The staging tree is disc-sized; leaving one behind after a
    // failed install would quietly consume the space the next attempt needs.
    QDir(staging).removeRecursively();

    if (ok)
    {
        // Promotion. QFile::rename will not overwrite, so an existing image at the destination is removed
        // first -- the caller may be rebuilding a hack it installed before. Only reached once the tool has
        // exited 0, so the file being replaced is only ever replaced by a complete one.
        if (QFile::exists(outputPath) && !QFile::remove(outputPath))
        {
            out.error = QStringLiteral("could not replace the existing image at %1").arg(outputPath);
            ok = false;
        }
        else if (!QFile::rename(partPath, outputPath))
        {
            out.error = QStringLiteral("the hack was built but could not be moved into place");
            ok = false;
        }
    }

    if (!ok)
    {
        // A partial image is worse than none: while it carries the ".part" name the scan ignores it, but
        // leaving one behind would consume disc-sized space and be written over by the next attempt rather
        // than replaced cleanly. Removed on every failing path, including a promotion that did not happen.
        //
        // outputPath itself is deliberately NOT removed here any more. It used to be, when the tool wrote
        // straight to it; now nothing has touched it on any failing path, so removing it would delete
        // whatever was already there -- for a rebuild of an installed hack, the working image.
        QFile::remove(partPath);
        return out;
    }

    out.ok = true;
    out.outputPath = outputPath;
    return out;
}
