#include "DiscCompose.h"
#include "DiscOverlay.h"
#include "RiivolutionPatch.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStorageInfo>
#include <QUuid>

namespace
{
    // The tool is given a generous ceiling rather than none: composing a disc is minutes of work, but a
    // process that has stopped making progress must not hold the install open forever.
    constexpr int kToolTimeoutMs = 45 * 60 * 1000;

    bool runTool(const QString& tool, const QStringList& args, QString* error)
    {
        QProcess p;
        p.start(tool, args);
        if (!p.waitForStarted(30000))
        {
            *error = QStringLiteral("the disc tool could not be started");
            return false;
        }
        if (!p.waitForFinished(kToolTimeoutMs))
        {
            p.kill();
            p.waitForFinished(5000);
            *error = QStringLiteral("the disc tool stopped responding");
            return false;
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
    // NOT MEASURED, and worth knowing before trusting it: no probe covers any of this. probe_riivolution is
    // the only test that calls composePatchedDisc, and both of its calls refuse at the parse, before the
    // tool is ever started -- so no .part file can exist in any test, and an assertion about one could not
    // fail. Everything below the parse (the convert, the promotion, the cleanup) is reasoned, not run.
    // Pinning it needs a stub tool the probe can point toolPath at, which does not exist yet.
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
