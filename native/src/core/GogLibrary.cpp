#include "GogLibrary.h"

#include <QSettings>
#include <QDir>
#include <algorithm>

namespace {

// Registry values are Windows-style paths regardless of the host OS (QDir::fromNativeSeparators is a
// no-op off-Windows), so convert backslashes explicitly.
QString winPathToSlash(QString p)
{
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p;
}

// Read one GOG game's fields for id `id`, either from the fake-registry INI (regProbeRoot non-empty) or the
// live 64-bit-view hive. The INI mirrors the registry shape: a [<id>] group with gameName/path/exe keys.
GogGame readGame(const QString& regProbeRoot, const QString& id)
{
    QString name, path, exe;
    if (!regProbeRoot.isEmpty())
    {
        QSettings ini(regProbeRoot, QSettings::IniFormat);
        name = ini.value(id + QStringLiteral("/gameName")).toString();
        path = ini.value(id + QStringLiteral("/path")).toString();
        exe  = ini.value(id + QStringLiteral("/exe")).toString();
    }
    else
    {
#ifdef Q_OS_WIN
        // Read the value names directly under the game's own key. NativeFormat honours the literal WOW6432Node
        // path — the 64-bit view where the 32-bit GOG installer's writes actually land.
        QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\GOG.com\\Games\\") + id,
                      QSettings::NativeFormat);
        name = reg.value(QStringLiteral("gameName")).toString();
        path = reg.value(QStringLiteral("path")).toString();
        exe  = reg.value(QStringLiteral("exe")).toString();
#endif
    }
    return { id, name, winPathToSlash(exe), winPathToSlash(path) };
}

// The set of registered game ids (registry subkeys under ...\GOG.com\Games, or the fake INI's groups).
QStringList gameIds(const QString& regProbeRoot)
{
    if (!regProbeRoot.isEmpty())
    {
        QSettings ini(regProbeRoot, QSettings::IniFormat);
        return ini.childGroups();
    }
#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\GOG.com\\Games"),
                  QSettings::NativeFormat);
    return reg.childGroups();
#else
    return {};
#endif
}

} // namespace

bool GogLibrary::isAvailable(const QString& regProbeRoot)
{
    return !installedGames(regProbeRoot).isEmpty();
}

QVector<GogGame> GogLibrary::installedGames(const QString& regProbeRoot)
{
    QVector<GogGame> out;
    for (const QString& id : gameIds(regProbeRoot))
    {
        const GogGame g = readGame(regProbeRoot, id);
        if (g.name.isEmpty() || g.exe.isEmpty()) continue; // an incomplete/uninstalled key -> skip
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const GogGame& a, const GogGame& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}
