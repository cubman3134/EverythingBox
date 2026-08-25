// THE DURABLE NAME A ROW IS FILED UNDER (issue #203).
//
// THE BUG, WHICH IS TWO BUGS. MainWindow::saveQueueAsPlaylist wrote `e.itemId = e.path = session_->tracks()`
// — and for a Subsonic queue those are SIGNED STREAM URLS, carrying the user (`u`), the salted password hash
// (`t`) and the salt (`s`) in the query. PlaylistStore does no scrubbing and CloudSync::isPerItemStoreKey puts
// `playlists/` in the synced bundle, so the credential rode the CloudMerge document to every device on the
// account — exactly what #200 took out of `recent/`. Sitting on top of it is a plain functional bug that
// points at the fix: a stream url EXPIRES, so a saved playlist of Subsonic tracks is a list of links rather
// than a list of tracks.
//
// WHY THIS IS NOT A StoredUrl::location() CALL. Here the url is the row's IDENTITY, not a label, and #200's
// rule was chosen precisely NOT to touch identities: take the query off a Subsonic stream url and every track
// on that server becomes the same string (`…/rest/stream.view`), which is not a scrub, it is deleting the
// playlist. So the question has to be answered properly first: WHAT IS THE DURABLE NAME OF A TRACK?
//
//   * For a Subsonic track it is the QUALIFIED TRACK ID — `sub<US><serverId><US>track<US><remoteId>` — which
//     is credential-free, stable across a password change, and already what `syncKey_`, MusicRemap's
//     `indexId` and the whole browse layer call that track. The signed url is the one-shot artefact.
//   * For a local file it is the file path, exactly as before. Nothing about a local queue changes.
//   * For anything else — an addon-resolved audiobook chapter, a stream from a server this install no longer
//     has — there IS no durable name, and the row still has to survive. It keeps a credential-free spelling
//     of its own url (StoredUrl::identity, the narrow rule) and stays exactly where the user put it.
//
// RULE 1, WHICH OUTRANKS THE SECURITY FIX: A ROW IS NEVER DROPPED AND NEVER MERGED WITH A ROW IT IS NOT.
// PcGameRemap's property, and MusicRemap's, and the reason both are safe to run over real installs. Losing
// somebody's hand-curated playlist to a credential fix would be a worse outcome than the leak.
//
// WHAT LIVE TV DOES INSTEAD, AND WHY IT IS DIFFERENT. An IPTV channel's url is frequently the only handle the
// channel has, and its credentials are commonly in the url's PATH (`…/live/<user>/<pass>/<id>.ts`), which no
// rule here reaches (#200 says why, at length, and declines to guess). There is no durable name to move a
// Live TV favourite onto, so its identity is left ALONE and the leak is closed from the other end: CloudMerge
// stops putting `livetv:` favourites — and their tombstones, which carry the same url — into the synced
// document at all. That is not a new principle; it is the one `iptv/*` already follows, for this same url.
#pragma once
#include <QPair>
#include <QString>
#include <QVector>

namespace StoredIdentity
{
    // (serverId, root) for every Subsonic server configured on the ACTIVE profile, in the exact spelling
    // SubsonicClient::streamUrl concatenated onto. The impure half, kept to one function.
    QVector<QPair<QString, QString>> serverRoots();

    // THE RULE. Pure: everything it needs is an argument, so probe_cloudmerge drives every arm of it over a
    // table of strings with no ini, no server and no network.
    //
    //   `playPath`  what the player was handed — a local file path, or a signed stream url.
    //   `indexHint` the durable name the CALLER already knows, when it knows one. For a running music queue
    //               that is MainWindow's musicQueueIndexPaths_ (playback path -> index path), the table
    //               #193 built for exactly this reason. Empty when there is none.
    //
    // Order of preference, and each step is a fallback for the one above failing rather than an alternative:
    //   1. the hint, if it names something other than the play path itself (scrubbed on the way through, so a
    //      caller cannot smuggle a url in as a "durable name" — StoredUrl::title's precedent);
    //   2. the qualified track id recovered FROM the url, when it is a stream url of a configured server.
    //      This is what lets the one-time sweep re-identify rows written by an older build, which have no
    //      hint anywhere and never will;
    //   3. StoredUrl::identity(playPath) — the row survives, credential-free, under a name that is still
    //      re-identifiable by a later pass (§the header of StoredUrl::identity).
    //
    // Empty in, empty out. Never returns an empty string for a non-empty input: a row with no identity is a
    // row no reader can reach.
    QString resolve(const QString& playPath, const QString& indexHint,
                    const QVector<QPair<QString, QString>>& roots);

    // resolve() against the servers this install actually has. The one call site outside a probe.
    QString forRow(const QString& playPath, const QString& indexHint = QString());

    // THE MIGRATION over `playlists/<profile>/items`, in place. Returns true when something was rewritten.
    //
    // REPEATABLE AND IDEMPOTENT, NOT ONE-SHOT AND STAMPED — #194 increment 2's choice, for its reasons and
    // one of its own:
    //   * the mapping is a function of WHICH SERVERS ARE CONFIGURED, and the user can change that. Remove a
    //     server and re-add it and the rows it names must become nameable again; a stamp delivers the first
    //     pass and strands every later one.
    //   * `playlists/` SYNCS, and whole-object newest-wins means a peer on an older build can push a
    //     tokenised copy of a playlist back over a cleaned one at any time. A one-shot would clean that
    //     install exactly once, in the past. This runs at startup and again on the tail of every playlist
    //     merge, so an inbound tokenised row is cleaned before anything reads it.
    // Idempotence is structural rather than promised: an already-qualified id is not a stream url and not a
    // network url, so step 2 declines it and step 3 returns it byte for byte. Cheapness is separate — a store
    // with nothing to change is never written back, and an install with no playlists is never even parsed.
    //
    // It deliberately does NOT bump a playlist's `updatedAt`. That field is the merge clock, and raising it
    // would make a cleaned-but-stale local copy outrank a genuinely newer edit made on another device — a
    // security fix eating an edit, which is the one outcome worse than the leak.
    bool sweepPlaylists();
}
