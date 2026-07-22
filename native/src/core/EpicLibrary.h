// Reads the locally-installed Epic Games Store library (no login / network needed): parses the launcher's
// per-app manifest files and lists installed *games*. Games launch through the Epic launcher URI scheme
// (com.epicgames.launcher://apps/<AppName>?action=launch&silent=true) handed to the OS — fire-and-forget,
// exactly like steam://. QtCore-only (QDir/QFile/QJson), so it links into the app and runs headless in CI.
//
// The manifests live at %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item (one JSON object per
// installed app). The manifests dir is injectable (manifestsRoot override) so the probe parses fixtures in a
// temp dir and never touches the real launcher.
//
// Game vs non-game discriminator (documented from real manifests on this machine — Unreal Engine / Quixel
// Bridge / Fab Plugin all fail it): an entry is a launchable game iff its "AppCategories" array contains
// "games", it is not still downloading (bIsIncompleteInstall == false), and it is a BASE game rather than DLC
// (MainGameAppName empty or equal to AppName — a non-empty MainGameAppName that differs points at a parent
// game, i.e. the entry is DLC). Engine components/plugins carry categories like "engines"/"plugins" (or none)
// and are filtered out.
#pragma once
#include <QString>
#include <QVector>
#include <QByteArray>

struct EpicGame
{
    QString appName;         // Epic AppName (the launch id) — e.g. "Fortnite"
    QString name;            // DisplayName (label)
    QString installLocation; // absolute install dir (for existence checks / future art)
};

namespace EpicLibrary
{
    // The default manifests dir (%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests) exists and holds at least
    // one .item file. A non-empty manifestsRoot override points detection at a fixture dir (tests) instead.
    bool isAvailable(const QString& manifestsRoot = QString());

    // Installed games from every *.item under the manifests dir (deduped by AppName, sorted by name; non-games
    // and DLC filtered per the discriminator above). Empty manifestsRoot = the real %ProgramData% location.
    QVector<EpicGame> installedGames(const QString& manifestsRoot = QString());

    // com.epicgames.launcher://apps/<AppName>?action=launch&silent=true — the URI the launcher answers to.
    QString launchUrl(const QString& appName);

    // Pure parse of one .item JSON body -> an EpicGame. A filtered/invalid entry (non-game, DLC, incomplete,
    // malformed JSON, or missing AppName/DisplayName/InstallLocation) returns an EpicGame with an EMPTY appName
    // — the caller drops those. Exposed so the discriminator is probe-testable without a manifests dir.
    EpicGame parseManifest(const QByteArray& json);
}
