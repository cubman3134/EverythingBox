// Per-profile, per-CATEGORY playlists: user-curated lists of catalog items. A playlist belongs to one of the
// four inherent buckets (video / audio / game / reading — see core/MediaCategories.h), so e.g. a single Video
// playlist can mix episodes and movies from any video catalogue. A "Playlists" folder shows at the category
// level and at every catalogue root of that category. Stored as JSON in mymediavault.ini under the active
// profile, so each user has their own. Enough of each item is kept (like a FavoriteItem) to display it and
// re-open/resolve it through its addon (each entry carries its own addonId — playlists may be mixed-source).
//
// Migration: earlier builds keyed each playlist by a single catalogKey ("addonId|catalogId|catalogType").
// migrateToCategories() folds that to categoryKey = mediaCategory(catalogType) once (stamped, idempotent),
// preserving the original catalogKey in legacyKey; ids and items are never touched.
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
    // Local-file entries (a Recent/Downloaded game added from its item menu): re-open by path through
    // openRecent instead of via an addon. Empty for ordinary addon-catalog entries.
    QString path;          // absolute file path to re-open
    QString kind;          // "game" | "pcgame" | … (openRecent routing kind)
};

struct Playlist
{
    QString id;           // unique (a uuid)
    QString categoryKey;  // the bucket this belongs to: "video" | "audio" | "game" | "reading"
    QString legacyKey;    // pre-migration catalogKey ("addonId|catalogId|catalogType"), preserved for provenance
    QString name;
    QVector<PlaylistEntry> items;
    qint64  updatedAt = 0; // epoch seconds of the last mutation (multi-device merge: whole-object newest-wins)
};

namespace PlaylistStore
{
    QVector<Playlist> forCategory(const QString& categoryKey); // this category's playlists (active profile)
    bool get(const QString& id, Playlist& out);                // false if no such playlist
    QString create(const QString& categoryKey, const QString& name); // returns the new playlist's id
    void rename(const QString& id, const QString& name);
    void remove(const QString& id);
    void addItem(const QString& id, const PlaylistEntry& item); // appended, de-duped by itemId
    void removeItem(const QString& id, const QString& itemId);
    bool contains(const QString& id, const QString& itemId);

    // One-time (per profile), stamped, idempotent: fold any legacy catalogKey-scoped playlist to a
    // categoryKey via mediaCategory(catalogType), preserving the original in legacyKey. Runs automatically on
    // every store access; safe to call directly. Returns true if it wrote a migration this call.
    bool migrateToCategories();
}
