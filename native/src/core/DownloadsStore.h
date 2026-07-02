// A persistent, uncapped record of content that's been fully downloaded to disk (the "Downloaded" folder in
// each catalogue). Distinct from RecentStore (which is capped and also holds streamed/played items): this is
// only things saved for keeps - library downloads, PC games, and installed games. Machine-global (a file on
// disk isn't tied to a profile) and stored as a JSON array in mymediavault.ini.
#pragma once
#include <QString>
#include <QVector>

struct DownloadedItem
{
    QString path;   // absolute local file to re-open (a ROM/movie/book file, or a PC game's exe)
    QString title;  // display label
    QString kind;   // "video" | "audio" | "document" | "game" | "pcgame"
    QString thumb;  // optional poster image (path or http url)
    QString key;    // stable identity for de-dup (an addon item id); empty -> use path
    QString system; // games only: canonical SystemCatalog id (e.g. "psx"), or "pc" for a PC game; groups the
                    // Downloaded folder per console. Empty for non-games.
};

namespace DownloadsStore
{
    QVector<DownloadedItem> list();          // newest first
    void add(const DownloadedItem& item);    // move-to-front + de-dup by key/path (uncapped)
    void remove(const QString& pathOrKey);
}
