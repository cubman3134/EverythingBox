// Per-profile favourites: catalog items a user has starred. They appear in a "Favorites" section on the
// Home page and re-open their detail page when clicked. Stored as a JSON list in mymediavault.ini, keyed by the
// active profile (so each user has their own). Enough of the MediaItem is kept to display + re-open it.
#pragma once
#include <QString>
#include <QVector>

struct FavoriteItem
{
    QString addonId;       // source addon (to resolve the LoadedAddon when re-opening)
    QString itemId;        // the addon's item id (getDetail/getMeta key) - also the favourite's identity
    QString title;
    QString subtitle;
    QString type;          // media type (movie/series/album/...) - drives the icon
    QString thumbnailUrl;  // poster/cover to show
    bool expandable = false;
    // Local-file favourites (a Recent/Downloaded game starred from its item menu): these re-open by path
    // through openRecent (which recovers the console from the Recent/Downloads store) instead of via an addon.
    // Empty for ordinary addon-catalog favourites.
    QString path;          // absolute file path to re-open
    QString kind;          // "game" | "pcgame" | … (openRecent routing kind)
};

namespace FavoritesStore
{
    QVector<FavoriteItem> list();               // for the active profile
    void add(const FavoriteItem& item);         // de-duped by itemId, newest first
    void remove(const QString& itemId);
    bool isFavorite(const QString& itemId);
}
