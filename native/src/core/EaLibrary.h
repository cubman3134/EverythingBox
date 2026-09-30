// EA app (EA Desktop) games installed on this machine (issue #60, increment 2). The fifth installed-library
// importer after Steam / Epic / GOG / Battle.net / Ubisoft, shaped like Ubisoft's: a PURE parser over plain
// data, fed by a thin Windows-only reader, so every rule is probe-testable with no EA app installed.
//
// THE DATA (read, never written). Every game the EA app (and the Origin client before it) installs carries
// its own manifest, <install dir>/__Installer/installerdata.xml:
//   <DiPManifest>                                     (the current form; the legacy Origin form is <game>)
//     <contentIDs><contentID>1026023</contentID>…</contentIDs>          the id the launch URI takes
//     <gameTitles><gameTitle locale="en_US">Battlefield 1</gameTitle>…</gameTitles>
//   <game><metadata><localeInfo locale="en_US"><title>…</title></localeInfo></metadata>
//         <contentIDs><contentID>…</contentID></contentIDs></game>      (legacy)
// The install dirs themselves come from two places:
//   * the Windows Uninstall entry the EA installer writes for each game (32-bit view, and the 64-bit one for
//     completeness): InstallLocation + DisplayName, recognised by its UninstallString — the game's own
//     "…EAInstaller\<title>\Cleanup.exe" uninstall_game -autologging (or __Installer\Cleanup.exe);
//   * every folder directly under an EA library root (<Program Files>\EA Games, and the x86 one).
// Sources: Lutris's EA app service (lutris/services/ea_app.py: every folder of Program Files/EA Games,
// __Installer/installerdata.xml, contentIDs/contentID), Steam ROM Manager's EA Desktop parser
// (ea-desktop.parser.ts: */__Installer/installerdata.xml, DiPManifest/gameTitles/gameTitle + contentIDs, and
// the legacy game/metadata/localeInfo/title form), real EA registry exports (the Uninstall\{GUID} key with
// Publisher "Electronic Arts", InstallLocation and the EAInstaller Cleanup.exe uninstall_game string), and
// ODZEN's EA scanner (the same Uninstall recognition plus installerdata.xml).
//
// NOT READ: EA Desktop's own install list, C:\ProgramData\EA Desktop\<hash>\IS. It is AES-encrypted with a key
// derived from this machine's hardware ids (GameFinder's EADesktop handler; genie notes the derivation fails
// against real files), so it is neither plain data nor stable. The legacy Origin LocalContent\*.mfst files are
// not read either: they are Origin's, and an Origin-era install already has the Uninstall entry and the
// installerdata.xml read here.
//
// LAUNCH is origin2://game/launch?offerIds=<content id>&autoDownload=1 handed to the OS — the form Lutris's EA
// app service and the GOG Galaxy Origin integration send (Steam ROM Manager and CataclysmGame/game-scanner use
// the same origin2://game/launch?offerIds= route). The EA app still registers origin2:. The id is refused unless
// it is a plain token (see isValidId), so nothing odd is ever pasted into the URI.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

struct EaGame
{
    QString id;          // the game's content id (installerdata.xml's first valid contentID)
    QString name;        // the manifest's game title, else the Uninstall DisplayName, else the folder name
    QString installDir;  // forward slashes, no trailing slash
    // False when shown from the persisted last-good scan because the source was UNREADABLE (issue #62). Default
    // true, added LAST so positional aggregate construction still compiles (the BattleNetGame precedent).
    bool    available = true;
};

// One candidate install, as plain data. The live reader produces these; a probe (or the EB_UITEST fixture
// seam, through gatherSnapshot) builds them by hand.
struct EaInstallRecord
{
    enum Source { Uninstall, LibraryFolder };
    Source     source = LibraryFolder;
    QString    keyName;          // the Uninstall subkey name, or the library folder's own name (informational)
    QString    installDir;       // Uninstall: InstallLocation.  LibraryFolder: the folder itself.
    QString    displayName;      // Uninstall: DisplayName
    QString    uninstallString;  // Uninstall: UninstallString — what marks an Uninstall entry as an EA game's
    QByteArray installerData;    // <installDir>/__Installer/installerdata.xml, raw (UTF-8 or UTF-16); empty = none
};

namespace EaLibrary
{
    // What one installerdata.xml says. `ids` are every contentID in document order, trimmed (unvalidated);
    // `title` is the en_US title when there is one, else the first non-empty title. `ok` is false when the XML
    // does not parse.
    struct InstallerData { bool ok = false; QStringList ids; QString title; };
    InstallerData parseInstallerData(const QByteArray& xml);

    // True for a usable content id: 1–64 characters of [A-Za-z0-9._:-], starting with a letter or digit. It
    // is pasted into the launch URI and the "ea:" record key, so an odd one is refused, never "cleaned up".
    bool isValidId(const QString& id);

    // True when an Uninstall entry is an EA game's: its UninstallString runs the EA installer's cleanup
    // ("EAInstaller" or the "uninstall_game" verb). The EA app's own entry has neither.
    bool isEaUninstallString(const QString& uninstallString);

    // "origin2://game/launch?offerIds=<id>&autoDownload=1"; EMPTY for an id isValidId refuses.
    QString launchUri(const QString& id);

    // The content id a launch or a Recent names: from its record key ("ea:<id>") when it has one, else from
    // an origin2://game/launch?offerIds=<id>[,…] URI (the first id). EMPTY unless it passes isValidId.
    QString idFrom(const QString& key, const QString& uri);

    // THE PURE PARSER. Rules, in order:
    //   * an Uninstall record counts only when isEaUninstallString; a LibraryFolder record always does;
    //   * the dir is normalised (forward slashes, no trailing one); a record with no dir, or whose dir does not
    //     exist on disk, is NOT installed and is dropped;
    //   * the id is the first contentID in its installerdata.xml that passes isValidId — no manifest, an
    //     unparseable one, or no valid id: dropped (there is nothing to launch it by);
    //   * records are merged BY ID (an Uninstall entry and a library folder, or both registry views, describe
    //     the same game): the first installed record's dir wins, so a duplicate id is ONE game;
    //   * the name is the manifest title, else the Uninstall DisplayName, else the install folder's own name;
    //   * the result is sorted by name (case-insensitively), then id, so it does not depend on enumeration order.
    QVector<EaGame> parseSnapshot(const QVector<EaInstallRecord>& snapshot);

    // Turn candidate installs into a full snapshot, touching only the filesystem: every immediate subfolder of
    // each library root becomes a LibraryFolder record, and every record whose installerData is empty gets its
    // <installDir>/__Installer/installerdata.xml read (capped; a missing file stays empty). Portable.
    QVector<EaInstallRecord> gatherSnapshot(const QVector<EaInstallRecord>& uninstall, const QStringList& libraryRoots);

    // The fixture form of the seam's input:
    //   { "uninstall": [ { "key": "{GUID}", "DisplayName": "...", "InstallLocation": "...",
    //                      "UninstallString": "..." } ],
    //     "libraryRoots": [ "C:/Program Files/EA Games" ] }
    // Malformed JSON yields nothing.
    QVector<EaInstallRecord> uninstallFromJson(const QByteArray& json);
    QStringList              libraryRootsFromJson(const QByteArray& json);

    // The live reader: Windows only (both HKLM Uninstall views + the EA library roots, then gatherSnapshot).
    // Off Windows it is compiled but returns NOTHING (there is no EA app to read), and hasLiveReader() says so.
    QVector<EaInstallRecord> readLiveSnapshot();
    bool                     hasLiveReader();

    // What the app reads: the live data — or, ONLY with EB_UITEST set, the fixture file named by
    // EB_UITEST_EA_FIXTURE (the #80 / #98 test-channel shape), put through gatherSnapshot. Never both.
    QVector<EaInstallRecord> currentSnapshot();

    QVector<EaGame> installedGames();   // parseSnapshot(currentSnapshot())
    bool            isAvailable();      // at least one installed game
}
