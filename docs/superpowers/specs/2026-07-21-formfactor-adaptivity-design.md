# Subsystem D: Form-Factor Adaptivity (TV + Mobile) — Design

**Date:** 2026-07-21
**Status:** Phase 1 complete: TV + Mobile modes on desktop; auto+override; touch on the contract. Phase 2 (Android provisioning) next.
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
