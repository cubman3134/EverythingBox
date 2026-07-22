# External Player Handoff Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stremio-style external-player handoff: a default-player setting + a one-off per-play action, on Windows (exe launch) and Android (ACTION_VIEW Intent).

**Architecture:** One `ExternalPlayer` core module (detection + launch, seam-testable), Settings accessors, a General-panel Choice row, and routing at the existing play entry points. Spec: `docs/superpowers/specs/2026-07-22-external-player-design.md`.

**Tech Stack:** Qt 6.8.3, QProcess (desktop), QJniObject Intent (Android), headless probes.

## Global Constraints

- Branch: `player/external-handoff` off main. Standing autonomy: merge gate proceeds on the controller's judgment.
- ANCHOR ON FUNCTION NAMES; lines drift.
- Settings keys exactly: `player/external` (`builtin|vlc|mpc|custom|android-intent`, default `builtin`), `player/externalPath`.
- Restricted profiles NEVER see or reach external handoff (`ProfileStore::current().restricted`).
- Fire-and-forget stated: no resume/sync/completion from external sessions; RecentStore::add still fires on the open path.
- Detection must be seam-testable (injectable probe roots — no probe touches the real registry or launches anything).
- Probe env recipe: PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` and `/c/mpv-dev`; `QT_QPA_PLATFORM=offscreen`; `QT_PLUGIN_PATH=C:\Qt\6.8.3\msvc2022_64\plugins`; `BUILD_DIR=C:/Users/cubma/Project Goliath/build`; runner + ci.yml registration for any new probe; FULL build before suite (linked-into-the-product lesson: the app target gets ExternalPlayer.cpp in the SAME task that creates it).
- Live: deploy Release over `C:\MyMediaVault-app\MyMediaVault.exe`; MMV_UITEST=1; uitest.py only; byte-identical ini restores; throwaway profiles only for restricted checks.
- Desktop suite + perf gates unchanged.

---

### Task 1: ExternalPlayer core + Settings + probe

**Files:**
- Create: `native/src/core/ExternalPlayer.h`, `native/src/core/ExternalPlayer.cpp`
- Modify: `native/src/core/Settings.h/.cpp` (accessor pattern), `native/CMakeLists.txt` (app target source + probe target), `native/tools/probe_extplayer.cpp` (create), `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces (Produces):**

```cpp
// native/src/core/ExternalPlayer.h
namespace ExternalPlayer {
enum class Kind { Builtin, Vlc, Mpc, Custom, AndroidIntent };
struct Detected { Kind kind; QString path; QString display; };
// Detection: probeRoot overrides for tests (empty = real system). Desktop-only meaning;
// on Android returns just the AndroidIntent pseudo-entry.
QVector<Detected> detect(const QString& fsProbeRoot = QString(),
                         const QString& regProbeRoot = QString());
Kind    configuredKind();               // Settings player/external -> Kind (unknown -> Builtin)
QString configuredPath();               // resolved exe path for the configured kind (Custom -> externalPath; Vlc/Mpc -> detect())
bool    available();                    // a usable external target exists (config + detection + platform)
// Launch: true on successful handoff start. Desktop: QProcess::startDetached.
// Android: ACTION_VIEW Intent; false if no activity (caller notifies).
bool    launch(const QString& urlOrPath);
}
// Settings additions: externalPlayer()/setExternalPlayer(QString), externalPlayerPath()/setExternalPlayerPath(QString)
```

- [ ] **Step 1 (probe RED):** probe_extplayer — QCoreApplication + CHECK; fake fs root with `VideoLAN/VLC/vlc.exe` + `MPC-HC/mpc-hc64.exe` under it → detect(fakeRoot, fakeReg) finds both with right kinds/paths; empty root → empty (no real-system leakage in probe mode); Settings round-trip incl. unknown string → Builtin; routing decision table as pure logic if implemented as a helper (defer to Task 2 if routing lives in MainWindow — then probe only detection+settings here). Sentinel EXTPLAYER-OK. Configure-fail = RED.
- [ ] **Step 2:** Implement (registry probe via QSettings NativeFormat on the real path ONLY when regProbeRoot empty; fs probe = QFileInfo checks under the root or the real Program Files env vars). Android branch `#ifdef Q_OS_ANDROID`: detect() returns the AndroidIntent entry; launch() = QJniObject Intent ACTION_VIEW with `video/*`, try/catch-style isValid checks, false on failure.
- [ ] **Step 3:** Build probe + mymediavault (SAME task — the store links into the app now), suite green, commit `feat: ExternalPlayer detection/launch core (extplayer T1)`.

---

### Task 2: Setting row + play routing + per-play actions

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (General panel builder — the Choice row beside its siblings; the play entry points: `openVideoPath`, `playStream`, `openLibraryItem`'s video branch — route through a single `routePlay(urlOrPath, ...)` helper that consults ExternalPlayer + restricted; detail action wiring for "Open in external player…"/"Play with built-in player"), `native/src/ui/MainWindow.h`

**Interfaces:**
- Consumes Task 1's names exactly. Produces: `MainWindow::routePlay` — ONE routing helper; every external handoff goes through it (RecentStore::add preserved on both routes; `notify(tr("No app can play this."))` on launch failure with built-in fallback).

- [ ] **Step 1:** The General row (Choice: options from detect() + Custom…; Custom flow = OSK path entry + optional native-dialog Action row; Android build shows Built-in/Ask-another-app). Setter-verbatim: Settings::setExternalPlayer + the path.
- [ ] **Step 2:** routePlay: default external && !restricted → ExternalPlayer::launch (fallback to built-in + notify on false); per-play actions in the detail flow (external action visible only when available() && !restricted; built-in alternative visible when default is external). VERIFY the video entry points funnel (the sync track's syncKey_ work mapped them: openVideoPath, playStream, openLibraryItem video branch — audio/readers stay built-in per spec).
- [ ] **Step 3:** Build + suite. Live (stub-exe technique per the spec if no real player installed): default→external routes (stub logs URL); one-off action; built-in alternative; restricted profile (throwaway) sees NO external anywhere; failure path notifies + falls back. Screenshots ep2-*. Ini byte-identical.
- [ ] **Step 4:** Commit `feat: external-player setting + per-play handoff routing (extplayer T2)`.

---

### Task 3: close-out — spec status, gates, final review, merge

- [ ] Spec Status → `Complete: desktop verified; Android Intent code in place (emulator Intent check rides the next Android session; TV pass still pending).` — the Android emulator check is OPTIONAL here: attempt it only if the emulator is still healthy (AVD mmv_phone exists from Phase 2; APK rebuild via CI is NOT required for a desktop-only merge — record the Android-side as code-complete/device-unverified in the spec, consistent with the TV-pass precedent).
- [ ] Full suite + perfbaseline (3 runs, the re-anchor note from the sync track applies — prefer an idle-machine run).
- [ ] Fable whole-branch review; fix rounds; merge+push+redeploy under standing autonomy.

## Self-Review (done at write time)

- Spec coverage: setting ✅T2; per-play ✅T2; restricted-hidden ✅T2 (+probe/live); launch desktop+Android ✅T1; limitations stated ✅ (routePlay keeps RecentStore); detection seam ✅T1. Non-goals untouched.
- Type consistency: ExternalPlayer::{Kind,Detected,detect,configuredKind,configuredPath,available,launch}, Settings::{externalPlayer,setExternalPlayer,externalPlayerPath,setExternalPlayerPath}, MainWindow::routePlay — used identically across tasks.
- Ambiguity resolved: Android emulator verification optional at close-out (code-complete recorded honestly); audio/readers stay built-in.
