// Standalone (external) emulators that My Media Vault launches as a child process - the RetroBat / ES-DE
// model: keep a copy of each emulator under <emulators-root>/<id>/, run it with the ROM, and monitor it
// until it exits. Used for systems that can't run as an in-process libretro core (e.g. GameCube/Wii via
// Dolphin, which is hardware-rendered). Cf. SystemCatalog (in-process libretro cores).
#pragma once
#include <QString>
#include <QStringList>
#include <QList>

struct ExternalEmulator
{
    QString id;            // stable key, also the "emulators/<id>" folder name
    QString displayName;   // shown in UI
    // Command-line args. {rom} is replaced with the ROM path; {fs} with fullscreenArgs/windowedArgs (so the
    // emulator's fullscreen flag lands in the right spot - some parsers want options before the file).
    QString argsTemplate;
    QString fullscreenArgs; // substituted for {fs} when "launch full screen" is on  (flags differ per emulator)
    QString windowedArgs;   // substituted for {fs} when it's off (keeps the toggle authoritative)
    QString homepage;      // where to get it manually (shown if auto-install isn't possible)

    // Find-rules: candidate binary paths relative to "emulators/<id>/", first match wins. Per-OS because
    // the layout differs (Windows .exe in a versioned subfolder, macOS .app bundle, Linux binary/AppImage).
    QStringList winBinaries;
    QStringList macBinaries;
    QStringList linuxBinaries;

    // Auto-install: a JSON endpoint listing per-OS download artifacts, and the artifact "system" label to
    // match for each platform. The archive is fetched and extracted into "emulators/<id>/".
    QString updateJsonUrl;
    QString winArtifact;
    QString macArtifact;
    QString linuxArtifact;
    QString flatpakAppId;  // non-empty => Linux build is a Flatpak: install via flatpak, launch via "flatpak run"
    // Some emulators publish each OS from a separate repo; when set, these override updateJsonUrl for that OS.
    QString winUpdateUrl;
    QString macUpdateUrl;
    QString linuxUpdateUrl;
};

namespace EmulatorRegistry
{
    inline const QList<ExternalEmulator>& all()
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
        };
        return list;
    }

    inline const ExternalEmulator* byId(const QString& id)
    {
        for (const auto& e : all())
            if (e.id == id)
                return &e;
        return nullptr;
    }
}
