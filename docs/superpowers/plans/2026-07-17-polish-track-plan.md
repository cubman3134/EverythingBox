# Polish Track Implementation Plan (Phase 2, Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Audit the themed home for jarring moments (three sweeps), get the user's triage, fix the objective consistency block wholesale and the triaged taste items one by one — each with before/after evidence.

**Architecture:** Discovery-then-fix, mirroring the perf track: two audit tasks produce a ranked jank inventory; a controller-run triage gate collects the user's fix/skip calls; an objective-block task lands the no-taste fixes; a repeating taste-fix template lands the rest. Spec: `docs/superpowers/specs/2026-07-17-polish-track-design.md`.

**Tech Stack:** Python (uitest pipe) for sweeps; Qt 6 C++ for fixes; existing probe suite as the regression net.

## Global Constraints

- "Jarring" boundary (spec, verbatim): adding a short standard fade (~150 ms, one shared constant) where a hard cut exists is IN; changing layout, colors, spacing, or the theme's look is OUT.
- Empty states get ONE consistent quiet message row, reused everywhere, styled from existing theme values.
- All user-feedback events route through `Notifier` per the inventory's policy table; durations from one constants header. Status-bar usage survives only where the policy table explicitly keeps it.
- One fix per commit; visual fixes carry a before/after screenshot pair committed under `docs/superpowers/polish/`; probe_nav + full suite green on every commit.
- HANDS OFF: FavoritesStore write path, OSK focus handoff (running sessions), poster-cache eviction + async ensureBios (chip follow-ups).
- Audit screenshots are BULKY: the full sweep set lives in `.superpowers/polish-audit/` (gitignored scratch); ONLY the inventory's key-evidence images and per-fix before/after pairs are committed, downscaled to 640px wide (`python -c "from PIL import Image; ..."` or ffmpeg/magick if available — any tool; committed images must be ≤150 KB each).
- Machine facts (as all prior tracks): build `cmake --build build --config Release [--target <t>]`; probes need Qt bin (+ /c/mpv-dev for the runner) on PATH and offscreen QPA; deploy Release over the existing exe in `C:\MyMediaVault-app` (keep name); drive ONLY via `python native/tools/uitest.py` with `MMV_UITEST=1` (add `MMV_PERF=1` only when measuring); never SendKeys/focus; kill the app after; protect user data (playlist "Weekend Picks"; byte-identical ini restore for any seed). `rtk` proxy hook is normal.

---

### Task 1: Flow sweep + empty-state matrix

**Files:**
- Create: `native/tools/polishsweep.py`
- Create: `.superpowers/polish-audit/` (scratch output: `step-NN-<name>.png`, `trans-NN-<name>-a.png`/`-b.png`, `notes.md`)

**Interfaces:**
- Consumes: `native/tools/uitest.py` (`_send`, `state`), the themed-XMB nav map documented in `docs/superpowers/perf/2026-07-17-baseline.md` "Route fixups" (single `right` per category; Enter opens a NavMenu overlay on games; single Escape exits the core).
- Produces: the screenshot set + `notes.md` (one line per step: what was on screen per state JSON, anything visibly rough) that Task 2 ranks. Naming contract Task 2 relies on: `step-NN-<slug>.png` for stills, `trans-NN-<slug>-a.png`/`-b.png` for cut pairs (a = frame at keypress, b = ~200 ms later).

- [ ] **Step 1: Write the sweep script.** `polishsweep.py` walks (screenshotting each step; for every page CHANGE, take the `-a` shot immediately after the key send and the `-b` shot after 200 ms — a hard cut shows two unrelated frames with no intermediate):
  XMB root → each category left-to-right (root still per category) → into Games → Recent folder → item selection ±3 rows (metadata panel settling: shot at 0 ms and 400 ms after a row change) → open a game's NavMenu overlay → cancel → drill a non-game category (Video → Movies → a detail page if the backend is up) → Playlists folder → Weekend Picks → back to root → search (`/` → OSK visible → cancel) → Esc menu open/close → launch the local game (Jurassic Park via Games ▸ Recent) → shot the RetroView entry cut pair → Escape back (shot the exit cut pair). Steps that fail (backend down, item missing) are recorded in `notes.md` as SKIPPED — never silently dropped. Reuse `perfbaseline.py`'s wait/kill patterns (import or copy its `wait_pipe`; `finally: proc.kill()`).
- [ ] **Step 2: Empty-state matrix.** For each synthetic level — Recent, Downloaded, Favorites (per-console), Playlists, search results — capture the EMPTY rendering. Producing emptiness without touching user data: per-console folders on a console whose stores are empty (the baseline notes Wii U had none); root Downloaded when the downloads store is empty (it was on this box); Favorites is empty everywhere today (known bug in another session — note it, don't fix); an empty search via an OSK query like `zzzzqqq` (0 results); empty playlist via the known "empty playlist doesn't visibly drill" repro IF a throwaway empty playlist can be created AND deleted through the app's own UI (create → verify → delete; abort this sub-item if deletion via UI isn't reachable — do NOT hand-edit the ini for this). Record in `notes.md` what each empty state actually shows (blank grid / message / nothing).
- [ ] **Step 3: Run the sweep** against the deployed app (current main build — deploy first if the exe predates main's HEAD). Review the output yourself: every named step has its file(s) or a SKIPPED note.
- [ ] **Step 4: Commit** (script only — screenshots stay scratch):

```bash
git add native/tools/polishsweep.py
git commit -m "polish: themed-home flow sweep + empty-state capture script"
```

---

### Task 2: Feedback-event inventory + ranked jank inventory

**Files:**
- Create: `docs/superpowers/polish/2026-07-17-jank-inventory.md`
- Create: `docs/superpowers/polish/evidence/` (downscaled key images only, ≤150 KB each)

**Interfaces:**
- Consumes: Task 1's screenshot set + `notes.md`; the codebase for the feedback grep; the debris list below (verbatim from the spec).
- Produces: the inventory doc Task 3's triage runs on. Item format contract (Tasks 4-5 parse it): `### J<NN>: <title>` + `**Category:** cut|empty|feedback|debris` + `**Evidence:**` (image path or `file:line`) + `**Proposed fix:**` one line + `**Cost:** trivial|small|medium` + `**Triage:** PENDING|OBJECTIVE`.

- [ ] **Step 1: Feedback-event inventory.** Grep every user-feedback call site: `notifier_->notify`, `notifyUser`, `statusMessage`, `statusBar()->showMessage`, `showToast`, `playerNotice` across `native/src/`. Tabulate: site (file:line), trigger (one phrase), channel, duration. Then write the POLICY table: one row per event CLASS (error, progress, success-confirmation, ambient status while a panel is open, player-overlay notice) proposing channel + a named duration constant (`kFeedbackShort`, `kFeedbackStandard`, `kFeedbackLong`, `kFeedbackSticky` — values chosen FROM the observed distribution, e.g. standard=4500 error=7000, decided in the table, not here). Every current site gets a policy assignment; sites that violate their class's policy become inventory items (Category: feedback).
- [ ] **Step 2: Rank the visual findings.** From Task 1's set: every hard cut (trans pair with no intermediate), every blank/none empty state, metadata-panel pops, plus anything `notes.md` flagged. One inventory item each, with the evidence image downscaled into `evidence/`.
- [ ] **Step 3: Fold in the debris list** (each pre-tagged `**Triage:** OBJECTIVE`): toast-duration scatter (4500/6000/7000 → constants header); stale comments flagged in phase-1 reviews (MainWindow.h notify block already fixed? verify — fix survivors only); unwired `nextTrack`/`prevTrack` slots (decide: wire to media keys if trivially correct, else delete the dead slots — investigate what the transport buttons actually call first and record it in the item); `SearchAggregator::cancel()` unwired (wire into search-level teardown per the plan-2 final review); `openLibraryItem` audio branch missing stable resumeKey (pass `item.id` as `setQueue`'s 4th arg); split-external status/toast asymmetry (assign per the policy table); probe_perf restart bound `<25` → `<30`; `perfbaseline.py` end-state assert (`themedCategory == "Games"` after the route, per its own docstring).
- [ ] **Step 4: Assemble + commit.** Inventory doc: intro, policy table, then items ranked (user-impact judgment: cuts the user hits every session rank above corner cases), each in the item format. Taste items `**Triage:** PENDING`, debris/feedback `OBJECTIVE`.

```bash
git add docs/superpowers/polish/2026-07-17-jank-inventory.md docs/superpowers/polish/evidence/
git commit -m "polish: ranked jank inventory (flow sweep + empty states + feedback policy + debris)"
```

---

### Task 3: TRIAGE GATE (controller-run — no subagent)

The controller presents the PENDING items to the user (grouped, with evidence images referenced by path so the user can open them), collects fix/skip per item, edits the inventory doc's `**Triage:**` fields to `FIX` or `SKIP (user)`, and commits:

```bash
git add docs/superpowers/polish/2026-07-17-jank-inventory.md
git commit -m "polish: user triage recorded in the jank inventory"
```

OBJECTIVE items proceed without user input. **No fix task may start before this commit exists.**

---

### Task 4: Objective consistency block

**Files:**
- Create: `native/src/ui/FeedbackPolicy.h` (the duration constants + a comment table mapping event class → channel; values verbatim from the inventory's policy table)
- Modify: every feedback call site the inventory assigned a policy change (exact list = the inventory's `feedback` + `debris` items; the inventory is the source of truth)
- Modify: `native/tools/probe_perf.cpp` (restart bound), `native/tools/perfbaseline.py` (end-state assert)

**Interfaces:**
- Consumes: the committed inventory (item list + policy table). Produces: `FeedbackPolicy.h` constants (`kFeedbackShort/Standard/Long/Sticky`, `int` milliseconds) that Task 5 fixes must also use for any feedback they touch.

- [ ] **Step 1:** Create `FeedbackPolicy.h` with the policy table's constants and the class→channel comment table. Replace every numeric duration at the call sites the inventory lists with the assigned constant; move sites the policy re-channels (status bar → Notifier or vice versa) accordingly. ONE COMMIT PER INVENTORY ITEM (`polish: J<NN> — <title>`), so each is revertable and reviewable in isolation.
- [ ] **Step 2:** The non-feedback debris items (unwired slots per the investigation recorded in the inventory, cancel() wiring, resumeKey, probe bound, runner assert) — one commit each, same message format.
- [ ] **Step 3:** After the block: full probe suite green; live smoke of three representative feedback events (an error toast, a progress notice, an ambient status) showing the new constants in effect — screenshots to `.superpowers/polish-audit/`.

---

### Task 5 (template, instantiated per triaged-FIX taste item): Fix J\<NN\>

**Binding rules:**
1. Scope = exactly the inventory item. The fade constant (if the fix is a transition) is `kUiFadeMs = 150` — add it to `FeedbackPolicy.h` on first use; every subsequent fade uses it.
2. Empty-state fixes use the ONE shared quiet-message row: first such fix creates it (a small helper — for QML surfaces an `EmptyNotice`-style element in `native/src/theme2/qml/elements/` styled from existing theme values; for widget surfaces a `browse::emptyNoticeItem(const QString& text)` MediaItem-row helper in `native/src/browse/SyntheticCatalogs.h` — pick per surface, record which in the commit); later fixes reuse it. No new visual language.
3. Before/after screenshot pair (downscaled ≤150 KB each) committed to `docs/superpowers/polish/evidence/J<NN>-before.png` / `-after.png`, referenced from the inventory item, whose `**Triage:**` flips to `FIXED (<commit>)`.
4. probe_nav + full suite green; live-verify the exact flow the item names.
5. Commit: `polish: J<NN> — <title>` (one per item).
6. If the fix turns out to require crossing the "jarring" boundary (layout/color/look changes), STOP and report — the controller re-triages with the user.

---

### Task 6: Close-out

**Files:**
- Modify: `docs/superpowers/polish/2026-07-17-jank-inventory.md` (every item FIXED/SKIP, none PENDING/FIX left)
- Modify: `docs/superpowers/specs/2026-07-17-polish-track-design.md` (Status → `Polish track complete: <F> fixed, <S> skipped by user; feedback policy landed. Phase 2 complete.`)

- [ ] **Step 1:** Audit the inventory: zero PENDING/FIX items remain; every FIXED names its commit; evidence pairs exist for visual fixes.
- [ ] **Step 2:** Full suite + a final flow-sweep re-run (`polishsweep.py`) — spot-check the fixed transitions/empty states show their new behavior; attach the delta observations to the inventory as `## After`.
- [ ] **Step 3:** Spec status update; commit `polish: close out the polish track — inventory complete, phase 2 done`.
