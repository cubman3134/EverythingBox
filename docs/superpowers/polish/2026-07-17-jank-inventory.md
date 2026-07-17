# MyMediaVault — ranked jank inventory (Polish Track, Task 2)

**Date:** 2026-07-17 · **Branch:** `polish/track-plan1` · **Author:** Polish Task 2

Ranked inventory of visual jank, empty-state gaps, feedback-policy violations, and code
debris found across the themed-home flow sweep (Task 1: 79 screenshots + `notes.md`), a
full grep of the user-feedback call sites, and the debris list carried over from the
spec. Items are ordered by user impact — things the user hits **every session** rank above
corner cases; code debris sits at the bottom. Taste calls are `**Triage:** PENDING`;
objective defects/policy violations are `**Triage:** OBJECTIVE`.

Item format (parsed by Tasks 4–5): `### J<NN>: <title>` + `**Category:**` (cut | empty |
feedback | debris) + `**Evidence:**` + `**Proposed fix:**` + `**Cost:**` + `**Triage:**`.

---

## Feedback-event inventory

Every user-feedback call site across `native/src/` (`notifier_->notify`, `notifyUser`,
`statusMessage`, `statusBar()->showMessage`, `showToast`, `playerNotice`). Grouped by
event class; representative sites only (the full grep is ~90 sites — the duration values
below are the observed set, not a sample).

| Site (file:line) | Trigger (phrase) | Channel | Duration (ms) |
|---|---|---|---|
| HomeView.cpp:3376 | `showToast()` definition — default arg | toast | **4500** (0 = sticky) |
| HomeView.cpp:2293 | "Removed from Favorites." | toast | 2500 |
| HomeView.cpp:2306 | "Added … to Favorites." | toast | 2500 |
| HomeView.cpp:1580 / 2339 | "Added … to …" (playlist) | toast | 3500 |
| HomeView.cpp:2358 | "Uninstalled/Removed …" | toast | 3000 |
| HomeView.cpp:1451 / 2118 / 3027 | "No add-ons…" / "already being prepared…" | toast | 4000 |
| HomeView.cpp:750 | "No results for …" | toast | 6000 |
| HomeView.cpp:2749 / 3271 | "No playable/stream source for …" | toast | 7000 |
| HomeView.cpp:2710 | "No copies of … were found." | toast | 8000 |
| HomeView.cpp:2694 | "Can't reach the file provider …" | toast | 9000 |
| HomeView.cpp:2744 | provider notice | toast | 12000 |
| HomeView.cpp:2080 / 2611 / 2660 / 2733 | "Finding…/Loading…/Preparing…" (in-flight) | toast | 0 / 20000 |
| MainWindow.cpp:229 / 233 / 240 | Achievement unlocked / progress / none | statusBar | 8000 / 6000 / 5000 |
| MainWindow.cpp:625 | "Track N of M" | statusBar | 3000 |
| MainWindow.cpp:748 | RetroView `statusMessage` → statusBar | statusBar | 3000 (pinned) |
| MainWindow.cpp:1571 / 1574 | split-pane "no emulator" / "own window" | statusBar | 6000 / 5000 |
| MainWindow.cpp:1392 / 1564 | themed error notice → `notify` | Notifier | 4500 / plan.errorMs |
| MainWindow.cpp:1404 / 1519 | player-overlay "Up next…" / stream notice | playerNotice | 6000 / 20000 |
| MainWindow.cpp:580 | "Finding another source…" | playerNotice | 30000 |
| GameLauncher.cpp:221 / 244 | "Can't run game…" | notifyUser | plan.errorMs / 7000 |
| GameLauncher.cpp:177 / 257 | core download %, log lines (in-flight) | statusMessage | 0 (sticky) |
| GameLauncher.cpp:308 / 313 / 384 / 396 | installed / msg / no emulator / already running | statusMessage | 5000 / 9000 / 6000 / 4000 |
| RetroView.cpp:805 / 991 | "The emulator core crashed…" | statusMessage | (→ 3000 via MW:748) |
| RetroView.cpp:1375 / 1395 | "State saved/loaded slot N" | statusMessage | (→ 3000) |

### Proposed feedback POLICY

Named constants, values chosen **from the observed distribution above** (the durations
already cluster into four bands — these constants name the bands):

```
constexpr int kFeedbackShort    = 2500;  // brief success confirmations (2500–3000 today)
constexpr int kFeedbackStandard = 4500;  // default info/confirmation (already the toast default)
constexpr int kFeedbackLong     = 7000;  // errors that must be read (5000–12000 today)
constexpr int kFeedbackSticky   = 0;     // in-flight progress; cleared when the op resolves
```

| Event class | Channel (policy) | Constant | Value |
|---|---|---|---|
| **Error** (couldn't do the thing) | `notifier_->notify` (themed) or `showToast` | `kFeedbackLong` | 7000 |
| **Progress** (in-flight: Finding…/Downloading…) | `showToast` / `statusMessage` **sticky**, replaced by the result | `kFeedbackSticky` | 0 |
| **Success-confirmation** (did the thing: Added/Removed) | `showToast` | `kFeedbackShort` | 2500 |
| **Ambient status** (while a panel/Settings is open) | `statusBar()->showMessage` | `kFeedbackStandard` | 4500 |
| **Player-overlay notice** (over the video surface) | `notifier_->playerNotice` | `kFeedbackLong` | 7000 |

Every current site maps to one of these classes. Sites whose duration or channel violates
its class become the `feedback` items below (J06–J08, J10, J11).

---

## Ranked inventory

### J01: OSK search-submit crashes the app
**Category:** debris
**Evidence:** `evidence/search-osk-presubmit.png` (OSK renders fine pre-submit); crash on "Done". WER: Qt6Core.dll, 0xc0000005, fault offset 0x68e98. Repro 3× (Task 1 report §Notable rough spots #1).
**Proposed fix:** fix the search-submit path (the OSK "Done" handler); a core action (search) and playlist-create are unusable until then.
**Cost:** medium
**Triage:** FIXED (9109d7c) — harness-only UAF (UiTestServer), reclassified: real users never hit it

### J02: Movie action menu occludes its own detail panel
**Category:** debris
**Evidence:** `evidence/movie-action-menu-occludes-detail.png` — the Play/Favorite/Add/Download menu centers over the detail column, clipping the title ("Obsession"→"bsession"), every metadata label ("Released"→"eleased"), and the synopsis' first column.
**Proposed fix:** offset the QML action menu clear of the detail column (anchor it over the poster/left rail, not centered over the text), or dim+shift the detail panel while the menu is open.
**Cost:** small
**Triage:** OBJECTIVE — the menu and the text it describes fight for the same pixels on every movie/playlist open.

### J03: RetroView launch is a hard cut to black
**Category:** cut
**Evidence:** `evidence/retroview-launch-cut-black.png` (`trans-42-retroview-enter-b`); themed home → black core surface with no intermediate frame.
**Proposed fix:** brief crossfade/hold (fade themed home out to black before the core surface swaps in) so the launch reads as a transition, not a flash.
**Cost:** small
**Triage:** SKIP (user) — keep the instant cut.

### J04: RetroView exit is a hard cut back to home
**Category:** cut
**Evidence:** `evidence/retroview-exit-cut-home.png` (`trans-44-retroview-exit-b`); black core → full themed home instantly on a single Escape.
**Proposed fix:** symmetric fade-in of themed home on core exit (pairs with J03).
**Cost:** small
**Triage:** SKIP (user) — keep the instant cut.

### J05: XMB category/drill swaps are instantaneous (no transition)
**Category:** cut
**Evidence:** `notes.md` trans-02/04/06/08/10 (category swaps) and trans-12 (drill into Recent) — the `a` (at-keypress) and `b` (+200 ms) frames are identical, i.e. the page has fully changed by the keypress frame; no slide/fade between categories.
**Proposed fix:** short horizontal slide/fade on category change and a subtle push on drill-in, matching the XMB idiom.
**Cost:** medium
**Triage:** FIX — user-approved motion pass (short horizontal slide on category change, subtle push on drill).

### J06: Error-toast durations are scattered (5000–12000 ms)
**Category:** feedback
**Evidence:** HomeView.cpp:2694 (9000), 2710 (8000), 2744 (12000), 2749/3271 (7000), 750 (6000); GameLauncher.cpp:244 (7000); MainWindow.cpp various 6000/8000.
**Proposed fix:** route all error-class notices through `kFeedbackLong` (7000) via the constants header.
**Cost:** small
**Triage:** FIXED (468e136) — HomeView error toasts (No results / provider unreachable / no copies / provider notice / no playable source / no stream source) + GameLauncher `notifyUser` and `CorePlan::errorMs` (default and archive-extract) all → `kFeedbackLong`.

### J07: Success-confirmation toast durations are scattered (2500/3000/3500 ms)
**Category:** feedback
**Evidence:** HomeView.cpp:2293/2306 (2500), 2358 (3000), 1580/2339 (3500).
**Proposed fix:** route success confirmations through `kFeedbackShort` (2500).
**Cost:** trivial
**Triage:** FIXED (2d0c765) — Favorites add/remove, playlist add (both sites), uninstall/remove → `kFeedbackShort`.

### J08: Achievement toasts use the occluded status bar
**Category:** feedback
**Evidence:** MainWindow.cpp:229/233/240 — "🏆 Achievement unlocked" goes to `statusBar()->showMessage` (8000/6000/5000), but the status bar sits under the themed-home QQuickWidget and the full-screen player surface, so the unlock may never be seen.
**Proposed fix:** treat achievement pop-ups as the **player-overlay** class (`notifier_->playerNotice`, `kFeedbackLong`) while a game is on screen; they earn a visible channel.
**Cost:** small
**Triage:** FIXED (6d4aad3) — root-cause corrected the proposed channel: `playerNotice_` is parented to the mpv `player_` widget, which is HIDDEN during a RetroView emulator game (exactly when achievements fire), so playerNotice would be invisible. Instead: the unlock's visible channel is the existing `retro_->showAchievement` on-screen popup (the redundant, occluded `statusBar()` echo is dropped), and the two gameLoaded summaries + the unsupported-version notice route through the window-level `notify()` overlay ("over ANY view") at `kFeedbackLong`.

### J09: Audiobooks detail panel is bound to the wrong record
**Category:** debris
**Evidence:** `evidence/audiobooks-wrong-record-binding.png` — list highlights "The Skinner" (Neal Asher, 2004) while the detail panel shows "The Skinner" by **Jane Austen** with a **Sense and Sensibility** cover/synopsis.
**Proposed fix:** fix the detail-panel binding for the Audiobooks rail so the panel follows the selected row (likely an index/id mismatch between the list model and the metadata lookup).
**Cost:** small
**Triage:** OBJECTIVE — wrong data shown, not taste. (Corner case: only the Audiobooks rail.)

### J10: Core-crash error is shown for only 3 seconds
**Category:** feedback
**Evidence:** RetroView.cpp:805/991 emit "The emulator core crashed and was stopped." on `statusMessage`, which MainWindow.cpp:748 pins to 3000 ms — an error on the short ambient channel.
**Proposed fix:** surface core-crash as an **error**-class notice (`notify`, `kFeedbackLong`); the 3000 ms path is fine for save/load-state confirmations but too short for a crash the user must notice.
**Cost:** small
**Triage:** FIXED (434005b) — added a `RetroView::coreError` signal; the two `core_.crashed()` sites emit it instead of `statusMessage`; MainWindow routes it to `notify(kFeedbackLong)`. Save/load-state stays on the 3000 ms `statusMessage` path.

### J11: Split-external launch — status/toast asymmetry
**Category:** feedback
**Evidence:** MainWindow.cpp:1564 (error → `notifier_->notify`) vs 1571/1574 ("no emulator" / "opens in its own window" → `statusBar()->showMessage`) in the same split-pane branch.
**Proposed fix:** assign per the policy table — the "opens in its own window" and "no emulator configured" notices are ambient-status (`statusBar`, `kFeedbackStandard`) only if the status bar is visible in split view; otherwise route through `notify`. Make the two sibling notices use one channel.
**Cost:** trivial
**Triage:** FIXED (a4fa3e4) — both siblings now use `notifier_->notify` (matching the branch's error sibling), since the status bar isn't a reliable channel in split view. "No emulator configured" is an error → `kFeedbackLong`; "opens in its own window" is ambient info (the game did launch full-screen) → `kFeedbackStandard`.

### J12: Empty Downloaded / Favorites folders vanish silently
**Category:** empty
**Evidence:** HomeView.cpp:3488 (`if (!f.present) continue`), roots 3512-3516, consoles 3543-3547 — the synthetic Downloaded/★ Favorites folders are guard-hidden entirely when their store is empty. Empty state = the folder is **absent**, not a blank grid (Task 1 empty-state matrix).
**Proposed fix:** decide the empty affordance — either keep guard-hidden (current, defensible) or render the folder with a one-line "Nothing here yet" placeholder so the feature is discoverable. (Favorites is empty everywhere due to a known separate bug, so it never appears at all today.)
**Cost:** small
**Triage:** SKIP (user) — keep guard-hidden; clean rails preferred.

### J13: Search 0-results empty state is unreachable
**Category:** empty
**Evidence:** Task 1 empty-state matrix — submitting any OSK query crashes (J01) before results render, so the "No results for …" toast (HomeView.cpp:750) and empty grid can't be exercised.
**Proposed fix:** none of its own — unblocked automatically by J01. Re-verify the 0-results rendering once the crash is fixed.
**Cost:** trivial
**Triage:** OBJECTIVE — blocked-by J01, tracked so it isn't forgotten.

### J14: Empty playlists show only a "➕ New playlist…" row
**Category:** empty
**Evidence:** `evidence/empty-playlists-only-newrow.png` (`empty-07`) — Games ▸ Playlists with no playlists renders a single create-affordance row.
**Proposed fix:** none required — this is a clean, unambiguous empty-collection affordance. Logged only so the ranker can confirm it's intentional.
**Cost:** trivial
**Triage:** SKIP (user) — the create row is the empty state; no caption.

### J15: Feedback duration constants header (enable J06–J08, J10, J11)
**Category:** debris
**Evidence:** durations `4500`/`6000`/`7000` etc. are scattered magic numbers across HomeView.cpp, MainWindow.cpp, GameLauncher.cpp (see the feedback inventory).
**Proposed fix:** add the `kFeedbackShort/Standard/Long/Sticky` constants (values above) in one shared header and migrate every call site to them; the feedback items reference these names.
**Cost:** small
**Triage:** FIXED (fd56007) — `native/src/ui/FeedbackPolicy.h` created; call sites migrated per the feedback items (J06–J08, J10, J11).

### J16: Unwired `nextTrack` / `prevTrack` slots (investigated)
**Category:** debris
**Evidence:** MainWindow.cpp:1490-1498 — `nextTrack()`→`session_->next()`, `prevTrack()`→`session_->prev()`, declared as slots MainWindow.h:97-98. **Investigation:** no `connect(...)` references either slot anywhere (grep clean). The visible transport buttons ⏮/⏭ (MainWindow.cpp:471,475) are wired to `player_->prevChapter()`/`nextChapter()` (lines 734-735, tooltips "Previous/Next chapter") — **chapter** nav, not track skip. Automatic track advance runs through `PlaybackSession::queueFinished` → `onTrackEnded()` (1500-1503). There is no media-key handling (`grep` finds no `Qt::Key_MediaNext`). So both slots are genuinely dead.
**Proposed fix:** **delete** `nextTrack`/`prevTrack` (slots + declarations). Wiring them to hardware media keys is non-trivial (needs a global media-key hook) and out of polish scope; manual per-track skip in the now-playing audio view would be a new feature, not debris cleanup. `PlaybackSession::next/prev` stay (used by auto-advance).
**Cost:** trivial
**Triage:** OBJECTIVE.

### J17: `SearchAggregator::cancel()` is unwired
**Category:** debris
**Evidence:** SearchAggregator.cpp:35-39 defines `cancel()` (clears `reqs_`/`reqSrc_`); no caller exists (grep clean — HomeView never calls `agg_->cancel()`).
**Proposed fix:** wire `cancel()` into search-level teardown (call it where a search is abandoned — closing the OSK / leaving the search view / starting a new query), per the plan-2 final review, so stale in-flight results don't stream into the next context.
**Cost:** trivial
**Triage:** OBJECTIVE.

### J18: `openLibraryItem` audio branch has no stable resume key
**Category:** debris
**Evidence:** MainWindow.cpp:3738-3742 — the `type == "audio"` branch calls `session_->setQueue({ url }, 0)` with no resumeKey, unlike the sibling audiobook branch (3736) which passes `item.id`. A re-resolved stream URL changes each open, so resume/Recent keying is lost.
**Proposed fix:** pass `item.id` as `setQueue`'s 4th (resumeKey) arg, mirroring the audiobook branch.
**Cost:** trivial
**Triage:** OBJECTIVE.

### J19: Stale `notify` comment survivors
**Category:** debris
**Evidence:** flagged in phase-1 reviews (MainWindow.h notify block). **Verified:** the block at MainWindow.h:77-80 is accurate to current behavior (documents Notifier delegation + toast plumbing) — not stale.
**Proposed fix:** no change needed for the MainWindow.h block; if any other `notify`-related comment referencing the old begin-after-`setQueue` resume path survives (see PlaybackSession.cpp:56 which correctly documents the fix), leave it. Close unless a survivor is found during the feedback migration.
**Cost:** trivial
**Triage:** OBJECTIVE.

### J20: `probe_perf` restart bound too tight (`<25` → `<30`)
**Category:** debris
**Evidence:** probe_perf.cpp:66-70 — `restartOk = … parts[2].toLongLong() < 25` for the begin-overwrite-restarts-the-clock unit check.
**Proposed fix:** loosen the bound to `< 30` (the 5 ms nominal run has enough scheduling jitter on a loaded box to occasionally exceed 25 ms).
**Cost:** trivial
**Triage:** OBJECTIVE.

### J21: `perfbaseline.py` never asserts its end-state
**Category:** debris
**Evidence:** perfbaseline.py docstring (line 14) claims "Verified end screen (every run): themedCategory=Games, themedSelection=Recent", but `run_route` (through line 114) just `proc.kill()`s without checking the final `_state()`.
**Proposed fix:** add an end-state assert after the normalize loop — `assert _state().get("themedCategory") == "Games"` (and log/append to `skipped` on mismatch) so a route that drifts off Games/Recent fails loudly instead of emitting a silently-incomparable baseline.
**Cost:** trivial
**Triage:** OBJECTIVE.

---

## Verified non-issues (recorded, not items)

- **Metadata pop on scroll — NOT reproduced.** `trans-14/15/16` (0 ms `a` vs 400 ms `b`)
  are essentially identical: full title/logo/year/last-played/synopsis are present in the
  at-keypress frame. Commit `2a578ab` (instant cached/gamelist metadata) is working — no
  plain-title flash for cached items. Only a subtle video-preview fade-in differs.
- **OSK grid render** is correct when settled (an early all-"1" grab was a timing artifact).
- **RetroView screenshots grab real content** (Jurassic Park boot) — the launch/exit cuts
  in J03/J04 are genuine hard transitions, not black-grab artifacts.
- **Per-console empty grids** could not be produced (backend up; Wii U now fully populated —
  the baseline's "Wii U empty" is stale).

## Evidence index (`docs/superpowers/polish/evidence/`, downscaled ≤150 KB, 640px wide)

- `movie-action-menu-occludes-detail.png` — J02
- `audiobooks-wrong-record-binding.png` — J09
- `retroview-launch-cut-black.png` / `retroview-exit-cut-home.png` — J03 / J04
- `search-osk-presubmit.png` — J01 / J13
- `empty-playlists-only-newrow.png` — J14

Full 79-shot set + `notes.md` remain scratch under `.superpowers/polish-audit/` (not committed).
