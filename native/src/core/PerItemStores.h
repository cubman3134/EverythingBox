#pragma once
#include <QString>   // QLatin1String comes with it; nothing else is needed and nothing else is included

// THE PER-ITEM STORES: the one list of key prefixes the CloudMerge progress document owns (issue #332).
//
// A per-item store is one whose rows merge ITEM BY ITEM — union by id, newest stamp wins, a tombstone or a
// husk to carry a deletion — rather than being replaced wholesale. Two consequences follow, and they are why
// this list has more than one consumer:
//
//   * SYNC (CloudSync::isPerItemStoreKey). The merge document owns these keys EXCLUSIVELY, so they must not
//     ride the heavy settings bundle in either direction. Outbound, one per-item tick would flip the
//     stateHash and re-upload the whole zip; inbound, applyBundle would write the row RAW, bypassing the
//     merge that stops two devices clobbering — or resurrecting — each other's rows.
//   * THE SETTINGS TRANSACTION (SettingsTxn::inScope). The same keys are written continuously by playback,
//     by a cloud merge landing mid-visit, and by editors that commit on their own confirm step, so a Discard
//     that rolled them back would throw away data the user had already finished changing.
//
// ONE LIST, BECAUSE TWO OF THEM DRIFTED. Each predicate used to carry its own copy, and SettingsTxn's
// comment asserted the two matched. It did not: that one held ten prefixes while the sync table had grown to
// twenty-one, and the eleven that never crossed are exactly where issue #322 lived — a Discard reverting
// per-item stores that had already committed themselves. A removed vocabulary word came back, a metadata
// reset was undone behind a confirm that said it could not be, and a finished home-row edit was thrown away.
// The two lists were reconciled by hand once; that fixes today and not tomorrow, because the next store
// somebody adds would be registered in one place and not the other, and the failure would again be silent,
// plausible, and found by accident.
//
// SO: A NEW PER-ITEM STORE IS ADDED HERE, ONCE, AND EVERY CONSUMER INHERITS IT. Give it its own entry with
// its own reason, as every entry below has one — the reason is the part a later reader needs, and it is the
// part a second copy of the list always loses first. probe_cloudmerge asserts that the two consumers agree
// over THIS ARRAY, walked, rather than over a restatement of it, so a consumer that grows a rule of its own
// is red before it ships; probe_settingstxn walks it against its own predicate for the same reason.
//
// PURE ON PURPOSE — QString and nothing else. SettingsTxn is QtCore-only so probe_settingstxn links lean,
// while CloudSync is a QObject over a network backend, so anything shared between them had to be lighter
// than either. Nothing here allocates or touches a store; it is a table and a startsWith.
namespace peritem
{
// The prefixes, in the order they were added. A prefix ends in '/' unless the store's own key spelling says
// otherwise, and the boundary matters: "follow/" deliberately does not match the schedule settings under
// "following/", and "audiobookmarks/" is one character away from the settings group "audiobooks/".
inline constexpr const char* const kPrefixes[] = {
    "resume/",
    "recent/",
    "marks/",
    "favorites/",
    "playlists/",
    "stats/",
    "playstats/",
    "deleted/",
    // Saved filter presets (issue #184): owned by the CloudMerge document, same as favourites/playlists.
    // Riding the heavy bundle too would make one preset save flip the stateHash and re-upload the whole
    // zip, and an inbound bundle would write the row raw — bypassing the newest-ts + tombstone merge that
    // keeps a peer from resurrecting a deleted preset.
    "filterpresets/",
    // Personal TV channels (issue #179): owned by the CloudMerge document, same family and same reasons as
    // filterpresets above. A channel is a source + an ordering + a start epoch; riding the heavy bundle too
    // would make one channel edit flip the stateHash and re-upload the whole zip, and an inbound bundle
    // would write the row raw — bypassing the newest-ts + tombstone merge that keeps a peer from
    // resurrecting a deleted channel.
    "channels/",
    // Followed series (issue #155). The SYNCED half of the follow feature: "I follow this show" is a
    // statement about the user, not about this box, so it rides the merge document exactly as a favourite
    // does — one follow press must not flip the heavy bundle's stateHash and re-upload the whole zip, and
    // an inbound bundle would write the row raw, bypassing the newest-ts + tombstone merge that keeps a
    // peer from resurrecting an unfollowed series. The matched prefix is "follow/" with the slash, which
    // deliberately does NOT match the schedule settings under "following/" (those are ordinary synced
    // preferences and must keep riding the bundle) nor the device-local snapshots under "followsnap/".
    "follow/",
    // Per-item metadata corrections (issue #24): owned by the merge document, same as the rest. Riding the
    // heavy bundle too would make a single title fix flip the stateHash and re-upload the whole zip, and an
    // inbound bundle would write the blob raw — bypassing the newest-updatedAt merge that keeps two devices'
    // corrections from clobbering each other.
    "metaoverrides/",
    // Per-game launch overrides (issue #51): the game's preferred core/emulator/extra-args. Owned by the
    // CloudMerge document, same family and same reasons as metaoverrides — one override save must not flip
    // the stateHash and re-upload the whole zip, and an inbound bundle would write the blob raw, bypassing
    // the newest-updatedAt + husk merge that keeps two devices' overrides (and a clear) from clobbering.
    "launchopts/",
    // Per-item playback-speed memory (issue #140). Owned by the CloudMerge document, same family and same
    // reasons as metaoverrides/launchopts: a narrator's ideal speed is a property of the CONTENT, so it
    // should follow the user across devices (per-item-synced, NOT device-local); riding the heavy bundle
    // too would make one speed change flip the stateHash and re-upload the whole zip, and an inbound bundle
    // would write the row raw — bypassing the newest-updatedAt merge that keeps two devices' speeds from
    // clobbering. The inverse of #64/#75/#103's device-local carve-outs — probe_cloudmerge asserts both.
    "speed/",
    // Per-item lyric offset (issue #142). Same family, same reasoning as speed: how far out a track's .lrc
    // file runs is a property of the CONTENT (of the lyric file shipped beside it), not of this machine, so
    // it should follow the user across devices — per-item-synced, NOT device-local. Riding the heavy bundle
    // too would make one ±0.5 s nudge flip the stateHash and re-upload the whole zip, and an inbound bundle
    // would write the row raw, bypassing the newest-updatedAt merge that keeps two devices' nudges from
    // clobbering each other. probe_cloudmerge asserts both classifications.
    "lyricoffset/",
    // Per-item TRACKER LINKS (issue #156). Which AniList entry a shelf row IS. The INVERSE classification
    // of the tracker credentials in CloudSync::isDeviceLocalKey, and for the reasons speed/ and
    // lyricoffset/ are per-item-synced: a link is a property of the CONTENT, not of this machine, and it
    // costs the user a prompt per item to establish, so it should follow them across devices. Riding the
    // heavy bundle too would make one link flip the stateHash and re-upload the whole zip, and an inbound
    // bundle would write the blob raw, bypassing the newest-updatedAt merge that keeps two devices'
    // links (and an unlink husk) from clobbering each other. probe_cloudmerge asserts it is
    // per-item-synced and NOT device-local.
    "trackerlink/",
    // Per-book bookmarks (issue #136). A bookmark is a POSITION the issue explicitly wants to "survive
    // switching devices", so it SYNCS per-item (per-profile, NOT device-local) and rides the CloudMerge
    // document — favourites/playlists shape (union by id, newest-ts, delete tombstone). Riding the heavy
    // bundle too would make one bookmark flip the stateHash and re-upload the whole zip, and an inbound
    // bundle would write the row raw, bypassing the tombstone merge that keeps a peer from resurrecting a
    // deleted bookmark. probe_cloudmerge asserts it is per-item-synced and NOT device-local.
    "bookmarks/",
    // Per-book highlights (issue #136). A highlight is a statement about the BOOK — the passage a reader
    // marked — not about this device, so it syncs on exactly the bookmark terms above: per-item, per-
    // profile, NOT device-local, riding the CloudMerge document with the union-by-id + newest-ts + delete-
    // tombstone rule. (Contrast #239's open-failure state, which really IS about this device and stays
    // local.) The prefix is "highlights/" with the slash; probe_cloudmerge asserts it is per-item-synced
    // and NOT device-local so a later edit to either table cannot reclassify it silently.
    "highlights/",
    // The looked-up vocabulary list (issue #137). A word you had to look up is a fact about the READER,
    // not about the machine they were holding at the time, so it syncs on exactly the highlight terms
    // above: per-item, per-profile, NOT device-local, riding the CloudMerge document with the union-by-id
    // + newest-ts + delete-tombstone rule. The id is the WORD (VocabularyStore::idFor), so two devices
    // that met the same word converge on ONE row instead of listing it twice. probe_lookup asserts it is
    // per-item-synced and NOT device-local so a later edit to either table cannot reclassify it silently.
    "vocabulary/",
    // Per-item audio bookmarks (issue #140). A bookmarked POSITION in an audiobook/podcast is a property of
    // the CONTENT the issue wants to "survive switching devices", exactly like #136's reading bookmarks and
    // resume — so it SYNCS per-item (per-profile, NOT device-local) and rides the CloudMerge document with
    // the favourites/bookmarks shape (union by id, newest-ts, delete tombstone). Riding the heavy bundle too
    // would DOUBLE-sync it — one bookmark flips the stateHash and re-uploads the whole zip, and an inbound
    // bundle writes the row raw, bypassing the tombstone merge that keeps a peer from resurrecting a deleted
    // bookmark. NOTE it does NOT match the device-local "audio/" prefix in CloudSync ("audiobookmarks" has
    // a 'b', not a '/', at that boundary) — probe_cloudmerge asserts it is per-item-synced AND not
    // device-local, so a future refactor of either table cannot break the classification silently.
    "audiobookmarks/",
    // Per-game pad2key profiles (issue #105). Owned by the CloudMerge document (a `pad2key` section, husk-on-
    // clear), same family as launchopts/speed/bookmarks: which keys a pad synthesises for a game is a property
    // of the game+user, not the device, so it SYNCS. Riding the heavy bundle too would DOUBLE-sync it — one
    // toggle flips the stateHash + re-uploads the zip, and an inbound bundle writes the row raw, bypassing the
    // newest-updatedAt merge. So it must be carved out here (the store defined the CloudMerge section but this
    // exclusion was missing). probe_cloudmerge asserts it is per-item-synced and NOT device-local.
    "pad2key/",
    // The "you missed" per-show dismissal watermarks (issue #25). Here rather than in CloudSync's
    // device-local table, and that is the design decision rather than a filing choice: a dismissal SHOULD
    // follow the user — waving away a month of a show on the TV and being nagged about it on the phone
    // an hour later is the complaint the marks sync already exists to answer. It rides the merge
    // document rather than the heavy bundle for the family's usual reason (one button press must not
    // flip the stateHash and re-upload the whole zip) and for one of its own: the bundle overwrites,
    // and this store's whole correctness argument is that the only write is `max`.
    "missed/",
    // The profile's HOME ARRANGEMENT (issue #161, filed as #333). It had a CloudMerge section from the
    // day it shipped — serializeHomeRows/mergeHomeRows, whole-list newest-wins over a UNION of the row
    // set — and was missing from this table, so the two halves of the sync disagreed about what it is:
    // the merge document unioned two devices' rows, while the heavy bundle carried the whole value and
    // applyBundle wrote it RAW inbound. Whichever landed last decided, and a bundle landing last replaced
    // the local arrangement with the peer's, DROPPING every row the peer did not carry. That is the one
    // outcome a synced row list must never produce (HomeRows.h, issue #314: a device keeps a row it
    // cannot even draw, precisely so a sync cannot erase it for the device that can). Here, the merge
    // document owns it alone: the union survives, and one row drag stops flipping the stateHash and
    // re-uploading the whole zip. probe_cloudmerge section 41 drives both arrival orders.
    "homerows/",
};

inline constexpr int kPrefixCount = int(sizeof(kPrefixes) / sizeof(kPrefixes[0]));

// True when `key` belongs to one of the stores above — the answer both consumers give, from the one table.
inline bool isKey(const QString& key)
{
    for (const char* p : kPrefixes)
        if (key.startsWith(QLatin1String(p))) return true;
    return false;
}
} // namespace peritem
