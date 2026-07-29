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
    QString normalizeTitle(const QString& raw);

    // The grouping key used by the catalog builder: the igdb id when there is one, else the
    // normalised title. Two entries group together iff their mergeKey matches.
    QString mergeKey(const QString& title, const QString& igdbId);

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
