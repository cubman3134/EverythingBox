// A local ROM library laid out RetroBat / EmulationStation-Desktop-Edition style: <root>/<system>/<roms>.
// The root is configurable (Settings::romsFolder, default <data>/roms). We create a subfolder per emulated
// system so the user just drops ROMs in, then scan those folders and surface the games — each launches
// through the normal game path (openGamePath), picking the right core/emulator from the system id.
#pragma once
#include <QString>
#include <QVector>

struct GameSystem;

namespace RomLibrary
{
    struct Rom
    {
        QString path;       // absolute path to the ROM file
        QString title;      // display name (file base name)
        QString systemId;   // SystemCatalog id (used as the launch systemHint)
        QString systemName; // system display name
    };

    struct SystemGroup
    {
        QString systemId;
        QString systemName;
        QString folder;     // the on-disk subfolder name it was found under
        QVector<Rom> roms;  // sorted by title
    };

    // The configured library root (Settings::romsFolder). Never empty.
    QString root();

    // The canonical (ES-DE/RetroBat-style) folder name for a system id — e.g. "genesis" -> "megadrive".
    QString folderFor(const QString& systemId);

    // Reverse of folderFor, but tolerant: matches the canonical name, our own id, common aliases, and finally
    // a console-name match. Returns nullptr if the folder isn't a system we emulate.
    const GameSystem* systemForFolder(const QString& folderName);

    // Create the root and a subfolder for every emulated system (plus a README), so the tree is ready to fill.
    void ensureStructure();

    // Scan the library: one group per system folder that actually contains ROMs, each with its games.
    QVector<SystemGroup> scan();

    // Pull any ROMs sitting in the library folders (e.g. added manually) into the Downloaded list, so they
    // show up in the home's Downloaded folder alongside downloaded games — playable without visiting Library.
    // Only adds ROMs not already recorded (so it doesn't reshuffle the list). Returns how many were added.
    int syncToDownloads();
}
