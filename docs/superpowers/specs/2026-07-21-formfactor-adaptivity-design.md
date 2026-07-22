# Subsystem D: Form-Factor Adaptivity (TV + Mobile) — Design

**Date:** 2026-07-21
**Status:** Phase 2 built + emulator-verified: Android APKs (arm64 + x86_64) green in CI; themed home boots on Android (assets bootstrap proven), auto form-factor detection live, touch/OSK/Back/lifecycle smoke passed (x86_64 emulator). TV-DEVICE PASS PENDING (user-deferred) — leanback launcher/banner, TV auto-detect, remote walk, video+core playback on hardware remain unverified.
**Origin:** The UI-refactor request's last clause — "All UI and Player UI should be
intuitive on all systems such as tv or mobile" — after subsystems A (Nav Contract)
and B (themed surfaces everywhere, themed default ON) closed.

## Decisions (user-set)

- **Scope: both phases, sequenced under this one spec.** Phase 1 makes the themed UI
  form-factor adaptive, entirely buildable and verifiable on the desktop. Phase 2
  provisions the result onto Android / Android TV.
- **Verification hardware:** an Android TV device (adb sideload) + the Android Studio
  emulator for phone-shaped verification. No physical phone.
- **Mode selection: auto + override.** Detection picks a sensible default; an explicit
  "Display mode" setting always wins.
- **Touch scope: all four surfaces** — menu navigation, video/audio player chrome,
  the four readers, and an on-screen virtual gamepad for RetroView.

## Architecture (Approach A — form-factor layer inside the theme engine)

One `FormFactor` input threads through the existing theme engine; themes are not
forked per device. Touch becomes a gesture adapter that feeds the Nav Contract —
never a parallel input path. (The B-track lessons are load-bearing here: the two
worst whole-branch findings — the mouse-bypass capture leak and the ThemedPanelHost
re-entrancy family — were both input sneaking around the graph. Touch enters through
the graph or not at all.)

### Phase 1 — Adaptive UI (desktop-verifiable)

**1. FormFactor core** (`native/src/theme2/FormFactor.{h,cpp}`)

- Mode enum: `Desktop | Tv | Mobile`. Stored setting `display/mode` =
  `auto | desktop | tv | mobile` (default `auto`), surfaced as a Choice row in the
  themed Appearance panel (setter-verbatim discipline, like every panel row).
- Auto resolution: Android + leanback feature → Tv; Android + touchscreen → Mobile;
  desktop → Desktop. A desktop session that is fullscreen on a large display may
  show a ONE-TIME "Use TV mode?" prompt (NavConfirm, Cancel-focused, never forced,
  never repeated after either answer; stored `display/tvPromptDone`).
- Derived tokens (one struct, resolved from mode): `uiScale` (Desktop 1.0, Tv ~1.3,
  Mobile ~1.15 — tuned during implementation), `minHitPx` (Mobile ≥ the 44px-equivalent
  scaled by DPR; Desktop/Tv unset), safe-area insets (Tv: ~5% overscan margin each
  edge; Mobile: OS safe areas in Phase 2), `density` (row heights / spacing factor).
- Exposure: ONE QML context property (`form.mode`, `form.uiScale`, ...) registered
  beside the existing theme/nav properties; a C++ singleton accessor for widget-side
  consumers (players, OSK, Esc menu). `formFactorChanged` signal; mode change
  restyles live surfaces via the same mechanism as the Appearance theme
  apply-on-select (restyle, no underlay rebuild; full re-layout lands on surface
  rebuild). Optional per-mode theme overrides in theme.json (`"tv": { ... }` blocks,
  the `settingsPanel` precedent) — engine applies tokens by default so every
  existing theme works unmodified.

**2. TV pass (10-foot)**

- Safe-area insets applied at every surface root: ThemeView, ThemedPanelHost /
  SettingsPanel.qml, reader chrome, player chrome, NavOverlay/Osk/Esc menu.
- Distance sizing: `uiScale` on fonts, row heights, OSK keys, Esc-menu items,
  panel rows. Focus visibility spot-checked across all six themes at TV scale.
- Controller-first polish only; no layout forking.

**3. Touch input model** (`native/src/ui/nav/NavTouch.{h,cpp}` + QML handlers)

- A gesture adapter translating touch into graph operations:
  - tap on an item → `select(zone, index)` then `activate()` through the SAME
    host dispatch every other input uses (the hardened re-entrancy machinery);
  - flick/drag on a zone → kinetic viewport scroll (contentY only; selection
    unchanged — the next tap commits selection);
  - left-edge swipe → the one Back router; Mobile mode additionally shows a
    persistent visible Back affordance on themed surfaces.
- Legacy MouseArea paths in themed QML are REPLACED by the adapter (the
  no-direct-selection-writes CI grep already polices bypasses; extend it to
  police direct activate-from-touch where applicable).
- `minHitPx` enforced in Mobile mode: panel rows, OSK keys, item delegates,
  player chrome buttons.

**4. Player + reader touch**

- Video/audio (mpv chrome): tap toggles chrome visibility; seek bar draggable
  (drag → the existing player seek command path); double-tap left/right = ±10s.
  Swipe-for-volume: DEFERRED follow-up.
- Readers: tap zones (left/right = page turn, center = chrome toggle); swipe page
  turns; pinch-to-zoom on comic/PDF via PinchHandler feeding the EXISTING zoom
  APIs. Ebook font controls get `minHitPx`.
- Audiobook / nowplaying transport touch-sized in Mobile mode.

**5. Virtual gamepad (RetroView overlay)**

- One standard QML overlay: D-pad, A/B/X/Y, L/R, Start/Select; opacity setting.
  Shown in Mobile mode (toggleable elsewhere). Injects into the same input path
  the keyboard mapping uses (libretro input state), so cores see a normal pad.
- v1 is ONE layout; per-core/custom layouts are out of scope.

**6. Verification (Phase 1)**

- Probes: FormFactor token-derivation unit probe (mode × tokens table);
  touch-adapter translation probe (synthetic QTouchEvent sequences → asserted
  graph select/activate/back ops, incl. flick-does-not-change-selection);
  inset-application probe (root margins reflect tokens per mode). Registered in
  run-headless-probes.sh AND buildable in ci.yml (the CI-must-build-it lesson).
- uitest.py grows a `touch` verb (tap x y / flick / pinch) so live walks can
  drive gestures against the deployed build.
- Live: TV-mode walk (six themes, screenshots), Mobile-mode walk driven by
  synthesized touch (menus, a panel edit via OSK, player chrome, one reader,
  virtual pad pressing buttons in a running core).
- Perf gates unchanged: startup medians ±20%, nav.select no regression.

### Phase 2 — Android provisioning (TV-first)

- Bring the QML theme engine into the Android build: remove the Android exclusion
  (`native/CMakeLists.txt` gates QML off on Android today — Android currently
  ships the classic Widgets UI that B2 retired on desktop); Qt-for-Android
  qtdeclarative kit; bundle themes + addon JS as Android assets copied to
  `AppPaths::dataDir()` on first run (the android/README plan).
- Ride the existing release.yml self-provisioning CI job for libmpv/SDL2 (arm64).
- Lifecycle: pause/resume core + mpv on `applicationStateChanged`; immersive
  fullscreen for video/RetroView; TV manifest polish (real 320×180 banner —
  leanback + touchscreen-not-required are already in the manifest).
- Input: Android TV D-pad/Back → Qt keys → the Nav Contract (verify, expected
  free); phone touch → the Phase 1 adapter (expected free); phone Back button →
  the Back router.
- Verification: the user's Android TV device via adb (launcher row entry, remote
  D-pad full walk, a movie via libmpv, an NES core with a controller); the
  emulator for phone-shaped touch walks + virtual pad.
- OUT of scope (per native/docs/android-port.md, unchanged): external standalone
  emulators (already gated), Play Store distribution, SAF folder-picker polish
  beyond what storage access minimally needs.

**Phase 2 known risks (spec-level, resolved during planning/implementation):**
1. The CI Android job has NEVER had a green run ("needs a CI run to confirm the
   Qt-for-Android/NDK/QT_HOST_PATH wiring").
2. ABI mismatch for the emulator: CI provisions arm64 libmpv + cores; the
   emulator is x86_64. Either provision an x86_64 variant for a debug/emulator
   build, or use ARM system images (slow), or accept TV-device-only for
   native-playback verification and emulator for UI-only walks.
3. QML-on-Android provisioning is the step the original port doc explicitly
   deferred; unknown-unknowns likely (asset qrc paths, software rendering,
   QQuickWidget-on-Android behavior — QQuickWidget is used on desktop; Android
   may need QQuickView/window rework — investigate FIRST in Phase 2 planning).

## Success criteria

- Phase 1: desktop walks in Tv and Mobile modes pass with zero graph bypasses
  (CI greps + probes green); all six themes legible at TV scale; player, readers,
  OSK, and virtual pad finger-operable via synthesized touch; startup/nav perf
  gates unchanged.
- Phase 2: on the Android TV device — appears in the TV launcher, full remote
  walk of the themed UI, a movie plays, an NES core plays with a controller;
  on the emulator — touch navigation and the virtual pad work phone-shaped.

## Non-goals

- Per-form-factor layout forks (Approach B) — one themed UI, token-adapted.
- Per-core custom virtual-pad layouts; swipe-volume; iOS (not feasible — JIT);
  Play Store packaging; casting/netplay changes (separately tracked as unverified).

## Phase 1 follow-ups (deferred from Tasks 1–7 verification)

Non-blocking items surfaced during Phase 1; carry into Phase 2 planning or a polish pass:

- **Xmb / Channels kinetic scroll** — these two elements are `Item` + positioned
  Repeaters (custom slide / paged), not Views, so mobile kinetic flick was deferred
  (tap-to-move + page arrows kept). No custom physics until there's a real need.
- **Xmb leaf-category touch open** — a leaf category with no item column (Settings)
  opens by tap-to-select then tap-again on the focal icon (`gotoCat` "no column → open"
  branch); confirm this two-tap-to-open reads well on real touch hardware.
- **Ebook (reflow) pinch** — excluded by design; pinch-to-zoom is scoped to comic/PDF
  (`zoomDelta`) only. Revisit if reflow font-scale-by-pinch is wanted.
- **Swipe-for-volume** — deferred player gesture (spec non-goal for Phase 1).
- **Carousel left-edge contention** — the 12px edge-back `MouseArea` overlaps a
  horizontal Carousel's left edge; verified fine on desktop-synth touch, wants a
  real-device check that a genuine horizontal flick from the very edge still pages.
- **Mixed-DPR / multi-monitor** — tokens verified at DPR 1.5; validate scale/inset
  math across mixed-DPR displays and on a genuine phone DPR.
- **Qt6::GuiPrivate coupling** — `mymediavault` links `Qt6::GuiPrivate` for the qpa
  touch-synthesis header (uitest); recheck this still resolves on a Qt version bump
  and on the Android Qt kit.
- **grabGesture(PinchGesture) follow-up** — intentionally NOT used (non-deterministic
  on this build); pinch reads raw two-finger separation. Revisit whether the native
  gesture recognizer is dependable on Android before relying on it.
- **Magic-number constants** — the 80px swipe / 12px edge / 24px slop / 350ms
  double-tap / 15% pinch-step literals are inline; fold into named constants.
- **probe_meta / probe_navqml first-run flake** — occasionally FAIL on the first
  runner pass in a full-suite context (offscreen QML/GPU timing); green standalone and
  on re-run. Pin down the warmup ordering so CI doesn't need a retry.
- **ReaderChromeHost outer-strip staleness on mode flip** — `layoutStrips` doesn't
  re-run on `FormFactor::changed`, so the outer strip keeps its old sizing until the
  reader is re-presented. Self-heals on next present; wire it to the signal if a live
  mid-read flip needs to reflow.
- **Osk / NavOverlay construction-read metrics** — these read their sizing statics at
  construction, so an overlay/OSK left open across a mode change keeps the old metrics
  until it's closed and reopened. `applyFormFactorWidgets` pushes new statics but doesn't
  re-lay-out an already-open surface. Fine in practice (a mode flip closes the surface);
  revisit if a mid-open reflow is ever needed.
- **VirtualPad::layoutZones resize-only** — the pad recomputes its hit zones on resize;
  a mode flip that leaves the widget geometry unchanged keeps the old hit sizes until the
  next resize. Force a relayout on `FormFactor::changed` if a same-geometry mode flip must
  resize the touch zones.
- **Mobile tap-interrupting-flick one-tap activation** — a tap that interrupts an
  in-progress flick may activate the item under the finger. Wants a real-device check with
  the Carousel left-edge item to confirm it doesn't mis-fire an activation.

## Phase 2 follow-ups (deferred from Tasks 1–6 + emulator verification)

Phase 2 built the Android provisioning and proved it on the x86_64 emulator (APKs green in
CI on both ABIs; themed home boots, `assets:/mmv` extraction proven on-device, auto→Mobile,
touch/OSK/Back/lifecycle smoke passed). The **TV-device pass is user-deferred** ("close it out
for now until we can test more") — the items below carry the unverified surface and the
structural debt into a later hardware session.

**TV-device pass (deferred — the whole leanback path is unverified):**

- **TV-device verification runbook (make it turnkey later).** No Android-TV **x86_64** system
  image exists for any API (TV images are only `x86` 32-bit or `arm64-v8a`), so the emulator
  can only run a TV-**res** phone image — the real leanback launcher/banner, TV auto-detect,
  remote D-pad walk, and native video+core playback were never exercised. When TV hardware is
  available: enable ADB debugging on the device → `adb connect <device-ip>:5555` (or USB) →
  `adb install -r MyMediaVault-android-arm64.apk` (TV devices are arm64; the arm64 APK from the
  green CI run is the artifact) → confirm the app appears in the **leanback launcher row** (the
  320×180 banner + `LEANBACK_LAUNCHER` category are already in the manifest, Task 3) →
  `FormFactor::resolveAuto()` should return **Tv** via `UiModeManager.getCurrentModeType() ==
  UI_MODE_TYPE_TELEVISION` → full remote D-pad walk of the themed UI, one movie via libmpv, one
  NES core with a physical controller. This is the Phase-2 success criterion that remains open.

**Android structural / packaging debt (surfaced during Tasks 4–6):**

- **SDL2 controller on Android (SDLActivity).** SDL2-for-Android (gamepad) is still omitted from
  the APK (Task 4). Physical-controller input on Android needs the SDLActivity/JNI integration;
  v1 relies on remote D-pad → Qt keys and the on-screen virtual pad. Wire SDL2 when a real pad
  path is wanted on-device.
- **APK release signing.** Both APKs are **debug (unsigned)** (Task 4). Release distribution
  needs a keystore + signing config in `release.yml` before any non-dev install.
- **SAF (Storage Access Framework) folder picking.** Android scoped storage means user library
  folders should be chosen via SAF rather than raw filesystem paths; only the minimal storage
  access needed to boot is in place. Out of scope for the emulator pass; needed for real
  user-library use on-device.
- **CloudSync desktop.ini path-rewrite hazard.** CloudSync's Windows `desktop.ini` path-rewrite
  handling is a desktop artifact; validate/exclude it on the Android sync path so a Windows-only
  hidden file is never written into `AppPaths::dataDir()` on Android.

**Confirmed low-severity / robustness follow-ups:**

- **Mobile "back-at-root" overlay clipped (LOW, emulator finding #5).** Pressing `KEYCODE_BACK`
  at the home root brings up a right-anchored menu/confirm overlay that renders mostly clipped
  beyond the right edge on the 1080×2400 phone aspect (only a sliver + one button visible; a
  swipe did not pull it in). All primary screens lay out correctly — isolated overlay-positioning
  bug in Mobile mode. Fold into the mobile-layout pass. Evidence:
  `docs/superpowers/verification/2026-07-21-android/d2t6fix-menu.png`, `…-drawer.png`.
- **`resolveAuto()` JNI missing exception check (defensive).** The `Q_OS_ANDROID` branch of
  `FormFactor::resolveAuto()` calls `getSystemService` / `getCurrentModeType` via `QJniObject`
  without a `QJniEnvironment::checkAndClearExceptions()` after the calls. It works (and falls
  back to Mobile on an invalid object), but a pending JNI exception should be cleared defensively
  so a later JNI call isn't tripped by a leftover pending exception.
- **libc++ `find | head` fragility in `release.yml`.** The "Provide libmpv" step locates the NDK
  runtime with `find "$ANDROID_NDK_ROOT" -path "*/$TRIPLE/libc++_shared.so" | head -1` to swap in
  jdtech's newer libc++ (fix for the pre-main `__from_chars_floating_point` UnsatisfiedLinkError).
  A `head -1` over an unordered `find` is order-fragile if the NDK layout ever ships more than one
  match per triple; pin the expected path or assert a single match.
- **Immersive re-assert after IME (Task 3).** Qt re-applies fullscreen system-UI on window focus;
  if a specific device ROM drops the immersive flags after an IME/permission dialog, a re-assert
  on `ApplicationActive` may be needed. Left out pending device evidence (avoid speculative JNI) —
  revisit on the TV/phone hardware pass.
- **Screenshot hygiene.** The Task-6 fix round committed 14 verification PNGs to the **repo root**;
  they were relocated to `docs/superpowers/verification/2026-07-21-android/` (byte-identical
  duplicates dropped) during close-out. Verification screenshots belong under
  `docs/superpowers/verification/<date>-<topic>/`, never the repo root; earlier-phase `b2t*`/`d1t*`
  root PNGs remain untracked session artifacts (left uncommitted).
- **Play Store packaging — out of scope** (spec non-goal, unchanged): AAB packaging, Play signing,
  and store metadata are explicitly not part of this subsystem.
