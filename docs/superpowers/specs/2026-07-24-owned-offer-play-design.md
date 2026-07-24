# Owned ⇒ Offer "Play" (regardless of stream provider) — Design

**Date:** 2026-07-24
**Status:** Draft — approved through brainstorming; awaiting user spec review before plan.
**Origin:** The strong follow-up recorded at the id-resolver close-out (`2026-07-24-local-library-id-resolver-design.md`).
The resolver makes an owned local movie's "On disk" badge light up on a catalog tile, but a **metadata-only
catalog (aiocatalog with no stream/debrid addon) offers no "Play" action** on the detail — so Seam B
prefer-local can never fire there, and the badged tile can't play the local file. This surfaces Play whenever a
directly-playable local file exists for the item, so owned-but-unstreamable films play from their tile.

## Scope reality (scout, 2026-07-24)

- **One shared action-gate:** `HomeView::classicActionGates(const MediaItem&)` (`HomeView.cpp:3750-3789`) is read
  by BOTH the classic detail buttons (`showMeta`, `:3836/:3841`) and the themed detail action row
  (`themedDetailData`, `:3621/:3625`) — `ActionGates` is documented as "ONE definition … share it"
  (`HomeView.h:223-226`). Today the Play predicate (`:3779`) is
  `g.play = isSteam || isRemotePlayable || g.readable || isBridgedAudio || isBridgedGame;` — all false for a
  metadata-only local-leaf movie ⇒ no Play.
- **Two Play routes, one broken for this case:**
  - **Classic** Play button (`:618-631`) calls `resolvePlay(top.addon, top.item, …)` directly; `resolvePlay`'s
    head (`:3084-3095`) is the Seam B local short-circuit (`localPathFor(it.id)` → else `it.imdbStreamId` →
    `openItem(local:video)`), so classic Play works end-to-end the moment the gate opens.
  - **Themed** Play verb → `MainWindow::runThemedDetailAction` (`:3927`) → `HomeView::playThemedLeaf(idx)`
    (`:3675`). For a metadata-only movie, `needsImdb` (`:3706`) fires an async `requestMeta` and returns;
    `resolvePlay` is reached only later in `onMetaReady` (`:3983`) **and only if
    `mgr_->hasStreamProvider(...)`** (`:3982`) — else it shows a "No stream source" toast. So the themed path
    **never reaches the local short-circuit** when no stream provider exists. `playThemedLeaf` already has a
    `local:video` early-return (`:3693`), but that only triggers when the item's mime is already `local:video`
    (not the case for a catalog movie tile).
- **Ownership id:** the detail item is `stack_.last().item` (classic) / `items_[browseRowMap_[idx]]` (themed);
  for a non-expandable movie both are the same catalog item with the `id` the resolver keyed into `OwnedIndex`
  (`tmdb:movie:…` / `tt…`). `LocalLibrary` is already included in `HomeView.cpp:12` and `index()` is already
  called in `resolvePlay`.

## Design

Two edits, both in `native/src/ui/HomeView.cpp`.

1. **Open the shared gate for a directly-playable owned item** (in `classicActionGates`, at the `g.play` line
   `:3779`). Add a term computed as the **exact precondition of the Seam B short-circuit** — a real local file
   exists for this id:
   ```cpp
   const bool ownedPlayable =
        !LocalLibrary::index().localPathFor(item.id).isEmpty()
     || (!item.imdbStreamId.isEmpty() && !LocalLibrary::index().localPathFor(item.imdbStreamId).isEmpty());
   g.play = isSteam || isRemotePlayable || g.readable || isBridgedAudio || isBridgedGame || ownedPlayable;
   ```
   **Deliberately NOT `ownsId`:** `ownsId` is true for a *series container* (you own episodes of it), which has
   no directly-playable file — using `localPathFor` (the short-circuit's own key) means "offer Play" ⟺ "a local
   file will actually play," so no dead Play button appears on a container. Surfaces Play on both surfaces at once.

2. **Fix the themed detour** — add a prefer-local early-return at the TOP of `playThemedLeaf` (before the
   `needsImdb` branch at `:3706`), mirroring `resolvePlay`'s head and the existing `local:video` early-return:
   ```cpp
   // Prefer-local: an owned item plays its local file directly, without the meta-fetch/stream-provider detour
   // (a metadata-only catalog otherwise dead-ends at "No stream source" though the file is on disk).
   {
       QString lp = LocalLibrary::index().localPathFor(it.id);
       if (lp.isEmpty() && !it.imdbStreamId.isEmpty())
           lp = LocalLibrary::index().localPathFor(it.imdbStreamId);
       if (!lp.isEmpty() && QFileInfo::exists(lp)) {
           MediaItem local = it; local.url = lp; local.mime = QStringLiteral("local:video");
           emit openItem(local);
           return;
       }
   }
   ```
   Adapt `it` to `playThemedLeaf`'s actual leaf variable. Classic Play needs no change (it already reaches
   `resolvePlay`'s short-circuit).

The button label stays plain **"Play"** — it transparently prefers local (the shipped Seam B behavior); the
"On disk" badge already signals the file is local. When an item is BOTH remotely playable and owned, Play was
already offered and the short-circuit already prefers local — the new term is idempotent.

## Error / edge handling

| Situation | Behavior |
|---|---|
| Owned movie, no stream provider | Play now offered on both surfaces; activating plays the local file (Seam B). |
| Series container (own ≥1 episode) | `localPathFor(seriesId)` is empty (episodes key `seriesId:S:E`) ⇒ no Play on the container (correct — containers aren't directly playable). |
| Not owned | Gate unchanged; no Play (today's behavior). |
| Owned AND remotely playable | Play already offered; short-circuit prefers local — idempotent, no double Play. |
| File moved/deleted since resolve | `localPathFor` may return a stale path; the short-circuit's `QFileInfo::exists` guard (and the gate term's parity with it) means a vanished file falls through — classic to normal resolution, themed to the meta/stream path. (Gate term uses `localPathFor` non-empty; the short-circuit re-checks `exists` — a rare stale-path Play tap then proceeds to normal resolution, never a crash.) |

## Verification

- **Code-walk** (the gate is a private member reading `stack_`, not a pure function; no existing detail-action
  probe). The `OwnedIndex` primitives the new term uses (`localPathFor`) are already probe-covered
  (`probe_locallib`). Extracting the predicate for a probe is out of scope for a two-line gate term.
- **Live (the payoff the resolver smoke couldn't complete):** portable throwaway with aiocatalog + a resolved
  owned movie (no stream provider) → the **themed detail now shows "Play"** → activating **plays the local
  file** (mpv opens the on-disk path, not a stream resolve). Also confirm a non-owned metadata-only movie still
  shows no Play, and a series container shows no Play. Real deployed app untouched.
- Suite + app compile (no probe change; a pure additive gate term + a themed early-return).

## Non-goals

- TV/episode resolution (still the separate follow-up; this only makes an already-owned leaf playable).
- Any change to Seam A (badge), the resolver, or the classic Play route (already works).
- A distinct "Play from disk" label or a separate local-vs-stream chooser (Play prefers local transparently).
