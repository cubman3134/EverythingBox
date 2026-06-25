#include "SteamLibrary.h"

#include <QSettings>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <algorithm>

// Locate the Steam install directory (cached for the process).
static QString steamPath()
{
    static QString cached;
    static bool resolved = false;
    if (resolved) return cached;
    resolved = true;

#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QSettings::NativeFormat);
    QString p = reg.value(QStringLiteral("SteamPath")).toString();
    if (p.isEmpty())
    {
        QSettings reg2(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam"), QSettings::NativeFormat);
        p = reg2.value(QStringLiteral("InstallPath")).toString();
    }
    cached = QDir::fromNativeSeparators(p);
#elif defined(Q_OS_MAC)
    cached = QDir::homePath() + QStringLiteral("/Library/Application Support/Steam");
#else
    for (const QString& c : { QDir::homePath() + QStringLiteral("/.steam/steam"),
                              QDir::homePath() + QStringLiteral("/.local/share/Steam"),
                              QDir::homePath() + QStringLiteral("/.steam/root") })
        if (QDir(c).exists()) { cached = c; break; }
#endif
    if (!cached.isEmpty() && !QDir(cached).exists()) cached.clear();
    return cached;
}

bool SteamLibrary::isAvailable() { return !steamPath().isEmpty(); }

// Every Steam library root (the main install plus any extra drives added in Steam), from libraryfolders.vdf.
static QStringList libraryRoots()
{
    QStringList roots;
    const QString steam = steamPath();
    if (steam.isEmpty()) return roots;
    roots << steam; // the primary library lives in the Steam dir itself

    QFile f(steam + QStringLiteral("/steamapps/libraryfolders.vdf"));
    if (f.open(QIODevice::ReadOnly))
    {
        const QString text = QString::fromUtf8(f.readAll());
        QRegularExpression re(QStringLiteral("\"path\"\\s*\"([^\"]*)\""));
        auto it = re.globalMatch(text);
        while (it.hasNext())
        {
            QString p = it.next().captured(1);
            p.replace(QStringLiteral("\\\\"), QStringLiteral("/"));
            p.replace(QLatin1Char('\\'), QLatin1Char('/')); // vdf escapes backslashes
            if (!p.isEmpty()) roots << QDir::cleanPath(p);
        }
    }
    roots.removeDuplicates();
    return roots;
}

// Steam's own tools / redistributables show up as "apps" - keep them out of the games list.
static bool isHiddenTool(const QString& name)
{
    static const char* junk[] = { "Steamworks Common Redistributables", "Steamworks SDK Redist",
                                  "Steam Linux Runtime", "Proton", "SteamVR", "Steam Controller" };
    for (const char* j : junk)
        if (name.contains(QLatin1String(j), Qt::CaseInsensitive)) return true;
    return false;
}

QVector<SteamGame> SteamLibrary::installedGames()
{
    QHash<QString, QString> byId; // appid -> name (dedupes a game that appears in two libraries)
    const QRegularExpression reId(QStringLiteral("\"appid\"\\s*\"(\\d+)\""),
                                  QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression reName(QStringLiteral("\"name\"\\s*\"([^\"]*)\""));

    for (const QString& root : libraryRoots())
    {
        QDir apps(root + QStringLiteral("/steamapps"));
        const QStringList manifests = apps.entryList({ QStringLiteral("appmanifest_*.acf") }, QDir::Files);
        for (const QString& m : manifests)
        {
            QFile f(apps.filePath(m));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString text = QString::fromUtf8(f.readAll());
            const auto idm = reId.match(text);
            if (!idm.hasMatch()) continue;
            const auto nm = reName.match(text);
            const QString name = nm.hasMatch() ? nm.captured(1) : idm.captured(1);
            if (isHiddenTool(name)) continue;
            byId.insert(idm.captured(1), name);
        }
    }

    QVector<SteamGame> out;
    out.reserve(byId.size());
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it)
        out.push_back({ it.key(), it.value() });
    std::sort(out.begin(), out.end(), [](const SteamGame& a, const SteamGame& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString SteamLibrary::posterUrl(const QString& appid)
{
    const QString steam = steamPath();
    if (!steam.isEmpty())
    {
        // Steam pre-downloads the vertical capsule into its cache; prefer it (offline + reliable).
        const QStringList locals = {
            steam + QStringLiteral("/appcache/librarycache/") + appid + QStringLiteral("_library_600x900.jpg"),
            steam + QStringLiteral("/appcache/librarycache/") + appid + QStringLiteral("/library_600x900.jpg"),
        };
        for (const QString& p : locals)
            if (QFile::exists(p)) return p;
    }
    return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/") + appid
           + QStringLiteral("/library_600x900.jpg");
}

QString SteamLibrary::launchUrl(const QString& appid)
{
    return QStringLiteral("steam://rungameid/") + appid;
}
