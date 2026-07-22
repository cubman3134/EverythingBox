# Category Playlists + Channels (personal TV network) — Design

**Date:** 2026-07-21
**Status:** Approved design; plan queues after the marks/shelves track.
**Origin:** User request — playlists scoped per CATEGORY (one Video playlist mixes TV
episodes + movies + anything), a random-pick function, and channel-style random
AUTOPLAY: "functionally create a TV network where you can put what you want on there."

## Existing foundation (survey)

`PlaylistStore` (native/src/core/PlaylistStore.h) is already per-profile with entries
that resolve BOTH addon items (addonId/itemId) and local files (path/kind) — the
resolution machinery for mixed playlists exists. Its scope is the limiter: playlists
key on ONE catalog (`catalogKey = addonId|catalogId|catalogType`). The user's
"Weekend Picks" playlist lives here and MUST survive migration.

## Design

### 1. Category scope

- `Playlist.catalogKey` → `Playlist.categoryKey` ("video" | "games" | "audio" |
  "reading"). One-time migration maps each existing playlist's catalogType to its
  category (content untouched; ids stable; Weekend Picks intact — migration is
  additive-rewrite with the old key preserved in a `legacyKey` field for rollback).
- The item "Add to playlist…" menu lists ALL the item's category playlists regardless
  of the catalog being browsed. The Playlists folder appears at the category level.

### 2. Random pick

- Playlist action **"Play random"**: uniform pick over entries, open via the existing
  per-entry open path (addon resolve or openRecent). Games included (pick + launch).

### 3. Channel mode (random autoplay)

- Playlist action **"Start channel"** (video/audio playlists; games are pick-only —
  no meaningful autoplay-next after quitting a game):
  - Plays a random entry; when playback ENDS (natural end, not user stop), an
    interstitial "Next: <title> — starting in 5s" (Nav-Contract card, Cancel-focused
    per house style; Back/Cancel exits the channel) then the next random entry.
  - **Shuffle bag**: no repeats until every entry has aired once, then reshuffle
    (pure random reruns kill the TV feel). Bag state is session-only (a fresh
    channel start reshuffles).
  - Resume positions apply per item (landing on a half-watched movie continues it —
    flipping to a channel mid-film).
  - User stop / Back exits channel mode; the next natural end does NOT chain.
  - Mixed-type entries within video (episodes + movies) chain uniformly.
- Channel state lives in the playback-session layer (where the audio queue's
  next-track chaining already lives — reuse that seam, not a parallel one).

### 4. Future (recorded, NOT built)

- Smart playlists (auto-populated by date range / genre / etc.): `Playlist` gains an
  optional `rule` field later; manual playlists have empty rules. Nothing in this
  track may preclude it.

## Verification

- Probe: migration (catalogKey → categoryKey table incl. Weekend Picks' shape,
  legacyKey preserved, ids stable); shuffle-bag semantics (no repeat until
  exhausted, reshuffle after, uniform-ish distribution sanity over N draws).
- Live: mixed playlist (episode + movie) built on a throwaway profile; Play random;
  Start channel → natural-end chaining with interstitial; Cancel exits; resume-mid-
  item behavior; Weekend Picks intact byte-for-byte post-migration (the migration
  runs on the REAL ini — snapshot first, verify Weekend Picks entries identical,
  restore only if verification fails).
- Suite + perf gates unchanged.

## Non-goals

- Smart playlists (future); weighted/scheduled programming ("prime time") — future
  channel polish; cross-category playlists; games autoplay chaining.
