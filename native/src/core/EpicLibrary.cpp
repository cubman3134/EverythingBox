#include "EpicLibrary.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <algorithm>

// The real manifests dir: %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests. Empty off-Windows / when
// ProgramData isn't set.
static QString defaultManifestsRoot()
{
#ifdef Q_OS_WIN
    const auto env = QProcessEnvironment::systemEnvironment();
    QString pd = env.value(QStringLiteral("ProgramData"));
    if (pd.isEmpty()) pd = env.value(QStringLiteral("ALLUSERSPROFILE"));
    if (pd.isEmpty()) pd = QStringLiteral("C:/ProgramData");
    return QDir::fromNativeSeparators(pd)
           + QStringLiteral("/Epic/EpicGamesLauncher/Data/Manifests");
#else
    return QString();
#endif
}

static QString manifestsDir(const QString& manifestsRoot)
{
    return manifestsRoot.isEmpty() ? defaultManifestsRoot() : manifestsRoot;
}

// Manifest paths are Windows-style regardless of the host OS (QDir::fromNativeSeparators is a no-op
// off-Windows), so convert backslashes explicitly.
static QString winPathToSlash(QString p)
{
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p;
}

EpicGame EpicLibrary::parseManifest(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return {};                                  // malformed JSON -> dropped
    const QJsonObject o = doc.object();

    const QString appName = o.value(QStringLiteral("AppName")).toString();
    const QString display = o.value(QStringLiteral("DisplayName")).toString();
    const QString install = o.value(QStringLiteral("InstallLocation")).toString();
    if (appName.isEmpty() || display.isEmpty() || install.isEmpty()) return {}; // not a real installed app

    // Still downloading -> not launchable yet.
    if (o.value(QStringLiteral("bIsIncompleteInstall")).toBool()) return {};

    // DLC: MainGameAppName points at a DIFFERENT parent app. A base game leaves it empty or equal to AppName.
    const QString mainGame = o.value(QStringLiteral("MainGameAppName")).toString();
    if (!mainGame.isEmpty() && mainGame != appName) return {};

    // Game vs engine/plugin: the "AppCategories" array must contain "games". Unreal Engine / Quixel Bridge /
    // Fab Plugin carry "engines"/"plugins" (or none) and are filtered here.
    bool isGame = false;
    for (const QJsonValue& c : o.value(QStringLiteral("AppCategories")).toArray())
        if (c.toString().compare(QStringLiteral("games"), Qt::CaseInsensitive) == 0) { isGame = true; break; }
    if (!isGame) return {};

    return { appName, display, winPathToSlash(install) };
}

bool EpicLibrary::isAvailable(const QString& manifestsRoot)
{
    QDir d(manifestsDir(manifestsRoot));
    if (!d.exists()) return false;
    return !d.entryList({ QStringLiteral("*.item") }, QDir::Files).isEmpty();
}

QVector<EpicGame> EpicLibrary::installedGames(const QString& manifestsRoot)
{
    QHash<QString, EpicGame> byApp; // AppName -> game (dedupe a duplicated manifest)
    QDir d(manifestsDir(manifestsRoot));
    if (d.exists())
    {
        const QStringList items = d.entryList({ QStringLiteral("*.item") }, QDir::Files);
        for (const QString& m : items)
        {
            QFile f(d.filePath(m));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const EpicGame g = parseManifest(f.readAll());
            if (!g.appName.isEmpty()) byApp.insert(g.appName, g); // empty appName = filtered
        }
    }

    QVector<EpicGame> out;
    out.reserve(byApp.size());
    for (auto it = byApp.constBegin(); it != byApp.constEnd(); ++it) out.push_back(it.value());
    std::sort(out.begin(), out.end(), [](const EpicGame& a, const EpicGame& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString EpicLibrary::launchUrl(const QString& appName)
{
    return QStringLiteral("com.epicgames.launcher://apps/") + appName
           + QStringLiteral("?action=launch&silent=true");
}
