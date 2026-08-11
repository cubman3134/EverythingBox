// Per-book bookmarks (issue #136). A bookmark is a labelled ReaderAnchor — one tap from the reader menu,
// listed beside the TOC, jump-to on select. The store owns the LIST; the reader owns capturing the anchor
// (chapter + character offset for a book, page for pdf/comic) and jumping back to it.
//
// SHAPE — favourites/playlists/presets, not speed/metaoverrides. A bookmark is per-PROFILE (annotations are
// per-viewer, "Per-profile, like everything else" in the issue), and a REMOVE is a real deletion that must
// survive a merge, so this store uses the {items, tombs} shape with a delete Tombstone — NOT the husk shape
// the GLOBAL per-item stores (speed/metaoverrides) use for a "clear". Layout on the shared portable
// everythingbox.ini (QtCore only, same AppPaths::dataDir() posture as FavoritesStore):
//   bookmarks/<profile>/items  -> a JSON array of {id, bookKey, anchor:{...}, label, ts}, ALL books together
//   deleted/bookmarks/<profile>/<md5(id)> -> the delete tombstone for a removed bookmark id (Tombstones)
// One flat per-profile list keeps the CloudMerge section a near-verbatim twin of favourites (union by id,
// newest-ts wins, a tombstone at-or-after an item's ts suppresses it); list(bookKey) FILTERS that list to one
// book and sorts it into reading order.
//
// IDENTITY is the POSITION, deterministically. A bookmark's stable merge id is md5(bookKey | canonical-anchor)
// — so bookmarking the same spot twice is idempotent (it folds onto the one id, refreshing the label/ts), and
// two devices that bookmark the same passage converge on ONE row instead of duplicating. This mirrors
// favourites keying on the item's identity; a bookmark's "item" is the spot in the book. An add of a spot that
// was previously removed CLEARS its tombstone (Tombstones::remove — the resurrected row must not self-suppress
// on the next merge), and the fresh ts beats any peer's stale tombstone regardless.
//
// CLOUD SYNC. The "bookmarks/" prefix is in CloudSync::isPerItemStoreKey (NOT isDeviceLocalKey): a bookmark is
// a position the issue explicitly wants to "survive switching devices", so it SYNCS per-item and rides the
// lightweight CloudMerge document, never the heavy settings bundle. probe_cloudmerge pins the classification
// and the merge; probe_bookmarks pins the store.
#pragma once
#include <QString>
#include <QVector>
#include <functional>
#include "../ebook/ReaderAnchor.h"

namespace BookmarkStore
{
    struct Bookmark
    {
        QString      id;       // stable merge id = md5(bookKey | canonical anchor); position-derived, deterministic
        QString      bookKey;  // the book's natural stable key (its file path / addon item id), pre-hash
        ReaderAnchor anchor;   // where in the book
        QString      label;    // optional human label (a chapter title / "N%"); empty is allowed
        qint64       ts = 0;   // epoch seconds of the last write (multi-device merge: newest-ts wins per id)
    };

    // The ini key the active profile's bookmark list lives under ("bookmarks/<profile>/items"), and the
    // tombstone store namespace for it ("bookmarks/<profile>") — named here so the store and CloudMerge's
    // serializer/merger cannot drift on the spelling.
    QString itemsKey();
    QString tombstoneStore();

    // The deterministic merge id for a bookmark at `anchor` in `bookKey`. Independent of time, so an identical
    // spot always maps to the same id (idempotent add, cross-device convergence). Empty bookKey -> empty id.
    QString idFor(const QString& bookKey, const ReaderAnchor& anchor);

    // Add (or refresh) a bookmark at `anchor` in `bookKey`, stamped now. Idempotent by position: re-adding the
    // same spot folds onto the one id and updates its label/ts. Clears any delete tombstone for that id.
    // Returns the stored Bookmark; an empty bookKey is a no-op returning a default-constructed Bookmark.
    Bookmark add(const QString& bookKey, const ReaderAnchor& anchor, const QString& label = QString());

    // Remove the bookmark with `id` from the active profile and record a delete tombstone so a peer that still
    // holds it cannot resurrect it on merge. No-op for an empty/unknown id.
    void remove(const QString& id);

    // This book's bookmarks, sorted in reading order (ReaderAnchor::inReadingOrder). Empty bookKey -> empty.
    QVector<Bookmark> list(const QString& bookKey);

    // Every bookmark in the active profile (all books), unsorted. For the annotation-list panel / diagnostics.
    QVector<Bookmark> all();

    // Multi-device sync trigger, mirroring FavoritesStore::setChangeHook: a lightweight std::function fired
    // after every mutation so MainWindow can (re)arm the debounced Drive push. QtCore-clean; unset in probes.
    void setChangeHook(std::function<void()> hook);
}
