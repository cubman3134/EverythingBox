// Zip-slip / path-traversal guard shared by the two whole-archive extractors — ArchiveRom's miniz .zip
// loop and SevenZip::extractAllToDir. A member's name inside an archive is attacker-controlled: a
// malicious archive can name an entry "../../evil.exe", "/etc/passwd", "C:\\Windows\\system32\\x" or a
// UNC "\\\\host\\share\\x", so a naive `destDir + "/" + name` writes OUTSIDE destDir. This became a live
// exposure once user/content-server-fetched disc archives (.zip/.7z) started routing through extractAll
// (the multi-file disc-image fix); before that only the self-update path fed it archives we produce.
//
// Every extracted member's destination MUST pass through join(): it resolves the name under destDir and
// refuses anything that escapes. Returns the safe absolute output path, or an empty QString when the
// member must be rejected — the callers treat an empty result as an unsafe archive and abort extraction.
#pragma once
#include <QString>
#include <QDir>

namespace ArchiveSafePath
{
    // Resolve `memberName` under `destDir`, or return an empty QString if it would escape destDir. Handles
    // both '/' and '\' separators, "." / ".." segments, and absolute / drive-letter / UNC member names.
    inline QString join(const QString& destDir, const QString& memberName)
    {
        // Normalise separators first so the "..", absolute and prefix checks below all see one form. A
        // member can carry either separator regardless of the host OS (a Windows-made archive uses '\',
        // a Unix-made one '/'), and QDir::cleanPath only collapses '/'.
        QString name = memberName;
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));

        // An ABSOLUTE member is never joined under destDir — reject it by name, before QDir can resolve it
        // to a filesystem root or another drive. POSIX-absolute "/x" and UNC "//host/share/x" both begin
        // with '/'. A Windows drive spec is "C:/x", "C:x" or a bare "C:" (second char ':'); note that
        // QDir::cleanPath("<base>/C:/x") keeps the "C:" and so would escape, which is exactly why the drive
        // form has to be caught here rather than left to the resolved-prefix test below.
        if (name.isEmpty())
            return QString();
        if (name.startsWith(QLatin1Char('/')))
            return QString();
        if (name.size() >= 2 && name.at(1) == QLatin1Char(':'))
            return QString();

        // Resolve "." / ".." against an absolute, cleaned base, then require the result to be STRICTLY
        // under it. The base itself (a member that resolves to destDir with nothing to write) is rejected.
        // Appending '/' to baseAbs before startsWith stops a sibling like "<base>-evil" from matching as
        // though it were inside "<base>". Both strings share the same baseAbs prefix verbatim, so the
        // comparison is exact on every platform (no case-folding needed — joined is built from baseAbs).
        const QString baseAbs = QDir::cleanPath(QDir(destDir).absolutePath());
        const QString joined  = QDir::cleanPath(baseAbs + QLatin1Char('/') + name);
        if (joined == baseAbs || !joined.startsWith(baseAbs + QLatin1Char('/')))
            return QString();
        return joined;
    }
}
