#include "DiscOverlay.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace
{
    // True when `child` is inside `root` once both are LEXICALLY resolved. Resolving ".." is the point:
    // "a/../../b" only escapes after it is resolved, so comparing the unresolved strings would accept it.
    // QDir::cleanPath does resolve it, and probe case 7 measures that.
    //
    // Two things this deliberately does NOT do, stated because the guarantee is weaker than "canonical":
    //
    //   absolutePath()+cleanPath() are pure string work and never touch the disk, so a SYMLINK is not
    //   followed -- QDir::canonicalPath() would follow it. A link inside the tree pointing out of it is
    //   therefore accepted. Taken here because both trees are ones this code just produced (DolphinTool's
    //   extraction output, and an archive we unpacked), so such a link could only arrive from the archive,
    //   and refusing hostile archive ENTRIES belongs in the extractor rather than in this check. Not
    //   measured: no probe case builds a symlink, since creating one needs privilege on Windows.
    //
    //   Qt::CaseInsensitive is right on NTFS, where "disc/x" and "DISC/x" are one file -- but on a
    //   CASE-SENSITIVE filesystem it is a false ACCEPT: "/tmp/DISC/x" counts as inside "/tmp/disc" while
    //   being a different directory. This app ships on Android TV, so that filesystem is real and not
    //   hypothetical. Left alone on purpose: changing it correctly means running it on that platform.
    bool contained(const QString& root, const QString& child)
    {
        const QString r = QDir::cleanPath(QDir(root).absolutePath()) + QLatin1Char('/');
        const QString c = QDir::cleanPath(QDir(child).absolutePath());
        return c.startsWith(r, Qt::CaseInsensitive);
    }

    bool copyOver(const QString& from, const QString& to, int* written, QString* error)
    {
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (QFile::exists(to) && !QFile::remove(to))
        {
            *error = QStringLiteral("could not replace %1").arg(to);
            return false;
        }
        if (!QFile::copy(from, to))
        {
            *error = QStringLiteral("could not write %1").arg(to);
            return false;
        }
        ++*written;
        return true;
    }
}

QString DiscOverlay::discFilesRoot(const QString& discRoot)
{
    // Decided by what the extraction produced rather than by the system we think we are patching: a Wii
    // disc extracts under DATA/, a GameCube disc does not.
    const QString wii = discRoot + QStringLiteral("/DATA/files");
    if (QFileInfo(wii).isDir()) return wii;
    return discRoot + QStringLiteral("/files");
}

DiscOverlay::Result DiscOverlay::apply(const QString& discRoot, const QString& modRoot,
                                       const RiivolutionPatch::Parsed& parsed)
{
    Result out;

    // A document the parser REFUSED must not be applied here either. The refusal is the feature -- see
    // RiivolutionPatch.h -- and a caller that forgot to check would otherwise get a silently mis-patched
    // disc rather than an error.
    if (!parsed.ok)
    {
        out.error = parsed.refusal;
        return out;
    }

    const QString filesRoot = discFilesRoot(discRoot);
    if (!QFileInfo(filesRoot).isDir())
    {
        out.error = QStringLiteral("the extracted disc has no files directory at %1").arg(filesRoot);
        return out;
    }

    QString patchRoot = modRoot;
    if (!parsed.root.isEmpty())
        patchRoot = modRoot + QLatin1Char('/') + parsed.root.mid(parsed.root.startsWith(QLatin1Char('/')) ? 1 : 0);

    // op.create is parsed and deliberately NOT consulted here. In Riivolution it says whether the loader may
    // invent a disc entry the stock game does not have; composing a whole disc, we always can, so there is
    // no case in which create="false" would make us do something different. Named rather than left as a
    // quietly unread field, following the rule RiivolutionPatch.h sets for what it cannot honour. Not
    // measured: no probe case separates create="true" from create="false", because nothing here diverges.
    for (const auto& op : parsed.ops)
    {
        const QString src = patchRoot + QLatin1Char('/') + op.externalPath;
        const QString dst = filesRoot + QLatin1Char('/')
                            + op.discPath.mid(op.discPath.startsWith(QLatin1Char('/')) ? 1 : 0);

        // Both ends are checked. The source comes out of an archive we did not build and the destination
        // is built from an attribute in that same archive, so either can be shaped to escape. Each half is
        // pinned separately -- probe case 7 escapes on the disc end, case 13 on the source end -- because
        // deleting either half alone leaves the other half's case green.
        if (!contained(modRoot, src) || !contained(filesRoot, dst))
        {
            out.error = QStringLiteral("this mod tries to write outside the game's own files (%1)")
                            .arg(op.discPath);
            return out;
        }

        if (!QFileInfo(src).exists())
        {
            // Not an error: a document may map a folder the distribution does not ship.
            continue;
        }

        if (op.kind == RiivolutionPatch::Op::File)
        {
            if (!copyOver(src, dst, &out.filesWritten, &out.error)) return out;
            continue;
        }

        QDirIterator it(src, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString one = it.next();
            const QString rel = QDir(src).relativeFilePath(one);
            const QString target = dst + QLatin1Char('/') + rel;
            if (!contained(filesRoot, target))
            {
                out.error = QStringLiteral("this mod tries to write outside the game's own files (%1)").arg(rel);
                return out;
            }
            if (!copyOver(one, target, &out.filesWritten, &out.error)) return out;
        }
    }

    out.ok = true;
    return out;
}
