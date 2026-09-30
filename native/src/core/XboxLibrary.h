// Xbox app / PC Game Pass games installed on this machine (issue #60, increment 3). The sixth installed-library
// importer after Steam / Epic / GOG / Battle.net / Ubisoft / EA, shaped like theirs: a PURE parser over plain
// data, fed by a thin Windows-only reader, so every rule is probe-testable with no Xbox app installed.
//
// WHERE THE GAMES ARE (read, never written). The Xbox app installs PC games into "gaming roots": a folder on a
// drive (by default <drive>:\XboxGames) that the app records in a hidden <drive>:\.GamingRoot file:
//   uint32 LE magic 0x58424752 ("RGBX"), uint32 LE folder count, then that many NUL-terminated UTF-16LE folder
//   paths, relative to the drive root (a real file names "XboxGames").
// Every folder directly under a gaming root is one package; its AppxManifest.xml sits in the folder itself or
// in its Content\ subfolder. Older "advanced management" installs live the same way under
// <drive>:\Program Files\ModifiableWindowsApps.
// Sources: GameFinder's Xbox handler (erri120/GameFinder, GameFinder.StoreHandlers.Xbox/XboxHandler.cs:
// GetAppFolders = every drive's ".GamingRoot" + "Program Files\ModifiableWindowsApps"; ParseGamingRootFile =
// the byte layout above, with a test fixture of exactly those bytes; FindAllGames = appxmanifest.xml in the
// folder or its Content\ subfolder), and Playnite's UWP enumeration (JosefNemec/Playnite, Common/Programs2.cs
// GetUWPApps: Package/Identity, Properties/DisplayName, the FIRST Applications/Application's Id).
//
// GAMES VS OTHER PACKAGES. A package is a game when a MicrosoftGame.config sits beside its AppxManifest.xml,
// its root element is <Game>, and its <ExecutableList> names at least one <Executable>. MicrosoftGame.config is
// the Microsoft GDK's per-title configuration, which every GDK-packaged PC game ships (microsoft/Xbox-GDK-Samples,
// e.g. SimpleTriangleDesktop's <Game><ExecutableList><Executable Id="Game"/>); an ordinary Store app has none.
// A GDK DLC package carries one too, but with no <ExecutableList> (the same samples' DLCPackage/Package_PC) — it
// is content for a game, not something to launch, so it is not a game here. The gaming root alone is NOT used
// as the signal: it is a folder anyone can put anything in. (Consequence: a legacy UWP-only title with no
// MicrosoftGame.config is not listed. GameFinder lists every folder under the roots; this is stricter.)
//
// THE PACKAGE FAMILY NAME is <Identity Name>_<publisher id>, where the publisher id is the 13-character
// Crockford base32 (lowercase, 0-9a-z without i l o u) of the first 8 bytes of SHA-256 over the Identity
// Publisher string in UTF-16LE, with one zero bit appended. Checked against this machine's installed packages
// ("CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" -> 8wekyb3d8bbwe, and
// four others); probe_importers pins those pairs.
//
// LAUNCH is shell:AppsFolder\<PFN>!<AppId> — the package's Application User Model ID (AUMID) under the shell's
// Apps folder, which is how Playnite starts one (explorer.exe with that argument). The AppId comes from the
// manifest; see parseSnapshot for which one when there are several. Both halves are refused unless they are
// the plain tokens the manifest schema allows, so nothing odd is ever pasted into the string.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

struct XboxGame
{
    QString id;          // the AUMID "<PFN>!<AppId>": the launch string's payload and the "xbox:" record key
    QString pfn;         // package family name
    QString appId;       // the chosen <Application Id>
    QString name;        // the display name, else the install folder's name, else the PFN
    QString installDir;  // the package root (the folder holding AppxManifest.xml); forward slashes, no trailing one
    // False when shown from the persisted last-good scan because the source was UNREADABLE (issue #62). Default
    // true, added LAST so positional aggregate construction still compiles (the BattleNetGame precedent).
    bool    available = true;
};

// One <Application> of a manifest. `listed` is false when its VisualElements say AppListEntry="none" (the app
// is kept out of Start's list — a helper, not what the user opens).
struct XboxApp
{
    QString id;
    bool    listed = true;
};

// One installed package, as plain data. The live reader produces these (through recordFromManifests); a probe
// builds them by hand.
struct XboxPackageRecord
{
    QString          pfn;          // package family name
    QString          installDir;   // the package root
    QString          displayName;  // the manifest's (or MicrosoftGame.config's) display name; may be empty
    QVector<XboxApp> apps;         // the manifest's <Application>s, in document order
    bool             isGame = false;
};

namespace XboxLibrary
{
    // What one AppxManifest.xml says, matched by LOCAL name (the foundation namespace has had several URIs).
    // `ok` is false when the XML does not parse or its root is not <Package>.
    struct AppxManifest { bool ok = false; QString name; QString publisher; QString displayName; QVector<XboxApp> apps; };
    AppxManifest parseAppxManifest(const QByteArray& xml);

    // What one MicrosoftGame.config says. `isGame`: its root is <Game> and <ExecutableList> names at least one
    // <Executable>. `displayName`: <ShellVisuals DefaultDisplayName>.
    struct GameConfig { bool ok = false; bool isGame = false; QString displayName; };
    GameConfig parseGameConfig(const QByteArray& xml);

    // The 13-character publisher id of an Identity Publisher string (see the header note); EMPTY for an empty one.
    QString publisherId(const QString& publisher);
    // "<name>_<publisherId(publisher)>"; EMPTY unless the result passes isValidPfn.
    QString familyName(const QString& name, const QString& publisher);

    // A package family name: a 3–50 character package name of [A-Za-z0-9.-], "_", and 13 characters of the
    // lowercase Crockford alphabet. Anything else is refused, never cleaned up.
    bool isValidPfn(const QString& pfn);
    // An <Application Id>: dot-separated parts, each a letter then letters/digits, at most 64 characters.
    bool isValidAppId(const QString& appId);

    // "shell:AppsFolder\<PFN>!<AppId>" for an AUMID "<PFN>!<AppId>"; EMPTY unless both halves are valid.
    QString launchUri(const QString& aumid);

    // The AUMID a launch or a Recent names: from its record key ("xbox:<AUMID>") when it has one, else from a
    // "shell:AppsFolder\<AUMID>" string. EMPTY unless both halves are valid.
    QString idFrom(const QString& key, const QString& uri);

    // The folders one .GamingRoot file names, relative to its drive root, in file order. EMPTY for a wrong
    // magic, a truncated file, or an implausible count.
    QStringList parseGamingRoot(const QByteArray& bytes);

    // One package folder's record from its two files' bytes (pure): the PFN from the manifest's Identity, the
    // display name (the manifest's, else MicrosoftGame.config's; an "ms-resource:" reference counts as none),
    // the apps, and isGame from the config (an empty or unparseable config: not a game).
    XboxPackageRecord recordFromManifests(const QString& installDir, const QByteArray& appxManifest,
                                          const QByteArray& gameConfig);

    // THE PURE PARSER. Rules, in order:
    //   * a record that is not a game (isGame false) is dropped;
    //   * the dir is normalised (forward slashes, no trailing one); no dir, or a dir that does not exist on
    //     disk: not installed, dropped;
    //   * the PFN must pass isValidPfn, or the record is dropped;
    //   * the AppId is the FIRST listed <Application> (document order) whose id is valid, else the first valid
    //     one at all (Playnite takes the first; a helper hidden from Start is passed over when there is a
    //     listed one); no valid id: dropped (there is nothing to launch);
    //   * records merge BY PFN (two roots can name the same folder): the first installed one wins;
    //   * the name is the display name, else the install folder's name (its parent's when the manifest sat in
    //     Content\), else the PFN; an "ms-resource:" display name counts as missing;
    //   * the result is sorted by name (case-insensitively), then id.
    QVector<XboxGame> parseSnapshot(const QVector<XboxPackageRecord>& snapshot);

    // Every package folder under these drive roots: each root's .GamingRoot folders, plus
    // "<root>/Program Files/ModifiableWindowsApps" when it exists. Touches only the filesystem; portable.
    QStringList libraryFoldersForRoots(const QStringList& driveRoots);

    // A record for every immediate subfolder of each library folder that holds an AppxManifest.xml (in itself or
    // in Content\), with MicrosoftGame.config read from beside it (each file capped). Portable.
    QVector<XboxPackageRecord> gatherSnapshot(const QStringList& libraryFolders);

    // The fixture form of the seam's input: { "driveRoots": [ "D:/", ... ] }. Malformed JSON yields nothing.
    QStringList driveRootsFromJson(const QByteArray& json);

    // The live reader: Windows only (every fixed / removable drive's roots, then gatherSnapshot). Read-only, no
    // admin, no package API. Off Windows it is compiled but returns NOTHING, and hasLiveReader() says so.
    QVector<XboxPackageRecord> readLiveSnapshot();
    bool                       hasLiveReader();

    // What the app reads: the live data — or, ONLY with EB_UITEST set, the drive roots named by the fixture file
    // EB_UITEST_XBOX_FIXTURE (the #80 / #98 test-channel shape), put through the same gather. Never both.
    QVector<XboxPackageRecord> currentSnapshot();

    QVector<XboxGame> installedGames();   // parseSnapshot(currentSnapshot())
    bool              isAvailable();      // at least one installed game
}
