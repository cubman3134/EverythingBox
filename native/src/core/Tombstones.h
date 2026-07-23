// Deletion tombstones for the multi-device merge (mdsync T1). When an item is REMOVED from a per-item store
// (a favourite un-starred, a playlist deleted, a tag retired from the vocabulary), we record a dated tombstone
// so the CloudMerge pass (T2) can distinguish "this device deleted it" from "the other device never had it" —
// a tombstone that is newer than a remote item's own timestamp suppresses that item on merge (no resurrection),
// while a newer re-add of the same key beats an older tombstone. Hidden is NOT a delete: hiding an item is a
// mark, not a removal, so it gets a timestamp on its blob (ItemMarks) and never a tombstone.
//
// Storage (QtCore only, on the shared portable mymediavault.ini next to the other stores):
//   deleted/<store>/<hashLeaf>  ->  JSON { "key": <original key>, "ts": <epoch seconds> }
// The <store> namespace mirrors the source store's own namespacing so per-profile stores stay isolated — the
// caller passes a per-profile store name (see the wired sites below), and one profile's tombstones are never
// visible under another's. <hashLeaf> is the MD5-hex of the original key (the SyncOffsets/ItemMarks lesson):
// keys may be URL- or path-shaped (or carry '/'), which QSettings would fold into colliding group paths, so we
// hash for a collision-safe ini leaf and keep the ORIGINAL key in the value JSON so all() can return it intact.
//
// Store namespaces wired in T1 (documented here as the contract T2's serializers read against):
//   * FavoritesStore::remove       -> store "favorites/<profile>",       key = itemId
//   * PlaylistStore::remove        -> store "playlists/<profile>",       key = playlist id (uuid)
//   * ItemMarks::removeTagEverywhere -> store "marks/<profile>/tagVocab", key = tag name (vocab space)
#pragma once
#include <QString>
#include <QVector>

namespace Tombstones
{
    struct Entry
    {
        QString key;   // the ORIGINAL key that was deleted (itemId / playlist id / tag name)
        qint64  ts;    // epoch seconds when the deletion was recorded
    };

    // Record (or refresh) a tombstone for <key> in <store>, stamped with the current epoch second. An empty
    // key or store is a no-op. Recording the same key again overwrites the timestamp (a later delete wins).
    void record(const QString& store, const QString& key);

    // Record a tombstone with an EXPLICIT epoch-second ts — the import path CloudMerge uses to persist a remote
    // device's tombstone faithfully (its own ts, not now). Never downgrades: if a newer local tombstone for the
    // same key already exists, its ts is kept (max wins). An empty key/store or ts<=0 is a no-op.
    void record(const QString& store, const QString& key, qint64 ts);

    // Erase one tombstone (key) from <store> if present. Used when a deletion is UNDONE locally (a tag
    // re-created via setTags, a shelf re-pinned) so the resurrected item is not self-suppressed on the next
    // merge. No-op if the key/store is empty or no such tombstone exists.
    void remove(const QString& store, const QString& key);

    // All live tombstones for <store>, as {original key, ts}. Empty when the store has none.
    QVector<Entry> all(const QString& store);

    // Drop tombstones strictly OLDER than <olderThanDays> days across every store (age > N days is removed;
    // exactly N days old is kept). Returns the number dropped. Bounds the deleted/* footprint over time.
    int compact(int olderThanDays = 30);
}
