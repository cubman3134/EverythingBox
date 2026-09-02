// Standalone (external) emulators that EverythingBox launches as a child process - the RetroBat / ES-DE
// model: keep a copy of each emulator under <emulators-root>/<id>/, run it with the ROM, and monitor it
// until it exits. Used for systems that can't run as an in-process libretro core (e.g. GameCube/Wii via
// Dolphin, which is hardware-rendered). Cf. SystemCatalog (in-process libretro cores).
//
// DATA-DRIVEN (issue #52, mirroring SystemCatalog / #92). The table below is the BUILT-IN base — the
// emulators the app ships knowing how to auto-install. On top of it, `<data>/emulators/*.json` may ADD a
// user's own emulator (point it at a binary they already have) or OVERRIDE fields of a built-in one WITHOUT
// a rebuild. The merge is byte-for-byte SystemCatalog's:
//   * a data entry whose `id` is not in the built-in table is APPENDED as a new emulator;
//   * a data entry whose `id` matches a built-in overrides ONLY the fields it names (field-level, so a file
//     can swap `argsTemplate` alone without restating the binaries);
//   * a malformed file (bad JSON, wrong top-level type, an entry with no `id`) is LOGGED AND SKIPPED — it
//     can never crash startup and can never drop the built-in table.
// With NO data files present, all() is byte-for-byte the built-in table: probe_useremulators pins that
// round-trip (built-in -> JSON -> in-memory == built-in) and the no-regression property.
//
// AUTO-INSTALL STAYS A BUILT-IN PRIVILEGE. A user entry points at a binary they already have (an absolute
// path in `binary`) and carries no update URL, so hasInstallSource() is false for it: the download/extract
// machinery is never entered, and resolveBinaryFrom() returns the absolute path directly so it reads as
// installed with no fetch. A built-in (or an override of one) keeps its update URL and installs as before.
//
// WIRING AN EMULATOR TO A SYSTEM. A ROM routes to a standalone emulator through GameSystem::externalEmulator
// (SystemCatalog), which holds an emulator id. So to make a user emulator launchable for a system, point that
// system at the emulator id via `<data>/systems/*.json` (#92) — e.g. add/override a system with
// `"externalEmulator":"myemu"`. Because byId() returns the MERGED registry, that id resolves to the user
// entry and GameLauncher launches it.
//
// THERE ARE TWO BINDINGS, and BOTH are load-bearing (the `systems` field is no longer informational metadata):
//   * GameSystem::externalEmulator — this emulator is the system's DEFAULT engine (gc -> Dolphin). A ROM on
//     that system launches it with no user choice.
//   * ExternalEmulator::systems — this emulator CAN RUN those systems, which makes it SELECTABLE for them
//     without moving their default. EmulationTarget.h's boundEmulatorsFor reads it: emulationTargetsFor offers
//     a standalone target for every emulator bound this way (after the system's cores, so the default still
//     leads) and resolveEmulationTarget's standalone arm accepts an explicit pick of one. The built-in case is
//     ares/n64: Nintendo 64 keeps mupen64plus_next as its default because that is the only N64 engine
//     RetroAchievements works on, and ares is offered beside it. A user's own emulator JSON declaring
//     `"systems":["snes"]` binds exactly the same way. (`extensions` remains informational.)
// The args string is cut shell-style, so an argument that must contain a space goes in double quotes
// ("argsTemplate": "--config \"My Profile\" {rom}") - see argsTemplate below. A ready-to-copy example ships at
// native/resources/emulators/example-emulators.json; to make one launch, drop it in <data>/emulators/ and
// add a matching system to <data>/systems/ (e.g. {"id":"arcade","externalEmulator":"mame-standalone",
// "extensions":["zip"]}) so a ROM in that system routes to the user emulator.
#pragma once
#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <functional>
#include "AppPaths.h"

// ---- the NATIVE-PORT game binding (issue #233) -----------------------------------------------------------
// A native port (a static recompilation of one N64 title into a PC executable — Zelda64Recomp and its
// siblings) is structurally a standalone emulator that can run EXACTLY ONE GAME. Everything else about it is
// an ExternalEmulator already: a GitHub release, per-OS artifacts, an install folder, a binary to spawn. The
// ONE new concept is this: it binds to a GAME, not to a system.
//
// THAT DISTINCTION IS LOAD-BEARING, and it is why this is a separate field from `systems` rather than a value
// in it. `systems` means "this emulator can run every game on that system", and EmulationTarget.h's
// boundEmulatorsFor turns it into a picker row on EVERY game of that system. A port put there would offer
// Zelda64Recomp on Super Mario 64. So a port leaves `systems` EMPTY and fills this instead; the two bindings
// are read by different code (boundEmulatorsFor vs NativePorts::portForGame) and can never be confused.
//
// THE FIELD NAMES ARE NOT OURS, AND THAT IS THE POINT. They are the per-title schema of the RetComM catalog
// (github.com/TechnicallyComputers/retcomm-catalog, SCHEMA.md), which already encodes exactly this binding —
// title identity, per-OS release assets, launch binaries — with hashes HashVerify can consume. Mirroring it
// means a later increment can read that feed unchanged instead of translating it. What that schema does NOT
// have is how the port TAKES the ROM, and `romDelivery` below is our one documented extension to it.
//
// Empty on every real emulator — `name` empty IS "this is not a port" (ExternalEmulator::isNativePort()).
struct NativePortBinding
{
    // ---- identity of the ONE game this port runs (RetComM: id/name/kind/platform)
    QString     name;       // the GAME's display name ("The Legend of Zelda: Majora's Mask"). NOT the port's
                            // name — the port is credited by its release owner, see ExternalEmulator::displayName.
    QString     kind;       // "recomp" | "decomp"
    QString     platform;   // SystemCatalog system id the game belongs to ("n64")
    QString     description;
    QString     authorNotes;// RetComM `author_notes` — the message from the port's own authors, shown to the user
    QString     notes;      // RetComM `notes` — maintainer footnotes; deliberately NOT shown to the user

    // ---- rom_identity. The digests are CARRIED AND DELIBERATELY NOT GATED ON in increment 1: the match is by
    // title/region and the port's own check is what refuses a wrong dump (Zelda64Recomp refuses one itself,
    // loudly), where hashing every candidate row would cost a full 32 MB read per row on a browse repaint.
    // Increment 2 turns these into a HashVerify gate — which is why they are here in HashVerify's own shapes.
    QStringList crc32, md5, sha1, sha256;
    QList<qint64> sizes;        // byte lengths of the accepted dump(s)
    // No-Intro / Redump basenames. RetComM calls these "search hints, not hard matching" for its own hub; for
    // US they are ALSO the title-match candidates and the ONLY place a region ("(USA)") is stated, because the
    // schema has no region field. NativePorts::acceptedRegions derives the region gate from these.
    QStringList filenames;
    QStringList discSerials;    // PSX-style serials; empty for N64
    bool        requireCue = false;
    QList<int>  trackCounts;
    QStringList romExtensions;  // RetComM `rom_extensions`, WITH the leading dot as that schema writes them

    // ---- release + install (RetComM: release.*, install_dir_name, launch.*)
    QString releaseRepo;        // release.github, "owner/repo". Its OWNER segment is the port's credited name.
    bool    allowPrerelease = false;
    QString assetGlobWindows, assetGlobLinux, assetGlobMacos;
    QString installDirName;     // RetComM's folder name under its apps/. EB installs under emulators/<id>/;
                                // carried so a consumed feed round-trips, honouring it is increment 2.
    QString launchWindows, launchLinux, launchMacos;

    // RetComM `build.generate.engine` — "snesrecomp" | "psxrecomp" | "gbarecomp". Its PRESENCE is what makes an
    // entry the SELF-COMPILED tier (issue #248): such a title ships no binary, it is compiled on this machine
    // from the named recompiler plus the user's own dump. Empty = the PRE-BUILT tier, which is every entry this
    // build ships and the only tier increment (a) can act on. Carried, and read for the row's tier label, so
    // increment (c) adds the build itself without reshaping the catalog or the row model.
    QString buildEngine;

    // ---- OUR EXTENSIONS to that schema, named so they read as ours.
    // How the port takes the ROM: "in_app_menu" = it asks in its own UI and converts the file itself
    // (Zelda64Recomp); "beside_exe" = the file must sit next to the executable; "cli_path" = it takes a path on
    // the command line. ONLY "in_app_menu" is implemented in increment 1 — the other two are spellable before
    // they are honourable, and NativePorts::romDeliverySupported is the ONE place that says which are live.
    QString romDelivery;
    // The port's own licence, as an SPDX-ish string ("GPL-3.0"). Shown on the Recomps row and on the card,
    // because a port is somebody else's program under somebody else's terms and #248 requires that to be
    // legible BEFORE it is fetched — psxrecomp's PolyForm Noncommercial being the case that makes it matter.
    // Empty is honest ("the catalogue does not say"), never a guess.
    QString license;
    // A release the catalogue PINS ("1.2.2"). Compared against the tag EB recorded when it installed this port
    // (NativePorts::readInstalledTag) to derive `update available`. Empty = the catalogue makes no claim about
    // which release is current, which is the shipped state: the live release lookup arrives with the feed in
    // increment (b), and this is the field it lands in.
    QString releaseTag;
};

inline bool operator==(const NativePortBinding& a, const NativePortBinding& b)
{
    return a.name == b.name && a.kind == b.kind && a.platform == b.platform && a.description == b.description
        && a.authorNotes == b.authorNotes && a.notes == b.notes
        && a.crc32 == b.crc32 && a.md5 == b.md5 && a.sha1 == b.sha1 && a.sha256 == b.sha256
        && a.sizes == b.sizes && a.filenames == b.filenames && a.discSerials == b.discSerials
        && a.requireCue == b.requireCue && a.trackCounts == b.trackCounts && a.romExtensions == b.romExtensions
        && a.releaseRepo == b.releaseRepo && a.allowPrerelease == b.allowPrerelease
        && a.assetGlobWindows == b.assetGlobWindows && a.assetGlobLinux == b.assetGlobLinux
        && a.assetGlobMacos == b.assetGlobMacos && a.installDirName == b.installDirName
        && a.launchWindows == b.launchWindows && a.launchLinux == b.launchLinux && a.launchMacos == b.launchMacos
        && a.buildEngine == b.buildEngine
        && a.romDelivery == b.romDelivery && a.license == b.license && a.releaseTag == b.releaseTag;
}
inline bool operator!=(const NativePortBinding& a, const NativePortBinding& b) { return !(a == b); }

struct ExternalEmulator
{
    QString id;            // stable key, also the "emulators/<id>" folder name
    QString displayName;   // shown in UI
    // Command-line args. {rom} is replaced with the ROM path; {fs} with fullscreenArgs/windowedArgs (so the
    // emulator's fullscreen flag lands in the right spot - some parsers want options before the file).
    //
    // QUOTING (issue #237). The string is cut into arguments on spaces, shell-style: put double quotes around
    // an argument that must CONTAIN a space, e.g. --config "My Profile" is two arguments, not three. {rom} and
    // {fs} are substituted AFTER the cut, so a ROM path with spaces never needs quoting.
    QString argsTemplate;
    QString fullscreenArgs; // substituted for {fs} when "launch full screen" is on  (flags differ per emulator)
    QString windowedArgs;   // substituted for {fs} when it's off (keeps the toggle authoritative)
    QString homepage;      // where to get it manually (shown if auto-install isn't possible)

    // Find-rules: candidate binary paths. A RELATIVE path is resolved under "emulators/<id>/"; an ABSOLUTE
    // path (a user pointing at an install they already have) is used verbatim. First existing match wins.
    // Per-OS because the layout differs (Windows .exe in a versioned subfolder, macOS .app bundle, Linux
    // binary/AppImage).
    QStringList winBinaries;
    QStringList macBinaries;
    QStringList linuxBinaries;

    // Auto-install: a JSON endpoint listing per-OS download artifacts, and the artifact "system" label to
    // match for each platform. The archive is fetched and extracted into "emulators/<id>/". A user entry
    // leaves these empty (hasInstallSource() false) so the download machinery is never entered.
    QString updateJsonUrl;
    QString winArtifact;
    QString macArtifact;
    QString linuxArtifact;
    QString flatpakAppId;  // non-empty => Linux build is a Flatpak: install via flatpak, launch via "flatpak run"
    // Some emulators publish each OS from a separate repo; when set, these override updateJsonUrl for that OS.
    QString winUpdateUrl;
    QString macUpdateUrl;
    QString linuxUpdateUrl;

    // ---- data-driven metadata (issue #52) — empty for most of the built-in table, whose systems select their
    // emulator through SystemCatalog's externalEmulator field. ares is the exception: it declares
    // systems=["n64"], which is what makes it SELECTABLE on a system whose default stays libretro. A user
    // emulator MAY declare the file extensions it handles and the system ids it runs; both are carried in the
    // schema (round-tripped), and `systems` binds it the same way (see the header note).
    QStringList extensions; // lowercase, no leading dot — file types this emulator handles (informational)
    QStringList systems;    // SystemCatalog system ids this emulator can run — LOAD-BEARING: EmulationTarget.h's
                            // boundEmulatorsFor offers/accepts this emulator on each (see the header note)

    // The native-port game binding (issue #233) — see NativePortBinding above. Empty for every entry in the
    // built-in emulator table; filled only by the port catalog (NativePorts.h), which is a SEPARATE registry.
    NativePortBinding port;

    // A port is exactly "an ExternalEmulator carrying a game binding". One spelling, so no caller invents a
    // second test (an empty `systems` does NOT mean port — most user entries have none either).
    bool isNativePort() const { return !port.name.isEmpty(); }
};

// Round-trip equality over the serialized schema fields (probe_useremulators pins fromJson(toJson(e)) == e).
inline bool operator==(const ExternalEmulator& a, const ExternalEmulator& b)
{
    return a.id == b.id && a.displayName == b.displayName && a.argsTemplate == b.argsTemplate
        && a.fullscreenArgs == b.fullscreenArgs && a.windowedArgs == b.windowedArgs && a.homepage == b.homepage
        && a.winBinaries == b.winBinaries && a.macBinaries == b.macBinaries && a.linuxBinaries == b.linuxBinaries
        && a.updateJsonUrl == b.updateJsonUrl && a.winArtifact == b.winArtifact && a.macArtifact == b.macArtifact
        && a.linuxArtifact == b.linuxArtifact && a.flatpakAppId == b.flatpakAppId
        && a.winUpdateUrl == b.winUpdateUrl && a.macUpdateUrl == b.macUpdateUrl && a.linuxUpdateUrl == b.linuxUpdateUrl
        && a.extensions == b.extensions && a.systems == b.systems && a.port == b.port;
}
inline bool operator!=(const ExternalEmulator& a, const ExternalEmulator& b) { return !(a == b); }

namespace EmulatorRegistry
{
    // The BUILT-IN table. all() below is this merged with any <data>/emulators/*.json. Kept as its own
    // accessor so the probe (and the merge) can name the base explicitly.
    inline const QList<ExternalEmulator>& builtinEmulators()
    {
        static const QList<ExternalEmulator> list = {
            {
                QStringLiteral("dolphin"), QStringLiteral("Dolphin"),
                QStringLiteral("-b -e {rom} {fs}"),   // -b: quit when emulation stops; -e: boot this file
                QStringLiteral("-C Dolphin.Display.Fullscreen=True"),   // fullscreenArgs
                QStringLiteral("-C Dolphin.Display.Fullscreen=False"),  // windowedArgs
                QStringLiteral("https://dolphin-emu.org/download/"),
                // Windows: the official .7z extracts to a "Dolphin-x64/" folder; also accept a flat or
                // RetroBat/ES-DE-style nested copy so an existing install is detected.
                { QStringLiteral("Dolphin-x64/Dolphin.exe"), QStringLiteral("Dolphin.exe"),
                  QStringLiteral("dolphin/Dolphin.exe"), QStringLiteral("dolphin-emu/Dolphin.exe") },
                { QStringLiteral("Dolphin.app/Contents/MacOS/Dolphin"), QStringLiteral("Dolphin.app") },
                { QStringLiteral("dolphin-emu"), QStringLiteral("Dolphin.AppImage") },
                QStringLiteral("https://dolphin-emu.org/update/latest/beta/"), // Dolphin-style {artifacts:[{system,url}]}
                QStringLiteral("Windows x64"),
                QStringLiteral("macOS (ARM/Intel Universal)"),
                QStringLiteral("Linux x86_64 (Flatpak)"),
                QStringLiteral("org.DolphinEmu.dolphin-emu"), // Linux build is a Flatpak
            },
            {
                // Nintendo 3DS. Citra itself was discontinued (Nintendo DMCA, 2024) and has no working
                // download; Azahar is the maintained successor (Citra + Lime3DS merged). Citra-family CLI:
                // boots a game by path, -f = full screen. Find-rules also detect an existing Citra/Lime3DS.
                QStringLiteral("azahar"), QStringLiteral("Azahar (3DS)"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("-f"),   // fullscreenArgs
                QString(),              // windowedArgs (default is windowed)
                QStringLiteral("https://azahar-emu.org/"),
                { QStringLiteral("azahar.exe"), QStringLiteral("citra-qt.exe"),
                  QStringLiteral("lime3ds-gui.exe"), QStringLiteral("lime3ds.exe") },
                { QStringLiteral("Azahar.app/Contents/MacOS/azahar"), QStringLiteral("azahar") },
                { QStringLiteral("azahar"), QStringLiteral("azahar.AppImage") },
                // GitHub releases API: {assets:[{name, browser_download_url}]} - matched by name substring.
                QStringLiteral("https://api.github.com/repos/azahar-emu/azahar/releases/latest"),
                QStringLiteral("windows-msvc"),   // -> azahar-windows-msvc-<ver>.zip (not installer/msys2/libretro)
                QStringLiteral("macos-universal"), // -> azahar-macos-universal-<ver>.zip
                QStringLiteral("azahar.appimage"), // -> azahar.AppImage (the plain, non-wayland desktop build)
                QString(),                         // not a Flatpak
            },
            {
                // Nintendo DS. melonDS ships a .zip for every OS (Win: melonDS.exe, mac: melonDS.app,
                // Linux: an AppImage inside the zip). CLI: boots a positional ROM, -f = full screen.
                QStringLiteral("melonds"), QStringLiteral("melonDS"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("-f"),   // fullscreenArgs
                QString(),              // windowedArgs (default is windowed)
                QStringLiteral("https://melonds.kuribo64.net/"),
                { QStringLiteral("melonDS.exe") },
                { QStringLiteral("melonDS.app/Contents/MacOS/melonDS"), QStringLiteral("melonDS.app") },
                { QStringLiteral("melonDS-x86_64.AppImage"), QStringLiteral("melonDS") },
                QStringLiteral("https://api.github.com/repos/melonDS-emu/melonDS/releases/latest"),
                QStringLiteral("windows-x86_64"),  // -> melonDS-<ver>-windows-x86_64.zip
                QStringLiteral("macos-universal"), // -> melonDS-<ver>-macOS-universal.zip
                QStringLiteral("appimage-x86_64"), // -> melonDS-<ver>-appimage-x86_64.zip (portable AppImage)
                QString(),                         // not a Flatpak
            },
            {
                // Wii U. Cemu ships per-OS on GitHub: Windows .zip (extracts to Cemu_<ver>/), macOS .dmg,
                // Linux a direct .AppImage. CLI: -g <game> loads it, -f = full screen.
                QStringLiteral("cemu"), QStringLiteral("Cemu"),
                QStringLiteral("{fs} -g {rom}"),
                QStringLiteral("-f"),   // fullscreenArgs
                QString(),              // windowedArgs (default is windowed)
                QStringLiteral("https://cemu.info/"),
                { QStringLiteral("Cemu.exe") },
                { QStringLiteral("Cemu.app/Contents/MacOS/Cemu"), QStringLiteral("Cemu.app") },
                { QStringLiteral("cemu.AppImage"), QStringLiteral("Cemu") },
                QStringLiteral("https://api.github.com/repos/cemu-project/Cemu/releases/latest"),
                QStringLiteral("windows-x64"), // -> cemu-<ver>-windows-x64.zip
                QStringLiteral("macos"),       // -> cemu-<ver>-macos-12-x64.dmg
                QStringLiteral("appimage"),    // -> Cemu-<ver>-x86_64.AppImage (direct AppImage)
                QString(),                     // not a Flatpak
            },
            {
                // Nintendo Switch. The original Ryujinx was discontinued (Nintendo, 2024); Ryubing is the
                // maintained fork, released via its own Forgejo (GitHub-compatible API). Win .zip ->
                // publish/Ryujinx.exe, macOS .app.tar.gz, Linux direct .AppImage. CLI: positional ROM,
                // --fullscreen for full screen.
                QStringLiteral("ryujinx"), QStringLiteral("Ryujinx (Ryubing)"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("--fullscreen"), // fullscreenArgs
                QString(),                      // windowedArgs (default is windowed)
                QStringLiteral("https://ryujinx.app/"),
                { QStringLiteral("Ryujinx.exe"), QStringLiteral("publish/Ryujinx.exe") },
                { QStringLiteral("Ryujinx.app/Contents/MacOS/Ryujinx"), QStringLiteral("Ryujinx.app") },
                { QStringLiteral("ryujinx.AppImage"), QStringLiteral("Ryujinx") },
                QStringLiteral("https://git.ryujinx.app/api/v1/repos/ryubing/ryujinx/releases/latest"),
                QStringLiteral("win_x64"),         // -> ryujinx-<ver>-win_x64.zip
                QStringLiteral("macos_universal"), // -> ryujinx-<ver>-macos_universal.app.tar.gz
                QStringLiteral("x64.appimage"),    // -> ryujinx-<ver>-x64.AppImage (not arm64)
                QString(),                         // not a Flatpak
            },
            {
                // PlayStation Portable. PPSSPP ships per-OS on GitHub: Windows .zip (PPSSPPWindows64.exe),
                // macOS .zip (PPSSPPSDL.app), Linux a direct .AppImage. CLI: positional ROM, --fullscreen.
                QStringLiteral("ppsspp"), QStringLiteral("PPSSPP"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("--fullscreen"), // fullscreenArgs
                QString(),                      // windowedArgs (default is windowed)
                QStringLiteral("https://www.ppsspp.org/download/"),
                { QStringLiteral("PPSSPPWindows64.exe"), QStringLiteral("PPSSPPWindows.exe") },
                { QStringLiteral("PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL"), QStringLiteral("PPSSPPSDL.app") },
                { QStringLiteral("ppsspp.AppImage"), QStringLiteral("PPSSPPSDL") },
                QStringLiteral("https://api.github.com/repos/hrydgard/ppsspp/releases/latest"),
                QStringLiteral("Windows-x64"),  // -> PPSSPP-<ver>-Windows-x64.zip (not ARM64)
                QStringLiteral("macos"),        // -> PPSSPPSDL-macOS-<ver>.zip
                QStringLiteral("x86_64.appimage"), // -> PPSSPP-<ver>-anylinux-x86_64.AppImage
                QString(),                      // not a Flatpak
            },
            {
                // PlayStation Vita. Vita3K ships a rolling "continuous" release on GitHub: Windows .zip
                // (Vita3K.exe at root), macOS .dmg, Linux a direct .AppImage. CLI: positional .vpk/folder
                // is installed & run; -F/--fullscreen for full screen.
                QStringLiteral("vita3k"), QStringLiteral("Vita3K"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("--fullscreen"), // fullscreenArgs
                QString(),                      // windowedArgs (default is windowed)
                QStringLiteral("https://vita3k.org/"),
                { QStringLiteral("Vita3K.exe") },
                { QStringLiteral("Vita3K.app/Contents/MacOS/Vita3K"), QStringLiteral("Vita3K.app") },
                { QStringLiteral("vita3k.AppImage"), QStringLiteral("Vita3K") },
                QStringLiteral("https://api.github.com/repos/Vita3K/Vita3K/releases/latest"),
                QStringLiteral("windows-latest"),  // -> windows-latest.zip (not windows-arm64-latest)
                QStringLiteral("macos-latest"),    // -> macos-latest.dmg (Intel; runs on Apple Silicon via Rosetta)
                QStringLiteral("x86_64.appimage"), // -> Vita3K-x86_64.AppImage
                QString(),                         // not a Flatpak
            },
            {
                // PlayStation 3. RPCS3 publishes each OS from a SEPARATE GitHub repo: Windows .7z
                // (rpcs3.exe at root), macOS .7z (rpcs3.app), Linux a direct .AppImage. CLI: positional
                // (S)ELF boots; --fullscreen only applies with --no-gui, which also boots straight to the
                // game (no GUI). Leaving the toggle off keeps the GUI (needed once to install firmware).
                QStringLiteral("rpcs3"), QStringLiteral("RPCS3"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("--no-gui --fullscreen"), // fullscreenArgs
                QString(),                               // windowedArgs (empty -> opens the GUI + boots the game)
                QStringLiteral("https://rpcs3.net/download"),
                { QStringLiteral("rpcs3.exe") },
                { QStringLiteral("rpcs3.app/Contents/MacOS/rpcs3"), QStringLiteral("rpcs3.app") },
                { QStringLiteral("rpcs3.AppImage"), QStringLiteral("rpcs3") },
                QString(),                               // updateJsonUrl (per-OS repos below)
                QStringLiteral("win64"),                 // -> ..._win64_msvc.7z (skips the .sha256)
                QStringLiteral("macos"),                 // -> ..._macos.7z
                QStringLiteral("linux64"),               // -> ..._linux64.AppImage
                QString(),                               // not a Flatpak
                QStringLiteral("https://api.github.com/repos/RPCS3/rpcs3-binaries-win/releases/latest"),
                QStringLiteral("https://api.github.com/repos/RPCS3/rpcs3-binaries-mac/releases/latest"),
                QStringLiteral("https://api.github.com/repos/RPCS3/rpcs3-binaries-linux/releases/latest"),
            },
            {
                // PlayStation (PS1). DuckStation, GitHub single repo: Windows .zip (the exe is named by build
                // config, duckstation-qt-x64-ReleaseLTCG.exe), macOS .zip (DuckStation.app), Linux a direct
                // .AppImage. CLI: positional ROM, -batch exits when the game stops, -fullscreen for full screen.
                // (The -installer.exe and -symbols.7z assets are skipped by the extension/symbols filters.)
                QStringLiteral("duckstation"), QStringLiteral("DuckStation"),
                QStringLiteral("-batch {fs} {rom}"),
                QStringLiteral("-fullscreen"),    // fullscreenArgs
                QStringLiteral("-nofullscreen"),  // windowedArgs
                QStringLiteral("https://www.duckstation.org/"),
                { QStringLiteral("duckstation-qt-x64-ReleaseLTCG.exe"),
                  QStringLiteral("duckstation-qt-x64-sse2-ReleaseLTCG.exe") },
                { QStringLiteral("DuckStation.app/Contents/MacOS/DuckStation"), QStringLiteral("DuckStation.app") },
                { QStringLiteral("duckstation.AppImage"), QStringLiteral("DuckStation") },
                QStringLiteral("https://api.github.com/repos/stenzek/duckstation/releases/latest"),
                QStringLiteral("windows-x64-release"), // -> duckstation-windows-x64-release.zip (not sse2/arm64/installer)
                QStringLiteral("mac-release"),         // -> duckstation-mac-release.zip
                QStringLiteral("x64.appimage"),        // -> DuckStation-x64.AppImage (not SSE2/arm)
                QString(),                             // not a Flatpak
            },
            {
                // PlayStation 2. PCSX2, GitHub single repo: Windows .7z (pcsx2-qt.exe at root), macOS .tar.xz
                // (PCSX2.app), Linux a direct .AppImage. CLI: -batch boots & exits when stopped, path after
                // "--", -fullscreen/-nofullscreen. (installer/symbols assets skipped by the filters.)
                QStringLiteral("pcsx2"), QStringLiteral("PCSX2"),
                QStringLiteral("-batch {fs} -- {rom}"),
                QStringLiteral("-fullscreen"),    // fullscreenArgs
                QStringLiteral("-nofullscreen"),  // windowedArgs
                QStringLiteral("https://pcsx2.net/downloads/"),
                { QStringLiteral("pcsx2-qt.exe") },
                { QStringLiteral("PCSX2.app/Contents/MacOS/PCSX2"), QStringLiteral("PCSX2.app") },
                { QStringLiteral("pcsx2.AppImage"), QStringLiteral("pcsx2-qt") },
                QStringLiteral("https://api.github.com/repos/PCSX2/pcsx2/releases/latest"),
                QStringLiteral("windows-x64-qt"), // -> pcsx2-<ver>-windows-x64-Qt.7z (not installer/symbols)
                QStringLiteral("macos"),          // -> pcsx2-<ver>-macos-Qt.tar.xz
                QStringLiteral("appimage-x64"),   // -> pcsx2-<ver>-linux-appimage-x64-Qt.AppImage (not flatpak)
                QString(),                        // not a Flatpak
            },
            {
                // Sega Dreamcast. Flycast, GitHub single repo: Windows .zip (flycast.exe), macOS .zip
                // (Flycast.app), Linux a direct .AppImage. CLI: positional CONTENT boots; settings via
                // "-config section:key=value", so full screen is "-config window:fullscreen=yes".
                QStringLiteral("flycast"), QStringLiteral("Flycast"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("-config window:fullscreen=yes"), // fullscreenArgs
                QStringLiteral("-config window:fullscreen=no"),  // windowedArgs
                QStringLiteral("https://github.com/flyinghead/flycast/releases"),
                { QStringLiteral("flycast.exe") },
                { QStringLiteral("Flycast.app/Contents/MacOS/Flycast"), QStringLiteral("Flycast.app") },
                { QStringLiteral("flycast.AppImage"), QStringLiteral("flycast") },
                QStringLiteral("https://api.github.com/repos/flyinghead/flycast/releases/latest"),
                QStringLiteral("win64"),           // -> flycast-win64-<ver>.zip (not the .appx)
                QStringLiteral("macos"),           // -> flycast-macOS-<ver>.zip
                QStringLiteral("x86_64.appimage"), // -> flycast-x86_64.AppImage
                QString(),                         // not a Flatpak
            },
            {
                // Original Xbox. xemu (QEMU-based), GitHub single repo: Windows .zip (xemu.exe), macOS .zip
                // (xemu.app, the signed build), Linux a direct .AppImage. CLI: positional disk image boots,
                // -full-screen for full screen. (dbg/pdb/unsigned/arm variants are skipped by the filters.)
                QStringLiteral("xemu"), QStringLiteral("xemu"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("-full-screen"), // fullscreenArgs
                QString(),                      // windowedArgs (default is windowed)
                QStringLiteral("https://xemu.app/"),
                { QStringLiteral("xemu.exe") },
                { QStringLiteral("xemu.app/Contents/MacOS/xemu"), QStringLiteral("xemu.app") },
                { QStringLiteral("xemu.AppImage"), QStringLiteral("xemu") },
                QStringLiteral("https://api.github.com/repos/xemu-project/xemu/releases/latest"),
                QStringLiteral("windows-x86_64"),  // -> xemu-<ver>-windows-x86_64.zip
                QStringLiteral("macos-universal"), // -> xemu-<ver>-macos-universal.zip (signed)
                QStringLiteral("x86_64.appimage"), // -> xemu-<ver>-x86_64.AppImage
                QString(),                         // not a Flatpak
            },
            {
                // Xbox 360. Xenia (Canary fork), GitHub single repo: Windows .7z (xenia_canary.exe), Linux a
                // direct .AppImage. There is NO macOS build (macArtifact empty -> handled with a clear message).
                // CLI: positional game (.iso/.xex) boots; gflags --fullscreen=true/false.
                QStringLiteral("xenia"), QStringLiteral("Xenia (Canary)"),
                QStringLiteral("{fs} {rom}"),
                QStringLiteral("--fullscreen=true"),  // fullscreenArgs
                QStringLiteral("--fullscreen=false"), // windowedArgs
                QStringLiteral("https://xenia.jp/"),
                { QStringLiteral("xenia_canary.exe"), QStringLiteral("xenia.exe") },
                { },                                  // no macOS build
                { QStringLiteral("xenia.AppImage"), QStringLiteral("xenia_canary") },
                QStringLiteral("https://api.github.com/repos/xenia-canary/xenia-canary/releases/latest"),
                QStringLiteral("windows"),            // -> xenia_canary_windows.7z
                QString(),                            // macArtifact: no macOS build
                QStringLiteral("linux"),              // -> xenia_canary_linux.AppImage
                QString(),                            // not a Flatpak
            },
            {
                // Atari Jaguar / Jaguar CD. BigPEmu (closed-source, richwhitehouse.com). No GitHub/JSON API,
                // so the updateJsonUrl is the download PAGE and the installer scrapes it for the build URL.
                // Windows .zip (BigPEmu.exe), Linux .tar.gz; no macOS build. Boots a positional ROM; full
                // screen is an in-app toggle (no CLI flag), so {fs} is empty.
                QStringLiteral("bigpemu"), QStringLiteral("BigPEmu"),
                QStringLiteral("{fs} {rom}"),
                QString(),  // fullscreenArgs (BigPEmu has no fullscreen CLI flag; toggled in-app)
                QString(),  // windowedArgs
                QStringLiteral("https://www.richwhitehouse.com/jaguar/index.php?content=download"),
                { QStringLiteral("BigPEmu.exe") },
                { },        // no macOS build
                { QStringLiteral("BigPEmu"), QStringLiteral("bigpemu") },
                QStringLiteral("https://www.richwhitehouse.com/jaguar/index.php?content=download"), // scraped HTML
                QStringLiteral("bigpemu_v"), // -> builds/BigPEmu_v<ver>.zip (not WinARM64 / -DEV)
                QString(),                   // macArtifact: no macOS build
                QStringLiteral("linux64"),   // -> builds/BigPEmu_Linux64_v<ver>.tar.gz (not LinuxARM64)
                QString(),                   // not a Flatpak
            },
            {
                // TeknoParrot - a Windows-only loader for modern PC-based arcade games. It is a launcher UI
                // (games are added/configured inside it), not a per-ROM emulator, so it has no ROM template
                // or SystemCatalog system: install and open it from Settings > Emulators. No macOS/Linux build.
                QStringLiteral("teknoparrot"), QStringLiteral("TeknoParrot"),
                QString(),   // argsTemplate: launched with no game (opens its own UI)
                QString(),   // fullscreenArgs
                QString(),   // windowedArgs
                QStringLiteral("https://teknoparrot.com/"),
                { QStringLiteral("TeknoParrotUi.exe") },
                { },         // no macOS build
                { },         // no Linux build
                QStringLiteral("https://api.github.com/repos/teknogods/TeknoParrotUI/releases/latest"),
                QStringLiteral("teknoparrot"), // -> TeknoParrotUi.zip
                QString(),                     // macArtifact: none
                QString(),                     // linuxArtifact: none
                QString(),                     // not a Flatpak
            },
            {
                // Nintendo 64. ares is a multi-system accuracy emulator; EverythingBox wires only its N64 core.
                // CLI is "ares [options]... game" and the system is auto-detected from the ROM, so no --system
                // (which this space-split argsTemplate could not quote anyway). --no-file-prompt is NOT optional:
                // any cart carrying the 64DD ("dd") or Transfer Pak ("tpak") attribute opens a BLOCKING file
                // dialog on load without it. No BIOS entry: ares compiles the PIF ROMs in as build resources.
                // Windows is portable by default — ares looks for settings.bml beside its own exe first.
                QStringLiteral("ares"), QStringLiteral("ares"),
                QStringLiteral("{fs} --no-file-prompt {rom}"),
                QStringLiteral("--fullscreen"),   // fullscreenArgs
                QString(),                        // windowedArgs (default is windowed)
                QStringLiteral("https://ares-emu.net/"),
                { QStringLiteral("ares.exe"), QStringLiteral("ares/ares.exe") },
                { QStringLiteral("ares.app/Contents/MacOS/ares"), QStringLiteral("ares.app") },
                { QStringLiteral("ares") },
                QStringLiteral("https://api.github.com/repos/ares-emulator/ares/releases/latest"),
                // FULL filenames, not the usual short platform marker: the release also publishes
                // ares-windows-x64-PDBs.zip and ares-macos-universal-dSYMs.zip, and the dSYMs archive is
                // listed BEFORE the real one in the assets array.
                QStringLiteral("ares-windows-x64.zip"),
                QStringLiteral("ares-macos-universal.zip"),
                QString(),                        // linuxArtifact — ares publishes no Linux binary
                QStringLiteral("dev.ares.ares"),  // Linux build is a Flatpak
                QString(), QString(), QString(),  // win/mac/linux update URL overrides — none
                { QStringLiteral("n64"), QStringLiteral("z64"), QStringLiteral("v64"), QStringLiteral("ndd") },
                // systems: the LOAD-BEARING binding. The n64 catalog row declares no externalEmulator — its
                // default stays mupen64plus_next, the only N64 engine RetroAchievements works on — so this
                // list is the whole reason the picker offers "ares (standalone)" for N64 at all.
                { QStringLiteral("n64") },
            },
        };
        return list;
    }

    // ---- pure: string-array field <-> QStringList -------------------------------------------------------
    // Read a JSON array-of-strings field into a QStringList (trimmed, empties dropped, optionally lowercased).
    // A non-array value yields an empty list. Kept in one place so every field parses identically. (Same shape
    // as SystemCatalog::jsonStrList — the two schemas share this primitive by convention, not by linkage.)
    inline QStringList jsonStrList(const QJsonValue& v, bool lower)
    {
        QStringList out;
        if (!v.isArray()) return out;
        for (const QJsonValue& e : v.toArray())
        {
            if (!e.isString()) continue;
            const QString s = lower ? e.toString().trimmed().toLower() : e.toString().trimmed();
            if (!s.isEmpty()) out.push_back(s);
        }
        return out;
    }

    // ---- pure: ExternalEmulator <-> canonical JSON ------------------------------------------------------
    // Canonical serialization: id + name always written; every other field written ONLY when non-empty, so
    // there is exactly one spelling per emulator and fromJson(toJson(e)) == e (probe_useremulators pins this).
    // `binary` is an INPUT-ONLY shorthand (see overlay) — never emitted; the per-OS binaries arrays are.
    inline QJsonObject toJson(const ExternalEmulator& e)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), e.id);
        o.insert(QStringLiteral("name"), e.displayName);
        auto putStr = [&](const char* key, const QString& v) {
            if (!v.isEmpty()) o.insert(QLatin1String(key), v);
        };
        auto putArr = [&](const char* key, const QStringList& v) {
            if (v.isEmpty()) return;
            QJsonArray a;
            for (const QString& s : v) a.push_back(s);
            o.insert(QLatin1String(key), a);
        };
        putStr("argsTemplate", e.argsTemplate);
        putStr("fullscreenArgs", e.fullscreenArgs);
        putStr("windowedArgs", e.windowedArgs);
        putStr("homepage", e.homepage);
        putArr("winBinaries", e.winBinaries);
        putArr("macBinaries", e.macBinaries);
        putArr("linuxBinaries", e.linuxBinaries);
        putArr("extensions", e.extensions);
        putArr("systems", e.systems);
        putStr("updateJsonUrl", e.updateJsonUrl);
        putStr("winArtifact", e.winArtifact);
        putStr("macArtifact", e.macArtifact);
        putStr("linuxArtifact", e.linuxArtifact);
        putStr("flatpakAppId", e.flatpakAppId);
        putStr("winUpdateUrl", e.winUpdateUrl);
        putStr("macUpdateUrl", e.macUpdateUrl);
        putStr("linuxUpdateUrl", e.linuxUpdateUrl);
        // NO `port` KEY, deliberately (#233). The native-port binding has its OWN schema — RetComM's, in
        // NativePorts.h — and this one is the emulator schema; writing the binding here would mint a second,
        // divergent spelling of it and let a <data>/emulators/*.json file declare a port, which is the one
        // thing keeping the two registries separate rules out. A port is serialized by NativePorts, and every
        // emulator's canonical JSON is byte-for-byte what it was before native ports existed.
        return o;
    }

    // Overlay the fields PRESENT in `o` onto `base`, returning the result. A key that is absent leaves the
    // base value untouched (this is what makes an override field-level — a file may swap `argsTemplate` alone).
    // The single primitive behind both "add a new emulator" (base = default {}) and "override a built-in".
    // `name` sets displayName (`displayName` is also accepted as an alias). `binary` is a shorthand for the
    // CURRENT-OS binaries list: it fills that list only when the OS-specific array key is not itself present.
    // Binary paths keep their case; extensions/systems are lowercased (matching is case-insensitive).
    inline ExternalEmulator overlay(const ExternalEmulator& base, const QJsonObject& o)
    {
        ExternalEmulator e = base;
        if (o.contains(QStringLiteral("id")))             e.id = o.value(QStringLiteral("id")).toString().trimmed();
        if (o.contains(QStringLiteral("name")))           e.displayName = o.value(QStringLiteral("name")).toString();
        if (o.contains(QStringLiteral("displayName")))    e.displayName = o.value(QStringLiteral("displayName")).toString();
        if (o.contains(QStringLiteral("argsTemplate")))   e.argsTemplate = o.value(QStringLiteral("argsTemplate")).toString();
        if (o.contains(QStringLiteral("fullscreenArgs"))) e.fullscreenArgs = o.value(QStringLiteral("fullscreenArgs")).toString();
        if (o.contains(QStringLiteral("windowedArgs")))   e.windowedArgs = o.value(QStringLiteral("windowedArgs")).toString();
        if (o.contains(QStringLiteral("homepage")))       e.homepage = o.value(QStringLiteral("homepage")).toString();
        if (o.contains(QStringLiteral("winBinaries")))    e.winBinaries = jsonStrList(o.value(QStringLiteral("winBinaries")), false);
        if (o.contains(QStringLiteral("macBinaries")))    e.macBinaries = jsonStrList(o.value(QStringLiteral("macBinaries")), false);
        if (o.contains(QStringLiteral("linuxBinaries")))  e.linuxBinaries = jsonStrList(o.value(QStringLiteral("linuxBinaries")), false);
        // `binary` shorthand: a single path fills the current-OS find-rule when its explicit array isn't given.
        if (o.contains(QStringLiteral("binary")))
        {
            const QString b = o.value(QStringLiteral("binary")).toString().trimmed();
            if (!b.isEmpty())
            {
#if defined(Q_OS_WIN)
                if (!o.contains(QStringLiteral("winBinaries")))   e.winBinaries = QStringList{ b };
#elif defined(Q_OS_MACOS)
                if (!o.contains(QStringLiteral("macBinaries")))   e.macBinaries = QStringList{ b };
#else
                if (!o.contains(QStringLiteral("linuxBinaries"))) e.linuxBinaries = QStringList{ b };
#endif
            }
        }
        if (o.contains(QStringLiteral("extensions")))     e.extensions = jsonStrList(o.value(QStringLiteral("extensions")), true);
        if (o.contains(QStringLiteral("systems")))        e.systems = jsonStrList(o.value(QStringLiteral("systems")), true);
        if (o.contains(QStringLiteral("updateJsonUrl")))  e.updateJsonUrl = o.value(QStringLiteral("updateJsonUrl")).toString().trimmed();
        if (o.contains(QStringLiteral("winArtifact")))    e.winArtifact = o.value(QStringLiteral("winArtifact")).toString();
        if (o.contains(QStringLiteral("macArtifact")))    e.macArtifact = o.value(QStringLiteral("macArtifact")).toString();
        if (o.contains(QStringLiteral("linuxArtifact")))  e.linuxArtifact = o.value(QStringLiteral("linuxArtifact")).toString();
        if (o.contains(QStringLiteral("flatpakAppId")))   e.flatpakAppId = o.value(QStringLiteral("flatpakAppId")).toString().trimmed();
        if (o.contains(QStringLiteral("winUpdateUrl")))   e.winUpdateUrl = o.value(QStringLiteral("winUpdateUrl")).toString().trimmed();
        if (o.contains(QStringLiteral("macUpdateUrl")))   e.macUpdateUrl = o.value(QStringLiteral("macUpdateUrl")).toString().trimmed();
        if (o.contains(QStringLiteral("linuxUpdateUrl"))) e.linuxUpdateUrl = o.value(QStringLiteral("linuxUpdateUrl")).toString().trimmed();
        // No `port` key here either — see the note in toJson above. `e.port` is carried through unchanged, so
        // an override of a port's EMULATOR fields (should one ever be written by hand) keeps its binding.
        return e;
    }

    // A full parse from a standalone object (default base). fromJson(toJson(e)) == e.
    inline ExternalEmulator fromJson(const QJsonObject& o) { return overlay(ExternalEmulator{}, o); }

    // True if `e` can auto-install (has any per-OS update source). A user entry points at an existing binary
    // and declares no update URL, so this is false for it — the download/extract machinery is never entered
    // (auto-install stays a built-in-table privilege). Shared by EmulatorManager so there is one oracle.
    inline bool hasInstallSource(const ExternalEmulator& e)
    {
        return !e.updateJsonUrl.isEmpty() || !e.winUpdateUrl.isEmpty()
            || !e.macUpdateUrl.isEmpty() || !e.linuxUpdateUrl.isEmpty();
    }

    // ---- pure: the "releases/latest 404" fallback (issue #233) -----------------------------------------
    // GitHub answers `/releases/latest` with 404 for a repository whose only releases are PRE-RELEASES — and
    // several native ports publish nothing else (MarioKart64Recomp's published builds are prereleases). The
    // list endpoint `/releases` answers for those, newest first. This is the URL to retry with, or "" when
    // the failing URL was not a `/releases/latest` at all (a Dolphin update URL, an HTML page) and there is
    // therefore nothing to fall back TO. Query strings and a trailing slash are tolerated.
    inline QString releasesFallbackUrl(const QString& latestUrl)
    {
        QString u = latestUrl.trimmed();
        const int q = u.indexOf(QLatin1Char('?'));
        if (q >= 0) u = u.left(q);
        while (u.endsWith(QLatin1Char('/'))) u.chop(1);
        if (!u.endsWith(QStringLiteral("/releases/latest"), Qt::CaseInsensitive)) return QString();
        u.chop(7);   // "/latest"
        return u;
    }

    // The release object to read artifacts out of, given the ARRAY `/releases` answers. GitHub returns them
    // newest first, so this is "the first entry that is a usable release": drafts are skipped (their assets
    // are not downloadable without a token), prereleases are NOT — a prerelease-only project is the entire
    // reason this path exists. An empty/all-draft array yields an empty object, which the caller reports as
    // "no download was listed" exactly as it would for a release with no matching asset.
    inline QJsonObject newestRelease(const QJsonArray& releases)
    {
        for (const QJsonValue& v : releases)
        {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("draft")).toBool()) continue;
            return o;
        }
        return QJsonObject{};
    }

    // Resolve a binary from a candidate list: an ABSOLUTE find-rule is returned verbatim when it exists (the
    // user-points-at-their-own-install case); a RELATIVE one is looked up under `baseDir`. First existing
    // match wins; "" if none exists. The pure core of EmulatorManager::resolveBinary (which layers on the
    // recursive-subfolder and Flatpak fallbacks). Kept here so the probe can pin it without linking QtNetwork.
    inline QString resolveBinaryFrom(const QStringList& cands, const QString& baseDir)
    {
        for (const QString& c : cands)
        {
            if (c.isEmpty()) continue;
            const QString p = QDir::isAbsolutePath(c) ? c : (baseDir + QStringLiteral("/") + c);
            if (QFileInfo::exists(p)) return p;
        }
        return QString();
    }

    // ---- pure: merge a set of data entries over a base list ---------------------------------------------
    // For each entry: a non-object, or one whose `id` is empty/missing, is reported via `warn` and skipped
    // (never dropping the base). An entry whose id matches a base emulator overrides its named fields; a new
    // id is appended. Deterministic: entries are applied in the order given, later winning on the same id.
    inline QList<ExternalEmulator> applyEntries(QList<ExternalEmulator> base, const QJsonArray& entries,
                                                const std::function<void(const QString&)>& warn = {})
    {
        auto note = [&](const QString& m) { if (warn) warn(m); };
        int idx = 0;
        for (const QJsonValue& v : entries)
        {
            const int here = idx++;
            if (!v.isObject()) { note(QStringLiteral("entry %1 is not an object — skipped").arg(here)); continue; }
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("id")).toString().trimmed();
            if (id.isEmpty()) { note(QStringLiteral("entry %1 has no \"id\" — skipped").arg(here)); continue; }

            int found = -1;
            for (int i = 0; i < base.size(); ++i) if (base[i].id == id) { found = i; break; }
            if (found >= 0) base[found] = overlay(base[found], o);       // override named fields of a built-in
            else            base.push_back(overlay(ExternalEmulator{}, o)); // append a new emulator
        }
        return base;
    }

    // ---- pure: read one file's bytes into an entry array ------------------------------------------------
    // An emulators file is EITHER a JSON array of emulator objects OR a single emulator object (wrapped into a
    // one-element array). Unparseable bytes fail with a reason in *err and yield no entries — the
    // "malformed => logged and skipped" primitive. A top-level scalar is rejected by Qt's parser as an ERROR
    // above, so a document that parses is always an array or an object.
    inline bool parseEntries(const QByteArray& bytes, QJsonArray* out, QString* err)
    {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
        if (pe.error != QJsonParseError::NoError)
        {
            if (err) *err = QStringLiteral("not valid JSON (%1 at offset %2)").arg(pe.errorString()).arg(pe.offset);
            return false;
        }
        if (doc.isArray()) { if (out) *out = doc.array(); return true; }
        QJsonArray a; a.push_back(doc.object());  // a lone object -> a one-element array
        if (out) *out = a;
        return true;
    }

    // ---- pure(ish): load a whole <data>/emulators directory over a base ---------------------------------
    // Reads *.json in name order (so the merge is deterministic and later files override earlier ones),
    // applying each file's entries over the accumulating registry. A file that fails to parse is reported via
    // `warn` and skipped — the base survives intact. An empty/absent dir returns the base unchanged. Only
    // QtCore file I/O, so probe_useremulators drives it against a temp directory.
    inline QList<ExternalEmulator> loadDataDir(const QString& dir, const QList<ExternalEmulator>& base,
                                               const std::function<void(const QString&)>& warn = {})
    {
        if (dir.isEmpty()) return base;
        QDir d(dir);
        if (!d.exists()) return base;

        QList<ExternalEmulator> out = base;
        const QFileInfoList files = d.entryInfoList(QStringList{ QStringLiteral("*.json") },
                                                    QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : files)
        {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly))
            {
                if (warn) warn(QStringLiteral("%1: cannot open — skipped").arg(fi.fileName()));
                continue;
            }
            const QByteArray bytes = f.readAll();
            f.close();

            QJsonArray entries;
            QString perr;
            if (!parseEntries(bytes, &entries, &perr))
            {
                if (warn) warn(QStringLiteral("%1: %2 — skipped").arg(fi.fileName(), perr));
                continue;
            }
            out = applyEntries(out, entries,
                               [&](const QString& m) { if (warn) warn(QStringLiteral("%1: %2").arg(fi.fileName(), m)); });
        }
        return out;
    }

    // The default location the app merges from: <data>/emulators (the *.json files sit alongside the
    // per-emulator install subfolders; loadDataDir only reads *.json, never the directories).
    inline QString dataEmulatorsDir() { return AppPaths::dataDir() + QStringLiteral("/emulators"); }

    // The merged registry: the built-in table with <data>/emulators/*.json applied over it. Computed ONCE on
    // first use (a new data file needs an app restart to take effect, like ES-DE). With no data files this is
    // the built-in table exactly — the no-regression rail probe_useremulators pins.
    inline const QList<ExternalEmulator>& all()
    {
        static const QList<ExternalEmulator> merged = loadDataDir(
            dataEmulatorsDir(), builtinEmulators(),
            [](const QString& m) { qWarning("EmulatorRegistry: %s", qUtf8Printable(m)); });
        return merged;
    }

    inline const ExternalEmulator* byId(const QString& id)
    {
        for (const auto& e : all())
            if (e.id == id)
                return &e;
        return nullptr;
    }
}
