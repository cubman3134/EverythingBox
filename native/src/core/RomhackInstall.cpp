#include "RomhackInstall.h"

#include "RomPatch.h"

#include <QDir>
#include <QCryptographicHash>
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

bool romMatches(const QString& romPath, const QString& crc32Hex, const QString& sha1Hex)
{
    if (crc32Hex.trimmed().isEmpty() && sha1Hex.trimmed().isEmpty()) return false;

    QFile f(romPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray rom = f.readAll();
    if (rom.isEmpty()) return false;

    if (!sha1Hex.trimmed().isEmpty())
    {
        const QString have = QString::fromLatin1(
            QCryptographicHash::hash(rom, QCryptographicHash::Sha1).toHex());
        return have.compare(sha1Hex.trimmed(), Qt::CaseInsensitive) == 0;
    }
    // Zero-padded to eight digits: a checksum with a leading zero is still eight characters wide, and
    // formatting it shorter would never match the source's own spelling of it.
    const QString have = QStringLiteral("%1").arg(RomPatch::crc32(rom), 8, 16, QLatin1Char('0'));
    return have.compare(crc32Hex.trimmed(), Qt::CaseInsensitive) == 0;
}

QString destinationForRom(const QString& title, const QString& ext, const QString& targetDir,
                          const QString& variantName)
{
    const QString safe = sanitizeHackTitle(title);
    if (safe.isEmpty()) return QString();
    const QString clean = ext.startsWith(QLatin1Char('.')) ? ext.mid(1) : ext;

    QString stem = safe;
    if (!variantName.trimmed().isEmpty())
    {
        // Sanitised SEPARATELY and joined afterwards, never composed into `title` by the caller and passed as
        // one string: sanitizeHackTitle caps at kMaxTitleChars, and a long hack name would then eat the
        // variant off the END — putting both revisions back on one path, which is the exact failure this
        // argument exists to stop, and invisible because the result still looks like a reasonable name.
        const QString variant = sanitizeHackTitle(variantName);
        // A variant that survives sanitising as nothing is a refusal, not a fallback: falling back to `safe`
        // would hand back the colliding name the caller passed a variant to avoid.
        if (variant.isEmpty()) return QString();
        // The variant usually IS the hack's own published file name ("Hack v1.2 (USA)"), which already
        // carries the title — qualifying the title with it would read "Hack v1.2 (Hack v1.2 (USA))". So when
        // it already says the title it stands alone, and only a bare revision marker ("usa", "rev1")
        // qualifies the title. Either branch is a pure function of (title, variant), so a re-install still
        // computes the same path and the short-circuit that adopts an existing file stays honest.
        stem = variant.contains(safe, Qt::CaseInsensitive)
                   ? variant
                   : (safe + QStringLiteral(" (") + variant + QLatin1Char(')'));
    }
    const QString name = stem + (clean.isEmpty() ? QString() : (QLatin1Char('.') + clean));
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
