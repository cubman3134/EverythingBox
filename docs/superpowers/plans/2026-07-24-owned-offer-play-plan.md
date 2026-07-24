# Owned ⇒ Offer "Play" Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface a "Play" action on a catalog detail whenever the local library holds a directly-playable file for that item, so an owned movie plays from its tile even when no stream/debrid provider exists.

**Architecture:** Two edits in `native/src/ui/HomeView.cpp`: (1) add an `ownedPlayable` term (the Seam B short-circuit's own precondition — `localPathFor`, not `ownsId`) to the shared `classicActionGates` Play predicate, which both the classic and themed detail surfaces read; (2) add a prefer-local early-return at the top of `playThemedLeaf` so the themed Play verb reaches the local file instead of dead-ending at the "No stream source" toast. Classic Play already routes through `resolvePlay`'s local short-circuit and needs no change.

**Tech Stack:** Qt 6.8.3, the shipped LocalLibrary `OwnedIndex` + Seam B, headless probes (no new probe — the `OwnedIndex` primitives are already probe-covered).

## Global Constraints

- **Branch:** `local/owned-play` off main. Standing autonomy through the merge gate. The pre-commit hook auto-bumps the patch version on every commit — expected; never hand-edit the version lines.
- **Scope:** exactly the two `HomeView.cpp` edits below. No new probe, no Seam A change, no resolver change, no classic-Play-route change, no TV/episode work, no "Play from disk" relabel. Non-goals per spec.
- **ANCHOR ON FUNCTION NAMES.** Scout anchors (main@ecf6609 / merged tree):

| Concern | Anchor |
|---|---|
| Shared gate | `HomeView::classicActionGates(const MediaItem&)` `HomeView.cpp:3750-3789`; Play predicate at `:3779` (`g.play = isSteam \|\| isRemotePlayable \|\| g.readable \|\| isBridgedAudio \|\| isBridgedGame;`) |
| Both consumers | classic `showMeta` `:3836/:3841` (`playBtn_->setVisible(gates.play)`); themed `themedDetailData` `:3621/:3625` (`if (gates.play \|\| directOpen) verbs << "play";`) |
| Themed Play detour | `playThemedLeaf(int idx)` `HomeView.cpp:3675`; existing `local:video` early-return `:3693`; `needsImdb` branch `:3706` (async `requestMeta` → returns); dead-ends in `onMetaReady` `:3982-3988` when `!hasStreamProvider` |
| Seam B short-circuit to mirror | `resolvePlay` head `HomeView.cpp:3084-3095` (`localPathFor(it.id)` → else `it.imdbStreamId` → `openItem(local:video)`) |
| Ownership key | detail item `id` = the resolver's `OwnedIndex` key (`tmdb:movie:…`/`tt…`); `LocalLibrary` already `#include`d `HomeView.cpp:12`, `index()` already called in `resolvePlay` |

- **`ownedPlayable` uses `localPathFor`, NOT `ownsId`** — `ownsId` is true for a series container (owns episodes), which has no directly-playable file; `localPathFor(item.id)` non-empty means "a real file will play," so no dead Play button on containers.
- **Env recipe:** PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` + `/c/mpv-dev`; build dir `build` (generated qt.conf, no `QT_PLUGIN_PATH`). **Harness runs the RELEASE binary — build `--config Release`.** App target: `mymediavault`. You edit an EXISTING file only → NO reconfigure; build `cmake --build build --target mymediavault --config Release --parallel`. Do NOT run a target-less build; if a build runs >5 min, report BLOCKED. Suite: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.

---

### Task 1: open the gate for owned items + fix the themed Play detour

**Files:** Modify `native/src/ui/HomeView.cpp` (two edits).

**Interfaces:** Consumes the shipped `LocalLibrary::index().localPathFor(const QString&)` and the `MediaItem::{id,imdbStreamId}` fields. Produces: `classicActionGates` offers Play for a directly-playable owned item; `playThemedLeaf` plays an owned item's local file directly.

- [ ] **Step 1: Edit 1 — the shared gate.** In `HomeView::classicActionGates`, immediately BEFORE the `g.play = …` line (`:3779`), add the `ownedPlayable` term and append it to `g.play`. Locate the exact existing line:
```cpp
    g.play = isSteam || isRemotePlayable || g.readable || isBridgedAudio || isBridgedGame;
```
Replace it with:
```cpp
    // Owned local file: offer Play even when no stream/debrid provider can resolve this catalog item, so the
    // on-disk copy plays from its tile. Use localPathFor (the Seam B short-circuit's own precondition), NOT
    // ownsId — ownsId is true for a series CONTAINER (owns episodes), which has no directly-playable file.
    const bool ownedPlayable =
           !LocalLibrary::index().localPathFor(item.id).isEmpty()
        || (!item.imdbStreamId.isEmpty() && !LocalLibrary::index().localPathFor(item.imdbStreamId).isEmpty());
    g.play = isSteam || isRemotePlayable || g.readable || isBridgedAudio || isBridgedGame || ownedPlayable;
```
(Confirm the parameter name is `item` — the function is `classicActionGates(const MediaItem& item)`. If it is named differently, match it.)

- [ ] **Step 2: Edit 2 — the themed prefer-local early-return.** In `HomeView::playThemedLeaf(int idx)` (`:3675`), find the existing `local:video` early-return (`:3693`) — it looks like:
```cpp
    if (it.mime == QStringLiteral("local:video") && !it.url.isEmpty()) { emit openItem(it); return; }
```
Immediately AFTER that existing early-return (and BEFORE the `needsImdb` branch at `:3706`), add the prefer-local short-circuit mirroring `resolvePlay`'s head:
```cpp
    // Prefer-local: an owned catalog item plays its on-disk file directly, WITHOUT the meta-fetch/stream-
    // provider detour below (a metadata-only catalog otherwise dead-ends at "No stream source" though the file
    // is on disk). Mirrors resolvePlay's head + the classic Play route.
    {
        QString lp = LocalLibrary::index().localPathFor(it.id);
        if (lp.isEmpty() && !it.imdbStreamId.isEmpty())
            lp = LocalLibrary::index().localPathFor(it.imdbStreamId);
        if (!lp.isEmpty() && QFileInfo::exists(lp)) {
            MediaItem local = it;
            local.url = lp;
            local.mime = QStringLiteral("local:video");
            emit openItem(local);
            return;
        }
    }
```
Use `playThemedLeaf`'s actual leaf-item variable name (the scout shows it as `it` — confirm by reading the function; it is derived from `items_[browseRowMap_[idx]]`). Ensure `#include <QFileInfo>` is present in `HomeView.cpp` (it is — `resolvePlay` uses `QFileInfo::exists`; verify, add if somehow absent).

- [ ] **Step 3: Build the app (Release), verify clean compile.**
```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --target mymediavault --config Release --parallel
```
Expected: `mymediavault.vcxproj -> …\build\Release\MyMediaVault.exe` with no errors.

- [ ] **Step 4: Full suite (regression check — no probe change).**
```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: `ALL HEADLESS PROBES PASSED` (the edits are additive UI-gate logic; nothing a probe covers changes).

- [ ] **Step 5: Commit.**
```bash
git add native/src/ui/HomeView.cpp
git commit -m "feat: owned items offer Play + themed prefer-local, even with no stream provider (owned-play)"
```

---

### Task 2: close-out — live verify + fable + merge

- [ ] **Step 1: Live verify (the payoff).** Portable-throwaway technique (copy the deployed data dir with `aiocatalog`, cloud-stripped; real app untouched; `MMV_UITEST` + `native/tools/uitest.py` per the `verify-app-gui-capture` memory). Seed `library/folder` with a fixture movie whose title resolves via aiocatalog (e.g. `Interstellar (2014)`, no NFO). Launch → let the resolver cache the match → open that movie's **themed detail** → verify a **"Play" action is now present** → activating it **plays the local file** (mpv opens the on-disk path; an error on a tiny fixture is fine — verify it routes local, not a stream/"No stream source" toast). Also verify: a **non-owned** metadata-only movie shows **no Play**; a **series container** shows **no Play**. Screenshots `ownedplay-detail.png`, `ownedplay-play.png`. If the throwaway can't surface a matching tile, record honestly + rely on code-walk (the ownership primitives are probe-covered) — but attempt it (this is the scenario the resolver smoke couldn't complete).
- [ ] **Step 2: Perf sanity.** The change is a per-detail-build gate term (an O(1) `localPathFor` hash lookup when a detail opens) + a play-time branch — off the browse/scroll hot path. A full perf baseline isn't warranted for a two-line gate; confirm the app starts and a detail opens without lag in the live pass, and note it. (If any lag is observed, capture a 3-run baseline; otherwise a one-line note suffices.)
- [ ] **Step 3: Fable review.** `scripts/review-package $(git merge-base main HEAD) HEAD`, most-capable model. Dimensions: the `ownedPlayable`-uses-`localPathFor`-not-`ownsId` distinction (no dead Play on a series container); the term mirrors `resolvePlay`'s short-circuit precondition exactly (so "Play offered" ⟺ "a file will play"); the themed early-return sits before `needsImdb` and after the existing `local:video` return, with the `QFileInfo::exists` guard (stale path falls through, never crashes); classic Play route unchanged; Seam A/resolver untouched; an owned-AND-remotely-playable item isn't double-offered Play (idempotent OR term). Fix rounds → merge.
- [ ] **Step 4: Merge + push + redeploy.** Update the spec Status → complete (live result recorded). Merge `local/owned-play` → main (resolve any version-line conflict by taking the higher patch), rebuild the combined tree, full suite green (**build all probe targets incl. probe_browse/probe_perf/probe_resolver/probe_importers/probe_locallib to catch any latent link break**), push, delete the branch, redeploy Release to `C:\MyMediaVault-app` (md5-verify), update `.superpowers/sdd/progress.md`, mark the chapter.

## Self-Review (done at write time)

- **Spec coverage:** Edit 1 gate term ✅T1; Edit 2 themed prefer-local ✅T1; `localPathFor`-not-`ownsId` (container safety) ✅T1; classic route unchanged ✅ (no task touches it); label stays "Play" ✅ (no relabel); live verify (owned plays / non-owned no Play / container no Play) ✅T2; fable + merge ✅T2. Non-goals (Seam A, resolver, TV, relabel) not built ✅.
- **Placeholder scan:** every code step carries the exact before/after; the two anchors name the exact existing lines to locate and the implementer confirms the variable/param names by reading (the plan can't guarantee a line number but names the surrounding code).
- **Type consistency:** `ownedPlayable`, `LocalLibrary::index().localPathFor`, `MediaItem::{id,imdbStreamId}`, `emit openItem(local)` with `mime="local:video"`, `classicActionGates`/`playThemedLeaf` — all match the shipped code the scout quoted; the themed early-return is byte-consistent with `resolvePlay`'s head.
- **Ambiguity resolved:** both edits land in ONE task (the gate without the themed fix would leave a dead themed Play button → they must ship together; a reviewer couldn't approve one without the other).
