# Player Sync Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Audio/subtitle sync adjustment (per-file + global default) in the player's tracks card, plus explicit verification of the already-existing 200% volume boost and track selection.

**Architecture:** `MpvWidget` gains the audio-delay property pair (mirroring sub-delay); a QtCore-only `SyncOffsets` store resolves per-file-else-global offsets keyed by the SAME resume key each play path already derives; the existing `showSubtitleMenu` overlay card grows AUDIO SYNC / SUBTITLE SYNC sections. Spec: `docs/superpowers/specs/2026-07-21-player-sync-controls-design.md`.

**Tech Stack:** Qt 6.8.3 MSVC2022, libmpv (`audio-delay`/`sub-delay` properties), headless probes.

## Global Constraints

- Branch: `player/sync-controls` off main. Commit per task; push only at the merge gate.
- ANCHOR ON FUNCTION NAMES; lines drift.
- Step size ±0.05 s per press; offsets clamped to ±10.0 s; mpv sign convention (positive `audio-delay` = audio later; positive `sub-delay` = subs later) surfaced in the readout as "+N ms".
- Resume-key discipline: reuse the EXACT `rkey` each play path computes (`playStream`: `resumeKey.isEmpty() ? url : resumeKey`, MainWindow.cpp ~:2694; local video path: find the equivalent beside its resume-position save). Empty/no key → global-only, no per-file write.
- Settings keys exactly: `sync/global/audio`, `sync/global/sub` (double, default 0.0); per-file `sync/files/<key>/audio`, `sync/files/<key>/sub`.
- Probe env recipe: PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` and `/c/mpv-dev`; `QT_QPA_PLATFORM=offscreen`; `QT_PLUGIN_PATH=C:\Qt\6.8.3\msvc2022_64\plugins`; `BUILD_DIR=C:/Users/cubma/Project Goliath/build`; runner `native/tools/run-headless-probes.sh`; new probe registered there AND in ci.yml's build-target list.
- Live verification: deploy Release over `C:\MyMediaVault-app\MyMediaVault.exe` (keep name); `MMV_UITEST=1`; uitest.py only; byte-identical ini restores (snapshot first — sync writes go to the real ini during verification, restore after); Weekend Picks untouched.
- Desktop suite + perf gates unchanged (startup ±20%, nav.select flat).

---

### Task 1: audio-delay plumbing + SyncOffsets store + probe

**Files:**
- Modify: `native/src/video/MpvWidget.h` (beside `subDelay`/`setSubDelay` decls ~:44), `native/src/video/MpvWidget.cpp` (beside their defs ~:481-487)
- Create: `native/src/core/SyncOffsets.h`, `native/src/core/SyncOffsets.cpp`
- Create: `native/tools/probe_sync.cpp`; Modify: `native/CMakeLists.txt` (probe target: sources `tools/probe_sync.cpp src/core/SyncOffsets.cpp src/core/Settings.cpp`, link `Qt6::Core`), `native/tools/run-headless-probes.sh` (add `"probe_sync SYNC-OK"`), `.github/workflows/ci.yml` (add `probe_sync` to the build-target list)

**Interfaces:**
- Produces:

```cpp
// MpvWidget additions — byte-parallel to the existing sub-delay pair:
double audioDelay() const;                 // mpv "audio-delay", 0.0 when no mpv
void   setAudioDelay(double seconds);

// native/src/core/SyncOffsets.h  (QtCore only; Settings-backed like FormFactor)
namespace SyncOffsets {
enum class Which { Audio, Sub };
struct Pair { double audio = 0.0; double sub = 0.0; };
Pair   resolve(const QString& key);                    // per-file if present else global; key empty => globals
void   savePerFile(const QString& key, Which w, double secs);   // clamped ±10.0; no-op on empty key
void   clearPerFile(const QString& key, Which w);      // removes the per-file entry
double globalDefault(Which w);                         // default 0.0; corrupt => 0.0
void   setGlobalDefault(Which w, double secs);         // clamped ±10.0
}
```

- [ ] **Step 1 (probe RED):** `probe_sync.cpp` — QCoreApplication + the CHECK macro shape from `probe_formfactor.cpp`; point the Settings store at the build-tree ini (same posture probe_formfactor uses) and reset the `sync/*` keys at start. Asserts: globals default 0.0; `setGlobalDefault(Audio, 0.08)` → `resolve("k1")` = {0.08, 0.0} (global applies); `savePerFile("k1", Audio, -0.2)` → resolve("k1").audio == -0.2 (per-file beats global) while resolve("k2").audio == 0.08; `clearPerFile("k1", Audio)` → back to 0.08; empty-key resolve == globals and empty-key savePerFile writes nothing (globals unchanged, no `sync/files//` junk key — assert the Settings child-group absence); clamp: savePerFile 99.0 → resolve == 10.0, setGlobalDefault(-99) → -10.0. Print `SYNC-OK`. Build target → run → expect compile FAIL (SyncOffsets absent).
- [ ] **Step 2:** Implement `SyncOffsets` per the interface (thin `Settings`-store wrappers; `store().beginGroup`/`remove` for clearPerFile; clamp helper `qBound(-10.0, secs, 10.0)`). Implement the `MpvWidget` pair copying the sub-delay bodies with `"audio-delay"`.
- [ ] **Step 3:** Reconfigure + build `probe_sync` → run → `SYNC-OK`; full runner green.
- [ ] **Step 4: Commit** `feat: audio-delay plumbing + SyncOffsets store (sync T1)`.

---

### Task 2: apply-on-play + card sync sections

**Files:**
- Modify: `native/src/ui/MainWindow.cpp`: the play paths (`playStream` ~:2680 and the local `openVideoPath` chain — wherever the resume position is applied after load is the anchor for applying offsets), `showSubtitleMenu` (~:5392, the card builder — new sections after SUBTITLES), the member area (current playback sync key `syncKey_`)
- Modify: `native/src/ui/MainWindow.h` (member + any helper decl)

**Interfaces:**
- Consumes: Task 1's `SyncOffsets` + `MpvWidget::{audioDelay,setAudioDelay,subDelay,setSubDelay}`; `FormFactor::hitClamp` (widget sizing); the card's existing `rowButton`/`sectionLabel`/`subLeftCol_` mechanics.

- [ ] **Step 1: apply-on-play.** Where each play path lands after load (beside the resume-position seek): set `syncKey_` to the path's rkey (empty when none), then `const auto off = SyncOffsets::resolve(syncKey_); player_->setAudioDelay(off.audio); player_->setSubDelay(off.sub);`. Cover BOTH the stream and local-file video paths (grep every `player_->` play entry; audio-only nowplaying uses the same MpvWidget — apply there too, sub offset harmless). Clear `syncKey_` when playback stops.
- [ ] **Step 2: card sections.** In `showSubtitleMenu` after the SUBTITLES section: AUDIO SYNC — readout row (disabled rowButton showing `formatMs(player_->audioDelay())`, e.g. "+150 ms"/"0 ms"), `− 50 ms` and `+ 50 ms` rows (`setAutoRepeat(true)`), Reset, Save as default; SUBTITLE SYNC — same four via a shared local lambda parameterized on Which/getter/setter (DRY — ONE lambda builds both sections). Step handler: `double v = qBound(-10.0, cur + delta, 10.0); player_->setAudioDelay(v); SyncOffsets::savePerFile(syncKey_, Which::Audio, v); readout->setText(formatMs(player_->audioDelay()));` (re-read after set so the readout shows mpv truth). Reset: `SyncOffsets::clearPerFile(syncKey_, w); apply global; update readout`. Save as default: `SyncOffsets::setGlobalDefault(w, current)`. All buttons through `FormFactor::hitClamp` minimum sizes. Keep the card's focus-chain registration (`subLeftCol_ <<`) for every new row so controller navigation works; the card must still fit on screen at TV scale — if it overflows, make the card scrollable or two-column (judgment; note the choice).
- [ ] **Step 3: build + suite green.** Full runner (no new probe legs — the store is probe-covered; the card is widget-side, live-verified next).
- [ ] **Step 4: live verification** (deploy; snapshot ini): play a library video → open the card → step audio sync +3 presses → readout "+150 ms" and `uitest state`/log confirms; step sub sync −2 → "−100 ms"; close+reopen the card → readouts persist; STOP and REPLAY the same file → offsets re-applied (readouts confirm on open); play a DIFFERENT file → readouts show the global (0 unless defaulted); Save as default on audio → different file now opens at that offset; Reset → back to global. Volume boost: drag/keys the slider to 200 → `state` (or a log line) confirms property 200. Tracks: on a multi-track file (find one in the library; if none, generate one with ffmpeg into the scratchpad and open via uitest `open`) — card lists tracks, switching audio/sub works (screenshot). Screenshots `pst2-*`. Restore ini byte-identical.
- [ ] **Step 5: Commit** `feat: audio/sub sync controls in the player card — per-file + global default (sync T2)`.

---

### Task 3: close-out — spec status, final review, merge gate

- [ ] **Step 1:** Spec Status → `Complete: sync controls live (per-file + global), boost + track selection verified as features.` Note in the spec's existing-features section the verification evidence date. Commit `player: close out sync controls`.
- [ ] **Step 2:** Full suite + perfbaseline 3 runs (medians within gates). Fable whole-branch review (main..player/sync-controls): sign-convention consistency (readout vs mpv), syncKey_ lifetime (stale key after stop→new-play races), card overflow at TV scale, store hygiene (no junk keys), the DRY lambda quality. Fix rounds to Merge-ready.
- [ ] **Step 3:** Merge gate per the standing pattern (merge + push + redeploy — the desktop exe changes this time).

## Self-Review (done at write time)

- Spec coverage: audio-delay pair → T1; SyncOffsets semantics → T1 (probe) ; apply-on-play + card UI → T2; boost/tracks verify-as-features → T2 Step 4; error handling (silent no-op sets + re-read, clamp, corrupt→0.0) → T1 probe + T2 readout re-read; non-goals untouched.
- Placeholders: none; all code steps carry code or exact anchors.
- Type consistency: `SyncOffsets::Which/Pair/resolve/savePerFile/clearPerFile/globalDefault/setGlobalDefault` and `audioDelay/setAudioDelay` used identically across tasks; `syncKey_` defined T2 Step 1, used T2 Step 2.
