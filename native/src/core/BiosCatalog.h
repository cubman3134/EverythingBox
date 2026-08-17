// Standalone-emulator BIOS routing. The actual BIOS files (copyrighted dumps) are no longer listed here or
// fetched from a hardcoded mirror: they come from the configured EBS/Allarr file provider's `bios:bios`
// catalog, keyed on a systemId, verified by MD5 (see AddonManager::ensureBiosAsync / CoreManager). What
// remains is the tiny emulator-id -> systemId map some standalone emulators (PCSX2/DuckStation) need so the
// launch path knows WHICH system's BIOS to fetch and whether to run the emulator portably.
#pragma once
#include <QString>
#include <QSet>

namespace BiosCatalog
{
    // Libretro systems that need a console BIOS/firmware in the system folder before their core will boot
    // (the CD systems, the Famicom Disk System, GBA, the Atari 5200 / ST, …). The launch path consults this
    // to decide whether to reach the BIOS server AT ALL: a system NOT listed here is a true zero-network
    // no-op, so a cartridge system (NES/SNES/Genesis/N64/GB…) is never delayed by a catalog round-trip and a
    // slow or unreachable BIOS server can't stall a launch that needs no BIOS. This is the same set the old
    // per-system table drove before BIOS moved to the server (drop-retrobios); the actual files + MD5 still
    // come from the server (see AddonManager::ensureBiosAsync). Mirrors forExternalEmulator's local map below
    // — the launch path must know WHICH systems need a BIOS locally even though the files are served remotely.
    // A new server BIOS system is added on the launch path by adding its id here (Settings ▸ BIOS Check can
    // already fetch any server system on demand regardless).
    inline bool systemNeedsBios(const QString& systemId)
    {
        static const QSet<QString> kBiosSystems = {
            QStringLiteral("psx"),     QStringLiteral("saturn"),  QStringLiteral("3do"),
            QStringLiteral("ps2"),     QStringLiteral("nes"),     QStringLiteral("gba"),
            QStringLiteral("segacd"),  QStringLiteral("a5200"),   QStringLiteral("atarist"),
            QStringLiteral("pcecd"),
        };
        return kBiosSystems.contains(systemId);
    }

    // Standalone emulators that can't boot without a copyrighted BIOS. Maps the emulator id (EmulatorRegistry)
    // to the system whose BIOS it needs and whether to keep that BIOS under our folder via a portable marker.
    // Kept here (rather than as ExternalEmulator fields) so the emulator registry stays untouched.
    struct ExternalBios
    {
        QString systemId; // BIOS to fetch (the server bios:bios catalog's systemId), empty => needs none
        bool portable;    // drop a portable.ini marker next to the binary so config + bios stay in our folder
    };

    inline ExternalBios forExternalEmulator(const QString& emulatorId)
    {
        if (emulatorId == QStringLiteral("pcsx2"))
            return { QStringLiteral("ps2"), true };
        // DuckStation can't boot a PS1 disc without a BIOS. It already runs portable (portable.txt) and auto-scans
        // its "bios" folder by hash, so we just fetch the PS1 BIOS into <dir>/bios — no portable.ini/PCSX2.ini.
        // Without this, DuckStation's -batch launch fails to boot and exits instantly (code 0) with no message.
        if (emulatorId == QStringLiteral("duckstation"))
            return { QStringLiteral("psx"), false };
        return { QString(), false };
    }
}
