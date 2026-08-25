// THE ONE-TIME SWEEP OF WHAT EARLIER BUILDS ALREADY WROTE DOWN (issue #200).
//
// StoredUrl is the rule and RecentStore/PlaybackSession/ConsumptionStats apply it from now on. That protects
// new writes and nothing else — every install that has ever played an addon-resolved stream is still holding
// the token in its ini today, and would go on syncing it for ever. A fix that only guards the future leaves
// the problem exactly where it is for everyone who has used the app.
//
// So: one pass, over every store keyed off a playback path, cleaning in place.
//
//   recent/<profile>/items            path / key / title / thumb of every row
//   deleted/recent/<profile>/<hash>   the tombstone IDENTITIES, which for a keyless row ARE the url
//   resume/<hash>/title               the display label (the completeBaseName slice — found live)
//   stats/<profile>/<device>/items/…  the same label inside the consumption-stats blob
//
// (playstats/* needs nothing: it stores no title, and its key is already a SHA-1 of the identity. resume's
// own key is likewise an MD5. A hash of a token is not a token — only the fields kept in the clear are
// swept, which is also why the sweep is small.)
//
// STAMPED, IDEMPOTENT, ONE-SHOT — PlaylistStore::migrateToCategories is the in-tree shape, including its
// rule that a failed write must NOT stamp: stamping a lost migration marks the install done while the token
// is still on disk, and no later run would retry. The stamp lives under "device/", which CloudSync carves
// out of the synced bundle and SettingsTxn excludes, because "this install's ini has been cleaned" is a fact
// about this install; a synced stamp would tell a machine that has never run the sweep that it has.
//
// A one-shot is enough BECAUSE it is not the only defence: a peer running an older build can still push
// tokenised rows into the merge document, and CloudMerge scrubs those on the way in rather than relying on
// this ever running again.
#pragma once

namespace CredentialScrub
{
    // Clean every already-stored playback record. Returns true when something was actually rewritten (the
    // probe's evidence that the sweep did work, not merely that it ran). A no-op once stamped.
    bool run();

    // The stamp, named so the probe and the carve-out assertions cannot drift from the writer.
    const char* stampKey();
}
