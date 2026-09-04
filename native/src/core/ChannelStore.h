// Per-profile storage for personal TV channels (issue #179, increment 1) — a channel's SOURCE, ORDERING and
// START EPOCH, kept beside the other small per-profile stores and synced with them.
//
// Layout: channels/<profile>/items -> JSON array of { id, name, src, srcid, ord, start, beg, ts }. QtCore
// only (no Quick/Widgets), so it rides the same profile-scoped ini FavoritesStore/FilterPresetStore use.
//
// THE SHAPE IS FilterPresetStore's, DELIBERATELY, down to the field names it shares, because the two rules
// that store was rewritten to obey (#184) apply here unchanged and neither is optional:
//
//   * IDENTITY IS THE id, NOT THE NAME. A rename is a mutable-name edit that keeps the id, so a rename plus a
//     concurrent edit on another device collapse onto ONE row (newest ts wins) instead of the delete+add a
//     name-keyed merge would spell — which would leave the renamed channel AND the peer's edit of its old
//     name side by side. New channels get a random id; there is no legacy row to back-fill (this store ships
//     with the field), so unlike presets there is no deterministic name-derived id here and none is wanted:
//     two devices that each create a same-named channel offline get two channels, which is honest.
//
//   * A DELETE LEAVES A DATED TOMBSTONE (Tombstones, store "channels/<profile>", keyed by id), never a bare
//     row removal. A bare removal is indistinguishable from "this device never knew that channel", so a peer
//     still holding the deleted channel would re-add it on the next merge. Bounded by Tombstones::compact(30)
//     like the rest, which costs a resurrection if a peer is dormant for 31 days — the accepted trade
//     everywhere in this family, and a fair one for a channel (re-deleting it is one action).
//
// WHAT IS *NOT* HERE: the lineup. A channel names a source; the items are enumerated fresh at tune time
// (ChannelLineup) and the timeline is computed (channels::buildDay), never stored. Storing the lineup would
// make a channel a snapshot of a playlist rather than a view of it, and would put a per-day timeline into the
// sync document for no reason at all — every device computes the same one from these eight fields.
#pragma once
#include "Channels.h"

#include <QString>
#include <QVector>
#include <functional>

namespace ChannelStore
{
    QVector<channels::Channel> list();                    // for the active profile, newest first
    bool    get(const QString& id, channels::Channel& out);   // false when there is no such channel
    QString add(channels::Channel ch);                    // mints an id + ts, prepends, returns the id
    bool    update(const channels::Channel& ch);          // by id; false when the id is unknown. Re-dates ts.
    void    remove(const QString& id);                    // + a dated tombstone (see the header)

    // UI refresh hook, mirroring FavoritesStore/FilterPresetStore: fired after every mutation so the home can
    // rebuild its shelves. A std::function, not a Qt signal (the store stays QtCore-clean); unset in probes.
    void setChangeHook(std::function<void()> hook);
}
