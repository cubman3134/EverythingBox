#include "BattleNetLibrary.h"

#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

namespace {

// Registry values are Windows-style paths regardless of the host OS (QDir::fromNativeSeparators is a no-op
// off-Windows), so convert backslashes explicitly — same helper shape as GogLibrary.
QString winPathToSlash(QString p)
{
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p;
}

// Lowercased, punctuation-stripped, whitespace-collapsed title — the code-map lookup key.
QString titleKey(const QString& t)
{
    QString s = t.toLower();
    static const QRegularExpression nonAlnum(QStringLiteral("[^a-z0-9]+"));
    s.replace(nonAlnum, QStringLiteral(" "));
    return s.simplified();
}

// Curated Battle.net product codes, LONGEST-PREFIX-FIRST (order is load-bearing: "starcraft ii" MUST precede
// "starcraft", else every SC2 title silently resolves to s1 and launches StarCraft Remastered).
// Keys are titleKey()-normalized DisplayName prefixes; a title that starts with a key takes its code (so
// "World of Warcraft Classic" resolves to wow).
// Only codes we are confident of live here. A title with NO row falls back to launching its install-dir exe,
// which is the SAFE path — a WRONG code is worse than none, because a non-empty code suppresses that fallback.
// New rows must be confirmed against a real Battle.net install before being added.
const QVector<QPair<QString, QString>>& codeTable()
{
    static const QVector<QPair<QString, QString>> t = {
        { QStringLiteral("world of warcraft"),     QStringLiteral("wow")  },
        { QStringLiteral("diablo iii"),            QStringLiteral("d3")   },
        { QStringLiteral("diablo ii resurrected"), QStringLiteral("osi")  },
        { QStringLiteral("overwatch"),             QStringLiteral("pro")  },
        { QStringLiteral("hearthstone"),           QStringLiteral("wtcg") },
        { QStringLiteral("starcraft ii"),          QStringLiteral("s2")   },
        { QStringLiteral("starcraft"),             QStringLiteral("s1")   },
        { QStringLiteral("warcraft iii"),          QStringLiteral("w3")   },
        { QStringLiteral("heroes of the storm"),   QStringLiteral("hero") },
    };
    return t;
}

// True for an .exe that is plumbing (installer/updater/launcher/crash handler), never the game itself.
bool isNotAGameExe(const QString& fileName)
{
    const QString n = fileName.toLower();
    return n.contains(QStringLiteral("uninstall")) || n.contains(QStringLiteral("unins"))
        || n.contains(QStringLiteral("launcher"))  || n.contains(QStringLiteral("updater"))
        || n.contains(QStringLiteral("battle.net")) || n.contains(QStringLiteral("crash"))
        || n.contains(QStringLiteral("setup"))     || n.contains(QStringLiteral("redist"))
        || n.contains(QStringLiteral("vcredist")) || n.contains(QStringLiteral("helper"));
}

// Directories that never hold the game binary but can hold tens of thousands of files — never descend into
// them, so the scan stays cheap on a multi-GB install.
bool isAssetDir(const QString& dirName)
{
    const QString n = dirName.toLower();
    return n == QStringLiteral("data") || n == QStringLiteral("assets") || n == QStringLiteral("cache")
        || n == QStringLiteral("logs") || n == QStringLiteral("errors") || n == QStringLiteral("interface")
        || n == QStringLiteral("wtf")  || n == QStringLiteral("screenshots")
        || n.startsWith(QLatin1Char('.'));
}

// Best-effort largest non-plumbing .exe under installDir, searched to a BOUNDED depth (the install root plus
// two levels). Blizzard titles routinely nest the binary — World of Warcraft ships it under `_retail_/`, others
// under `x64/` or `bin/` — so a top-level-only scan would leave a code-less title listed but unlaunchable. The
// depth cap + asset-dir skip keep this from walking a multi-GB game directory: only the launch FALLBACK needs
// it (a title with a curated battlenet:// code never consults the exe at all).
QString findGameExeAt(const QDir& d, int depthLeft)
{
    QString best; qint64 bestSize = -1;
    for (const QFileInfo& fi : d.entryInfoList(QStringList{ QStringLiteral("*.exe") }, QDir::Files))
    {
        if (isNotAGameExe(fi.fileName())) continue;
        if (fi.size() > bestSize) { bestSize = fi.size(); best = fi.absoluteFilePath(); }
    }
    if (depthLeft <= 0) return best;
    for (const QFileInfo& sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        if (isAssetDir(sub.fileName())) continue;
        const QString nested = findGameExeAt(QDir(sub.absoluteFilePath()), depthLeft - 1);
        if (nested.isEmpty()) continue;
        const qint64 sz = QFileInfo(nested).size();
        if (sz > bestSize) { bestSize = sz; best = nested; }   // deeper-but-bigger wins (the real binary)
    }
    return best;
}

QString findGameExe(const QString& installDir)
{
    if (installDir.isEmpty()) return QString();
    QDir d(installDir);
    if (!d.exists()) return QString();
    return findGameExeAt(d, 2);   // root + 2 levels: covers _retail_/, x64/, bin/, Game/bin/
}

// One uninstall subkey's fields, from the fake-registry INI or the live hive (both 64-bit and WOW6432Node
// views — Blizzard's installers write to either depending on the title's bitness).
struct RawEntry { QString displayName, publisher, installLocation; };

RawEntry readEntry(const QString& regProbeRoot, const QString& hive, const QString& sub)
{
    RawEntry e;
    if (!regProbeRoot.isEmpty())
    {
        QSettings ini(regProbeRoot, QSettings::IniFormat);
        e.displayName     = ini.value(sub + QStringLiteral("/DisplayName")).toString();
        e.publisher       = ini.value(sub + QStringLiteral("/Publisher")).toString();
        e.installLocation = ini.value(sub + QStringLiteral("/InstallLocation")).toString();
        return e;
    }
#ifdef Q_OS_WIN
    QSettings reg(hive + QLatin1Char('\\') + sub, QSettings::NativeFormat);
    e.displayName     = reg.value(QStringLiteral("DisplayName")).toString();
    e.publisher       = reg.value(QStringLiteral("Publisher")).toString();
    e.installLocation = reg.value(QStringLiteral("InstallLocation")).toString();
#else
    Q_UNUSED(hive); Q_UNUSED(sub);
#endif
    return e;
}

// (hive, subkey) pairs to inspect: the INI's groups under a fixture, else both live Uninstall views.
QVector<QPair<QString, QString>> entryKeys(const QString& regProbeRoot)
{
    QVector<QPair<QString, QString>> out;
    if (!regProbeRoot.isEmpty())
    {
        QSettings ini(regProbeRoot, QSettings::IniFormat);
        for (const QString& g : ini.childGroups()) out.push_back({ QString(), g });
        return out;
    }
#ifdef Q_OS_WIN
    static const QStringList hives = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
    };
    for (const QString& h : hives)
    {
        QSettings reg(h, QSettings::NativeFormat);
        for (const QString& g : reg.childGroups()) out.push_back({ h, g });
    }
#endif
    return out;
}

// The one registry walk, shared by installedGames() and isAvailable(). stopAtFirst returns as soon as one
// launchable game is accepted: isAvailable only needs existence, and a full scan enumerates both HKLM
// Uninstall views (500-1500 subkeys, a QSettings each) plus a filesystem probe per match — a caller doing
// `isAvailable() && !installedGames().isEmpty()` would otherwise pay for all of that twice. Under
// stopAtFirst a non-empty code is launch route enough, so the disk is only touched when the code is empty.
QVector<BattleNetGame> scan(const QString& regProbeRoot, bool stopAtFirst)
{
    QVector<BattleNetGame> out;
    for (const auto& hk : entryKeys(regProbeRoot))
    {
        const RawEntry r = readEntry(regProbeRoot, hk.first, hk.second);
        BattleNetGame g = BattleNetLibrary::parseUninstallEntry(r.displayName, r.publisher, r.installLocation);
        if (g.name.isEmpty()) continue;                                   // not Blizzard / incomplete
        bool dup = false;
        for (const BattleNetGame& e : out) if (e.name.compare(g.name, Qt::CaseInsensitive) == 0) dup = true;
        if (dup) continue;                                                // same title in both hive views
        if (stopAtFirst)
        {
            if (g.code.isEmpty() && findGameExe(g.installDir).isEmpty()) continue;  // no launch route
            out.push_back(g);
            return out;                                                   // existence answered; no sort needed
        }
        g.exe = findGameExe(g.installDir);
        if (g.code.isEmpty() && g.exe.isEmpty()) continue;   // no launch route ⇒ don't list a dead tile
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const BattleNetGame& a, const BattleNetGame& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

} // namespace

QString BattleNetLibrary::codeForTitle(const QString& displayName)
{
    const QString k = titleKey(displayName);
    if (k.isEmpty()) return QString();
    for (const auto& p : codeTable())
        if (k.startsWith(p.first)) return p.second;
    return QString();
}

QString BattleNetLibrary::launchUri(const QString& code)
{
    return code.isEmpty() ? QString() : (QStringLiteral("battlenet://") + code);
}

BattleNetGame BattleNetLibrary::parseUninstallEntry(const QString& displayName, const QString& publisher,
                                                    const QString& installLocation)
{
    BattleNetGame g;
    // startsWith, not contains: still matches "Blizzard Entertainment, Inc." while rejecting an unrelated
    // publisher whose string merely mentions Blizzard.
    if (!publisher.trimmed().startsWith(QStringLiteral("Blizzard Entertainment"), Qt::CaseInsensitive)) return g;
    if (displayName.trimmed().isEmpty()) return g;
    if (installLocation.trimmed().isEmpty()) return g;   // no install dir ⇒ nothing to list or launch
    g.name = displayName.trimmed();
    g.code = codeForTitle(g.name);
    g.installDir = winPathToSlash(installLocation);
    return g;
}

QVector<BattleNetGame> BattleNetLibrary::installedGames(const QString& regProbeRoot)
{
    return scan(regProbeRoot, /*stopAtFirst*/false);
}

bool BattleNetLibrary::isAvailable(const QString& regProbeRoot)
{
    return !scan(regProbeRoot, /*stopAtFirst*/true).isEmpty();
}
