// PC-game identity and source selection. The PC library used to be one folder per launcher (Steam,
// GOG, Epic, Battle.net) plus a folder of downloaded games, so the same game appeared several times
// with unrelated ids. This unit is what lets ONE entry carry all of them as sources — the same shape
// the video stream picker already uses for one movie with many streams.
//
// Pure and QtCore-only (the override store is a small ini map), so probe_pcgames links lean.
#pragma once
#include <QString>
#include <QVector>

namespace pcgame
{
    // The matching key. Lowercases, collapses whitespace, strips punctuation, trademark symbols and
    // edition noise (Game of the Year / Definitive / Remastered / Director's Cut / a trailing year).
    //
    // It deliberately does NOT strip SEQUEL NUMERALS, Arabic or Roman. "Hades" and "Hades II" are
    // different games: merging them removes one from the user's library, while failing to merge two
    // editions only shows a game twice. That asymmetry is why the numeral rule is a hard requirement
    // and not a nicety.
    //
    // The edition strip is a SUFFIX rule: those phrases are stripped only off the END of the title,
    // because mid-title they are part of the product name ("Command & Conquer Remastered Collection").
    //
    // KNOWN COLLISION, carried on purpose: the trailing-year strip merges a REMAKE with its original.
    // "Prey (2006)" and "Prey (2017)" both normalise to "prey", as do "Resident Evil 2" and "Resident
    // Evil 2 (2019)". The igdb id is what disambiguates them (see sameGame / mergeKey); a TITLE-ONLY
    // library will show the remake and the original as ONE entry. See the note in the .cpp for why the
    // year strip is nonetheless required.
    QString normalizeTitle(const QString& raw);

    // The grouping key used by the catalog builder: the igdb id when there is one, else the
    // normalised title. Two entries group together iff their mergeKey matches.
    //
    // A title can normalise to EMPTY ("!!!", "GOTY", "Enhanced Edition"). Such an entry gets a private
    // fallback key derived from its raw title, so it groups with nothing but itself instead of every
    // other empty-normalising entry landing in one bucket. Callers do not have to special-case it.
    QString mergeKey(const QString& title, const QString& igdbId);

    // THE PC-GAME ITEM ID. The single place that turns a game's title into the id everything else keys
    // on — the catalog tile, and therefore the user's favourite, hidden/completion/tags, play time,
    // consumption seconds and resume position, all of which are stored under a hash of THIS string.
    //
    // It exists because two callers used to build it independently (pcGamesCatalog and
    // pcgame::remapTable) from the same ingredients but not the same arguments, and the failure mode of
    // them disagreeing is invisible: the remap moves every record onto an id no lookup ever performs, so
    // the user's stars and hours simply stop existing with nothing logged. Two functions that must agree
    // by convention is how that happened, so there is now one function and both call it. Do not inline
    // "pcgame:" + mergeKey(...) anywhere; call this.
    //
    // DELIBERATELY TITLE-ONLY, i.e. NOT igdb-aware. mergeKey will key on an igdb id when handed one, and
    // doing that here would be a real improvement in one respect — it is what separates "Prey (2006)"
    // from "Prey (2017)", which the year strip otherwise fuses. It is rejected anyway, on two grounds:
    //   * NOTHING SUPPLIES ONE at the point the id is minted. The catalog builder is given four launcher
    //     scans and a downloads list; no metadata resolver runs there, and none can, because the id has
    //     to exist before the tile it would fetch metadata for.
    //   * AN ID MUST NOT DEPEND ON A NETWORK RESULT. Metadata resolution is partial and varies run to
    //     run — a game resolved today and unresolved tomorrow would change identity between two refreshes
    //     and strand its own records, which is precisely the harm this whole unit exists to prevent. An
    //     id has to be a pure, total function of what the local scan yields.
    // Returns an EMPTY string for a title with nothing to group on; callers treat that as "skip", never
    // as a key (see rule 1 in PcGameRemap.h).
    QString itemId(const QString& title);

    // Are these the same game? A user override wins; then, when BOTH sides carry an igdb id, that
    // decides (equal ids -> same, different ids -> NOT same, even if the titles agree); otherwise the
    // normalised titles decide. A missing id on one side is not a mismatch — it just means fall back.
    bool sameGame(const QString& titleA, const QString& igdbA,
                  const QString& titleB, const QString& igdbB);

    // The user's manual "these are/aren't the same" verdicts, keyed on the pair of normalised titles
    // and symmetric in the pair. This is the escape hatch that makes a fuzzy heuristic shippable: a
    // wrong merge is otherwise uncurable.
    bool overrideSaysSame(const QString& normA, const QString& normB);
    void setOverride(const QString& normA, const QString& normB, bool same);

#ifdef EB_PCGAMEID_TEST_SEAM
    // Test-only ini redirect, declared and compiled ONLY for probe_pcgames (same rule as
    // ProfilePasscode / SettingsTxn / ThemeChoice): without the macro this does not exist, so a
    // production call is a compile error rather than a silent process-wide redirect.
    //
    // It is not a nicety. The override store PERSISTS, so a probe run that writes into the app's real
    // ini leaves state behind that the NEXT run reads — and a mutation run that normalises titles
    // wrongly writes a key no correct build would ever produce, which then fails the reverted build
    // and reads as "the revert didn't work". The probe must own a fresh file.
    void setIniPathForTesting(const QString& path);
#endif

    // One way to launch a game.
    struct PcGameSource
    {
        enum Kind { LauncherInstalled, LauncherOwned, Downloaded, AddonAvailable };
        Kind    kind = LauncherInstalled;
        QString launcher;     // "steam" | "epic" | "gog" | "battlenet"; empty for an addon source
        QString launchId;     // appid / appName / gog id / battle.net code
        QString exePath;      // when the launch is a direct exe
        QString launchUrl;    // when the launch is a protocol URL
        QString addonItemId;  // Downloaded / AddonAvailable
        QString label;        // the picker row
        bool    ready = false; // launches NOW, with no download
    };

    // Which source should Play use? Returns an index, or -1 meaning "ask the user".
    //
    // Exactly one ready source -> that one. Several ready, or none ready -> ask. It must NEVER return
    // a not-ready source: Play is a single keypress, and silently starting a multi-gigabyte download
    // from it is precisely what this function exists to prevent.
    int pickAutoSource(const QVector<PcGameSource>& all);
}
