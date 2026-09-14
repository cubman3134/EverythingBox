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
#include <QList>
#include <QSet>
#include <QString>

namespace GamelistStore
{
    // ---- THE matching rule (#401), public and pure -------------------------------------------------------
    //
    // "Does this list already have that ROM" is decided in exactly one place, here. lookup() and has() use it
    // against the gamelist beside a ROM on disk; LibraryBundle's gamelist transfer (#292) uses the same
    // functions on the target's lists, so the plan the source makes and the landing the target does can never
    // disagree with what this device's own UI then shows.
    //
    // A ROM is listed when some entry's <path> has the same file name, or the same base name (extension
    // ignored), or when the ROM's clean title equals the clean title of an entry's path base or its <name>
    // (tags in () and [] dropped, punctuation dropped). Every comparison is case-insensitive.

    // One <game> as the matcher sees it: its <path> exactly as stored and its <name>.
    struct ListedGame
    {
        QString path;
        QString name;
    };

    // The keys of one list, built once and asked many times.
    class ListMatcher
    {
    public:
        void add(const ListedGame& game);
        bool lists(const QString& romFileName) const;

    private:
        QSet<QString> byFile_, byBase_, byClean_;
    };

    ListMatcher matcherFor(const QList<ListedGame>& games);
    bool listsRom(const QList<ListedGame>& games, const QString& romFileName);

    // The fuzzy key: every (...) and [...] tag dropped, then only lower-case letters and digits kept.
    QString cleanTitle(const QString& s);

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
