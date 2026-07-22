// A small persistent list of recently opened content (videos, audio, books/PDFs, games). Stored as a
// JSON array in mymediavault.ini so it survives restarts; the Home screen's "Recent" tab lists it and the
// main window re-opens an entry by its kind. Newest first, de-duplicated by path, capped.
#pragma once
#include <QString>
#include <QVector>

struct RecentItem
{
    QString path;   // absolute file path / URL to re-open
    QString title;  // display label
    QString kind;   // "video" | "audio" | "document" | "game" | "pcgame" | "steamgame"
                    // A "steamgame" is a native Steam launch: path is the steam://rungameid/<appid> URL, key is
                    // "steam:<appid>", thumb is the vertical capsule; re-opening hands the URL back to Steam.
    QString thumb;  // optional poster image (path or http url); empty -> a type placeholder is drawn
    QString key;    // stable identity for resume + de-dup (e.g. an addon item id); empty -> use path. A
                    // streamed item's URL changes each session, so resume/de-dup key on this instead.
    QString system; // games only: the resolved SystemCatalog id (e.g. "psx", "gc") the game launched with,
                    // so re-opening picks the right console instead of guessing from a shared extension
                    // (.iso/.cue/.chd/.bin). Empty for non-games / legacy entries.
    qint64  ts = 0; // last-opened time (unix seconds); set on add(). Lets cross-device sync merge by recency.
};

namespace RecentStore
{
    QVector<RecentItem> list();          // newest first
    void add(const RecentItem& item);    // move-to-front + de-dup by path + cap
    void remove(const QString& pathOrKey); // drop the entry whose path or key matches
    void clear();

    // How a Recent of a given kind is re-launched (the pure dispatch table). "steamgame"/"pcgame" relaunch
    // through their native launchers; the media kinds re-open their recorded file/URL. MainWindow::openRecent
    // switches on this so the app and the headless probe share one definition of the dispatch.
    enum class Relaunch { SteamGame, PcGame, Video, Audio, Document, Game, Unknown };
    Relaunch relaunchFor(const QString& kind);
}
