// RE-IDENTIFYING THE ROWS EARLIER BUILDS ALREADY WROTE (issue #203, the Live TV half).
//
// Every `livetv:` favourite and playlist entry in an existing install is spelled `livetv:<stream url>`, with
// the credential in it. LiveTvIdentity says what those rows SHOULD be called; this is what re-files them.
//
// IT IS DRIVEN BY A CHANNEL LIST, NOT BY A CLOCK, and that is the whole design. A url can only be turned into
// an identity by finding the entry it belongs to, so the migration needs the M3U — and fetching every
// configured source at startup, to repair rows the user may not open for weeks, is a network call this app has
// no business making on the launch path. So it runs at the moment a channel list is in hand and free: the
// Live TV shelf's own fetch, and the open-time resolve. "First load" means the first time this device has
// actually seen the channels, which is also the first moment the answer exists.
//
// THREE OUTCOMES, and the third is the point:
//   * URL MATCH — the row's url is still in the list. Re-filed under that entry's identity. This is the
//     ordinary case and it is exact.
//   * NAME MATCH — the url has changed (the credential rotated, the provider moved the path) but a channel of
//     that name is there. Re-filed under that entry's identity. This is what makes the fix survive the very
//     event that motivates it.
//   * NO MATCH — left EXACTLY as it is, credential and all. The row keeps working (its url is the only thing
//     that can play it) and stays out of the synced document, which is precisely the 02a18bd behaviour,
//     narrowed from "every livetv: row" to "livetv: rows whose id is still a url". Another source, or the
//     same one after a refresh, may name it later; nothing is stamped, so it will be tried again every time.
//
// REPEATABLE AND IDEMPOTENT, NOT ONE-SHOT — StoredIdentity::sweepPlaylists' reasoning, for the same reasons
// plus one of its own: which channels exist is a function of which SOURCES are configured, and the user can
// change that at any time. A stamp would deliver the first pass and strand every later one.
//
// NOTHING IS EVER REMOVED. It rewrites two string fields of rows that are already there. It does not add,
// reorder, drop, de-duplicate or re-time anything, and it deliberately does not touch a favourite's `ts` or a
// playlist's `updatedAt`: those are the merge clock, and raising one would make a repaired-but-stale local
// copy outrank a genuinely newer edit made on another device — a security fix eating an edit.
#pragma once
#include "LiveTvIdentity.h"
#include <QVector>

namespace LiveTvMigrate
{
    // Re-identify this profile's Live TV favourites and playlist entries against one source's freshly parsed
    // channel list. Returns true when something was rewritten (nothing is written back when nothing moved).
    bool withChannels(const QVector<LiveTvIdentity::Channel>& channels);
}
