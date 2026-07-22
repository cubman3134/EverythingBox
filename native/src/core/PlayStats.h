// Per-game play history: when a game was last played and how long it's been played in total. Recorded at
// launch (last-played) and on the game's exit (accumulated session time), stored in mymediavault.ini and
// shown as facts in the info panel. Per-profile, keyed by a hash of the game's stable identity (its addon
// item id when it has one, else the file/exe path — the same identity RecentStore de-dups on).
#pragma once
#include <QString>

namespace PlayStats
{
    struct Stat
    {
        qint64 lastPlayed   = 0; // epoch seconds of the most recent launch (0 = never played)
        qint64 totalSeconds = 0; // accumulated across all completed sessions
        int    sessions     = 0; // number of completed (timed) sessions
    };

    // The canonical identity for a game: its stable key (addon item id) when present, else its path. Matches
    // how RecentStore identifies an entry, so a game recorded at launch is found again from the info panel.
    QString identity(const QString& key, const QString& path);

    Stat get(const QString& identity);

    void markPlayed(const QString& identity);                  // stamp lastPlayed = now (call at launch)
    void addSession(const QString& identity, qint64 seconds);  // += seconds, ++sessions, lastPlayed = now

    // Sum of totalSeconds across every game in the ACTIVE profile — the "Played" rollup the Stats panel shows
    // (games are read from PlayStats at display; not migrated into ConsumptionStats). 0 when nothing was played.
    qint64 profileTotalSeconds();

    QString formatLastPlayed(qint64 epochSecs); // "Today" / "Yesterday" / "3 days ago" / a date
    QString formatDuration(qint64 seconds);     // "45m" / "2h 15m" / "under a minute"
}
