#include "RomLibrary.h"
#include "Settings.h"
#include "SystemCatalog.h"
#include "DownloadsStore.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
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
                QStringLiteral("My Media Vault — ROM library\r\n\r\n"
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
            const bool ok = sys->extensions.isEmpty() ? isDiscOrArcadeRom(ext)
                                                       : sys->extensions.contains(ext);
            if (!ok) continue;
            Rom r;
            r.path = path;
            r.title = QFileInfo(path).completeBaseName();
            r.systemId = sys->id;
            r.systemName = sys->name;
            g.roms.push_back(r);
        }
        if (g.roms.isEmpty()) continue; // only surface systems that actually have games

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
