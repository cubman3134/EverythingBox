// Ubisoft Connect games installed on this machine (issue #60, increment 1). The fourth installed-library
// importer after Steam / Epic / GOG / Battle.net, and shaped like Battle.net's: a PURE parser over plain data,
// fed by a thin Windows-only registry reader, so every rule is probe-testable with no Ubisoft client installed.
//
// THE REGISTRY LAYOUT (read, never written). Ubisoft Connect is a 32-bit program, so its keys live in the
// WOW6432Node view on a 64-bit Windows:
//   HKLM\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\<id>          InstallDir       (the authority)
//   HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Uplay Install <id>
//                                                                    DisplayName, InstallLocation
// The 64-bit views of both are read as well (Playnite falls back to the 64-bit Installs key when the 32-bit
// one is absent). Sources: Playnite's UplayLibrary (Installs\<id>\InstallDir, name = install folder), the GOG
// Galaxy Ubisoft integration (Installs\<id>\InstallDir, uplay:// verbs), Rayman Control Panel's finder and
// Solaire/genie (the "Uplay Install <id>" uninstall key's DisplayName / InstallLocation in the 32-bit view),
// and CataclysmGame/game-scanner's notes (uplay://launch/<id>/0).
//
// LAUNCH is uplay://launch/<id>/0 handed to the OS — the same fire-and-forget URL route Epic takes. The id is
// the Installs subkey name, which is always a decimal number; anything else is refused rather than pasted into
// a URL.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

struct UbisoftGame
{
    QString id;          // the Ubisoft install id (the Installs subkey / "Uplay Install <id>" suffix), digits only
    QString name;        // DisplayName, else the install folder's name
    QString installDir;  // forward slashes, no trailing slash
    // False when shown from the persisted last-good scan because the source was UNREADABLE (issue #62). Default
    // true, added LAST so positional aggregate construction still compiles (the BattleNetGame precedent).
    bool    available = true;
};

// One registry key's worth of the snapshot, as plain data. The live reader produces these; a probe (or the
// EB_UITEST fixture seam) builds them by hand.
struct UbisoftRegRecord
{
    enum Source { Installs, Uninstall };
    Source  source = Installs;
    QString keyName;          // Installs: "<id>".  Uninstall: "Uplay Install <id>" (other keys are ignored).
    QString installDir;       // Installs\<id>\InstallDir
    QString displayName;      // Uninstall\Uplay Install <id>\DisplayName
    QString installLocation;  // Uninstall\Uplay Install <id>\InstallLocation
};

namespace UbisoftLibrary
{
    // True for a usable Ubisoft install id: non-empty, ASCII digits only, at most ten of them. An id is pasted
    // into the launch URL and the "ubi:" record key, so an odd one is refused, never "cleaned up".
    bool isValidId(const QString& id);

    // "uplay://launch/<id>/0"; EMPTY for an id isValidId refuses.
    QString launchUri(const QString& id);

    // The install id a launch or a Recent names: from its record key ("ubi:<id>") when it has one, else from
    // a uplay://launch/<id>/... URI. EMPTY unless the result passes isValidId.
    QString idFrom(const QString& key, const QString& uri);

    // THE PURE PARSER. Rules, in order:
    //   * an Installs record's id is its key name; an Uninstall record's is what follows "Uplay Install" (any
    //     other uninstall key is not Ubisoft's and is skipped). Both are trimmed, then must pass isValidId;
    //   * records are merged BY ID (the two hives, and both registry views, describe the same game), the first
    //     non-empty value of each field winning — so a duplicate id is ONE game;
    //   * the install dir is InstallDir, falling back to the uninstall InstallLocation only when InstallDir is
    //     empty; a game with neither, or whose dir does not exist on disk, is NOT installed and is dropped;
    //   * the name is DisplayName, falling back to the install folder's own name;
    //   * the result is sorted by name (case-insensitively), then id, so it does not depend on registry order.
    QVector<UbisoftGame> parseSnapshot(const QVector<UbisoftRegRecord>& snapshot);

    // The fixture form of a snapshot:
    //   { "installs":  [ { "key": "<id>", "InstallDir": "..." } ],
    //     "uninstall": [ { "key": "Uplay Install <id>", "DisplayName": "...", "InstallLocation": "..." } ] }
    // Malformed JSON yields an empty snapshot.
    QVector<UbisoftRegRecord> snapshotFromJson(const QByteArray& json);

    // The live registry reader: Windows only. Off Windows it is compiled but returns NOTHING (there is no
    // Ubisoft Connect to read), and hasLiveReader() says so.
    QVector<UbisoftRegRecord> readRegistrySnapshot();
    bool                      hasLiveReader();

    // What the app reads: the registry — or, ONLY with EB_UITEST set, the fixture file named by
    // EB_UITEST_UBISOFT_FIXTURE (the #80 / #98 test-channel shape). Never both.
    QVector<UbisoftRegRecord> currentSnapshot();

    QVector<UbisoftGame> installedGames();   // parseSnapshot(currentSnapshot())
    bool                 isAvailable();      // at least one installed game
}
