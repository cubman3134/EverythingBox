// "Follow this series" — the per-profile, SYNCED mark behind issue #155. A peer of the favourite in every
// respect that matters: per profile, one row per item, stored as a JSON list in the portable
// everythingbox.ini, carrying enough of the item to re-open it AND to re-ask its source for children, and
// merged across devices by the SAME rule favourites use (union by itemId keeping the newest ts, with a
// deletion tombstone that a strictly-newer re-add beats).
//
// WHY THE FAVOURITES SHAPE AND NOT ITEMMARKS' HUSK. ItemMarks (#132) writes a husk on clear because its rows
// carry several independent marks and "cleared" has to be distinguishable from "never known". A follow is a
// single boolean with a payload: unfollowing is a DELETE of the whole row, which is exactly what a tombstone
// is for, and it is what FavoritesStore/PlaylistStore/BookmarkStore already do. Following the favourites
// idiom also means CloudMerge gains a serializer that is the favourites serializer with a different key,
// rather than a fifth merge rule to get right.
//
// Layout, per profile (or "default"), mirroring favourites exactly so the two read alike:
//   follow/<profileId>/items      -> JSON array of the followed rows
//   deleted/follow/<profileId>/…  -> Tombstones for unfollowed itemIds
//
// SYNC CLASSIFICATION. "follow/" is a PER-ITEM STORE key (CloudSync::isPerItemStoreKey): it rides the small
// CloudMerge progress document, never the heavy settings bundle — one follow press must not flip the bundle's
// stateHash and re-upload the whole zip, and an inbound bundle would write the row raw, bypassing the merge.
// The DEVICE-LOCAL half of this feature (what each device has already seen) is a different store entirely —
// see FollowSnapshot.h, which is carved out under its own prefix for the opposite reason.
//
// QtCore only (a QSettings wrapper, no Quick/Widgets), so it links into probe_follow and runs headless.
#pragma once
#include <QString>
#include <QVector>
#include <functional>

struct FollowItem
{
    QString addonId;       // source addon (the refresh re-resolves the LoadedAddon by this to ask for children)
    QString itemId;        // the source's item id — the follow's identity, and the getDetail key
    QString title;
    QString subtitle;
    QString type;          // media type (series/podcast/manga/...) — drives the icon and the detail route
    QString thumbnailUrl;
    qint64  ts = 0;        // epoch seconds this follow was added (multi-device merge: newest-ts wins)
};

namespace FollowStore
{
    QVector<FollowItem> list();                  // for the active profile, newest first
    void add(const FollowItem& item);            // de-duped by itemId; stamps ts at the mutation site
    void remove(const QString& itemId);          // + a deletion tombstone, so a peer cannot resurrect it
    bool isFollowed(const QString& itemId);
    int  count();                                // how many series this profile follows

    // Multi-device sync trigger (mdsync T2): a change-callback fired after add/remove to (re)arm the debounced
    // push, set once by MainWindow. A std::function rather than a Qt signal so the store keeps zero
    // QObject/Quick/Widgets dependency; unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);
}
