// The file half of #248 (d): the stamp a build writes about itself, and the kept copy of the build that
// worked. See RecompUpdates.h for the rules; this file only carries them out.
//
// EVERY PATH HERE IS DERIVED FROM THE INSTALL DIRECTORY THE CALLER PASSED IN, and nothing in this unit knows
// where installs live. That is what keeps it linkable into a QtCore-only probe — and it is also the safety
// property, because a recursive delete whose target came from a catalogue id is one sanitising bug away from
// being a recursive delete of the emulators folder. The guards below refuse anything that is not a named
// directory with a named parent.
#include "RecompUpdates.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace recompupdate {

namespace {

// The one gate every function here passes through. A usable install directory has a NAME and a PARENT, and
// its name is not a dot-folder (which is what the kept copies themselves live in).
bool usable(const QString& installDir, QString* name = nullptr, QString* parent = nullptr)
{
    const QString d = installDir.trimmed();
    if (d.isEmpty()) return false;
    const QFileInfo fi(QDir::cleanPath(d));
    const QString n = fi.fileName();
    const QString p = fi.absolutePath();
    if (n.isEmpty() || n == QStringLiteral(".") || n == QStringLiteral("..")) return false;
    if (n.startsWith(QLatin1Char('.'))) return false;
    if (p.isEmpty() || QDir::cleanPath(p) == QDir::cleanPath(fi.absoluteFilePath())) return false;
    if (name) *name = n;
    if (parent) *parent = p;
    return true;
}

bool dirHasContent(const QString& dir)
{
    QDir d(dir);
    if (!d.exists()) return false;
    return !d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty();
}

}  // namespace

QString stampPath(const QString& installDir)
{
    QString name, parent;
    if (!usable(installDir, &name, &parent)) return QString();
    return QDir::cleanPath(installDir) + QStringLiteral("/eb-recomp-build.json");
}

BuildStamp readStamp(const QString& installDir)
{
    const QString path = stampPath(installDir);
    if (path.isEmpty()) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    // A stamp is a few hundred bytes. A bigger file is not one, and reading it whole is how a probe becomes
    // a way to make this app allocate a gigabyte.
    const QByteArray bytes = f.read(64 * 1024);
    f.close();
    return decodeStamp(bytes);
}

bool writeStamp(const QString& installDir, const BuildStamp& s)
{
    const QString path = stampPath(installDir);
    if (path.isEmpty()) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray body = encodeStamp(s);
    const bool ok = f.write(body) == body.size();
    f.close();
    return ok;
}

bool markLaunched(const QString& installDir)
{
    BuildStamp s = readStamp(installDir);
    if (!s.valid) return false;
    if (s.launched) return true;   // idempotent: a second run of the same build is not a second promotion
    s.launched = true;
    return writeStamp(installDir, s);
}

QString keptDirFor(const QString& installDir)
{
    QString name, parent;
    if (!usable(installDir, &name, &parent)) return QString();
    return parent + QStringLiteral("/.eb-previous/") + name;
}

bool hasKept(const QString& installDir)
{
    const QString kept = keptDirFor(installDir);
    return !kept.isEmpty() && dirHasContent(kept);
}

qint64 dirBytes(const QString& dir)
{
    if (dir.trimmed().isEmpty() || !QDir(dir).exists()) return -1;
    qint64 total = 0;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

qint64 keptBytes(const QString& installDir)
{
    const QString kept = keptDirFor(installDir);
    if (kept.isEmpty() || !dirHasContent(kept)) return -1;
    return dirBytes(kept);
}

bool keepAside(const QString& installDir, QString* why)
{
    QString name, parent;
    if (!usable(installDir, &name, &parent))
    {
        if (why) *why = QStringLiteral("that isn't a folder this app installs into");
        return false;
    }
    const QString kept = keptDirFor(installDir);
    if (kept.isEmpty())
    {
        if (why) *why = QStringLiteral("there is nowhere to keep the previous build");
        return false;
    }
    // NOTHING TO KEEP is a success. It is the first build of this entry, and there is no previous copy whose
    // survival anybody is relying on.
    if (!dirHasContent(installDir)) return true;

    // ONE KEPT COPY, EVER. Dropping the older one first is what stops a person who rebuilds five times
    // carrying five dead builds around — #248 (d) item 5, and the reason it is here rather than in a
    // housekeeping pass is that this is the only moment the app knows a kept copy has been superseded.
    if (QDir(kept).exists() && !QDir(kept).removeRecursively())
    {
        if (why) *why = QStringLiteral("an older kept build is in the way and could not be removed");
        return false;
    }
    QDir().mkpath(QFileInfo(kept).absolutePath());
    // A RENAME, NOT A COPY. Same volume by construction (the kept folder is a sibling of the install), so it
    // is atomic: either the previous build is entirely out of the way or it is entirely where it was. A copy
    // could half-succeed, and half a previous build is worse than none.
    if (!QDir().rename(QDir::cleanPath(installDir), kept))
    {
        if (why) *why = QStringLiteral("the installed copy could not be moved — it may still be running");
        return false;
    }
    return true;
}

bool dropKept(const QString& installDir)
{
    const QString kept = keptDirFor(installDir);
    if (kept.isEmpty()) return false;
    if (!QDir(kept).exists()) return true;   // already gone; the caller's post-condition holds
    return QDir(kept).removeRecursively();
}

bool restoreKept(const QString& installDir, QString* why)
{
    QString name, parent;
    if (!usable(installDir, &name, &parent))
    {
        if (why) *why = QStringLiteral("that isn't a folder this app installs into");
        return false;
    }
    const QString kept = keptDirFor(installDir);
    if (kept.isEmpty() || !dirHasContent(kept))
    {
        if (why) *why = QStringLiteral("there is no previous build kept for this one");
        return false;
    }
    // The replacement goes first. If it cannot be removed the kept copy is untouched and still restorable,
    // which is the only ordering that leaves a failure recoverable.
    if (QDir(installDir).exists() && !QDir(installDir).removeRecursively())
    {
        if (why) *why = QStringLiteral("the current copy could not be removed — it may still be running");
        return false;
    }
    if (!QDir().rename(kept, QDir::cleanPath(installDir)))
    {
        if (why) *why = QStringLiteral("the previous build could not be moved back");
        return false;
    }
    return true;
}

}  // namespace recompupdate
