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
**Triage:** FIXED (see report table) — user decided: anchor the chooser BESIDE the detail panel. The inline chooser's x is now derived from the metadata panel itself (`x: meta.x - width - xmb.width*0.02` — right edge pinned to the panel's left edge minus the panel's 0.02·width gap idiom), so it sits over the item-column area and can never occlude the panel's title/facts/synopsis at any resolution/crossX (all terms are fractions; resulting x = 0.30·width, fully on-screen). Anchor-only change — size/color/style untouched. Evidence: `evidence/J02-before.png` / `evidence/J02-after.png`. Original escalation (root cause): re-verified LIVE (`.superpowers/polish-audit/j02-occlusion-live.png`); the fix crossed into layout/design judgment. The inventory was right that it's QML: the offender is the themed XMB **inline action chooser** in `native/src/theme2/qml/elements/Xmb.qml:375-384` (the `Rectangle { id: actions }`, Play/Favorite/Add/Download over the selected leaf) — NOT the C++ nav-kit `NavMenu`. It's positioned `x: xmb.crossX + xmb.width*0.11; width: xmb.width*0.26` — the comment claims "a clear gap from the item column" but it only clears the LEFT column and runs its right half INTO the metadata/detail panel (`meta`, anchored on the right), so it clips the selected item's title/facts (confirmed live: the "ABI-DOS" detail title is split behind the menu). Why it's still a design call, not a determinate fix: the item column, the chooser, and the `meta` panel are all sized in THEME FRACTIONS (crossX/width/height), so "move it clear" means choosing a new anchor rule that stays clear of the detail panel on every theme+resolution without colliding with the item column or shrinking the chooser illegibly — plus deciding between the inventory's two alternatives (reposition vs. dim+shift the detail). Escalated for a themed-layout owner to set the anchor rule in `Xmb.qml`.

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
**Triage:** FIXED (see report table) — the XMB column now lives in a motion container (`col` in `Xmb.qml`): a category change slides it in from the direction of travel (0.05·width) and a drill in/out pushes it subtly from the right (0.02·width), both with an opacity settle. ONE shared duration: `kUiFadeMs = 150` added to `native/src/ui/FeedbackPolicy.h` (the canonical value), bridged to QML as the ThemeView root's `uiMotionMs` (host-set by MainWindow at XMB build; QML falls back to the same 150). The swap itself stays instant — the new content is in place when the settle starts — and each swap RESTARTS the animation, so rapid key repeats never queue: live rapid-input check (20 category flips in 0.42 s, 20 up/downs at 4.3 ms/key send, state round-trip 0.2–15 ms after the storm, end state exact) shows no added input latency. Paging appends are exempt (first-row identity check) so infinite-scroll doesn't twitch. Evidence: `evidence/J05-before.png` (audit trans pair — a/b frames identical, no intermediate) / `evidence/J05-after.png` (t≈30 ms mid-slide frame beside the settled frame).

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
**Triage:** FIXED (e4810b5) — root cause: `HomeView::onMetaReady`'s themed-`/meta` branch keyed off the LIVE `themedMetaIndex_`, but the addon `/meta` request is async and `themedMetaIndex_` advances on every hover. A slow response (the Audiobooks rail has no gamelist/MetaCache short-circuit, unlike games/movies, so it always waits on the addon) landed after the user scrolled on and painted the previous item's synopsis/cover onto the new row. Fix: capture `themedMetaReqIndex_` when the request is issued and bind/emit the response to THAT index; drop it if the selection has moved on (mirrors the game-aggregator path, which already guarded this). Cannot live-repro (addon backend down) — verified by code path.

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
**Triage:** FIXED (verified-only, 48e476d) — the J01 crash fix DID unblock the search-submit path. Re-verified live (`.superpowers/polish-audit/j13-01..09`): "/" opened the "Search everything" OSK, typed "qzx", "Done" submitted, the pipe returned `ok` with NO crash (previously crashed on submit), the results grid was navigable, and opening a result reached the detail page. Two caveats, both non-defects: (1) a TRUE 0-results empty grid ("No results for …" toast) was not reproducible here — a query-ignoring live-TV addon always returns its channel list, so "search everything" never came back empty (results included live channels + loose book matches). (2) The themed results GRID (`themedView=browse`) captured black under the occluded `uitest shot`, while the continuously-animating XMB home + all category/detail views captured perfectly — a capture-harness limitation for the static browse QQuickWidget (its software surface doesn't flush under background grab), not a rendering defect (state confirms the view is populated and navigable). Residual gap: the empty-state pixels themselves weren't positively captured; no evidence of bad rendering.

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
**Triage:** FIXED (c00b895) — re-confirmed grep-clean (only the defs + decls, no `connect`), deleted both slot definitions (MainWindow.cpp) and declarations (MainWindow.h). `PlaybackSession::next/prev` kept.

### J17: `SearchAggregator::cancel()` is unwired
**Category:** debris
**Evidence:** SearchAggregator.cpp:35-39 defines `cancel()` (clears `reqs_`/`reqSrc_`); no caller exists (grep clean — HomeView never calls `agg_->cancel()`).
**Proposed fix:** wire `cancel()` into search-level teardown (call it where a search is abandoned — closing the OSK / leaving the search view / starting a new query), per the plan-2 final review, so stale in-flight results don't stream into the next context.
**Cost:** trivial
**Triage:** FIXED (00ad54a) — `agg_->cancel()` wired into the two search-abandon paths: `selectRecent()` (Back/Home out of the search view — the Back-from-search gesture routes here) and `selectType()` (switching to a catalog). A new query already clears via `agg_->start()`. Stops stale results + the trailing "No results" toast leaking into the next context.

### J18: `openLibraryItem` audio branch has no stable resume key
**Category:** debris
**Evidence:** MainWindow.cpp:3738-3742 — the `type == "audio"` branch calls `session_->setQueue({ url }, 0)` with no resumeKey, unlike the sibling audiobook branch (3736) which passes `item.id`. A re-resolved stream URL changes each open, so resume/Recent keying is lost.
**Proposed fix:** pass `item.id` as `setQueue`'s 4th (resumeKey) arg, mirroring the audiobook branch.
**Cost:** trivial
**Triage:** FIXED (4df740c) — the `type == "audio"` branch now calls `session_->setQueue({ url }, 0, {}, item.id)`. (probe_playback already asserts `setQueue resumeKey keys resume by the stable id`.)

### J19: Stale `notify` comment survivors
**Category:** debris
**Evidence:** flagged in phase-1 reviews (MainWindow.h notify block). **Verified:** the block at MainWindow.h:77-80 is accurate to current behavior (documents Notifier delegation + toast plumbing) — not stale.
**Proposed fix:** no change needed for the MainWindow.h block; if any other `notify`-related comment referencing the old begin-after-`setQueue` resume path survives (see PlaybackSession.cpp:56 which correctly documents the fix), leave it. Close unless a survivor is found during the feedback migration.
**Cost:** trivial
**Triage:** FIXED (bdb5af7) — verified during the J06–J11/J18 migration: MainWindow.h:76–80 notify block is accurate to current behavior; PlaybackSession.cpp:55–56 is a correct historical note, not a stale survivor. No `notify` comment survivors found. No code change (documentation-only close).

### J20: `probe_perf` restart bound too tight (`<25` → `<30`)
**Category:** debris
**Evidence:** probe_perf.cpp:66-70 — `restartOk = … parts[2].toLongLong() < 25` for the begin-overwrite-restarts-the-clock unit check.
**Proposed fix:** loosen the bound to `< 30` (the 5 ms nominal run has enough scheduling jitter on a loaded box to occasionally exceed 25 ms).
**Cost:** trivial
**Triage:** FIXED (a912986) — `probe_perf.cpp` restart bound `< 25` → `< 30`. Rebuilt; probe_perf still PASS (begin-overwrite restarts the clock).

### J21: `perfbaseline.py` never asserts its end-state
**Category:** debris
**Evidence:** perfbaseline.py docstring (line 14) claims "Verified end screen (every run): themedCategory=Games, themedSelection=Recent", but `run_route` (through line 114) just `proc.kill()`s without checking the final `_state()`.
**Proposed fix:** add an end-state assert after the normalize loop — `assert _state().get("themedCategory") == "Games"` (and log/append to `skipped` on mismatch) so a route that drifts off Games/Recent fails loudly instead of emitting a silently-incomparable baseline.
**Cost:** trivial
**Triage:** FIXED (4574ea3) — after the escape-normalize loop, `run_route` now checks `_state()`: a `themedCategory != "Games"` (or a non-Recent `themedSelection`) appends an "end-state drift" note to `skipped`, which the report surfaces as a loud SKIPPED line. Syntax-checked (`ast.parse`); can't run live here (needs the deployed app + UI drive), verified by inspection.

### J22: Residual raw error-duration sweep in MainWindow.cpp
**Category:** feedback
**Evidence:** Task 4's reviewer: error-class feedback sites in `native/src/ui/MainWindow.cpp` still carried raw millisecond literals after the J06–J11 policy migration — `castError` (204, 6000), `applyFailed` (387, 8000), and the dual `showMessage`+`notify` error sites in the download/extract paths (2782/2812/3836/3862/3885/3936/3948/3960, 6000/8000), among others surfaced by an exhaustive `showMessage|notify(` duration grep of the file.
**Proposed fix:** classify every raw-duration site in MainWindow.cpp honestly and route the error-class ones through `kFeedbackLong` (FeedbackPolicy.h, already included); leave sticky/progress literals and non-error sites alone.
**Cost:** small
**Triage:** FIXED (see report table) — all 46 error-class sites (50 literals) now use `kFeedbackLong`: cast/update-apply errors (204/387), next-source failure feedback (1413 — mixed channel: it also carries the "Finding another source…" progress line, but the must-read failure class sets the duration), "no next episode" (1525), emulator-busy rejection (1671), reader/document open failures (1799/1806/1813/3700/3724/3728/4086), missing file (2463), incorrect PIN (2494), download/save/extract failure sites incl. every dual showMessage+notify pair (2733/2782–2813/3836–3984), PC-game cancel/relaunch failures (3212/3225 — were already 7000, now named), no-subtitle-found (3572), no-playable-file (3601), manga assembly/download/save failures (4074/4119/4139/4142), uninstaller start failure (4335). One mixed site split by outcome: Google Drive push result (5102) → `ok ? kFeedbackShort : kFeedbackLong` (success confirmation vs error). Deliberately untouched (honest classification, non-error): success confirmations (203/205/262/2843/3339/3571/4065/4861/5039/5271), progress/launch notices (2860/2924/3070/3608 + all sticky `0` literals), ambient status (630/753), the update-available info notice (384, 12000 — informational must-read, not an error; left for a future policy call), and `plan.errorMs` (1562, already policy-fed via CorePlan since J06). **Residual closed in the close-out commit:** the one literal outside MainWindow.cpp that this sweep's reviewer chain surfaced — `HomeView.cpp:2143`, the dual-outcome `dlNext()` result toast — carried a raw `6000` J06 missed. Classified by outcome (mirrors the 5102 Google-Drive split): success "Queued N item(s)…" → `kFeedbackShort`, error "Nothing here could be downloaded." → `kFeedbackLong`. Honest scope note: a close-out re-grep of HomeView.cpp found three *further* error-class raw literals J06's HomeView sweep also missed (2429 `6000`, 2770 `7000`, 2774 `5000`) — outside loose-end #1's flagged line, recorded as a Follow-up candidate rather than folded in silently. That follow-up is now closed by **J23** (the exhaustive HomeView re-sweep).

### J23: Residual raw error-duration sweep in HomeView.cpp (dlNext dual toast + full re-grep)
**Category:** feedback
**Evidence:** `HomeView.cpp:2143` — the dual-outcome `dlNext()` completion toast still carried a raw `6000` for BOTH outcomes ("Queued N item(s)…" success vs "Nothing here could be downloaded." error) after the J06/J22 sweeps. A full re-grep of HomeView.cpp (including continuation-line durations, which single-line greps miss) surfaced nine more error-class raw literals: 1453 ("No add-ons to search.", 4000), 1560 (wrong-level playlist rejection, 3500), 2121/3034 ("A download is already being prepared…" busy rejections, 4000 — J22's MainWindow:1671 emulator-busy precedent classifies busy rejections as error-class), 2429 ("favourite's source addon isn't available", 6000), 2624 ("No readable pages…", 7000 — right value, unnamed), 2709/2753 (the two "isn't ready yet — still caching" play failures, 10000), 2770 ("No sources found…", 7000), 2774 ("Nothing to play…", 5000).
**Proposed fix:** split the dual site by outcome (mirrors J22's Google-Drive 5102 split) and route every error-class literal through `kFeedbackLong` (FeedbackPolicy.h, already included); leave sticky/progress literals and non-error sites alone.
**Cost:** small
**Triage:** FIXED (this commit) — `dlNext()` result toast split by outcome: `dlQueued_ > 0 ? kFeedbackShort : kFeedbackLong` (success confirmation vs must-read error); all nine error-class literals above → `kFeedbackLong`. Deliberately untouched (honest classification, non-error): 3033 ("“%1” is already saved.", 4000 — informational reassurance, the item IS saved; nearest class is Standard, left for a policy call like J22's update-available notice), 2617 ("Loading “%1”…", 20000 — progress with a safety timeout, not sticky-0 by design: the resolve callback may never fire), every sticky `0` progress literal (2083/2132/2666/2739/2764/3019/3050), and the `showToast` definition's `4500` default arg (it IS `kFeedbackStandard`'s value — the policy default). HomeView.cpp now has no raw non-zero feedback duration outside those named calls.

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

## Follow-up candidates

Non-blocking items surfaced *during* the polish track but deliberately left outside its scope
(each would be its own small task; none gate "phase 2 complete"). Recorded here so they aren't lost.

- **(a) NavMenu overlap on the installed-game leaf detail.** J02 fixed the themed XMB *inline
  action chooser* (QML, `Xmb.qml`). But an **installed** game leaf (e.g. Kirby / Jurassic Park
  reached via Recent) opens the C++ nav-kit **`NavMenu`** instead, which *also* overlaps the detail
  panel (`.superpowers/polish-audit/j02-before-raw.png`). J02's triage explicitly named only the QML
  chooser, so the NavMenu case is untouched. Fix lives in the C++ nav kit (`src/ui/nav/NavMenu`),
  not `Xmb.qml` — different element, out of J02's QML scope.
- **(b) Async-loaded categories take the drill-push, not the directional slide.** In `Xmb.qml`'s
  `col.kick()` (J05 motion), a category change sets `dir` from `catIndex vs lastCat`. But when a
  category's items load **asynchronously**, the first `onItemsChanged` fires while `xmb.items` is
  empty — the early-return at the empty-guard (`if (!xmb.items.length) { lastKey=""; return }`) runs
  *after* `lastCat` was already advanced, so when the real items arrive `catIndex === lastCat` →
  `dir = 0` → the item takes the subtle drill push (`0.02·width`) instead of the directional category
  slide (`0.05·width`). Purely cosmetic (motion flavor on slow-loading rails); the swap itself is
  correct. Fix would carry a "pending direction" across the empty-load early-return.
- **(c) UiTestServer sibling UAF hazard (harness-only).** The crash fix (`9109d7c`) guards the
  suspended readyRead frame against a freed **client socket** (`QPointer<QLocalSocket>`). The
  narrower sibling — `UiTestServer` (`this`/`hooks_`) itself being destroyed (Settings ▸ Debug
  toggle) while a command is suspended in a blocking OSK prompt — is untouched; it would need a
  `QPointer` on `this` too. Harness-only, and the channel's design already precludes a human toggling
  Settings mid-automation, so it was left. See `polish-crash-report.md` Concerns.
- **(d) In-inventory `FIXED (<hash>)` refs are not exactly traceable.** Each item's inventory flip was
  written into the *same* commit as its fix, so the in-file short hash points at the pre-amend commit
  object (self-reference can't be exact), and the three final-batch items read `FIXED (see report
  table)` rather than a hash. The **authoritative** hashes live in the task reports:
  `polish-task-4-report.md` (J06–J21 table), `polish-task-5-report.md` (J02 `2cac77a` / J22 `1ac1e84`
  / J05 `cb975a1`), `polish-crash-report.md` (J01 `9109d7c`). A future pass could rewrite the in-file
  refs to the settled branch hashes.
- **(e) HomeView error-class literals J06's sweep missed (beyond loose-end #1's 2143).** CLOSED by
  **J23** — the full HomeView re-sweep this entry called for. The three flagged literals (2429/2770/
  2774) plus six more error-class sites (1453/1560/2121/2624/2709/2753/3034 — per-class triage
  reclassified the busy/guard rejections as error-class, following J22's MainWindow:1671 precedent)
  now use `kFeedbackLong`; 2617 (progress-with-timeout) and 3033 (informational reassurance) were
  triaged non-error and left, as recorded in J23's triage.
