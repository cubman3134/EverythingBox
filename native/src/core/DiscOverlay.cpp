#include "DiscOverlay.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace
{
    // True when `child` is inside `root` once both are resolved. Resolution is the point: "a/../../b" only
    // escapes after it is resolved, so comparing the unresolved strings would accept it.
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
    const QString filesRoot = discFilesRoot(discRoot);
    if (!QFileInfo(filesRoot).isDir())
    {
        out.error = QStringLiteral("the extracted disc has no files directory at %1").arg(filesRoot);
        return out;
    }

    QString patchRoot = modRoot;
    if (!parsed.root.isEmpty())
        patchRoot = modRoot + QLatin1Char('/') + parsed.root.mid(parsed.root.startsWith(QLatin1Char('/')) ? 1 : 0);

    for (const auto& op : parsed.ops)
    {
        const QString src = patchRoot + QLatin1Char('/') + op.externalPath;
        const QString dst = filesRoot + QLatin1Char('/')
                            + op.discPath.mid(op.discPath.startsWith(QLatin1Char('/')) ? 1 : 0);

        // Both ends are checked. The source comes out of an archive we did not build and the destination
        // is built from an attribute in that same archive, so either can be shaped to escape.
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
