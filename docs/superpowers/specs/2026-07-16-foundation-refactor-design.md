# Foundation Refactor — Phase 1 Design

**Date:** 2026-07-16
**Status:** Plan 1 implemented (MainWindow seams + synthetic catalogs); plan 2 pending (async browse providers + sweep)

## Problem

MyMediaVault has grown two god-files that own most of the app's behavior:

- `native/src/ui/MainWindow.cpp` — 5,944 lines. Owns page routing, nav dispatch, the Esc
  menu, video/audio playback and queues, resume persistence, emulator launch and
  hotkey-watch, stream/M3U handling, document opening, and notifications.
- `native/src/ui/HomeView.cpp` — 3,997 lines. Owns two presentation modes (XMB, carousel),
  focus management, and the content logic for addon catalogs, playlists, recents,
  downloads, favorites, Steam games, and global search.

Consequences: error handling is inconsistent (many silent failures), nav behavior is
implemented several similar-but-different times, loading strategies vary per content
type, and changes are risky because everything is coupled.

## Goals

1. **Reusable** — behavior lives in focused services with one clear purpose each.
2. **Robust** — one shared user-visible error/feedback channel; no silent failures.
3. **Fast** — one shared async-load-with-cache pattern for browse content (deep perf
   work is phase 2).
4. **User friendly** — nav consistency falls out of shared code paths; visual polish is
   phase 2, on top of this foundation.

Behavior is otherwise unchanged in this phase. The only intentional visible change:
failures that were silent become visible notifications.

## Non-goals (phase 2+)

- Visual polish / styling overhaul.
- Deep performance profiling and optimization.
- Speculative restructuring of RetroView, AddonManager, EmulatorManager beyond trims
  exposed by this phase's extractions.
- `libretro.h` (7,978 lines) is a vendored API header — out of scope entirely.

## Target architecture

### MainWindow → thin shell (~800 lines)

Keeps only: page stack, nav-key dispatch to the active page, fullscreen/window chrome,
Esc menu. Delegates to new services:

| Service | Location | Absorbs |
|---|---|---|
| `PlaybackSession` | `native/src/media/` | Video/audio playback state machine: queues, track stepping (`nextTrack`/`prevTrack`/`onTrackEnded`), next-episode logic, resume begin/persist/finish. Single owner of "what is playing". Emits signals; MainWindow renders. |
| `GameLauncher` | `native/src/launch/` | `openGamePath` → emulator selection → libretro vs external launch, `ensureEmu`, `runEmulator`, hotkey watch (`startEmuHotkeyWatch`/`pollEmuExitHotkey`), exit handling. |
| `StreamResolver` | `native/src/media/` | Stream URL opening, M3U fetch/parse (`openM3u`/`handleM3u`), audio streams. Pure-ish logic; unit-testable. |
| `Notifier` | `native/src/ui/` | `notify`, `showPlayerNotice`, `showNextSourceFeedback`, notice positioning. Becomes the single app-wide success/failure feedback channel. |

### HomeView → presentation + focus (~1,200 lines)

Keeps XMB/carousel presentation and focus management. Content comes from a
`BrowseLevel` interface in `native/src/browse/`:

```
class BrowseLevel {
  title(); items(); activate(index); back(); hasMore(); loadMore();
  // + signals: itemsChanged, error(message)
};
```

Each current content type becomes a small provider (~100 lines): `RecentsLevel`,
`DownloadsLevel`, `FavoritesLevel`, `PlaylistsLevel`, `SteamLevel`, `SearchLevel`,
`CatalogLevel` (addon catalogs). All providers share one async-load-with-cache path
(reusing the existing MetaCache prefetch model) and report failures through `Notifier`.

Adding a future level (e.g. collections) = one new provider file.

## Extraction order

Each step is one commit; the app builds and the touched flow is verified working before
the next step begins.

1. **Notifier** — small, everything depends on it, unlocks the robustness channel.
2. **StreamResolver** — smallest playback seam; proves the extraction pattern.
3. **PlaybackSession** — video first, then the audio queue.
4. **GameLauncher** — touchiest (hotkey watch, external emulators); last MainWindow seam.
5. **BrowseLevel + RecentsLevel** (simplest provider), then one provider per commit:
   downloads, favorites, playlists, Steam, search, addon catalogs.
6. **Sweep** — targeted trims in RetroView/AddonManager only where the extractions
   exposed duplication.

## Error handling

During each extraction, every discovered failure path (bad ROM, dead addon, missing
media, network error, launch failure) is routed through `Notifier` with a short
user-visible message. No new dialogs — `Notifier` uses the existing notice/toast UI and
respects the nav-kit rule (no QDialog/QMessageBox).

## Verification

Per commit:

- Release build (deploys must be Release per project convention).
- Drive the touched flow via `MMV_UITEST=1` + `native/tools/uitest.py` (key/state/shot),
  no window focus required.
- `probe_nav` gate — nav regressions are the historical risk in these files.
- For playback/launch seams: launch one video, one audio queue item, one libretro game,
  one external-emulator game.

## Risks

- **Hidden coupling** in MainWindow (member state shared across concerns). Mitigation:
  extract one seam at a time; keep signals/slots as the boundary; never two seams in
  one commit.
- **Nav regressions** — mitigated by probe_nav per commit.
- **Hard-won edge cases** (git log shows many) — mitigated by moving code, not
  rewriting it; behavior-preserving extraction first, cleanup second.

## Phase 2 (separate spec, after this lands)

Performance profiling (startup, scroll, load), visual polish pass, and any remaining
UX friction — all far easier on top of the extracted structure.
