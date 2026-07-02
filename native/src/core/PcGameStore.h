// Remembers where each downloaded PC (Windows) game landed so re-opening it launches the installed game
// directly instead of re-running its installer. Keyed by the addon item id (a game's stable release id) and
// stored in mymediavault.ini. Machine-global (an install isn't per-profile).
#pragma once
#include <QString>

namespace PcGameStore
{
    struct Entry
    {
        QString dir;               // where we extracted/downloaded it (<data>/games/pc/<name>)
        QString exe;               // the resolved game executable to launch (empty until found)
        bool installerRan = false; // we've already handed its setup.exe to the OS at least once
    };

    Entry get(const QString& id);
    void setDir(const QString& id, const QString& dir);
    void setExe(const QString& id, const QString& exe);
    void setInstallerRan(const QString& id, bool ran);
}
