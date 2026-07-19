# Themed Surfaces B2 Implementation Plan (Phase 3, Subsystem B, Plan 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every settings surface renders themed on the Nav Contract, reader exits land on the themed surface they came from, classic paths get deprecation logging, themed mode becomes the default — closing subsystem B: no classic-styled surface reachable in themed mode.

**Architecture:** One generic themed panel surface (`SettingsPanel.qml` + a C++ `PanelModel` of row descriptors + `buildPanelNavGraph`) renders every panel that is data-shaped — the classic `showPanel` builders become descriptor lists, one conversion per task. The genuinely interactive holdouts (controller capture, live download progress, native file dialogs) keep their specialized mechanics inside themed shells with documented exceptions. Spec: `docs/superpowers/specs/2026-07-19-themed-surfaces-design.md` (B2 phase).

**Tech Stack:** The established stack — NavGraph contract + NavThemeGraph builders, ThemedTextField/ThemedChoice (externalEdit + OSK), theme.json styling values, probe_navqml, uitest.

## Global Constraints

- Migration rule (spec): themed = shared graph builder + probe shape-test + two-state components for all input. QML registers leaf zones; graph shapes are C++ builders.
- B2 exit criterion (spec, binding): **no classic-styled surface reachable in themed mode at all**; settings fully themed; hardening list closed; `themedHome/enabled` defaults ON.
- Documented exceptions to "no classic surface": native `QFileDialog` (an OS surface, not app chrome — emulator-folder and ROMs-path pickers keep it); the uninstall `NavConfirm` card (already in-window + popup-free; restyle only if trivial).
- Preserve the scout-mapped landmines verbatim: Downloads' in-place progress updates + freed-pointer guard (`dlPanelOpen_` discipline); parental-PIN `Osk::getText` nesting; `retro_->stop()` before core-picker/input-mapping; `showDialogPanel`'s QUEUED finished-connect rationale.
- Classic mode remains functional until the final cleanup (post-B2, user-approved later): themed-mode routing goes to the new surfaces; classic-mode routing keeps the old panels. Deprecation logging (one `mwLog` line per classic entry) fires only when a classic surface is entered WHILE `themedHomeEnabled()` — that's the future-cleanup signal.
- Gates every task: probe_nav + probe_navqml + full suite; close-out adds latency (≤2 ms vs 16 ms), polishsweep, watchdog review.
- Machine facts as all tracks (build/PATH/offscreen/deploy-keep-name/MMV_UITEST/uitest-only/protect-data/rtk; ini byte-identical restores; Weekend Picks intact).
- Scout anchors (verified 2026-07-20): showPanel/showDialogPanel MainWindow.cpp:4778-4827; panelRow :4830; panel nav :1420-1481 (NavTextField two-state Nav.h:43-67); call-site table + control inventory in the scout report (.superpowers/sdd/ — the B2 Task-1 brief carries the table verbatim); Esc menu :890-912; startup picker :1605; themed→settings flow :764/:2716 (LEAVES the themed surface today); themedHome/enabled read :2200-2207 default false, writer :2943; deprecation candidates list; QFileDialog sites :1949/:5202.

---

### Task 1: ThemedPanel infrastructure + the Settings hub (proof)

**Files:**
- Create: `native/src/theme2/PanelModel.h` (descriptors), `native/src/theme2/ThemedPanelHost.{h,cpp}` (the page host + `PanelBridge`), `native/src/theme2/qml/elements/SettingsPanel.qml`
- Modify: `native/src/ui/nav/NavThemeGraph.h` (`buildPanelNavGraph(NavGraph*, int rowCount)` — one Vertical `"panelRows"` zone + a `"panelBack"` single-row zone, edges, containment per the established SELF-edge pattern), `native/tools/probe_navqml.cpp` (§18 shape-test), qrc, all six `theme.json` (a `settingsPanel` styling block: colors/accent fall back to theme values — styling only, panels aren't per-theme views)
- Modify: `native/src/ui/MainWindow.cpp` (themed-mode `openSettingsHub` routes to the themed panel; classic keeps `showPanel`)

**Interfaces (all later tasks consume verbatim):**

```cpp
// One row of a themed settings panel. The classic showPanel builders become lists of these.
struct PanelRow {
    enum Kind { Action, Toggle, Choice, TextField, Info, Progress, Separator, LogView };
    // LogView: scrollable read-only text (Debug log) — activate = scroll mode, Esc = back
    // (mirrors NavTextField's read-only two-state semantics). Rendered only when used (Task 3).
    Kind kind = Action;
    QString id;            // stable row id — activation dispatches on it
    QString label;         // left text
    QString value;         // right text (Info), current text (TextField), current option (Choice)
    QStringList options;   // Choice options
    bool checked = false;  // Toggle state
    int  progress = -1;    // 0..100 for Progress rows
    bool enabled = true;
    bool destructive = false; // styled with the warning accent (Uninstall)
};
class ThemedPanelHost : public QWidget {
    // showPanel-themed analogue: present(title, rows, onActivate, onBack).
    // onActivate(rowId, newValue) — Toggle flips deliver "0"/"1"; Choice delivers the picked option;
    // TextField delivers committed text (via ThemedTextField externalEdit -> host runs Osk); Action delivers "".
    void present(const QString& title, const QVector<PanelRow>& rows,
                 std::function<void(const QString& rowId, const QString& newValue)> onActivate,
                 std::function<void()> onBack);
    void updateRow(const QString& rowId, const PanelRow& row); // in-place (Progress/Info live updates)
};
```

- Rendering: `SettingsPanel.qml` = title bar + a ListView of row delegates by Kind (Action=row button; Toggle=row+check glyph; Choice=ThemedChoice in externalEdit-style cycle; TextField=ThemedTextField externalEdit — host answers `editRequested` with `Osk::getText`; Info=static; Progress=bar; Separator=section header). Styling from the `settingsPanel` theme block with hard fallbacks.
- The panel is a LEVEL on the back router (`pushLevel("panel:<title>")`); nested panels stack levels; Back pops one panel exactly like classic's per-panel onBack chain.
- Convert `openSettingsHub` themed-mode: the ~14 hub rows become `PanelRow{Action,...}` descriptors; activation dispatches to the same `open*` methods (which themselves route themed/classic per mode).

- [ ] Steps: probe §18 RED (builder absent) → infrastructure → GREEN → hub conversion → live: themed home ▸ Settings opens the THEMED hub (no classic panel flash), every row reachable, Back pops to home, drill into one child (it may still be classic this task — that's fine, logged) → suite → commit `themed: ThemedPanel infrastructure + settings hub on the contract`.

---

### Task 2: General settings (the form beast)

**Files:** `native/src/ui/MainWindow.cpp` (`openGeneralSettings` themed branch: the 8 sections become descriptor rows), `ThemedPanelHost` if gaps appear (report them — don't fork the API silently).

Mechanics: checkboxes → Toggle rows writing the same Settings keys; subtitle-language combo → Choice; OpenSubtitles/Trakt credentials + ROMs path → TextField rows (OSK via externalEdit; ROMs "Change…" row stays an Action invoking the native QFileDialog — documented exception); parental PIN flows keep `Osk::getText` nesting (the OSK is already a contract level). Section headers → Separator rows. Every row's write path calls the EXACT classic setter (same Settings keys — grep each classic checkbox/edit handler and reuse).

- [ ] Steps: convert → live matrix: toggle two settings and verify persistence (ini) + effect (e.g. autoplay-next flag readable), edit one credential via OSK round-trip, parental PIN gate drill (set PIN → lock → reopen Settings → PIN prompt), ROMs Change opens the native dialog → suite → commit `themed: General settings on the contract`.

---

### Task 3: The simple + form panels (Cloud, OAuth, RetroAchievements, BIOS, Stream prompt, Debug)

**Files:** `native/src/ui/MainWindow.cpp` (six themed branches → descriptor lists).

Per scout: Cloud Sync = 4 state-gated Action rows (visibility → `enabled`/omission); Cloud Client Setup = 2 TextFields + Save; RA = login form (user/pass TextFields — password masking: ThemedTextField gains a `mask: bool` rendering dots, OSK unchanged) or signed-in state (apikey TextField + actions); BIOS Check = Info/RichText report (Info rows per system + tick glyphs; the Download/Repair + Open Folder Actions) — rich text degrades to plain per-system rows; Stream prompt = TextField + Play Action (feeds `openStreamUrl` exactly); Debug = the log as a scrollable read-only zone (reuse the two-state scroll semantics: activate = scroll mode, Esc = back — mirror NavTextField's read-only behavior in the panel's log row Kind: add `LogView` Kind rendering a scrollable text area whose activation enters scroll mode) + Refresh/Clear/Open-location Actions + the UI-test Toggle.

- [ ] Steps: one commit per panel (six commits, `themed: <panel> on the contract`); live per panel: drive every control kind once (RA login form entry via OSK, BIOS report renders with real system data, stream prompt round-trips a lavfi URL to playback, Debug log scrolls + refresh works, cloud rows reflect signed-out state honestly) → suite green at each → done.

---

### Task 4: Downloads (live progress) + Emulator Manager

**Files:** `native/src/ui/MainWindow.cpp` (both panels), `ThemedPanelHost::updateRow` exercised for real.

Downloads: each job = a Progress row + per-job Action rows (Pause/Resume/Retry/Cancel/Remove as a NavMenu on activate — the established overlay pattern — rather than five rows per job); `updateDownloadRow` drives `updateRow(jobId,...)` in place — PRESERVE the freed-pointer discipline: the themed panel's equivalent of `dlPanelOpen_` (host tracks whether the downloads panel is the presented one; stray ticks no-op). Emulator Manager: folder Info row + Change Action (native dialog — exception), fullscreen Toggle, per-emulator Separator + status Info + Download/Update Action + Launch Action; download progress → the emulator's status row via `updateRow`.

- [ ] Steps: convert both → live: start a real small download if the backend offers one (else seed via the debug path or drive a paused/failed job's actions; be honest); emulator re-download of an installed one shows in-place progress then completes; Launch opens the emulator (and Esc-hotkey returns — regression) → suite → two commits.

---

### Task 5: The dialogs + Esc-menu styling (Profiles, core picker, input mapping)

**Files:** `native/src/ui/MainWindow.cpp`, `native/src/ui/ProfileDialog.*` (data extraction), `native/src/ui/SettingsDialog.*` (core-picker data), `native/src/ui/ControllerRemapDialog.*` (themed shell), `native/src/ui/nav/NavOverlay.cpp` (NavMenu themed styling hooks)

- Profiles (startup picker + switcher): profile list → panel rows (Action per profile + New/Edit flows; the name/icon second page → a nested panel level with a TextField + icon Choice). `mustChoose` startup variant = same panel, no Back (rootBack → quit confirm).
- Core picker (SettingsDialog): systems → Choice rows (core per system); the per-core options editor page → nested panel of that core's options (Choice/Toggle rows from the same data SettingsDialog reads). `retro_->stop()` discipline preserved.
- Input mapping (ControllerRemapDialog): themed SHELL — player/scope/turbo combos → Choice rows; each binding → an Action row showing the current binding; activation enters CAPTURE state (the existing capture machinery runs — its keyPressEvent/pad-poll IS the editing state of the two-state model; the row shows "press a key/button…" while capturing; Esc cancels capture per the two-state contract). The capture internals are NOT rewritten — the dialog's capture logic is extracted to be drivable from the themed shell (report honestly if extraction resists; a hosted-classic fallback for capture-only WITH deprecation logging is acceptable if the shell attempt proves high-risk — document which shipped).
- Esc menu: NavMenu gains themed styling (colors/accent from the active theme's values instead of hardcoded) — restyle only, mechanics untouched.

- [ ] Steps: one commit per surface (4 commits); live: switch profile themed end-to-end, startup picker (relaunch with 2 profiles), change a core + one core option, remap one pad button via capture + verify it works in a game, Esc menu shows themed colors → suite each → done.

---

### Task 6: Reader-exit origin restore + deprecation logging + default ON + hardening

**Files:** `native/src/ui/MainWindow.cpp`, `native/src/ui/nav/NavGraph.cpp`, `native/tools/probe_navqml.cpp`, `native/src/theme2/qml/elements/ThemedChoice.qml`, `native/tools/run-headless-probes.sh` (grep gate), `.github/workflows/ci.yml` if a new probe target appears

1. **Reader-exit → origin surface** (the B1 adjudication): `returnFromReader` in themed mode restores the themed surface the reader was opened FROM (detail if launched from detail — the audio page's view-pop is the pattern; browse otherwise). Drive the book-from-detail Play → reader → Back chain LIVE (never walked; the final review demanded it).
2. **Deprecation logging**: one `mwLog("deprecated-classic: <surface>")` at each classic entry reached while `themedHomeEnabled()` (the scout's §6 list: classic HomeView fallback, classic readers/audio paths, any panel that still routes classic).
3. **Default ON**: `themedHome/enabled` default `false` → `true` (:2203); verify first-run-shaped behavior (no stored key → themed home with the Default theme; the no-themes-on-disk fallback still works and logs).
4. **Hardening list** (A-seeded, verbatim): NavGraph `activate()` refuses hidden zones (one line + probe assert); watchdog `tick()` probe coverage (injected sampler/skip lambdas: consecutive-counter + skip-resets-run asserts); ThemedChoice empty-options guard (`beginEdit` no-op on `options.length===0` + probe); QML no-direct-selection-writes grep as a runner step (grep ThemeView.qml for `currentIndex =` etc. outside the bridge — FAIL the suite on a hit); the ThemeView-level QML probe pinning the XMB-buttons guard + grid-home rootBack (the §14 offscreen-QQuickWidget pattern proved feasible).
5. **Minor cleanup** (recorded list): comic spread label range; pdf/comic top-strip inset via `chromeTopReserve` parity; Lumen element ids; Image.qml font fallback; the dead reader back-branch comment; nowplayingAudio-missing fallback logging.

- [ ] Steps: one commit per numbered group (5 commits); the reader-origin change re-drives the full reader Back matrix (book from detail, book from browse, pdf/comic from browse, audio from detail — all land on their origin surface) → suite → done.

---

### Task 7: B2 close-out — the "no classic reachable" walk

- [ ] THE walk: in themed mode, drive EVERY surface the app has (home, browse, search, playlists, detail, all four readers, video player entry/exit, game launch/exit, every settings panel incl. dialogs, Esc menu, profile switch, startup picker) — zero classic-styled surfaces encountered; screenshot per surface; any classic sighting = a finding, fix before proceeding (except the two documented exceptions: native file dialogs, NavConfirm card).
- [ ] Deprecation-log audit: after the walk, grep the debug log — zero `deprecated-classic` lines during the themed walk (classic-mode spot-check DOES produce them when themed is manually disabled).
- [ ] Gates: full suite; perfbaseline 3 runs (≤2 ms vs 16 ms, table); polishsweep; watchdog review (zero unexplained). Startup re-check: `startup.total`/`firstpaint` medians vs the perf-track values (the themed default must not regress startup — the themed home was already the measured path, expect ~flat; investigate if >20% regression).
- [ ] Spec Status → `Subsystem B complete: B1 + B2 — themed everywhere, classic paths deprecation-logged, themed default ON. Subsystem D (TV+mobile) next.` Follow-ups recorded. Commit `themed: close out B2 — no classic surface reachable in themed mode`.
