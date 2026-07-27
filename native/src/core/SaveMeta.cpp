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
#include <QStringList>
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

    // Nothing here under THIS system id — but systemId is a property of how the user reached the ROM, not of
    // the ROM. GameLauncher prefers the caller's systemHint and falls back to SystemCatalog::forExtension,
    // which returns the FIRST catalog entry claiming the extension; .cue/.iso/.bin/.pbp are claimed by several
    // systems and two openGamePath call sites pass no hint at all. So the same file opened from the library
    // (hint -> "psx") and from "Open game file…" (no hint -> whatever the extension resolves to) would namespace
    // its save under two different systems, and the second launch would create an EMPTY save while 20 hours sat
    // three directories away, unreachable from any UI and syncing as a rival copy of the same game. Before
    // creating anything, look for this ROM's save under every OTHER system's namespace and keep using it.
    QVector<QFileInfo> others;
    const QFileInfoList dirs = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                                        QDir::Name);
    for (const QFileInfo& d : dirs)
    {
        const QFileInfo cand(root + QLatin1Char('/') + d.fileName() + QLatin1Char('/') + base + ext);
        if (cand.isFile()) others.push_back(cand);
    }
    if (others.isEmpty()) return ns;             // genuinely brand new -> namespaced
    if (others.size() == 1)
    {
        const QString found = root + QLatin1Char('/') + others.front().dir().dirName()
                              + QLatin1Char('/') + base + ext;
        qInfo().noquote() << QStringLiteral("saves: %1%2 already has a save under another system (%3) — using it")
                                 .arg(base, ext, others.front().dir().dirName());
        return found;
    }

    // Several systems hold a save with this base name. One of them is this ROM's; the others belong to
    // different games that merely share a file name — and we cannot tell which from here. The newest is the one
    // the user was most recently playing, which is the only defensible guess; say so out loud rather than
    // picking silently.
    const QFileInfo* newest = &others.front();
    for (const QFileInfo& fi : others)
        if (fi.lastModified() > newest->lastModified()) newest = &fi;
    QStringList where;
    for (const QFileInfo& fi : others) where << fi.dir().dirName();
    qInfo().noquote() << QStringLiteral("saves: %1%2 has saves under several systems (%3) — using the newest (%4)")
                             .arg(base, ext, where.join(QStringLiteral(", ")), newest->dir().dirName());
    return root + QLatin1Char('/') + newest->dir().dirName() + QLatin1Char('/') + base + ext;
}

void SaveMeta::sweepStrays()
{
    if (Settings::savesStraysSwept()) return;

    // The allowlist, and it is the whole safety argument: these are core-written save formats, and NOTHING
    // else is moved out of the install directory.
    //
    // ".state" is deliberately NOT here. Every state this app writes is a numbered slot ("X.state1", whose
    // suffix() is "state1"), so a bare ".state" in the app directory was written by some OTHER program — and
    // moving it would neither make it reachable nor leave its owner able to find it. saves/ is the wrong
    // directory outright (states live in states/), and states/ would be no better: the slot grid only ever
    // opens .state<N>, so the file would be preserved and synced to every device while staying permanently
    // unopenable. Leaving it where its owner put it is the only outcome that helps anyone.
    static const QSet<QString> kCoreSaveExts = {
        QStringLiteral("srm"),  QStringLiteral("sav"),  QStringLiteral("brm"), QStringLiteral("smpc"),
        QStringLiteral("mcd"),  QStringLiteral("mcr"),  QStringLiteral("eep"), QStringLiteral("fla")
    };

    const QString appDir   = AppPaths::dataDir();
    const QString savesDir = appDir + QStringLiteral("/saves");

    // The build is portable and dataDir() IS the application directory on desktop, so the install root can
    // legitimately BE the user's ROM or library folder — and ".srm"/".sav" beside the ROM is the standard
    // layout every other emulator uses. Sweeping there would silently relocate saves away from the ROMs they
    // belong to, which is exactly the irreversible tidying this track refuses. The flag is left unset so a
    // later folder change re-enables the sweep; the cost of re-taking this decision is one settings read.
    const auto sameDir = [](const QString& a, const QString& b) {
        if (a.isEmpty() || b.isEmpty()) return false;
        const QString ca = QFileInfo(a).canonicalFilePath();
        const QString cb = QFileInfo(b).canonicalFilePath();
        if (!ca.isEmpty() && !cb.isEmpty()) return QDir(ca) == QDir(cb);
        return QDir(a) == QDir(b);
    };
    for (const QString& userDir : { Settings::romsFolder(), Settings::libraryFolder() })
        if (sameDir(appDir, userDir))
        {
            qInfo().noquote() << QStringLiteral("save sweep: skipped — %1 is also a configured ROM/library "
                                                "folder, so loose saves belong to the ROMs beside them").arg(appDir);
            return;
        }

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
        // "A file another instance has open is skipped" has to be checked BEFORE the rename, not inferred from
        // its failure: QFile::rename falls back to COPYING the contents and then removing the source when the
        // engine rename fails. On Windows the source remove() then fails and Qt undoes the copy, degrading to
        // the skip we wanted — but on Android/Linux unlinking an open file always succeeds, so the fallback
        // would take a torn snapshot of a save being written and delete the original. The app is portable with
        // no single-instance guard, so "a second instance with a game running" is a supported state. Opening
        // ReadWrite first is what actually catches the Windows share-mode lock; it is not a lock on POSIX (no
        // mandatory locking), where same-filesystem rename(2) succeeds atomically anyway and the copy fallback
        // is never reached.
        {
            QFile probe(from);
            if (!probe.open(QIODevice::ReadWrite))
            {
                qInfo().noquote() << QStringLiteral("save sweep: %1 is open elsewhere — left in place").arg(from);
                anyFailed = true;
                continue;
            }
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
