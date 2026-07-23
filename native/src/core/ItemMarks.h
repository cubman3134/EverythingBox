// Per-profile item marks — the library-management store behind Playnite-class shelves/filtering. Each
// (profile, item) carries: hidden (bool), completion (None|InProgress|Finished|Abandoned|Planned), and a
// tag list; plus a per-profile tag vocabulary and a per-profile set of pinned tags (the tags that render as
// home shelves). Favorites are deliberately NOT here — they stay in the existing per-profile FavoritesStore
// (already verb-wired); the "Favorites" shelf builds FROM FavoritesStore. ItemMarks owns hidden/completion/
// tags only.
//
// Backed by the portable mymediavault.ini (same AppPaths::dataDir() posture as SyncOffsets/FavoritesStore —
// QtCore only, no Quick/Widgets). Layout, all namespaced by the active profile id (or "default"):
//   marks/<profile>/items/<itemHash>  -> JSON object { hidden, completion, tags } (one blob per item)
//   marks/<profile>/tagVocab          -> JSON array of the profile's known tags
//   marks/<profile>/pinnedTags        -> JSON array of pinned (shelf) tags
//
// Item keys are the SAME stable keys the app already uses (addon itemId / local path keys). They are hashed
// (MD5-over-UTF8 hex, the SyncOffsets lesson) BEFORE use as an ini group leaf, so keys that differ only in
// empty/duplicate '/' separators — or a URL-shaped key — never alias to the same entry. The store owns key
// sanitization; callers pass their natural keys. An empty key is a no-op on every writer and reads back {}.
//
// Cache: get() is the hot path — row/hidden filtering calls it once per catalog item. A QHash<itemHash,
// Marks> is built LAZILY for the ACTIVE profile on first read and reused, so a warm get() costs only a
// ProfileStore::currentId() read + a cheap profile compare (the self-healing profile-switch check) + one MD5
// of the caller key for the lookup — the blob parse and the "marks/<id>/items" group-string resolution happen
// once per build, not per call. invalidate() drops it (call on profile switch or any external ini change).
// ItemMarks' own writers invalidate for you.
//
// NOTE on shelves: the stored item keys are HASHED and MD5 is one-way, so a hash can NOT be reversed back to
// an item. Shelf/row building therefore INTERSECTS the current catalog: for each candidate item the builder
// hashes its natural keyFor and tests membership (itemKeysWithTag() returns the HASHED keys to test against,
// and get() is the cache-backed per-item lookup). There is intentionally no "list every item with tag X as
// objects" API — the catalog is the source of items; ItemMarks only answers per-key.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace ItemMarks
{
    enum class Completion { None, InProgress, Finished, Abandoned, Planned };

    struct Marks
    {
        bool        hidden = false;
        Completion  completion = Completion::None;
        QStringList tags;
        qint64      updatedAt = 0;  // epoch seconds of the last write (multi-device merge: newest-wins per item)
    };

    Marks get(const QString& key);                       // cached; empty/unknown key -> default {}

    void setHidden(const QString& key, bool hidden);
    void setCompletion(const QString& key, Completion c);
    void setTags(const QString& key, const QStringList& tags);  // also unions new tags into the vocab

    QStringList tagVocab();                              // the active profile's known tags
    void        removeTagEverywhere(const QString& tag); // vocab + strip from all items + unpin (per profile)

    QStringList pinnedTags();                            // the active profile's shelf-pinned tags
    void        setPinned(const QString& tag, bool pinned);

    QVector<QString> itemKeysWithTag(const QString& tag); // HASHED item keys carrying the tag (see NOTE above)

    bool anyHidden();                                    // fast: does the active profile have ANY hidden item

    void invalidate();                                  // drop the cache (profile switch / external change)
}
