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

// The curated Battle.net product codes. Keys are titleKey()-normalized DisplayName prefixes; a title that
// starts with a key takes its code (so "World of Warcraft Classic" resolves to wow). Unknown => empty code,
// which routes the launch to the install-dir exe instead of a battlenet:// URI.
const QVector<QPair<QString, QString>>& codeTable()
{
    static const QVector<QPair<QString, QString>> t = {
        { QStringLiteral("world of warcraft"),   QStringLiteral("wow")  },
        { QStringLiteral("diablo iv"),           QStringLiteral("osi")  },
        { QStringLiteral("diablo iii"),          QStringLiteral("d3")   },
        { QStringLiteral("diablo ii resurrected"), QStringLiteral("osi") },
        { QStringLiteral("overwatch"),           QStringLiteral("pro")  },
        { QStringLiteral("hearthstone"),         QStringLiteral("wtcg") },
        { QStringLiteral("starcraft ii"),        QStringLiteral("s2")   },
        { QStringLiteral("starcraft"),           QStringLiteral("s1")   },
        { QStringLiteral("warcraft iii"),        QStringLiteral("w3")   },
        { QStringLiteral("heroes of the storm"), QStringLiteral("hero") },
        { QStringLiteral("call of duty"),        QStringLiteral("cod")  },
    };
    return t;
}

// Best-effort: the largest top-level .exe under installDir that isn't an updater/uninstaller/launcher.
QString findGameExe(const QString& installDir)
{
    if (installDir.isEmpty()) return QString();
    QDir d(installDir);
    if (!d.exists()) return QString();
    QString best; qint64 bestSize = -1;
    for (const QFileInfo& fi : d.entryInfoList(QStringList{ QStringLiteral("*.exe") }, QDir::Files))
    {
        const QString n = fi.fileName().toLower();
        if (n.contains(QStringLiteral("uninstall")) || n.contains(QStringLiteral("unins"))
            || n.contains(QStringLiteral("launcher")) || n.contains(QStringLiteral("updater"))
            || n.contains(QStringLiteral("battle.net"))) continue;
        if (fi.size() > bestSize) { bestSize = fi.size(); best = fi.absoluteFilePath(); }
    }
    return best;
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
    if (!publisher.contains(QStringLiteral("Blizzard"), Qt::CaseInsensitive)) return g; // empty name => filtered
    if (displayName.trimmed().isEmpty()) return g;
    g.name = displayName.trimmed();
    g.code = codeForTitle(g.name);
    g.installDir = winPathToSlash(installLocation);
    return g;
}

QVector<BattleNetGame> BattleNetLibrary::installedGames(const QString& regProbeRoot)
{
    QVector<BattleNetGame> out;
    for (const auto& hk : entryKeys(regProbeRoot))
    {
        const RawEntry r = readEntry(regProbeRoot, hk.first, hk.second);
        BattleNetGame g = parseUninstallEntry(r.displayName, r.publisher, r.installLocation);
        if (g.name.isEmpty()) continue;                                   // not Blizzard / incomplete
        bool dup = false;
        for (const BattleNetGame& e : out) if (e.name.compare(g.name, Qt::CaseInsensitive) == 0) dup = true;
        if (dup) continue;                                                // same title in both hive views
        g.exe = findGameExe(g.installDir);
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const BattleNetGame& a, const BattleNetGame& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

bool BattleNetLibrary::isAvailable(const QString& regProbeRoot)
{
    return !installedGames(regProbeRoot).isEmpty();
}
