#include "SaveMeta.h"

#include "AppPaths.h"
#include "Settings.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QVector>

namespace
{

// The sidecar lives beside the trees it describes, NOT inside saves/: a file under saves/ would be swept up
// by SaveSync::scanLocal and synced as if it were a save, and every device would then fight over it.
QString metaPath() { return AppPaths::dataDir() + QStringLiteral("/saves-meta.json"); }

QMutex& metaMutex()
{
    static QMutex m;
    return m;
}

// One canonical spelling of a key, so a path built with native separators on Windows cannot file the same
// save twice. Callers pass a path already relative to the data dir; this only normalises its shape.
QString normKey(const QString& relPath)
{
    QString k = relPath;
    k.replace(QLatin1Char('\\'), QLatin1Char('/'));
    k = QDir::cleanPath(k);
    while (k.startsWith(QLatin1String("./"))) k.remove(0, 2);
    return k;
}

QJsonObject readSidecar()
{
    QFile f(metaPath());
    if (!f.open(QIODevice::ReadOnly)) return {};                 // absent is the normal first-run shape
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};        // a corrupt sidecar reads as "nothing recorded"
}

// Best-effort: the sidecar is descriptive, never authoritative. A failed write costs a title in one notice,
// so it is logged and swallowed rather than propagated into the save path that is trying to persist a game.
void writeSidecar(const QJsonObject& o)
{
    QSaveFile f(metaPath());
    if (!f.open(QIODevice::WriteOnly))
    {
        qInfo().noquote() << QStringLiteral("saves-meta: could not open %1 for writing").arg(metaPath());
        return;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));   // readable: a user chasing a save can read it
    if (!f.commit())
        qInfo().noquote() << QStringLiteral("saves-meta: could not write %1").arg(metaPath());
}

SaveMeta::Entry entryFromJson(const QJsonObject& o)
{
    SaveMeta::Entry e;
    e.title     = o.value(QStringLiteral("title")).toString();
    e.system    = o.value(QStringLiteral("system")).toString();
    e.romPath   = o.value(QStringLiteral("romPath")).toString();
    e.updatedAt = qint64(o.value(QStringLiteral("updatedAt")).toDouble());
    return e;
}

// How stale a recorded entry may be before an otherwise identical put() rewrites the file. Battery RAM
// autosaves every ~10 s per running game, and rewriting the whole sidecar that often buys nothing — the only
// field that would change is updatedAt.
constexpr qint64 kTouchIntervalMs = 60 * 1000;

} // namespace

void SaveMeta::put(const QString& relPath, const QString& title, const QString& system, const QString& romPath)
{
    const QString key = normKey(relPath);
    if (key.isEmpty()) return;

    QMutexLocker lk(&metaMutex());
    QJsonObject all = readSidecar();

    const Entry prev = entryFromJson(all.value(key).toObject());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (prev.title == title && prev.system == system && prev.romPath == romPath
        && prev.updatedAt != 0 && now - prev.updatedAt < kTouchIntervalMs)
        return; // nothing new to record

    QJsonObject e;
    e.insert(QStringLiteral("title"), title);
    e.insert(QStringLiteral("system"), system);
    e.insert(QStringLiteral("romPath"), romPath);
    e.insert(QStringLiteral("updatedAt"), double(now));
    all.insert(key, e);
    writeSidecar(all);
}

SaveMeta::Entry SaveMeta::lookup(const QString& relPath)
{
    const QString key = normKey(relPath);
    if (key.isEmpty()) return {};
    QMutexLocker lk(&metaMutex());
    return entryFromJson(readSidecar().value(key).toObject());
}

QString SaveMeta::titleFor(const QString& relPath)
{
    const QString recorded = lookup(relPath).title.trimmed();
    if (!recorded.isEmpty()) return recorded;
    // No entry — which is the permanent answer for every save written before the sidecar existed, and for any
    // save made on another device. Fall back to the file's own base name, and guard against an empty result so
    // callers can drop this straight into a user-facing sentence.
    const QString base = QFileInfo(relPath).completeBaseName();
    if (!base.isEmpty()) return base;
    return relPath.isEmpty() ? QStringLiteral("this save") : relPath;
}

QString SaveMeta::resolvePath(const QString& root, const QString& systemId,
                              const QString& base, const QString& ext)
{
    const QString flat = root + QLatin1Char('/') + base + ext;
    // No system to namespace under (an extension no catalog claims): the flat path IS the answer. Building
    // "<root>//<base>.srm" instead would yield a sync key that matches nothing scanLocal produces.
    if (systemId.isEmpty()) return flat;

    const QString ns = root + QLatin1Char('/') + systemId + QLatin1Char('/') + base + ext;
    if (QFileInfo::exists(ns))   return ns;
    if (QFileInfo::exists(flat)) return flat;  // keep writing where the existing save already is
    return ns;                                 // brand new -> namespaced
}

void SaveMeta::sweepStrays()
{
    if (Settings::savesStraysSwept()) return;

    // The allowlist, and it is the whole safety argument: these are core-written save formats, and NOTHING
    // else is moved out of the install directory.
    static const QSet<QString> kCoreSaveExts = {
        QStringLiteral("srm"),  QStringLiteral("sav"),  QStringLiteral("brm"), QStringLiteral("smpc"),
        QStringLiteral("mcd"),  QStringLiteral("mcr"),  QStringLiteral("eep"), QStringLiteral("fla"),
        QStringLiteral("state")
    };

    const QString appDir   = AppPaths::dataDir();
    const QString savesDir = appDir + QStringLiteral("/saves");

    // Top level only, files only, no symlinks — saves/ is a subdirectory, so nothing already filed is seen.
    QVector<QFileInfo> strays;
    const QFileInfoList top = QDir(appDir).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& fi : top)
        if (kCoreSaveExts.contains(fi.suffix().toLower())) strays.push_back(fi);

    if (strays.isEmpty()) { Settings::setSavesStraysSwept(true); return; }

    QDir().mkpath(savesDir);
    bool anyFailed = false;
    for (const QFileInfo& fi : strays)
    {
        const QString from = fi.absoluteFilePath();
        const QString dest = savesDir + QLatin1Char('/') + fi.fileName();
        if (QFileInfo::exists(dest))
        {
            // NEVER overwrite: the file already in saves/ is the one the app has been reading and syncing.
            qInfo().noquote() << QStringLiteral("save sweep: left %1 where it is — %2 already exists")
                                     .arg(from, dest);
            continue;
        }
        if (!QFile::rename(from, dest))
        {
            // Almost always "another instance has it open". A failed rename is a skip, not an error: the file
            // is untouched and the next launch tries again.
            qInfo().noquote() << QStringLiteral("save sweep: could not move %1 (in use?) — left in place").arg(from);
            anyFailed = true;
            continue;
        }
        qInfo().noquote() << QStringLiteral("save sweep: moved %1 -> %2").arg(from, dest);
    }

    // Only a rename FAILURE is worth retrying; "the destination already exists" is a decision, and re-taking
    // it every launch would just repeat the log line. Leaving the flag unset costs one directory listing.
    if (!anyFailed) Settings::setSavesStraysSwept(true);
}
