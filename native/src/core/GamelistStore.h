// EmulationStation / RetroBat gamelist.xml as a metadata source. A ROMs folder set up ES-style has, next to
// the ROMs, a gamelist.xml listing each game's scraped metadata + media (./images, ./videos). This reads
// that: given a ROM's path we find its system's gamelist.xml, match the <game> by ROM filename, and return
// the scraped card with every media role resolved to a local file (so it renders offline, no scrape needed).
//
// It's the first source in the metadata chain — checked before our own scrape cache and before hitting the
// online providers — so existing ES/RetroBat data is used as-is. write() persists a fresh scrape back into
// the same gamelist.xml + media folders (ES format) when the "keep scraped data" setting is on.
//
// Parsed gamelists are cached in memory per folder (a system's list is one XML for hundreds of games), so
// scrolling a console re-reads nothing.
#pragma once
#include "../addons/AddonModels.h"
#include <QString>

namespace GamelistStore
{
    // The scraped card for `romPath` from its system's gamelist.xml, media resolved to absolute local files
    // (only files that actually exist are included). valid == false when there's no gamelist or no match.
    MediaDetail lookup(const QString& romPath);

    // Cheap "is this ROM in a gamelist" check (parses/caches the folder once).
    bool has(const QString& romPath);

    // Persist a scraped result into the ROM's gamelist.xml + ./images ./videos (ES/RetroBat layout),
    // downloading remote art. Merges into an existing <game> (matched by ROM filename) or appends one.
    void write(const QString& romPath, const MediaDetail& detail);

    void clearCache(); // drop the in-memory parse cache (after an external change / a write)
}
