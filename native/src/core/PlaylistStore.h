// Per-profile, per-catalogue playlists: user-curated lists of catalog items. Each catalogue (keyed by its
// source addon + catalog id + type) shows a "Playlists" folder at the top; a playlist is an ordered set of
// items kept here. Stored as JSON in mymediavault.ini under the active profile, so each user has their own.
// Enough of each item is kept (like a FavoriteItem) to display it and re-open/resolve it through its addon.
#pragma once
#include <QString>
#include <QVector>

struct PlaylistEntry
{
    QString addonId;       // the item's source addon (to resolve the LoadedAddon when re-opening)
    QString itemId;        // the addon's item id - also the entry's identity within a playlist
    QString title;
    QString subtitle;
    QString type;          // media type (movie/track/book/…) - drives the icon + open path
    QString thumbnailUrl;  // poster/cover to show
    bool expandable = false;
};

struct Playlist
{
    QString id;           // unique (a uuid)
    QString catalogKey;   // the catalogue this belongs to ("addonId|catalogId|catalogType")
    QString name;
    QVector<PlaylistEntry> items;
};

namespace PlaylistStore
{
    QVector<Playlist> forCatalog(const QString& catalogKey); // this catalogue's playlists (active profile)
    bool get(const QString& id, Playlist& out);              // false if no such playlist
    QString create(const QString& catalogKey, const QString& name); // returns the new playlist's id
    void rename(const QString& id, const QString& name);
    void remove(const QString& id);
    void addItem(const QString& id, const PlaylistEntry& item); // appended, de-duped by itemId
    void removeItem(const QString& id, const QString& itemId);
    bool contains(const QString& id, const QString& itemId);
}
