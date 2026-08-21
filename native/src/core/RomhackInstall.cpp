#include "RomhackInstall.h"

#include "RomPatch.h"

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>

namespace RomhackInstall
{

// Longest hack title we keep. Hack names run long ("Final Fantasy VI: Ted Woolsey Uncensored Edition"), and a
// path that overruns MAX_PATH fails at write time with an error nobody can act on. 80 leaves room for the base
// name, the per-system folder and the extension inside a normal ROMs root.
static constexpr int kMaxTitleChars = 80;

QString sanitizeHackTitle(const QString& title)
{
    // Everything Windows reserves in a file name, plus the path separators, plus control characters. Replaced
    // with a space rather than removed so "Zelda:Parallel" reads as "Zelda Parallel", not "ZeldaParallel".
    static const QRegularExpression bad(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    QString s = title;
    s.replace(bad, QStringLiteral(" "));
    s.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    s = s.trimmed();
    if (s.size() > kMaxTitleChars) s = s.left(kMaxTitleChars).trimmed();
    // A trailing dot or space is legal to construct and impossible to open on Windows.
    while (!s.isEmpty() && (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' ')))) s.chop(1);
    return s;
}

QString destinationFor(const QString& baseRomPath, const QString& hackTitle, const QString& targetDir,
                       const QString& baseNameOverride)
{
    const QString safe = sanitizeHackTitle(hackTitle);
    if (safe.isEmpty()) return QString();

    const QFileInfo fi(baseRomPath);
    // The override is sanitised too: it comes from a library title, which can carry the same characters a
    // hack name can.
    const QString base = baseNameOverride.trimmed().isEmpty() ? fi.completeBaseName()
                                                              : sanitizeHackTitle(baseNameOverride);
    if (base.isEmpty()) return QString();
    const QString ext = fi.suffix();
    const QString name = base + QStringLiteral(" (") + safe + QLatin1Char(')')
                       + (ext.isEmpty() ? QString() : (QLatin1Char('.') + ext));
    return QDir(targetDir).absoluteFilePath(name);
}

QString install(const QString& baseRomPath, const QByteArray& patch, const QString& hackTitle,
                const QString& targetDir, QString* error, const QString& baseNameOverride)
{
    if (!QFileInfo::exists(baseRomPath))
    {
        if (error) *error = QObject::tr("The base game is missing.");
        return QString();
    }
    if (patch.isEmpty())
    {
        if (error) *error = QObject::tr("The patch is empty.");
        return QString();
    }

    const QString dest = destinationFor(baseRomPath, hackTitle, targetDir, baseNameOverride);
    if (dest.isEmpty())
    {
        if (error) *error = QObject::tr("That hack's name can't be used as a file name.");
        return QString();
    }
    // Refuse to write over the game we are patching FROM. A hack title that sanitises into the base name
    // would otherwise destroy the original, which is the one thing this feature promises never to do.
    if (QFileInfo(dest).absoluteFilePath() == QFileInfo(baseRomPath).absoluteFilePath())
    {
        if (error) *error = QObject::tr("That hack would overwrite the original game.");
        return QString();
    }

    QString werr;
    if (!RomPatch::writePatched(baseRomPath, patch, dest, &werr))
    {
        if (error) *error = werr;
        return QString();
    }
    return dest;
}

} // namespace RomhackInstall
