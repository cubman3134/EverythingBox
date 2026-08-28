#include "DiscOverlay.h"
#include "DiscCompose.h"   // cancelledMessage() only -- see the note on aborted() below
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QThread>

namespace
{
    // True when `child` is inside `root` once both are LEXICALLY resolved. Resolving ".." is the point:
    // "a/../../b" only escapes after it is resolved, so comparing the unresolved strings would accept it.
    // QDir::cleanPath does resolve it, and probe cases 7 and 15 measure that.
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

    // The overlay is the one stage of a compose that is neither a subprocess nor instant: a large mod
    // replaces thousands of files, and each copy is disc-scale I/O. Run on a worker (which is how
    // DiscCompose calls it), it therefore has to answer an interruption request too, or a cancel would sit
    // unheard until the copies finished. The flag is per-thread, so on the GUI thread or a probe's main
    // thread -- neither of which ever calls requestInterruption() -- this is always false and every
    // existing caller behaves exactly as before.
    //
    // The message comes from DiscCompose rather than being spelled again here: a cancelled overlay and a
    // cancelled convert are one event to the person reading it, and two copies of a user-facing sentence
    // is how they drift apart. DiscCompose.h includes nothing of ours, so this is not a cycle, and the two
    // units are compiled together everywhere either is used.
    bool aborted()
    {
        return QThread::currentThread()->isInterruptionRequested();
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

QString DiscOverlay::modRootForXml(const QString& payloadDir, const QString& xmlPath)
{
    const QString payload = QDir::cleanPath(QDir(payloadDir).absolutePath());
    if (xmlPath.isEmpty()) return payload;

    // Grandparent: the document's directory (`riivolution/`), then ITS directory -- the sd-card root the
    // format positions everything from. Pure string work on absolute paths, so a relative xmlPath resolves
    // against the process's cwd exactly as every other path in this file does.
    const QString docDir = QFileInfo(xmlPath).absolutePath();
    const QString grand  = QDir::cleanPath(QFileInfo(docDir).absolutePath());

    // Equal is the FLAT archive -- `<payload>/riivolution/x.xml` -- and returning `payload` for it is what
    // keeps that layout behaving exactly as it did before this function existed.
    if (grand == payload) return payload;

    // Outside the payload, or not a directory at all: fall back rather than anchor somewhere this code did
    // not unpack. `contained` is the same lexical check apply() uses, with the same stated limits.
    if (!contained(payload, grand) || !QFileInfo(grand).isDir()) return payload;
    return grand;
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
        // Per OP, and again per FILE below. A single folder op can be the whole mod, so checking only
        // between ops would leave the longest stretch of the copy unable to hear a cancel.
        if (aborted()) { out.error = DiscCompose::cancelledMessage(); return out; }

        const QString src = patchRoot + QLatin1Char('/') + op.externalPath;
        const QString dst = filesRoot + QLatin1Char('/')
                            + op.discPath.mid(op.discPath.startsWith(QLatin1Char('/')) ? 1 : 0);

        // Both ends are checked. The source comes out of an archive we did not build and the destination
        // is built from an attribute in that same archive, so either can be shaped to escape. Each half is
        // pinned separately, because deleting either half alone leaves the other half's case green:
        // case 13 escapes on the source end, case 15 on the disc end.
        //
        // Case 15 is a FILE op specifically, and that is not incidental. Case 7 also escapes on the disc
        // end, but it is a FOLDER op, so the per-file check in the loop below refuses it even with this
        // term gone -- measured: deleting `|| !contained(filesRoot, dst)` alone fails case 15 only, and the
        // whole suite was green before case 15 existed. For a File op there is no loop, so this term is the
        // only guard. The loop's check is therefore unreachable while this one stands -- not dead code, but
        // the thing that hid this term's removal.
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
            if (aborted()) { out.error = DiscCompose::cancelledMessage(); return out; }

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
