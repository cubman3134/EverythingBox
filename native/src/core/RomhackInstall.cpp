#include "RomhackInstall.h"

#include "RomPatch.h"

#include <QDir>
#include <QFile>
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

QString destinationForRom(const QString& title, const QString& ext, const QString& targetDir)
{
    const QString safe = sanitizeHackTitle(title);
    if (safe.isEmpty()) return QString();
    const QString clean = ext.startsWith(QLatin1Char('.')) ? ext.mid(1) : ext;
    const QString name = safe + (clean.isEmpty() ? QString() : (QLatin1Char('.') + clean));
    return QDir(targetDir).absoluteFilePath(name);
}

QString installRom(const QByteArray& rom, const QString& title, const QString& ext,
                   const QString& targetDir, QString* error)
{
    if (rom.isEmpty())
    {
        if (error) *error = QObject::tr("That download was empty.");
        return QString();
    }
    const QString dest = destinationForRom(title, ext, targetDir);
    if (dest.isEmpty())
    {
        if (error) *error = QObject::tr("That hack's name can't be used as a file name.");
        return QString();
    }

    // Written through a sibling ".part" and renamed, exactly as the patch path does: a half-written file
    // that already carries the final name is a library entry that looks playable and is not.
    const QString part = dest + QStringLiteral(".part");
    QFile out(part);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (error) *error = QObject::tr("Couldn't write to your ROMs folder.");
        return QString();
    }
    const qint64 written = out.write(rom);
    out.close();
    if (written != rom.size())
    {
        QFile::remove(part);
        if (error) *error = QObject::tr("Couldn't write the whole file — is the disk full?");
        return QString();
    }
    QFile::remove(dest);                      // a re-install replaces, so this stays idempotent
    if (!QFile::rename(part, dest))
    {
        QFile::remove(part);
        if (error) *error = QObject::tr("Couldn't put the file into your ROMs folder.");
        return QString();
    }
    return dest;
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
