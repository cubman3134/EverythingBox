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
