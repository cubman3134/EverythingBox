// Per-item audio bookmarks (issue #140). A bookmark is a labelled TIME anchor in an audio item — one tap from
// the now-playing transport drops the current position, listed in position order, jump-to on select. The store
// owns the LIST; the transport owns capturing the live position and seeking back to it.
//
// SHAPE — favourites/playlists, NOT speed/metaoverrides. Like #136's reading bookmarks, an audio bookmark is
// per-PROFILE (annotations are per-viewer, "All per-profile" in the issue), and a REMOVE is a real deletion that
// must survive a merge, so this store uses the {items, tombs} shape with a delete Tombstone — NOT the husk shape
// the GLOBAL per-item stores (speed/metaoverrides) use for a "clear". Layout on the shared portable
// everythingbox.ini (QtCore only, same AppPaths::dataDir() posture as BookmarkStore/FavoritesStore):
//   audiobookmarks/<profile>/items  -> a JSON array of {id, itemKey, posSec, label, ts}, ALL items together
//   deleted/audiobookmarks/<profile>/<md5(id)> -> the delete tombstone for a removed bookmark id (Tombstones)
// One flat per-profile list keeps the CloudMerge section a near-verbatim twin of #136's bookmarks (union by id,
// newest-ts wins, a tombstone at-or-after an item's ts suppresses it); list(itemKey) FILTERS that list to one
// item and sorts it into playback (position) order.
//
// IDENTITY is the POSITION, deterministically. A bookmark's stable merge id is md5(itemKey | whole-second) — so
// bookmarking the same spot twice is idempotent (it folds onto the one id, refreshing the label/ts), and two
// devices that bookmark the same passage converge on ONE row instead of duplicating. The identity rounds to a
// whole SECOND (the resolution a listener actually cares about); the stored posSec keeps its precise double. An
// add of a spot that was previously removed CLEARS its tombstone (the resurrected row must not self-suppress on
// the next merge), and the fresh ts beats any peer's stale tombstone regardless.
//
// CLOUD SYNC. The "audiobookmarks/" prefix is in CloudSync::isPerItemStoreKey (NOT isDeviceLocalKey): a bookmark
// position is a property of the CONTENT the issue wants to survive switching devices, exactly like a reading
// bookmark or the resume position — so it SYNCS per-item and rides the lightweight CloudMerge document, never the
// heavy settings bundle. (Note it does NOT match the device-local "audio/" prefix — "audiobookmarks" has no
// slash at that boundary — but that is asserted explicitly in probe_cloudmerge so a future refactor cannot break
// it silently.) probe_cloudmerge pins the classification + the merge; probe_audiobookmarks pins the store.
#pragma once
#include <QString>
#include <QVector>
#include <functional>

namespace AudioBookmarkStore
{
    struct Bookmark
    {
        QString id;       // stable merge id = md5(itemKey | whole-second); position-derived, deterministic
        QString itemKey;  // the audio item's stable key (the same key resume/speed use), pre-hash
        double  posSec = 0.0; // the bookmarked position in seconds (the precise double is kept)
        QString label;    // optional human label (a "12:34" timestamp / a note); empty is allowed
        qint64  ts = 0;   // epoch seconds of the last write (multi-device merge: newest-ts wins per id)
    };

    // The ini key the active profile's bookmark list lives under ("audiobookmarks/<profile>/items"), and the
    // tombstone store namespace for it ("audiobookmarks/<profile>") — named here so the store and CloudMerge's
    // serializer/merger cannot drift on the spelling.
    QString itemsKey();
    QString tombstoneStore();

    // The deterministic merge id for a bookmark at `posSec` in `itemKey`. Rounds to a whole second so an
    // identical spot always maps to the same id (idempotent add, cross-device convergence). Empty itemKey -> "".
    QString idFor(const QString& itemKey, double posSec);

    // Add (or refresh) a bookmark at `posSec` in `itemKey`, stamped now. Idempotent by position (whole second):
    // re-adding the same spot folds onto the one id and updates its label/ts/precise posSec. Clears any delete
    // tombstone for that id. Returns the stored Bookmark; an empty itemKey is a no-op returning a default one.
    Bookmark add(const QString& itemKey, double posSec, const QString& label = QString());

    // Remove the bookmark with `id` from the active profile and record a delete tombstone so a peer that still
    // holds it cannot resurrect it on merge. No-op for an empty/unknown id.
    void remove(const QString& id);

    // This item's bookmarks, sorted in playback (ascending position) order. Empty itemKey -> empty.
    QVector<Bookmark> list(const QString& itemKey);

    // Every bookmark in the active profile (all items), unsorted. For diagnostics / the probe.
    QVector<Bookmark> all();

    // Multi-device sync trigger, mirroring BookmarkStore::setChangeHook: a lightweight std::function fired after
    // every mutation so MainWindow can (re)arm the debounced Drive push. QtCore-clean; unset in probes.
    void setChangeHook(std::function<void()> hook);
}
