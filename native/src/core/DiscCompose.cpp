#include "DiscCompose.h"
#include "DiscOverlay.h"
#include "RiivolutionPatch.h"
#include <QDir>
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

    if (ok)
    {
        ok = runTool(toolPath, {QStringLiteral("convert"), QStringLiteral("-i"), tree,
                                QStringLiteral("-o"), outputPath, QStringLiteral("-f"), QStringLiteral("rvz"),
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

    if (!ok)
    {
        // A partial image is worse than none: it would be found by the library scan and shown as playable.
        QFile::remove(outputPath);
        return out;
    }

    out.ok = true;
    out.outputPath = outputPath;
    return out;
}
