// Reads the locally-installed GOG library from the Windows registry (works with or WITHOUT GOG Galaxy — the
// classic offline installers write the same keys). Each game records gameName / path / exe under
// HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<id>. GOG games are DRM-free plain executables, so a launch is just
// the resolved exe run through the app's MONITORED PC-exe path (launchPcExe) — NOT a fire-and-forget URI.
// QtCore-only (QSettings), so it links into the app and runs headless in CI.
//
// WOW6432Node is the 64-bit registry VIEW of the 32-bit hive: the 32-bit GOG installer writes to
// HKLM\SOFTWARE\GOG.com and Windows redirects it there, so a 64-bit process must read the WOW6432Node path
// EXPLICITLY (QSettings NativeFormat honours the literal key path — the spelling matters).
//
// Detection is injectable for tests (the ExternalPlayer regProbeRoot precedent): a non-empty regProbeRoot
// reads a fake registry — an INI file whose GROUPS are game ids and whose keys are gameName/path/exe — instead
// of the live HKLM hive, so the probe never touches the real registry.
#pragma once
#include <QString>
#include <QVector>

struct GogGame
{
    QString id;         // GOG gameID (the registry subkey name)
    QString name;       // gameName (label)
    QString exe;        // absolute path to the game exe (launch target — run via launchPcExe)
    QString installDir; // path (install folder)
};

namespace GogLibrary
{
    // At least one game is registered under HKLM\SOFTWARE\WOW6432Node\GOG.com\Games (or the injected fake).
    bool isAvailable(const QString& regProbeRoot = QString());

    // Installed games from the registry (sorted by name). Each entry needs a non-empty gameName + exe. An
    // empty regProbeRoot reads the real hive; a non-empty one reads the fake-registry INI (test fixture).
    // Non-Windows with no regProbeRoot returns {} (no registry).
    QVector<GogGame> installedGames(const QString& regProbeRoot = QString());
}
