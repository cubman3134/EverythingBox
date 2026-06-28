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
    QString argsTemplate;  // command-line args; the literal token {rom} is replaced with the ROM path
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
};

namespace EmulatorRegistry
{
    inline const QList<ExternalEmulator>& all()
    {
        static const QList<ExternalEmulator> list = {
            {
                QStringLiteral("dolphin"), QStringLiteral("Dolphin"),
                QStringLiteral("-b -e {rom}"),   // -b: quit when emulation stops; -e: boot this file
                QStringLiteral("https://dolphin-emu.org/download/"),
                // Windows: the official .7z extracts to a "Dolphin-x64/" folder; also accept a flat or
                // RetroBat/ES-DE-style nested copy so an existing install is detected.
                { QStringLiteral("Dolphin-x64/Dolphin.exe"), QStringLiteral("Dolphin.exe"),
                  QStringLiteral("dolphin/Dolphin.exe"), QStringLiteral("dolphin-emu/Dolphin.exe") },
                { QStringLiteral("Dolphin.app/Contents/MacOS/Dolphin"), QStringLiteral("Dolphin.app") },
                { QStringLiteral("dolphin-emu"), QStringLiteral("Dolphin.AppImage") },
                QStringLiteral("https://dolphin-emu.org/update/latest/beta/"),
                QStringLiteral("Windows x64"),
                QStringLiteral("macOS (ARM/Intel Universal)"),
                QStringLiteral("Linux x86_64 (Flatpak)"),
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
