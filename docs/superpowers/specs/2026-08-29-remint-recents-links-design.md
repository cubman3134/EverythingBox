# Re-mint a Recents link instead of replaying it

## Problem

Opening a debrid-backed movie or audiobook, leaving, and returning days later fails
with:

> The saved link for “…” has expired. Open it again from its library shelf to get a
> fresh one.

(`native/src/ui/MainWindow.cpp:1778`, raised off `MpvWidget::loadFailed`.)

The cause is that a Recents row records the **result** of a source resolution rather
than the **recipe** for one.

A TorBox stream is a signed, short-lived link minted by `torrents/requestdl`
(`EverythingBox.Server.Core/Debrid/TorBox/TorBoxService.cs`). `RecentItem.path`
stores that URL (`native/src/core/RecentStore.h:32`), and re-opening a row replays it
verbatim (`MainWindow.cpp:10991`).

Two independent things make that replay fail:

1. Since #200, `RecentStore::add` scrubs the stored path through
   `StoredUrl::location()`, which drops the **entire query** — because for a signed
   link the query *is* the credential, and it was syncing in cleartext to every
   device on the account (`native/src/core/StoredUrl.h:34`, seven such rows found on
   a live install).
2. TorBox's link carries its own TTL regardless.

So the replay is not merely stale after a few days; for a debrid URL it is dead
almost immediately. The existing message is an accurate description of a dead end.

The trade-off was recorded rather than resolved, at `MainWindow.cpp:10967`:

> …re-resolving through the addon is still the feature that would make replay work
> properly.

and

> Whichever way it goes, it is a call for the repository owner, not something to slip
> in under a fix.

This spec is that call. No open issue tracks it (all 87 checked); #203 is adjacent
(playlists and Live TV storing signed URLs as ids) but is a different store.

### Why Stremio does not have this

Stremio never stores a playback URL as the re-open target. A library row is
`(type, imdbId)`; opening it re-calls the addons' `/stream/…` and gets a freshly
minted link every time. The link is derived, never persisted.

### What already exists

Two pieces of the fix are already built.

**The re-resolve call.** `HomeView::requestNextSource` (`HomeView.cpp:6217`) re-asks
a source for a link and re-opens the item. Its context, `lastPlay_`
(`HomeView.h:933`), is in-memory and session-scoped, and a Recents re-open explicitly
discards it — `MainWindow.cpp:10857`, *"a Recent re-open has no live Allarr context
to swap sources"*.

**A durable, credential-free item id.** `IndexerSearchSource.EncodeId`
(`EverythingBox.Server/Sources/IndexerSearchSource.cs:286`) packs a release's title,
provider, **info hash**, magnet, size, seeders and media type into a base64url blob,
explicitly so it can "survive the round trip to the client and back". It
deliberately omits `DownloadUrl` because Prowlarr/Jackett enclosure URLs embed the
indexer manager's API key. `DecodeRelease` treats every client-supplied id as
untrusted and returns null rather than throwing on malformed input.

That blob never expires and pins a specific release. It is already what
`RecentItem.key` holds for a catalog stream play: `MainWindow.cpp:15674` sets
`rkey = item.id.isEmpty() ? url : item.id`, and `MainWindow.cpp:5969` already
anticipates this work — *"sync offsets follow the stable resume key (survives a
re-resolved debrid URL)"*.

The only thing missing from a Recents row is **which source to ask**.

## Behaviour

A Recents row that carries a source recipe re-mints its link when opened. One that
does not keeps today's behaviour exactly.

### Stored recipe

`RecentItem` gains three fields, all durable and credential-free:

| field | holds |
|---|---|
| `sourceAddon` | the file-provider addon's id |
| `sourceItemId` | the `meta:<blob>` release id, or `mv:tt…` / `ep:tt…:S:E` |
| `sourceKind` | `"direct"` or `"imdb"` — picks the resolve call |

`path` continues to be written scrubbed. **#200 is untouched**: the minted URL still
never reaches disk, and none of the three new fields is a credential. What is stored
is the means to *request* a new link, not a link.

`sourceKind` is recorded rather than inferred from the id's shape, so a future id
format cannot silently route to the wrong resolver.

### Two fidelity tiers

A `meta:<blob>` row re-mints the **identical release**, because the info hash is
inside the blob. The resumed position is therefore exact.

An `mv:tt…` row has only an IMDB id, so re-resolving re-runs the search and may
select a different release. This is the same guarantee Stremio gives, and it is the
best available when the row holds nothing more specific. It is not treated as an
error.

### Opening a row

`MainWindow.cpp:10991` currently calls `openStreamUrl(path, …)` /
`openAudioStream(path, …)`. It gains a fork:

* **Recipe present, addon installed** — show a "Getting a fresh link…" notice, call
  `resolveStream(addon, item, n=0)` or `resolveStreamByImdb(type, id, n=0)`, then
  open the returned URL. Resume keys on `rkey` = `item.id`, which re-minting does not
  change, so the position survives.
* **No recipe** — legacy rows, pasted links, Subsonic/Jellyfin/IPTV. Replay `path`;
  today's behaviour and today's message.
* **Recipe naming an addon this device does not have** — today's message. #77 (cloud
  sync of the addon roster) is open, so a row synced from another device can name an
  addon that is legitimately absent here. This is a defined degradation, not a bug.

A row that re-mints also has live source context, so `currentNextSourceCapable_`
becomes true for a resumed stream and the "Issue with Streaming" affordance works
there. It is greyed out today.

### When the re-mint fails

TorBox can evict a torrent from the account, and a release can stop being cached.

The app reports the failure and offers the source swap as an explicit action,
reusing the existing "Issue with Streaming" route rather than adding a second one.

It does **not** silently substitute another release. A substitution that happens
without being asked for puts the viewer some distance into a different cut with a
resume position that no longer refers to anything, and gives them nothing on screen
explaining why.

### Audiobooks

A multi-file release (#214) re-mints in two steps: re-resolve the release id, then
re-list its parts and match the stored part token by **file name**.

This works because #214 already made part tokens durable on purpose. `MainWindow.h`
above `remoteBookPartIds_`: *"Everything needed to MINT the link for a part when the
app reaches it, and nothing that expires"*, and the tokens are *"derived from the
book key and the file name, so a resume row written today is still readable by a
build that resolves a different release tomorrow"*.

The per-session tables `remoteBookPartIds_` and `remoteBookMinted_` rebuild from the
re-listing. `remoteBookGen_` already drops mints that arrive carrying a stale
generation, so a slow re-mint for a part the listener skipped past cannot play over
their choice.

A part token that matches no file in the re-listed release falls to the failure path
above.

## Testing

* `probe_cloudmerge` extended: the three new fields ride the merge document, and
  **none of them contains a query string** — the same invariant §34-35 already pins
  for `path`. This is the regression that would re-open #200.
* A new probe for the routing fork: recipe present → resolve is called and `path` is
  not opened; no recipe → `path` is replayed; addon absent → today's message.
  Registered in all three places CONTRIBUTING.md requires.
* Round-trip: a row written by this build and read back yields the same three fields;
  a legacy row missing them loads with them empty and takes the replay branch.
* Live gate: play a TorBox movie, backdate its Recents row, re-open from Continue
  Watching, confirm it plays and resumes at the correct second. Repeat for an
  audiobook part in a multi-file release.

## Out of scope

* **Downloads queue.** `DownloadManager.h:21` has the same rot ("a debrid link may
  expire; retry re-uses it — a dead link just fails again"). Same root cause,
  different store, separate change.
* **Playlists and Live TV favourites.** #203, different storage and a different fix.
* **Persisting `proxyHeaders`.** #59. That one genuinely would put session cookies and
  Authorization headers at rest; this design is built so it is not needed.
* **Auto-substituting a release on failure.** Argued against above.
