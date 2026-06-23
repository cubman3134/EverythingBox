// Per-profile favourites: catalog items a user has starred. They appear in a "Favorites" section on the
// Home page and re-open their detail page when clicked. Stored as a JSON list in goliath.ini, keyed by the
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
};

namespace FavoritesStore
{
    QVector<FavoriteItem> list();               // for the active profile
    void add(const FavoriteItem& item);         // de-duped by itemId, newest first
    void remove(const QString& itemId);
    bool isFavorite(const QString& itemId);
}
