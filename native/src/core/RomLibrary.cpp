#include "RomLibrary.h"
#include "Settings.h"
#include "SystemCatalog.h"
#include "DownloadsStore.h"
#include "AppPaths.h"
#include "DiscGroup.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>

namespace
{
// System ids whose canonical library folder differs from the id, using the RetroBat / ES-DE spelling so an
// existing collection drops in unchanged. Anything not listed uses its own id as the folder name.
const QHash<QString, QString>& folderOverrides()
{
    static const QHash<QString, QString> m = {
        { QStringLiteral("genesis"), QStringLiteral("megadrive") },
        { QStringLiteral("pce"),     QStringLiteral("pcengine") },
        { QStringLiteral("pcecd"),   QStringLiteral("pcenginecd") },
        { QStringLiteral("ws"),      QStringLiteral("wonderswan") },
        { QStringLiteral("a2600"),   QStringLiteral("atari2600") },
        { QStringLiteral("sg1000"),  QStringLiteral("sg-1000") },
        { QStringLiteral("coleco"),  QStringLiteral("colecovision") },
        { QStringLiteral("32x"),     QStringLiteral("sega32x") },
        { QStringLiteral("msdos"),   QStringLiteral("dos") },
        { QStringLiteral("3ds"),     QStringLiteral("n3ds") },
        { QStringLiteral("jaguar"),  QStringLiteral("atarijaguar") },
    };
    return m;
}

// Extra folder-name aliases accepted on scan (folder name -> our system id), for names that don't match an
// id or the override and that forConsoleName() wouldn't catch (ES-DE/RetroBat spellings, no spaces).
const QHash<QString, QString>& folderAliases()
{
    static const QHash<QString, QString> m = {
        { QStringLiteral("megadrive"),   QStringLiteral("genesis") },
        { QStringLiteral("genesis"),     QStringLiteral("genesis") },
        { QStringLiteral("mastersystem"),QStringLiteral("genesis") },
        { QStringLiteral("gamegear"),    QStringLiteral("genesis") },
        { QStringLiteral("pcengine"),    QStringLiteral("pce") },
        { QStringLiteral("tg16"),        QStringLiteral("pce") },
        { QStringLiteral("turbografx"),  QStringLiteral("pce") },
        { QStringLiteral("pcenginecd"),  QStringLiteral("pcecd") },
        { QStringLiteral("tgcd"),        QStringLiteral("pcecd") },
        { QStringLiteral("wonderswan"),  QStringLiteral("ws") },
        { QStringLiteral("wonderswancolor"), QStringLiteral("ws") },
        { QStringLiteral("atari2600"),   QStringLiteral("a2600") },
        { QStringLiteral("sg-1000"),     QStringLiteral("sg1000") },
        { QStringLiteral("colecovision"),QStringLiteral("coleco") },
        { QStringLiteral("sega32x"),     QStringLiteral("32x") },
        { QStringLiteral("dos"),         QStringLiteral("msdos") },
        { QStringLiteral("pc"),          QStringLiteral("msdos") },
        { QStringLiteral("n3ds"),        QStringLiteral("3ds") },
        { QStringLiteral("atarijaguar"), QStringLiteral("jaguar") },
        { QStringLiteral("gamecube"),    QStringLiteral("gc") },
        { QStringLiteral("wii"),         QStringLiteral("gc") },
        { QStringLiteral("gbc"),         QStringLiteral("gb") },
    };
    return m;
}

// Disc/arcade formats accepted for systems that declare no unambiguous extension of their own (CD & arcade
// systems whose formats collide with earlier consoles, so they're routed by folder here).
bool isDiscOrArcadeRom(const QString& ext)
{
    static const QSet<QString> s = {
        QStringLiteral("iso"), QStringLiteral("chd"), QStringLiteral("cue"), QStringLiteral("gdi"),
        QStringLiteral("cdi"), QStringLiteral("pbp"), QStringLiteral("m3u"), QStringLiteral("cso"),
        QStringLiteral("zip"), QStringLiteral("7z"),
    };
    return s.contains(ext);
}

// #49 multi-disc grouping glue. The pure grouping lives in DiscGroup.h; here is the disk I/O it deliberately
// keeps out: reading a user-authored .m3u to see which discs it already claims, and writing a generated .m3u
// into a cache dir so the real ROM folder stays untouched.

// The local disc paths a user .m3u already lists (absolute, cleaned). Directives (#…) and remote URLs are
// ignored; a relative entry resolves against the playlist's own folder — the same rule StreamResolver uses.
QStringList userM3uMembers(const QString& m3uPath)
{
    QFile f(m3uPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString text = QString::fromUtf8(f.readAll());
    const QString base = QFileInfo(m3uPath).absolutePath() + QLatin1Char('/');
    QStringList out;
    // CRLF repo: split on either line ending, never a bare "\n".
    for (QString line : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts))
    {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue; // directive / comment
        if (line.contains(QStringLiteral("://"))) continue;                 // remote entry — not a local disc
        const QString abs = QFileInfo(line).isAbsolute() ? line : base + line;
        out << QDir::cleanPath(QFileInfo(abs).absoluteFilePath());
    }
    return out;
}

// Write (or reuse) the generated .m3u for a multi-disc set under <data>/cache/m3u/<system>/. The file name is
// deterministic — a sanitised title plus a short hash of the normalised grouping key — so the same set always
// maps to the same file and a re-scan does not churn. Distinct sets have distinct keys (same key would have
// grouped them into one set), so the hash cannot collide two different sets onto one file. The body holds
// ABSOLUTE disc paths, so the cached playlist resolves no matter where it sits. Only rewrites when the
// content actually changed. Returns the .m3u path, or empty on write failure (caller falls back to disc 1).
QString writeGeneratedM3u(const QString& systemId, const DiscGroup::DiscSet& set)
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/cache/m3u/") + systemId;
    if (!QDir().mkpath(dir)) return QString();

    QString name = set.cleanTitle;
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 ._-]")), QStringLiteral("_"));
    name = name.simplified();
    if (name.isEmpty()) name = QStringLiteral("disc-set");
    const QString key = DiscGroup::normalizedKey(set.cleanTitle);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    const QString outPath = dir + QLatin1Char('/') + name + QLatin1Char('.') + hash + QStringLiteral(".m3u");

    DiscGroup::DiscSet abs = set;
    for (QString& m : abs.members) m = QFileInfo(m).absoluteFilePath();
    const QString content = DiscGroup::m3uContentFor(abs);

    QFile ex(outPath);
    if (ex.exists() && ex.open(QIODevice::ReadOnly))
    {
        const QString cur = QString::fromUtf8(ex.readAll());
        ex.close();
        if (cur == content) return outPath; // unchanged — reuse, don't touch the mtime
    }
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    out.write(content.toUtf8());
    out.close();
    return outPath;
}

// Collapse each multi-disc set in a scanned system group into ONE Rom pointing at a generated .m3u, and hide
// the individual disc members. A user-authored .m3u already in the group is kept untouched and the discs it
// lists are hidden (never re-grouped, never a competing playlist generated). Single files pass through as-is.
void collapseMultiDiscSets(RomLibrary::SystemGroup& g)
{
    QVector<RomLibrary::Rom> userPlaylists; // .m3u files the user wrote — kept exactly as-is
    QVector<RomLibrary::Rom> others;        // everything else — candidates for auto-grouping
    for (const RomLibrary::Rom& r : g.roms)
    {
        if (QFileInfo(r.path).suffix().toLower() == QStringLiteral("m3u")) userPlaylists.push_back(r);
        else                                                               others.push_back(r);
    }
    if (others.isEmpty()) return; // nothing to group (only playlists, or empty)

    // Discs a user playlist already claims are hidden from the grid and excluded from auto-grouping.
    QSet<QString> claimed;
    for (const RomLibrary::Rom& m : userPlaylists)
        for (const QString& p : userM3uMembers(m.path)) claimed.insert(p);

    QVector<QString>                     candidatePaths;
    QHash<QString, RomLibrary::Rom>      byPath; // path -> its original Rom, for single-file pass-through
    for (const RomLibrary::Rom& r : others)
    {
        byPath.insert(r.path, r);
        if (!claimed.contains(QDir::cleanPath(QFileInfo(r.path).absoluteFilePath())))
            candidatePaths.push_back(r.path);
    }

    QVector<RomLibrary::Rom> rebuilt = userPlaylists; // user playlists survive verbatim
    for (const DiscGroup::DiscSet& s : DiscGroup::groupDiscs(candidatePaths))
    {
        if (s.isMultiDisc)
        {
            const QString m3u = writeGeneratedM3u(g.systemId, s);
            RomLibrary::Rom r;
            r.path       = m3u.isEmpty() ? s.members.front() : m3u; // fall back to disc 1 if the cache write failed
            r.title      = s.cleanTitle;
            r.systemId   = g.systemId;
            r.systemName = g.systemName;
            rebuilt.push_back(r);
        }
        else
        {
            rebuilt.push_back(byPath.value(s.members.front())); // lone game — unchanged
        }
    }
    g.roms = rebuilt;
}
} // namespace

QString RomLibrary::root()
{
    return Settings::romsFolder();
}

QString RomLibrary::folderFor(const QString& systemId)
{
    return folderOverrides().value(systemId, systemId);
}

const GameSystem* RomLibrary::systemForFolder(const QString& folderName)
{
    const QString n = folderName.toLower().trimmed();
    if (n.isEmpty()) return nullptr;
    if (const GameSystem* s = SystemCatalog::byId(n)) return s;           // our own id as the folder
    const QString aliased = folderAliases().value(n);
    if (!aliased.isEmpty()) if (const GameSystem* s = SystemCatalog::byId(aliased)) return s;
    // Data-driven folder aliases (issue #92): a JSON-added system carries its own accepted folder names, so a
    // folder matching one routes to it. Built-in systems declare no folderAliases (they resolve above or via
    // forConsoleName), so this loop changes nothing for them.
    for (const GameSystem& s : SystemCatalog::systems())
        if (s.folderAliases.contains(n)) return &s;
    return SystemCatalog::forConsoleName(n);                              // a display/console name
}

void RomLibrary::ensureStructure()
{
    const QString base = root();
    QDir().mkpath(base);
    for (const GameSystem& s : SystemCatalog::systems())
        QDir().mkpath(base + QStringLiteral("/") + folderFor(s.id));

    // A short README so the layout is self-explanatory when the user opens the folder.
    const QString readme = base + QStringLiteral("/README.txt");
    if (!QFile::exists(readme))
    {
        QFile f(readme);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QString body =
                QStringLiteral("EverythingBox — ROM library\r\n\r\n"
                               "Drop game ROMs into the matching system folder below (RetroBat / EmulationStation\r\n"
                               "Desktop Edition layout). They then appear under \"Local ROMs\" in the Library.\r\n\r\n");
            for (const GameSystem& s : SystemCatalog::systems())
                body += folderFor(s.id) + QStringLiteral("/  —  ") + s.name + QStringLiteral("\r\n");
            f.write(body.toUtf8());
        }
    }
}

QVector<RomLibrary::SystemGroup> RomLibrary::scan()
{
    QVector<SystemGroup> groups;
    const QString base = root();
    QDir baseDir(base);
    if (!baseDir.exists()) return groups;

    const QFileInfoList dirs = baseDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& d : dirs)
    {
        const GameSystem* sys = systemForFolder(d.fileName());
        if (!sys) continue; // a folder we don't recognize as a system — leave it alone

        SystemGroup g;
        g.systemId = sys->id;
        g.systemName = sys->name;
        g.folder = d.fileName();

        // Walk the system folder (and any sub-folders) for files matching that system. The per-system
        // extension list is the filter, so gamelist.xml / box art / .srm saves are ignored automatically.
        QDirIterator it(d.absoluteFilePath(),
                        QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString path = it.next();
            const QString ext = QFileInfo(path).suffix().toLower();
            // Accept the system's native extensions, plus zip/7z everywhere: RetroBat / EmulationStation
            // commonly store even cartridge ROMs zipped (e.g. atari2600 "*.zip"), and most cores load them.
            const bool ok = sys->extensions.isEmpty()
                                ? isDiscOrArcadeRom(ext)
                                : (sys->extensions.contains(ext) || ext == QStringLiteral("zip") || ext == QStringLiteral("7z"));
            if (!ok) continue;
            Rom r;
            r.path = path;
            r.title = QFileInfo(path).completeBaseName();
            r.systemId = sys->id;
            r.systemName = sys->name;
            g.roms.push_back(r);
        }
        if (g.roms.isEmpty()) continue; // only surface systems that actually have games

        // #49: group disc siblings ("Game (Disc 1/2/3)") into one .m3u entry each, hiding the members.
        collapseMultiDiscSets(g);
        if (g.roms.isEmpty()) continue;

        std::sort(g.roms.begin(), g.roms.end(),
                  [](const Rom& a, const Rom& b) { return a.title.localeAwareCompare(b.title) < 0; });
        groups.push_back(g);
    }

    std::sort(groups.begin(), groups.end(),
              [](const SystemGroup& a, const SystemGroup& b) { return a.systemName.localeAwareCompare(b.systemName) < 0; });
    return groups;
}

int RomLibrary::syncToDownloads()
{
    // What's already tracked (by stable key, else path) — so re-runs don't churn or reorder the list.
    QSet<QString> have;
    for (const DownloadedItem& d : DownloadsStore::list())
        have.insert(d.key.isEmpty() ? d.path : d.key);

    int added = 0;
    for (const SystemGroup& g : scan())
        for (const Rom& r : g.roms)
        {
            const QString key = QStringLiteral("romlib:") + r.path;
            if (have.contains(key) || have.contains(r.path)) continue; // already in Downloaded
            DownloadedItem d;
            d.path = r.path;
            d.title = r.title;
            d.kind = QStringLiteral("game");
            d.key = key;
            d.system = r.systemId; // groups it under the right console in the Downloaded folder
            DownloadsStore::add(d);
            have.insert(key);
            ++added;
        }
    return added;
}
