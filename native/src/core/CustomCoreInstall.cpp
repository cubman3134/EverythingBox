#include "CustomCoreInstall.h"
#include "../libretro/CoreInspect.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

QString CustomCoreInstall::librarySuffix()
{
#if defined(Q_OS_WIN)
    return QStringLiteral(".dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral(".dylib");
#else
    return QStringLiteral(".so");
#endif
}

QString CustomCoreInstall::idFor(const QString& libraryName, const QString& filePath)
{
    QString id = CustomCores::sanitizeId(libraryName);
    if (!id.isEmpty()) return id;

    QString stem = QFileInfo(filePath).completeBaseName();
    // "fceumm_libretro" / "mgba_libretro_android" -> "fceumm" / "mgba". The marker is the buildbot's packaging
    // convention, not part of the core's name, and leaving it in would make every id read the same.
    const int marker = stem.indexOf(QStringLiteral("_libretro"), 0, Qt::CaseInsensitive);
    if (marker > 0) stem = stem.left(marker);
    return CustomCores::sanitizeId(stem);
}

bool CustomCoreInstall::loadFromFile(const QString& file, CustomCore* out, QString* error)
{
    const QFileInfo fi(file);
    if (file.trimmed().isEmpty() || !fi.exists() || !fi.isFile())
    {
        if (error) *error = QCoreApplication::translate("CustomCoreInstall", "There is no file at %1.").arg(file);
        return false;
    }

    // 1. ASK THE FILE WHAT IT IS, BEFORE COPYING IT ANYWHERE.
    CoreInspection info;
    if (!CoreInspect::inspect(fi.absoluteFilePath(), &info, error))
        return false;

    // 2. The id comes from the core's own name, so the same core is one registration however it was reached.
    const QString id = idFor(info.libraryName, fi.absoluteFilePath());
    if (id.isEmpty())
    {
        if (error) *error = QCoreApplication::translate(
                       "CustomCoreInstall", "%1 doesn't report a name, and its file name gives nothing usable to "
                                            "register it under.").arg(fi.fileName());
        return false;
    }

    // 3. Copy it in, unless it is already there. QFileInfo::canonicalFilePath resolves symlinks and case on the
    // platforms that need it, so a file already inside the folder is recognised as such and not copied onto
    // itself (which would truncate it).
    const QDir customDir(CustomCores::customDir());
    QString finalPath = fi.absoluteFilePath();
    const QString alreadyInside = QFileInfo(customDir.absolutePath()).canonicalFilePath();
    const QString parentOfFile  = QFileInfo(fi.absolutePath()).canonicalFilePath();
    if (alreadyInside.isEmpty() || parentOfFile != alreadyInside)
    {
        const QString dest = customDir.absoluteFilePath(fi.fileName());
        if (QFile::exists(dest) && !QFile::remove(dest))
        {
            if (error) *error = QCoreApplication::translate(
                           "CustomCoreInstall", "Couldn't replace the existing copy at %1.").arg(dest);
            return false;
        }
        if (!QFile::copy(fi.absoluteFilePath(), dest))
        {
            if (error) *error = QCoreApplication::translate(
                           "CustomCoreInstall", "Couldn't copy the core into %1.").arg(customDir.absolutePath());
            return false;
        }
        finalPath = dest;
    }

    // 4. Register (or update).
    CustomCore rec;
    rec.id             = id;
    rec.path           = QDir::toNativeSeparators(finalPath);
    rec.name           = info.libraryName.isEmpty() ? id : info.libraryName;
    rec.version        = info.libraryVersion;
    rec.extensions     = info.extensions;
    rec.supportsNoGame = info.supportsNoGame;
    rec.needFullpath   = info.needFullpath;
    rec.needs          = CoreInspect::unmetSentence(info);
    rec.addedAt        = QDateTime::currentMSecsSinceEpoch();
    if (!CustomCores::add(rec, error))
        return false;
    if (out) *out = rec;
    return true;
}

QStringList CustomCoreInstall::unregisteredInCustomDir()
{
    QStringList out;
    const QDir d(CustomCores::customDir());
    if (!d.exists()) return out;

    QStringList known;
    for (const CustomCore& c : CustomCores::all())
        known << QFileInfo(c.path).absoluteFilePath();

    const QFileInfoList files = d.entryInfoList(QStringList{ QStringLiteral("*") + librarySuffix() },
                                                QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fi : files)
        if (!known.contains(fi.absoluteFilePath()))
            out << fi.absoluteFilePath();
    return out;
}
