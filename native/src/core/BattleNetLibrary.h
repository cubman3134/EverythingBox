// Battle.net games installed by the Blizzard client. Detection reads the standard Windows Uninstall hive
// (Publisher == "Blizzard Entertainment") rather than Blizzard's undocumented binary product.db: the registry
// is stable, documented-by-convention, and — like GogLibrary — swappable for an INI fixture so the parse is
// probe-testable with no launcher installed. Launch prefers battlenet://<code> for titles in the curated
// code map, else the install-dir exe through the monitored launchPcExe path (the GOG mechanic).
#pragma once
#include <QString>
#include <QVector>

struct BattleNetGame
{
    QString code;        // battlenet:// launch code ("wow", "d3", …). EMPTY when the title has no known code.
    QString name;        // DisplayName
    QString installDir;  // InstallLocation, forward slashes
    QString exe;         // best-effort game exe under installDir (launch fallback); may be empty
};

namespace BattleNetLibrary
{
    // regProbeRoot empty => the live Uninstall hive; non-empty => a fake-registry INI (groups = uninstall
    // subkeys, keys = DisplayName/Publisher/InstallLocation), exactly the GogLibrary probe seam.
    bool                   isAvailable(const QString& regProbeRoot = QString());
    QVector<BattleNetGame> installedGames(const QString& regProbeRoot = QString());

    QString launchUri(const QString& code);              // "battlenet://" + code
    QString codeForTitle(const QString& displayName);    // curated title -> code; empty when unknown

    // Pure discriminator (the EpicLibrary::parseManifest seam): a non-Blizzard publisher yields an entry with
    // an EMPTY name, which callers drop. Exposed so the filter is probe-testable without any registry.
    BattleNetGame parseUninstallEntry(const QString& displayName, const QString& publisher,
                                      const QString& installLocation);
}
