# Form-Factor Phase 2 (Android / Android TV provisioning) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the themed, form-factor-adaptive app as an Android/Android TV APK — built by CI, verified on the user's Android TV (arm64, adb sideload) and a local emulator (x86_64), TV-first.

**Architecture:** CI-first (this PC has NO Android toolchain; GitHub Actions self-provisions Qt/NDK/libmpv — local additions are adb + later the emulator stack). QML comes ON for Android (the desktop themed UI IS the Android UI; the software Quick backend already in use sidesteps the classic Android GL issues). The unstarted core work is the bootstrap pipeline: a fresh Android install has an empty dataDir with no themes/addons. Spec: `docs/superpowers/specs/2026-07-21-formfactor-adaptivity-design.md` (Phase 2 section). Scout evidence: task reports dir.

**Tech Stack:** Qt 6.8.3 (host linux + android_arm64_v8a/android_x86_64 kits in CI), NDK r26b, jdtech libmpv AAR, androiddeployqt, adb, Android Studio emulator (local).

## Global Constraints

- Branch: `formfactor/d2-plan` off main. The CI tasks require PUSHING this branch to origin (github.com/cubman3134/MyMediaVault — private repo) to run workflow_dispatch builds — sanctioned for this plan; main still only moves at the merge gate.
- ANCHOR ON FUNCTION NAMES; scouted line numbers (main@c8d352c) drift.
- Desktop identity: every change must be a no-op for the desktop build (guards on Q_OS_ANDROID / ANDROID in CMake); full desktop suite green after every task.
- User-permission gates AT EXECUTION TIME (do not skip): downloading platform-tools (Task 5) and the SDK/emulator stack + license acceptance (Task 6) were pre-authorized by the user's plan choices, but the executing agent still STATES exactly what it downloads (name, source, size) before doing it, and the controller relays anything interactive (TV developer-mode enablement is a USER action).
- User data: TV/emulator installs use the app's own empty Android data dir (that's the bootstrap test); nothing on this PC's deployed app or ini is touched by Phase 2 tasks except standard desktop-suite runs.
- Probe env recipe (desktop suite): PATH prepend /c/Qt/6.8.3/msvc2022_64/bin and /c/mpv-dev, QT_QPA_PLATFORM=offscreen, QT_PLUGIN_PATH=C:\Qt\6.8.3\msvc2022_64\plugins, BUILD_DIR=C:/Users/cubma/Project Goliath/build; runner native/tools/run-headless-probes.sh; new probes registered in the runner AND built in ci.yml.
- Follow-ups already out of scope (record, don't build): SDL2 controller on Android (SDLActivity/JNI — remote D-pad + virtual pad cover v1), SAF folder picking, Play Store packaging, external emulators (gated), CloudSync desktop-ini path-rewrite hazard.

---

### Task 1: QML-on-Android + retire the no-QML break (desktop-verifiable)

**Files:**
- Modify: `native/CMakeLists.txt` (~:63-71 the `if(NOT ANDROID)` find_package gate), `native/src/ui/MainWindow.h` (~:328-338 host member types), `native/src/ui/MainWindow.cpp` (leak sites ~:446, ~:1153, ~:6222), `.github/workflows/release.yml` (host Qt modules ~:292, android Qt modules ~:304)

**Interfaces:**
- Produces: `MMV_HAVE_QML` resolvable on Android (find_package runs for ANDROID too; requires qtdeclarative in the kit — CI change rides along); a no-QML desktop configure LINKS (the pre-existing break retired).

- [ ] **Step 1:** Fix the 3 no-QML leak sites the cheap way: change `readerHost_`/`pdfHost_`/`comicHost_`/`themedPanelHost_` member declarations to their concrete types ONLY under `#ifdef MMV_HAVE_QML` is the wrong shape (members must exist unconditionally) — instead keep the pointer members as-is and fix the comparison sites: `:446-447` and `:6221-6222` compare against incomplete types — cast the MEMBER to QWidget* is impossible without the type; so either (a) declare those members as `QWidget*` and `static_cast` at the guarded use sites (kills both comparison leaks with zero new #ifdef), or (b) wrap the 3 sites in `#ifdef MMV_HAVE_QML`. Prefer (a) for the two host-pointer comparisons IF the guarded code doesn't depend on the concrete type pervasively (read first); else (b). Guard the `ThemeEngine::navGraph` call at ~:1153 (its guarded twin at ~:1029 is the template).
- [ ] **Step 2:** Lift the CMake gate: `find_package(Qt6 QUIET OPTIONAL_COMPONENTS Quick Qml QuickWidgets)` runs unconditionally (remove the `if(NOT ANDROID)` wrapper at ~:63; keep the QUIET/OPTIONAL semantics — kits without qtdeclarative still degrade gracefully with the existing STATUS message).
- [ ] **Step 3:** release.yml: add `qtdeclarative` to the host Qt install modules (~:292) and the android Qt install modules (~:304).
- [ ] **Step 4:** Verify: full desktop Release build + suite green; then the retired-break proof: `cmake -S native -B build-noqml -G "Visual Studio 18 2026" -A x64 -DMYMEDIAVAULT_BUILD_APP=ON -DCMAKE_DISABLE_FIND_PACKAGE_Qt6Quick=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib` then build `mymediavault` to a successful LINK. Delete build-noqml after.
- [ ] **Step 5:** Commit `feat: QML on Android + no-QML desktop build links again (D2 Task 1)`.

---

### Task 2: Bootstrap pipeline — assets into the APK, first-run extraction (desktop-verifiable; parallel-eligible with Task 3)

**Files:**
- Create: `native/src/core/AssetBootstrap.h`, `native/src/core/AssetBootstrap.cpp`
- Modify: `native/CMakeLists.txt` (Android packaging block ~:420: stage themes2/ + addons into `QT_ANDROID_PACKAGE_SOURCE_DIR`'s assets/), `native/src/main.cpp` (call bootstrap before addon/theme load), `native/android/README.md` (reality update)
- Create: `native/tools/probe_bootstrap.cpp` (+ CMake target, runner + ci.yml registration)

**Interfaces:**
- Produces: `AssetBootstrap::run(const QString& sourceRoot, const QString& dataDir, const QString& appVersion)` — pure function, testable with any source dir; `sourceRoot` is `"assets:/mmv"` on Android, a temp dir in the probe.

- [ ] **Step 1 (probe RED):** probe_bootstrap: temp source with `themes2/T/theme.json` + `addons/a/manifest.json`; asserts: fresh dataDir → both copied + version stamp written; re-run same version → no-op (mtime unchanged); bumped version → themes2 stock REFRESHED (overwritten), addons left alone if present (copy-if-absent — user-configured addons never clobbered); user-added theme dir untouched by refresh; missing sourceRoot → clean no-op. Sentinel BOOTSTRAP-OK.
- [ ] **Step 2:** Implement AssetBootstrap per those semantics (recursive copy via QDirIterator; stamp file `dataDir/.assets-version`). `main.cpp`: on `Q_OS_ANDROID` (and under an `MMV_TEST_BOOTSTRAP_SRC` env override for desktop testing) call it before AddonManager/ThemeEngine initialize.
- [ ] **Step 3:** CMake: in the ANDROID block, `file(COPY)` `${CMAKE_CURRENT_SOURCE_DIR}/themes2` and the shippable addons (at minimum `addons/aiocatalog` — enumerate what native/addons contains and ship all first-party ones) into `${QT_ANDROID_PACKAGE_SOURCE_DIR}/assets/mmv/` at configure time (or a custom target — pick the shape androiddeployqt picks up; document).
- [ ] **Step 4:** Suite green (incl. new probe, RED-demoed); desktop no-op proof (no bootstrap call without the env override); commit `feat: Android asset bootstrap — first-run themes/addons extraction (D2 Task 2)`.

---

### Task 3: Lifecycle, Android Back, immersive, TV banner (parallel-eligible with Task 2)

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (+.h): `onApplicationStateChanged(Qt::ApplicationState)` + connect in ctor; Android Back key routing; `native/src/main.cpp` (immersive/fullscreen on Android); `native/android/res/drawable/banner.png` (real 320×180) + verify `icon.png`; `native/android/AndroidManifest.xml` (only if the immersive theme needs a flag — otherwise untouched)

**Interfaces:**
- Consumes: `RetroView::setPaused(bool)`, `MpvWidget::setPaused(bool)`/`pause state`, `MainWindow::goBack()`.

- [ ] **Step 1:** Lifecycle handler: on `Qt::ApplicationSuspended`/`Inactive` (Android backgrounding) → remember-and-pause: if a core is running `retro_->setPaused(true)`; if video playing `player_->setPaused(true)`; mark what was paused-by-lifecycle. On `Qt::ApplicationActive` → resume ONLY what lifecycle paused (never resume something the user paused). Desktop no-op: gate the connect on `Q_OS_ANDROID` (but implement the handler unguarded so a probe can call it directly).
- [ ] **Step 2:** Android Back: Qt delivers the hardware/gesture Back as `Qt::Key_Back` — route it exactly like the existing Back arbitration (grep how goBack is reached from keys; add Key_Back beside it, guarded semantics identical; at root it must reach the exit-confirm flow, never instant-kill).
- [ ] **Step 3:** Immersive: on Android the window already runs fullscreen via Qt; add what's minimally needed for sticky-immersive over video/RetroView (investigate: Qt 6.8 handles it via `QWindow::showFullScreen` + the manifest's theme; if a manifest theme attr is needed, add it; do NOT hand-roll JNI unless unavoidable — document what you find).
- [ ] **Step 4:** Banner: generate a real 320×180 PNG from the app's existing icon/branding (simple: icon centered on a dark gradient, app name text) — commit the PNG; verify the manifest references it.
- [ ] **Step 5:** Probe leg (extend probe_formfactor or probe_bootstrap): call the lifecycle handler directly — running-core state → Suspended pauses, Active resumes, user-paused stays paused. Suite green; commit `feat: Android lifecycle pause/resume + Back routing + TV banner (D2 Task 3)`.

---

### Task 4: CI android job green — arm64 + x86_64 APK artifacts

**Files:**
- Modify: `.github/workflows/release.yml` (android job: `workflow_dispatch` trigger, ABI matrix, per-ABI artifact names, x86_64 libmpv sourcing)

- [ ] **Step 1:** Add `workflow_dispatch:` to release.yml's triggers (so the branch can build on demand without a tag).
- [ ] **Step 2:** Matrix the android job over `android_arm64_v8a` and `android_x86_64`: NDK ABI, Qt kit arch, the libmpv AAR extraction path (`jni/arm64-v8a` → `jni/${{ matrix.abi }}`) — FIRST verify the jdtech AAR actually ships x86_64 .so's (inspect the AAR listing in-task); if it doesn't, find the maintained alternative (jellyfin's libmpv AAR variants) or make x86_64 a UI-only build with a documented stub decision — do NOT silently ship a broken variant. Per-ABI artifact names.
- [ ] **Step 3:** Push `formfactor/d2-plan` to origin (sanctioned by this plan) and dispatch the workflow. Iterate on failures — each iteration is commit + push + dispatch; keep fixes minimal and logged in the report. Expected first-run problems (from the port doc): QT_HOST_PATH wiring, androiddeployqt pathing, qtdeclarative module name.
- [ ] **Step 4:** Success = both APK artifacts downloadable from the run. Record the run URL + artifact sizes. Commit any final workflow fixes.

---

### Task 5: TV-device verification (arm64) — needs the user

**Files:** none (verification task; report only)

- [ ] **Step 1:** Local adb: state the download to the user (platform-tools-latest-windows.zip, dl.google.com, ~12MB), get their OK in chat, unzip to the scratchpad or a tools dir (NOT the repo).
- [ ] **Step 2:** USER ACTIONS (relay via the controller, wait): enable Developer Mode + network/wireless debugging on the Android TV; provide the TV's IP (+ pairing code if Android 11+ pairing flow).
- [ ] **Step 3:** `adb connect <ip>` → `adb install mymediavault-arm64.apk` (the CI artifact). Launch from the TV launcher (leanback row — the banner from Task 3).
- [ ] **Step 4:** Verify (user drives the remote OR adb shell input keyevents where hands-off): first-run bootstrap seeds themes/addons (app lands on the themed home, NOT a blank screen — the Task 2 pipeline's proof); FormFactor auto-resolves TV (leanback) — Display mode reads TV; remote D-pad full walk (home → detail → settings panels → OSK); a movie via libmpv; an NES core via the remote D-pad (keyboard-mapped); Back button behavior (pops levels, exit-confirm at root); lifecycle: Home-button background + resume mid-game (pauses/resumes). Collect `adb logcat` for the report; screenshots via `adb exec-out screencap`.
- [ ] **Step 5:** File findings; small fixes → commit + re-dispatch CI + reinstall; structural findings → record for a follow-up round.

---

### Task 6: Emulator verification (x86_64) — parallel-eligible with Task 5 once Task 4 delivers artifacts

**Files:** none (setup + verification; report only)

- [ ] **Step 1:** State the downloads to the user before starting (cmdline-tools ~150MB + platform-tools + emulator + system-images;android-34;google_apis;x86_64 ~1.5GB + a TV image if available — total ~10GB with the SDK dirs; dl.google.com) and that `sdkmanager --licenses` accepts Google's SDK license terms; get the OK in chat (the plan choice pre-authorized intent; confirm specifics now).
- [ ] **Step 2:** Install to a dedicated dir (e.g. C:\Android\sdk — NOT the repo); accept licenses; create two AVDs: phone (Pixel-class, android-34 x86_64) and TV (1080p TV profile if the TV system image exists for x86_64; else phone-AVD-with-TV-resolution and note it).
- [ ] **Step 3:** Install the x86_64 APK; phone AVD: touch walk (one-tap nav, edge-back, flick, OSK taps), video touch chrome, reader gestures, the virtual pad on an NES core (FormFactor auto → Mobile via touchscreen), rotation sanity. TV AVD: D-pad walk, leanback launcher entry.
- [ ] **Step 4:** File findings same disposition as Task 5.

---

### Task 7: Close-out — spec status, follow-ups, final review, merge gate

- [ ] **Step 1:** Spec Status → `Phase 2 complete: Android/Android TV APKs built by CI, verified on TV hardware (arm64) + emulator (x86_64). Subsystem D complete.` Follow-ups recorded: SDL2 controller (SDLActivity), SAF folder picking, Play packaging, immersive polish findings, CloudSync path-rewrite hazard, anything Tasks 5-6 filed structural. Commit `formfactor: close out Phase 2 — Android provisioning verified`.
- [ ] **Step 2:** Full desktop suite + perfbaseline (desktop untouched proof). Fable whole-branch review (main..formfactor/d2-plan): desktop-no-op discipline, bootstrap overwrite semantics (user-data safety), lifecycle resume-only-what-we-paused, CI matrix hygiene. Fix rounds to Merge-ready.
- [ ] **Step 3:** Merge gate: present to the user (merge+push per standing pattern; deploy is desktop-unchanged so redeploy only if the suite rebuilt the exe).

## Self-Review (done at write time)

- Spec coverage: QML-into-Android ✅T1; assets/first-run ✅T2 (the scout's "largest un-started gap"); lifecycle+immersive+banner ✅T3; CI libmpv ride-along + dispatch trigger + ABI matrix ✅T4; TV adb verification ✅T5; emulator ✅T6; out-of-scope list preserved ✅constraints/T7.
- Ambiguities resolved: no-QML = retire-the-break (fix 3 sites) not full variant maintenance; bootstrap overwrite semantics stated (stock themes refresh on version bump, addons copy-if-absent, user themes untouched); controller = follow-up (remote D-pad + virtual pad cover v1); x86_64 libmpv = verify-first with a documented fallback.
- Permission hygiene: downloads + license acceptance surfaced at execution with sizes/sources despite plan-level pre-authorization; TV developer-mode is the user's hands.
