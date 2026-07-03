// Per-system BIOS / firmware files that a core or external emulator needs but can't ship with (they're
// copyrighted dumps). When a system is launched, CoreManager::ensureBios downloads any that are missing
// into the right folder (the libretro "system" dir for in-process cores; the emulator's own "bios" folder
// for standalone ones). Source: the Abdess/retrobios mirror, fetched raw over https.
//
// Only systems that actually need a BIOS to boot are listed; everything else returns an empty list and
// ensureBios is a no-op. File names are exactly what the core/emulator expects to find.
#pragma once
#include <QString>
#include <QStringList>
#include <QList>

struct BiosFile
{
    QString fileName; // exact name the core/emulator looks for (saved under the system/bios folder)
    QString url;      // direct https download
    QString md5;      // expected MD5 of a good dump (lowercase hex); empty => presence-only, no hash check
};

namespace BiosCatalog
{
    // raw.githubusercontent.com path under the retrobios "bios/" tree (spaces are %20-encoded here so the
    // literal URLs stay copy-pasteable). Kept in one place so the mirror is easy to repoint.
    inline QString retrobios(const QString& relPath)
    {
        return QStringLiteral("https://raw.githubusercontent.com/Abdess/retrobios/main/bios/") + relPath;
    }

    // Systems that need a BIOS (the keys forSystem() knows about), with display names — drives the BIOS
    // checker in Settings. Kept in sync with forSystem() below.
    struct BiosSystem { QString systemId; QString name; };
    inline const QList<BiosSystem>& systemsWithBios()
    {
        static const QList<BiosSystem> list = {
            { QStringLiteral("psx"),    QStringLiteral("PlayStation") },
            { QStringLiteral("saturn"), QStringLiteral("Sega Saturn") },
            { QStringLiteral("3do"),    QStringLiteral("3DO") },
            { QStringLiteral("ps2"),    QStringLiteral("PlayStation 2") },
        };
        return list;
    }

    inline const QList<BiosFile>& forSystem(const QString& systemId)
    {
        static const QList<BiosFile> none;

        // PlayStation (Beetle PSX / SwanStation): the three region BIOSes, looked up by name in the
        // system folder. Beetle wants the matching region; shipping all three covers any disc.
        static const QList<BiosFile> psx = {
            { QStringLiteral("scph5500.bin"), retrobios(QStringLiteral("Sony/PlayStation/scph5500.bin")), QStringLiteral("8dd7d5296a650fac7319bce665a6a53c") }, // JP
            { QStringLiteral("scph5501.bin"), retrobios(QStringLiteral("Sony/PlayStation/scph5501.bin")), QStringLiteral("490f666e1afb15b7362b406ed1cea246") }, // US
            { QStringLiteral("scph5502.bin"), retrobios(QStringLiteral("Sony/PlayStation/scph5502.bin")), QStringLiteral("32736f17079d0b2b7024407c39bd3050") }, // EU
        };

        // Sega Saturn (Beetle Saturn / Kronos): JP + US/EU BIOS, both named as the cores expect.
        static const QList<BiosFile> saturn = {
            { QStringLiteral("sega_101.bin"),  retrobios(QStringLiteral("Sega/Saturn/sega_101.bin")),  QStringLiteral("85ec9ca47d8f6807718151cbcca8b964") },  // JP
            { QStringLiteral("mpr-17933.bin"), retrobios(QStringLiteral("Sega/Saturn/mpr-17933.bin")), QStringLiteral("3240872c70984b6cbfda1586cab68dba") }, // US/EU
        };

        // Panasonic 3DO (Opera): the FZ-1 BIOS is the common default.
        static const QList<BiosFile> threedo = {
            { QStringLiteral("panafz1.bin"), retrobios(QStringLiteral("3DO%20Company/3DO/panafz1.bin")), QStringLiteral("f47264dd47fe30f73ab3c010015c155b") },
        };

        // PlayStation 2 (PCSX2, standalone): a single full BIOS dump is enough; PCSX2 auto-detects any
        // valid image in its bios folder. This is a late USA dump that covers most discs. (No canonical MD5
        // recorded for this particular dump, so it's checked for presence only.)
        static const QList<BiosFile> ps2 = {
            { QStringLiteral("ps2-0230a-20080220.bin"),
              retrobios(QStringLiteral("Sony/PlayStation%202/ps2-0230a-20080220.bin")), QString() },
        };

        if (systemId == QStringLiteral("psx"))    return psx;
        if (systemId == QStringLiteral("saturn")) return saturn;
        if (systemId == QStringLiteral("3do"))    return threedo;
        if (systemId == QStringLiteral("ps2"))    return ps2;
        return none;
    }

    // Standalone emulators that can't boot without a copyrighted BIOS. Maps the emulator id (EmulatorRegistry)
    // to the system whose BIOS it needs and whether to keep that BIOS under our folder via a portable marker.
    // Kept here (rather than as ExternalEmulator fields) so the emulator registry stays untouched.
    struct ExternalBios
    {
        QString systemId; // BIOS to fetch (forSystem key), empty => this emulator needs none
        bool portable;    // drop a portable.ini marker next to the binary so config + bios stay in our folder
    };

    inline ExternalBios forExternalEmulator(const QString& emulatorId)
    {
        if (emulatorId == QStringLiteral("pcsx2"))
            return { QStringLiteral("ps2"), true };
        return { QString(), false };
    }
}
